/* Per-process address spaces.
 *
 * Every task gets its own PML4. The top two regions are shared by all tasks:
 *
 *   PML4[511] -> the higher-half kernel (one set of tables, made by vmm_init)
 *   PML4[0]   -> per-process: low-identity direct map (kernel, US=0) in pd[0]
 *                plus this task's own user window (US=1) in pd[2]
 *
 * Because each task has its own user page table backed by its own physical
 * frames, two tasks mapping the same user virtual address see different memory.
 * The low 2 MiB identity map is replicated into every PML4 so the kernel can
 * always reach VGA / page-table frames whichever task's CR3 is loaded.
 */
#include "vmm.h"
#include "fs.h"
#include "ata.h"
#include "blockdev.h"     /* the ELF loader reads programs through the fs's block device */
#include "console.h"
#include "syscall.h"
#include "cpu.h"
#include "spinlock.h"
#include <stdint.h>

/* Guards the physical frame allocator (next_frame bump + free list). Most callers
 * also hold sched_lock, but window surfaces are allocated from the syscall layer
 * without it, so the allocator serialises itself. */
static spinlock_t frame_lock = SPINLOCK_INIT;

#define P    0x1
#define W    0x2
#define U    0x4
#define HUGE 0x80

#define GiB            0x40000000ULL
#define KERNEL_VMA     0xFFFFFFFF80000000ULL  /* higher-half base (see linker.ld) */
#define KERNEL_PHYS    0x200000      /* where the bootloader put the kernel */
#define USER_STACK_PAGES 16          /* 64 KiB stack, just below the data page    */

/* The kernel identity-maps ALL of physical RAM (2 MiB huge pages) so it can reach
 * any frame it hands out; the frame pool is that span minus the kernel image and
 * the 2 MiB user-window slot (which is per-process, not identity). The layout is
 * read from the firmware e820 map at init (`vmm_init` -> `parse_e820`), so the
 * pool spans EVERY RAM region the machine reports -- including RAM remapped ABOVE
 * the 4 GiB PCI hole -- with no fixed cap. On a PC the sub-4 GiB RAM stops at the
 * MMIO hole (~0xC0000000 on QEMU/i440fx) and the rest reappears at 0x100000000, so
 * the map and the allocator are MULTI-REGION: they skip the hole. `ident_top` is
 * the top of the highest RAM region (2 MiB-aligned), `ident_pds` the GiB-granular
 * page directories the low map spans. IDENT_MIN is a floor if e820 is unavailable. */
#define IDENT_MIN      0x2000000ULL  /* 32 MiB floor */
#define WINDOW_LO      0x400000ULL   /* user window virtual slot (USER_VBASE) ... */
#define WINDOW_HI      0x600000ULL   /* ... not identity-mapped, so skip in the pool */

static uint64_t ident_top  = IDENT_MIN;   /* top of the highest RAM region (set in vmm_init) */
static int      ident_pds  = 1;           /* GiB page directories the low map spans (1/GiB) */

/* The physical RAM regions from e820, 2 MiB-aligned and sorted ascending (region 0
 * is the sub-4 GiB block; later regions are RAM remapped above the hole). */
#define MAXRAM 8
static struct ramrgn { uint64_t base, top; } ram[MAXRAM] = { { 0, IDENT_MIN } };
static int      ram_n     = 1;
static uint64_t ram_total = IDENT_MIN;     /* sum of region sizes (real RAM, hole excluded) */
static int frame_in_ram(uint64_t phys) {   /* is this physical frame inside a RAM region? */
    for (int i = 0; i < ram_n; i++) if (phys >= ram[i].base && phys < ram[i].top) return 1;
    return 0;
}
static int      fb_pdpt_slot = 1;         /* pdpt_low slot for the on-demand user FB map  */
static uint64_t user_fb_vaddr = GiB;      /* = fb_pdpt_slot GiB; placed just above all RAM */

/* Window surfaces (shared between an app and the compositor) live in their own
 * PDPT slot, just past the framebuffer slot. Each window id gets a fixed
 * SURF_SLOT_BYTES vaddr window inside it; the same physical frames are mapped
 * here in both the owning app and the compositor (4 KiB pages, US=1). */
#define SURF_SLOT_BYTES 0x800000ULL       /* 8 MiB of vaddr per window (<=2048 pages)  */
static int      surf_pdpt_slot = 2;
static uint64_t surf_base = 2 * GiB;

/* Anonymous mappings (SYS_MMAP): private RAM a task asks for at runtime -- the
 * compositor's full-screen back buffer, a userspace heap arena, etc. They start
 * just past the surface slot and GROW UPWARD across PDPT slots on demand (each
 * task bumps its own brk), so the size is bounded only by RAM, never by a fixed
 * vaddr window. Freed (frames and all) when the address space dies. */
static int      anon_pdpt_slot = 3;
static uint64_t anon_base = 3 * GiB;

/* Device-MMIO window. Device BARs (the AHCI ABAR, an NVMe register block, ...)
 * live in the PCI hole, so they are NOT covered by the RAM identity map and need
 * an explicit mapping. We claim a FIXED higher-half PDPT slot (508, just below the
 * LAPIC/kernel/framebuffer slots at 509/510/511) and bump-allocate 2 MiB
 * cache-disabled huge pages out of it on demand (vmm_map_mmio). The PD hangs off
 * the shared higher-half PDPT, so the mapping appears in every address space. */
#define MMIO_PD_SLOT  508
#define MMIO_VBASE    0xFFFFFFFF00000000ULL    /* KERNEL_VMA - 2 GiB (slot 508) */
static uint64_t *mmio_pd = 0;                  /* PD for the MMIO window (NULL until vmm_init) */
static int       mmio_next = 0;                /* next free 2 MiB slot in mmio_pd */

extern char __bss_end[];             /* end of the kernel image (virtual; linker.ld) */

/* upt[] indices for the user window (USER_VBASE = 0x400000 -> pt index 0). The
 * window is 512 pages (2 MiB). Layout: code/data/bss at the bottom, the stack
 * just below the role/data page at the very top, with a large unmapped gap in
 * between (a stack overflow grows down, away from both code and data). */
#define UPT_DATA_IDX   511                              /* 0x5FF000 == USER_DATA_VADDR */
#define UPT_STACK_HI   UPT_DATA_IDX                      /* exclusive top of the stack  */
#define UPT_STACK_LO   (UPT_STACK_HI - USER_STACK_PAGES) /* 495 -> 0x5EF000             */

/* Loadable segments may fill the user window from USER_VBASE up to the stack
 * region -- the program is bounded by the window (~1.9 MiB), not a fixed buffer. */
#define USER_SEG_LIMIT (USER_VBASE + UPT_STACK_LO * 0x1000ULL)    /* 0x5EF000 */

/* Just enough ELF64 to load a static executable. */
struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
#define PT_LOAD     1
#define ET_EXEC     2
#define EM_X86_64   62

/* Physical-page allocator over free RAM in the first 2 MiB (kernel lives at
 * 2 MiB). Bumps a pointer for fresh frames and keeps a free list of reclaimed
 * ones, so exited tasks return their page tables to the pool. */
#define PHYS_MASK 0x000ffffffffff000ULL   /* page-frame bits of a PTE */

/* The frame pool is the identity-mapped low memory [0x100000, IDENT_TOP), minus
 * two reserved regions the bump pointer steps over: the kernel image (at
 * KERNEL_PHYS) and the 2 MiB user-window slot at [WINDOW_LO, WINDOW_HI) (which is
 * per-process, not identity-mapped, so frames can't live there). It panics
 * rather than scribbling on the kernel when it runs dry. Reclaimed frames come
 * back via the free list. */
static uint64_t next_frame = 0x100000;
static int      cur_rgn   = 0;       /* RAM region the bump pointer is currently in */
static uint64_t shared_pdpt_high;    /* physical addr, shared kernel mapping */
static uint64_t free_list = 0;       /* phys addr of head, 0 if empty */

/* Framebuffer geometry, captured at init so SYS_FBINFO can map it on demand. */
static int      fb_present = 0;
static uint64_t fb_phys_base;
static uint32_t fb_width, fb_height, fb_pitch;

static uint64_t kernel_phys_end(void) {
    uint64_t end = (uint64_t)__bss_end - KERNEL_VMA + KERNEL_PHYS;
    return (end + 0xfff) & ~0xfffULL;                 /* page-aligned */
}

/* Advance the bump pointer to the next allocatable RAM frame, stepping over the
 * kernel image, the user-window slot, and inter-region gaps (the sub-4 GiB PCI
 * hole), crossing into the next RAM region as needed. Returns 0 when the pool is
 * exhausted. Caller holds frame_lock. */
static uint64_t bump_next(void) {
    for (;;) {
        if (next_frame >= KERNEL_PHYS && next_frame < kernel_phys_end()) { next_frame = kernel_phys_end(); continue; }
        if (next_frame >= WINDOW_LO   && next_frame < WINDOW_HI)         { next_frame = WINDOW_HI;        continue; }
        if (next_frame <  ram[cur_rgn].base) { next_frame = ram[cur_rgn].base; continue; }
        if (next_frame >= ram[cur_rgn].top) {            /* past this region -> next one (skip the hole) */
            if (cur_rgn + 1 >= ram_n) return 0;
            cur_rgn++; next_frame = ram[cur_rgn].base; continue;
        }
        return next_frame;
    }
}

static uint64_t *frame_alloc(void) {
    uint64_t lf = spin_lock_irqsave(&frame_lock);
    uint64_t f;
    if (free_list) {
        f = free_list;
        free_list = *(volatile uint64_t *)f;
    } else {
        if (!bump_next()) {
            console_puts("[kernel] PANIC: out of physical frames\r\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
        f = next_frame;
        next_frame += 0x1000;
    }
    spin_unlock_irqrestore(&frame_lock, lf);
    uint64_t *p = (uint64_t *)f;     /* identity-mapped, so virt == phys */
    for (int i = 0; i < 512; i++)
        p[i] = 0;
    return p;
}

static void frame_free(uint64_t f) {
    uint64_t lf = spin_lock_irqsave(&frame_lock);
    *(volatile uint64_t *)f = free_list;
    free_list = f;
    spin_unlock_irqrestore(&frame_lock, lf);
}

/* Allocate `n` physically-contiguous, zeroed frames from the bump pointer
 * (stepping over the reserved regions as a block). Multi-page kernel stacks need
 * this -- they must be virtually contiguous, and here virt == phys. 0 on OOM. */
static uint64_t frame_alloc_contig(int n) {
    uint64_t need = (uint64_t)n * 0x1000;
    uint64_t lf = spin_lock_irqsave(&frame_lock);
    uint64_t base;
    for (;;) {
        if (!bump_next()) { spin_unlock_irqrestore(&frame_lock, lf); return 0; }
        uint64_t end;
        base = next_frame; end = base + need;
        if (base < kernel_phys_end() && end > KERNEL_PHYS) { next_frame = kernel_phys_end(); continue; }
        if (base < WINDOW_HI && end > WINDOW_LO)           { next_frame = WINDOW_HI;        continue; }
        if (end > ram[cur_rgn].top) {                      /* would span the region end/hole -> next region */
            if (cur_rgn + 1 >= ram_n) { spin_unlock_irqrestore(&frame_lock, lf); return 0; }
            cur_rgn++; next_frame = ram[cur_rgn].base; continue;
        }
        next_frame = end;
        break;
    }
    spin_unlock_irqrestore(&frame_lock, lf);
    for (uint64_t f = base; f < base + need; f += 0x1000) {     /* zero outside the lock */
        uint64_t *p = (uint64_t *)f;
        for (int i = 0; i < 512; i++) p[i] = 0;
    }
    return base;
}

/* Kernel stacks come from the frame pool, not a fixed .bss array, so the number
 * of tasks scales with RAM rather than a compile-time cap. */
uint64_t vmm_alloc_kstack(void)        { return frame_alloc_contig(KSTACK_SZ / 0x1000); }
void     vmm_free_kstack(uint64_t base) {
    if (!base) return;
    for (uint64_t f = base; f < base + KSTACK_SZ; f += 0x1000) frame_free(f);
}

/* Total RAM in bytes, from QEMU's fw_cfg (key FW_CFG_RAM_SIZE = 0x0003, an LE
 * uint64). Falls back to the floor if the value looks unavailable/insane, so the
 * pool always covers at least IDENT_MIN. */
#define FW_CFG_SELECT    0x510
#define FW_CFG_DATA      0x511
#define FW_CFG_RAM_SIZE  0x0003
static uint64_t detect_ram(void) {
    outw(FW_CFG_SELECT, FW_CFG_RAM_SIZE);
    uint64_t ram = 0;
    for (int i = 0; i < 8; i++) ram |= (uint64_t)inb(FW_CFG_DATA) << (i * 8);
    if (ram < IDENT_MIN || ram > 512ULL * GiB) ram = IDENT_MIN;   /* sanity */
    return ram & ~0x1fffffULL;                                    /* 2 MiB-aligned */
}

/* --- QEMU fw_cfg e820 memory map -------------------------------------------
 * FW_CFG_RAM_SIZE can't describe the sub-4 GiB PCI hole, so to map RAM ABOVE it
 * we read the firmware's e820 table (fw_cfg file "etc/e820"), which QEMU fills
 * with the real RAM regions (below-4G + above-4G) plus reserved holes. The
 * fw_cfg DIRECTORY fields are big-endian; the file payload is guest LE. */
#define FW_CFG_FILE_DIR  0x0019
#define E820_RAM         1
struct e820ent { uint64_t addr; uint64_t len; uint32_t type; } __attribute__((packed));
static void fw_read(void *buf, int n) { uint8_t *p = (uint8_t *)buf; for (int i = 0; i < n; i++) p[i] = inb(FW_CFG_DATA); }
/* Locate fw_cfg file "etc/e820": returns its select key, writes the entry count
 * to *n, or returns 0 if the firmware exposes no such file. */
static uint16_t e820_find(int *n) {
    outw(FW_CFG_SELECT, FW_CFG_FILE_DIR);
    uint32_t count; fw_read(&count, 4); count = __builtin_bswap32(count);
    for (uint32_t i = 0; i < count; i++) {
        struct { uint32_t size; uint16_t select; uint16_t res; char name[56]; } __attribute__((packed)) f;
        fw_read(&f, sizeof f);
        const char *want = "etc/e820"; int eq = 1;
        for (int k = 0; k < 9; k++) if (f.name[k] != want[k]) { eq = 0; break; }
        if (eq) { *n = (int)(__builtin_bswap32(f.size) / sizeof(struct e820ent)); return __builtin_bswap16(f.select); }
    }
    return 0;
}
/* Parse the firmware e820 into 2 MiB-aligned RAM regions (`ram[]`, sorted), and set
 * `ram_total` (real RAM) + `ident_top` (top of the highest region). Falls back to a
 * single [0, detect_ram()) region if the firmware exposes no e820 file. */
static void parse_e820(void) {
    int n = 0; uint16_t key = e820_find(&n);
    int cnt = 0;
    if (key && n > 0) {
        outw(FW_CFG_SELECT, key);
        for (int i = 0; i < n; i++) {
            struct e820ent e; fw_read(&e, sizeof e);    /* must read every entry to keep the stream aligned */
            if (e.type != E820_RAM || cnt >= MAXRAM) continue;
            uint64_t b = (e.addr + 0x1fffffULL) & ~0x1fffffULL;   /* round base up   */
            uint64_t t = (e.addr + e.len) & ~0x1fffffULL;         /* round top down  */
            if (t <= b || b > 512ULL * GiB) continue;             /* skip empty / the ~1 TB reserved tail */
            ram[cnt].base = b; ram[cnt].top = t; cnt++;
        }
    }
    if (cnt == 0) { ram[0].base = 0; ram[0].top = detect_ram(); cnt = 1; }   /* fallback: contiguous */
    for (int i = 1; i < cnt; i++)                       /* insertion-sort by base (tiny n) */
        for (int j = i; j > 0 && ram[j].base < ram[j - 1].base; j--) {
            struct ramrgn tmp = ram[j]; ram[j] = ram[j - 1]; ram[j - 1] = tmp;
        }
    ram_n = cnt;
    ram_total = 0;
    for (int i = 0; i < ram_n; i++) ram_total += ram[i].top - ram[i].base;
    ident_top = ram[ram_n - 1].top;                    /* top of the highest RAM region */
}

/* Build a per-process low-half identity map spanning [0, ident_top): up to
 * `ident_pds` page directories (one per GiB) of 2 MiB huge pages (kernel-only,
 * US=0), linked into pdpt_low[0..ident_pds-1] (US=1 so the user window inside pd[0]
 * is reachable). Only pages that fall inside a RAM region are mapped, so the PCI
 * hole between regions stays absent; a GiB slot that holds NO RAM is skipped
 * entirely (left absent). Returns the first PD, which carries the user-window slot
 * at [2] (pd0 is always built -- the sub-4 GiB region always covers [0,1 GiB)). */
static uint64_t *build_low_map(uint64_t *pdpt_low) {
    uint64_t *pd0 = 0;
    for (int pd = 0; pd < ident_pds; pd++) {
        uint64_t gib = (uint64_t)pd * GiB;
        if (pd != 0) {                                /* skip a hole-only GiB slot (but never pd0) */
            int any = 0;
            for (int i = 0; i < 512; i++) if (frame_in_ram(gib + (uint64_t)i * 0x200000ULL)) { any = 1; break; }
            if (!any) { pdpt_low[pd] = 0; continue; }
        }
        uint64_t *pdt = frame_alloc();                /* zeroed: unused entries stay absent */
        for (int i = 0; i < 512; i++) {
            uint64_t phys = gib + (uint64_t)i * 0x200000ULL;
            if (frame_in_ram(phys)) pdt[i] = phys | P | W | HUGE;
        }
        pdpt_low[pd] = (uint64_t)pdt | P | W | U;
        if (pd == 0) pd0 = pdt;
    }
    return pd0;
}

void vmm_init(struct boot_info *bi) {
    parse_e820();                                     /* fill ram[], ram_n, ram_total, ident_top */
    ident_pds = (int)((ident_top + GiB - 1) / GiB);   /* GiB slots up to the highest region (>=1) */
    fb_pdpt_slot  = ident_pds;                        /* first PDPT slot past ALL RAM */
    user_fb_vaddr = (uint64_t)fb_pdpt_slot * GiB;
    surf_pdpt_slot = ident_pds + 1;                   /* window surfaces go past the FB slot */
    surf_base      = (uint64_t)surf_pdpt_slot * GiB;
    anon_pdpt_slot = ident_pds + 2;                   /* anonymous mmap region grows from here up */
    anon_base      = (uint64_t)anon_pdpt_slot * GiB;

    uint64_t *pd_high = frame_alloc();
    pd_high[0] = KERNEL_PHYS | P | W | HUGE;          /* 0xFF..80000000 -> 2 MiB */

    uint64_t *pdpt_high = frame_alloc();
    pdpt_high[510] = (uint64_t)pd_high | P | W;       /* kernel */

    /* map the framebuffer at FB_VBASE (PDPT index 511) so it lives in the
     * shared higher half and stays mapped across per-process CR3 switches */
    if (bi->console == BOOT_CONSOLE_FB) {
        uint64_t fb_aligned = bi->fb_phys & ~0x1fffffULL;
        uint64_t *pd_fb = frame_alloc();
        for (int i = 0; i < 8; i++)                   /* 16 MiB window */
            pd_fb[i] = (fb_aligned + (uint64_t)i * 0x200000) | P | W | HUGE;
        pdpt_high[511] = (uint64_t)pd_fb | P | W;
        fb_present  = 1;                              /* remember for SYS_FBINFO */
        fb_phys_base = bi->fb_phys;
        fb_width = bi->width; fb_height = bi->height; fb_pitch = bi->pitch;
    }

    /* map the Local APIC MMIO page (phys 0xFEE00000) into the shared higher half
     * at LAPIC_VBASE (PDPT_high[509]), cache-disabled -- every CPU reaches its own
     * LAPIC through this one mapping regardless of the active CR3. */
    uint64_t *pd_apic = frame_alloc();
    pd_apic[0] = 0xFEE00000ULL | P | W | HUGE | 0x10 /*PCD*/;
    pdpt_high[509] = (uint64_t)pd_apic | P | W;

    /* MMIO window (PDPT_high[508]): empty PD now, filled on demand by vmm_map_mmio. */
    mmio_pd = frame_alloc();
    pdpt_high[MMIO_PD_SLOT] = (uint64_t)mmio_pd | P | W;

    /* Also wire the MMIO PD into the CURRENTLY-LIVE bootstrap page tables. The
     * scheduler only switches CR3 to a vmm-built address space (with the shared
     * higher-half PDPT above) after fs_mount; until then kmain runs on the boot
     * loader's tables (BIOS stage1 / UEFI both run the kernel from PML4[511]). A
     * device probed during kmain -- AHCI here, and the MBR scan that reads every
     * bdev -- maps and touches its ABAR before that switch, so the window must be
     * reachable now too. The bootstrap tables live in identity-mapped RAM, so their
     * physical addresses double as kernel pointers. */
    uint64_t *cur_pml4   = (uint64_t *)vmm_current_pml4();
    uint64_t *cur_pdpthi = (uint64_t *)(cur_pml4[511] & PHYS_MASK);
    cur_pdpthi[MMIO_PD_SLOT] = (uint64_t)mmio_pd | P | W;

    shared_pdpt_high = (uint64_t)pdpt_high;

    uint64_t pool = ram_total - 0x100000
                  - (kernel_phys_end() - KERNEL_PHYS)
                  - (WINDOW_HI - WINDOW_LO);
    console_puts("[kernel] frame pool: ");
    console_putdec(pool / 0x1000);
    console_puts(" frames (");
    console_putdec(ram_total / (1024 * 1024));
    console_puts(" MiB RAM");
    if (ram_n > 1) {                                   /* multi-region: RAM spans the PCI hole */
        console_puts(", "); console_putdec(ram_n); console_puts(" regions across the 4 GiB hole");
    }
    console_puts(")\r\n");
}

/* A bare kernel address space: the shared higher half plus the low-2-MiB
 * direct map, with no user window. Used by the ring-0 idle task. */
uint64_t vmm_kernel_pml4(void) {
    uint64_t *pml4     = frame_alloc();
    uint64_t *pdpt_low = frame_alloc();

    pml4[0]   = (uint64_t)pdpt_low | P | W;
    pml4[511] = shared_pdpt_high   | P | W;
    build_low_map(pdpt_low);                           /* identity-map all RAM, no user window */
    return (uint64_t)pml4;
}

/* Map (allocating a zeroed frame on first touch) the user page containing
 * `vaddr` and return its identity-mapped physical address for the kernel to
 * write through. `vaddr` must lie in the code window [USER_VBASE, limit). */
static uint8_t *user_page(uint64_t *upt, uint64_t vaddr) {
    uint32_t idx = (uint32_t)((vaddr - USER_VBASE) >> 12);
    if (!(upt[idx] & P))
        upt[idx] = (uint64_t)frame_alloc() | P | W | U;
    return (uint8_t *)(upt[idx] & PHYS_MASK);
}

/* Stream `len` file bytes (at offset `foff` within the program file whose first
 * disk sector is `base_lba`) into the user address space at vaddr `va` -- no
 * whole-file buffer, so program size is bounded by the window, not a fixed
 * scratch. Reads up to 8 sectors per disk op and writes a page at a time.
 * Returns 0, or -1 on a disk error. */
static int load_bytes(uint64_t *upt, uint32_t base_lba, uint32_t foff,
                      uint64_t va, uint32_t len) {
    uint8_t buf[4096];                              /* 8-sector bounce */
    while (len) {
        uint32_t soff = foff & 511;                 /* byte offset into the first sector */
        uint32_t want = soff + len;                 /* bytes needed from that sector's start */
        uint32_t secs = want > sizeof buf ? sizeof buf / 512 : (want + 511) / 512;
        if (bdev_read(fs_disk_bdev(), base_lba + foff / 512, secs, buf) < 0) return -1;
        uint32_t chunk = secs * 512 - soff;         /* usable bytes this round */
        if (chunk > len) chunk = len;
        for (uint32_t i = 0; i < chunk; ) {         /* copy per page, not per byte */
            uint8_t *pg = user_page(upt, va);
            uint32_t poff = (uint32_t)(va & 0xfff);
            uint32_t run  = 0x1000 - poff;
            if (run > chunk - i) run = chunk - i;
            for (uint32_t j = 0; j < run; j++) pg[poff + j] = buf[soff + i + j];
            i  += run;
            va += run;
        }
        foff += chunk; len -= chunk;
    }
    return 0;
}

/* Create a fresh address space and load the named ELF program into it. The ELF
 * header + program headers are read first; each PT_LOAD segment is then streamed
 * straight from disk to its p_vaddr (the gap up to p_memsz stays zero, giving
 * .bss). Segments may fill the user window up to the stack (~1.9 MiB). A private
 * stack and data page round it out. Returns the PML4 phys addr and the entry
 * point in *entry; 0 on failure (no such file, bad/oversized ELF, disk error). */
uint64_t vmm_create_user(const char *prog, uint64_t *entry) {
    /* `prog` may carry arguments ("ls /tmp"): the first whitespace-delimited token is
     * the ELF path (loaded here); the *whole* string is seeded into the task's data
     * page below as its argv (USER_DATA_VADDR -> getargs() in userspace). This is the
     * argv mechanism -- no new syscall, and zero-arg execs ("twm") are unchanged. */
    char path[256];
    { int i = 0; while (prog[i] == ' ') i++;            /* skip any leading spaces */
      int k = 0; while (prog[i] && prog[i] != ' ' && k < (int)sizeof(path) - 1) path[k++] = prog[i++];
      path[k] = 0; }
    const struct tosfs_ent *e = fs_find(path);
    if (!e || e->size < sizeof(struct elf64_ehdr)) return 0;
    uint32_t lba0 = fs_base_lba() + e->start_lba;      /* partition-relative -> disk LBA */

    /* read the header + program headers (first <=4 KiB; our static binaries keep
     * the phdrs right after the ehdr, well inside this) */
    uint8_t hdr[4096];
    uint32_t hbytes = e->size < sizeof(hdr) ? e->size : sizeof(hdr);
    if (bdev_read(fs_disk_bdev(), lba0, (hbytes + 511) / 512, hdr) < 0) return 0;

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)hdr;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return 0;
    if (eh->e_ident[4] != 2) return 0;                 /* ELFCLASS64 */
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) return 0;
    if (eh->e_phentsize < sizeof(struct elf64_phdr)) return 0;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > sizeof(hdr)) return 0;

    /* Validate every PT_LOAD segment up front, before allocating anything, so a
     * malformed/oversized ELF fails without leaking page-table frames. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(hdr + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_vaddr < USER_VBASE) return 0;
        if (ph->p_vaddr + ph->p_memsz > USER_SEG_LIMIT) return 0;   /* must fit the window */
        if (ph->p_offset + ph->p_filesz > e->size) return 0;
    }

    uint64_t *pml4     = frame_alloc();
    uint64_t *pdpt_low = frame_alloc();
    uint64_t *upt      = frame_alloc();

    pml4[0]   = (uint64_t)pdpt_low | P | W | U;
    pml4[511] = shared_pdpt_high   | P | W;             /* shared kernel */
    uint64_t *pd_low = build_low_map(pdpt_low);         /* identity-map all RAM */
    pd_low[2] = (uint64_t)upt | P | W | U;              /* user window @ 0x400000 overrides 4..6 MiB */

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(hdr + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        for (uint64_t v = ph->p_vaddr; v < ph->p_vaddr + ph->p_memsz; v += 0x1000)
            user_page(upt, v);                          /* allocate (zeroed) the whole segment */
        if (load_bytes(upt, lba0, (uint32_t)ph->p_offset,  /* fill the file-backed part */
                       ph->p_vaddr, (uint32_t)ph->p_filesz) < 0) {
            vmm_destroy_user((uint64_t)pml4);           /* disk error: reclaim everything */
            return 0;
        }
    }

    for (int i = UPT_STACK_LO; i < UPT_STACK_HI; i++)       /* multi-page stack */
        upt[i] = (uint64_t)frame_alloc() | P | W | U;
    uint64_t *data = frame_alloc();
    upt[UPT_DATA_IDX] = (uint64_t)data | P | W | U;

    /* seed this task's private data page with the full command line (argv): the path
     * token is argv[0], any tail is argv[1..]. userspace reads it via getargs(). */
    char *d = (char *)data;
    int i = 0;
    while (prog[i] && i < 0xfff) { d[i] = prog[i]; i++; }
    d[i] = 0;

    *entry = eh->e_entry;
    return (uint64_t)pml4;
}

/* Map the GOP framebuffer into the *calling* process (its CR3 is live) at
 * USER_FB_VADDR with US=1, in its own PDPT slot, and fill *out with the geometry
 * and the user pointer to pixel (0,0). Idempotent. Returns 0, or -1 on a
 * text-mode boot (no framebuffer). `out` is a pointer in the caller's space. */
int vmm_map_user_fb(struct fbinfo *out) {
    if (!fb_present) { out->present = 0; return -1; }

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)(cr3 & PHYS_MASK);
    uint64_t *pdpt = (uint64_t *)(pml4[0] & PHYS_MASK);

    if (!(pdpt[fb_pdpt_slot] & P)) {                   /* first call: build the mapping */
        uint64_t fb_aligned = fb_phys_base & ~0x1fffffULL;
        uint64_t *pd = frame_alloc();
        for (int i = 0; i < 8; i++)                    /* 16 MiB window, user-accessible */
            pd[i] = (fb_aligned + (uint64_t)i * 0x200000) | P | W | U | HUGE;
        pdpt[fb_pdpt_slot] = (uint64_t)pd | P | W | U;
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");   /* flush TLB */
    }

    out->present = 1;
    out->width   = fb_width;
    out->height  = fb_height;
    out->pitch   = fb_pitch;
    out->vaddr   = user_fb_vaddr + (fb_phys_base & 0x1fffffULL);
    return 0;
}

/* --- window surfaces (shared memory between an app and the compositor) ----- *
 * A surface is a run of contiguous frames. The same frames are mapped (US=1,
 * 4 KiB pages) into both the owner and the compositor at vmm_surface_vaddr(id),
 * so each draws/reads the same pixels through its own page tables. */
uint64_t vmm_ram_bytes(void)            { return ram_total; }   /* real RAM, the PCI hole excluded */
void vmm_fb_size(uint32_t *w, uint32_t *h) { *w = fb_width; *h = fb_height; }
uint64_t vmm_surface_vaddr(int id)      { return surf_base + (uint64_t)id * SURF_SLOT_BYTES; }
uint64_t vmm_current_pml4(void) {
    uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3)); return cr3 & PHYS_MASK;
}
void vmm_flush_self(void) {
    uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* Map `bytes` of device MMIO at physical `phys` into the shared higher half and
 * return a kernel pointer to it (or 0 if the window is exhausted). Carves
 * consecutive 2 MiB cache-disabled huge pages out of the MMIO window; the mapping
 * is shared across every address space and persists for the life of the system.
 * The returned pointer preserves `phys`'s offset within its 2 MiB page. */
void *vmm_map_mmio(uint64_t phys, uint64_t bytes) {
    if (!mmio_pd || bytes == 0) return 0;
    uint64_t aligned = phys & ~0x1fffffULL;
    uint64_t span    = (phys + bytes) - aligned;
    int pages = (int)((span + 0x1fffffULL) / 0x200000ULL);
    if (pages <= 0 || mmio_next + pages > 512) return 0;
    int start = mmio_next;
    for (int i = 0; i < pages; i++)
        mmio_pd[start + i] = (aligned + (uint64_t)i * 0x200000ULL) | P | W | HUGE | 0x10 /*PCD*/;
    mmio_next += pages;
    vmm_flush_self();                                  /* publish the new huge pages */
    return (void *)(MMIO_VBASE + (uint64_t)start * 0x200000ULL + (phys & 0x1fffffULL));
}

uint64_t vmm_alloc_surface(int nframes) { return frame_alloc_contig(nframes); }
void vmm_free_surface(uint64_t base, int nframes) {
    for (int i = 0; i < nframes; i++) frame_free(base + (uint64_t)i * 0x1000);
}

/* Map a single 4 KiB user page into address space `pml4_phys`, creating the PDPT
 * slot's PD and the PT on demand. The surface slot is separate from the huge-page
 * identity map, so its PD holds ordinary 4 KiB PTs. */
static void map_page_user(uint64_t pml4_phys, uint64_t vaddr, uint64_t phys) {
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t *pdpt = (uint64_t *)(pml4[0] & PHYS_MASK);
    int i3 = (vaddr >> 30) & 0x1ff, i2 = (vaddr >> 21) & 0x1ff, i1 = (vaddr >> 12) & 0x1ff;
    if (!(pdpt[i3] & P)) pdpt[i3] = (uint64_t)frame_alloc() | P | W | U;
    uint64_t *pd = (uint64_t *)(pdpt[i3] & PHYS_MASK);
    if (!(pd[i2] & P)) pd[i2] = (uint64_t)frame_alloc() | P | W | U;
    uint64_t *pt = (uint64_t *)(pd[i2] & PHYS_MASK);
    pt[i1] = phys | P | W | U;
}

/* Map a surface's frames into `pml4_phys` at vmm_surface_vaddr(id). Caller must
 * flush the TLB (vmm_flush_self) if pml4_phys is the live CR3. Returns the vaddr. */
uint64_t vmm_map_surface(uint64_t pml4_phys, int id, uint64_t phys_base, int nframes) {
    uint64_t v = vmm_surface_vaddr(id);
    for (int i = 0; i < nframes; i++)
        map_page_user(pml4_phys, v + (uint64_t)i * 0x1000, phys_base + (uint64_t)i * 0x1000);
    return v;
}

/* Clear a surface's PTEs in `pml4_phys` (used when a window is removed). Leaves
 * the PT/PD frames in place; they are reclaimed when the address space dies. */
void vmm_unmap_surface(uint64_t pml4_phys, int id, int nframes) {
    uint64_t v = vmm_surface_vaddr(id);
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t *pdpt = (uint64_t *)(pml4[0] & PHYS_MASK);
    for (int i = 0; i < nframes; i++) {
        uint64_t va = v + (uint64_t)i * 0x1000;
        int i3 = (va >> 30) & 0x1ff, i2 = (va >> 21) & 0x1ff, i1 = (va >> 12) & 0x1ff;
        if (!(pdpt[i3] & P)) continue;
        uint64_t *pd = (uint64_t *)(pdpt[i3] & PHYS_MASK);
        if (!(pd[i2] & P)) continue;
        uint64_t *pt = (uint64_t *)(pd[i2] & PHYS_MASK);
        pt[i1] = 0;
    }
}

/* Map `nframes` of fresh, private, zeroed RAM into the LIVE address space at the
 * caller's current anon break (*brk), growing the region upward: map_page_user
 * creates new PDPT/PD/PT tables on demand, so the region spans as many GiB as
 * asked for -- the size is bounded by physical RAM, never a fixed vaddr window.
 * Frames need not be contiguous (each page mapped independently), so this reuses
 * the free list well. Returns the base vaddr and advances *brk. The caller's own
 * CR3 is live, so we flush it. (frame_alloc panics on true RAM exhaustion.) */
uint64_t vmm_mmap(uint64_t *brk, int nframes) {
    if (nframes <= 0) return 0;
    if (*brk < anon_base) *brk = anon_base;            /* first use: start of the region */
    uint64_t cr3 = vmm_current_pml4();
    uint64_t v = *brk;
    for (int i = 0; i < nframes; i++)
        map_page_user(cr3, v + (uint64_t)i * 0x1000, (uint64_t)frame_alloc());
    *brk = v + (uint64_t)nframes * 0x1000;
    vmm_flush_self();
    return v;
}

/* True if every byte in [vaddr, vaddr+len) is mapped present AND user-accessible
 * (US=1) in the CURRENT address space. The syscall layer uses this to VALIDATE a
 * user pointer before the kernel dereferences it -- a bad pointer then fails the
 * syscall (returns -1) instead of taking a kernel-mode page fault that would halt
 * the whole machine (e.g. exec'ing with a path that fork didn't copy into the
 * child). Walks the live page tables, honouring 2 MiB huge pages; kernel-half and
 * non-canonical addresses are rejected because their PML4 entry lacks US. */
int vmm_user_ok(uint64_t vaddr, uint64_t len) {
    if (len == 0) return 1;
    if (vaddr + len < vaddr) return 0;                  /* wrap-around */
    uint64_t *pml4 = (uint64_t *)vmm_current_pml4();
    uint64_t end = vaddr + len;
    for (uint64_t a = vaddr & ~0xfffULL; a < end; a += 0x1000) {
        if (a >> 47) return 0;                          /* not low-canonical -> not a user pointer */
        uint64_t e = pml4[(a >> 39) & 0x1ff];
        if (!(e & P) || !(e & U)) return 0;
        uint64_t *pdpt = (uint64_t *)(e & PHYS_MASK);
        e = pdpt[(a >> 30) & 0x1ff];
        if (!(e & P) || !(e & U)) return 0;
        if (e & HUGE) continue;                         /* 1 GiB page */
        uint64_t *pd = (uint64_t *)(e & PHYS_MASK);
        e = pd[(a >> 21) & 0x1ff];
        if (!(e & P) || !(e & U)) return 0;
        if (e & HUGE) continue;                         /* 2 MiB page (the identity map) */
        uint64_t *pt = (uint64_t *)(e & PHYS_MASK);
        e = pt[(a >> 12) & 0x1ff];
        if (!(e & P) || !(e & U)) return 0;
    }
    return 1;
}

/* True if a user pointer holds a NUL-terminated string no longer than `max`,
 * with every page it spans mapped+user. Validates each page before touching it. */
int vmm_user_str_ok(uint64_t vaddr, int max) {
    for (int i = 0; i < max; i++) {
        uint64_t a = vaddr + (uint64_t)i;
        if (i == 0 || (a & 0xfff) == 0) { if (!vmm_user_ok(a, 1)) return 0; }
        if (*(volatile char *)a == 0) return 1;
    }
    return 0;                                           /* unterminated within max */
}

/* Copy an address space (for fork): a fresh PML4 with the shared kernel half and
 * the same identity map, plus private copies of every present user page (code,
 * stack, data) so the child sees identical memory at identical virtual addresses.
 * Runs on the parent's CR3, where both the parent's frames and the freshly
 * allocated child frames are identity-mapped, so the copies are plain memcpy. */
uint64_t vmm_fork(uint64_t parent_pml4) {
    uint64_t *ppml4 = (uint64_t *)parent_pml4;
    uint64_t  ppdpt = ppml4[0] & PHYS_MASK;
    uint64_t  ppd   = ((uint64_t *)ppdpt)[0] & PHYS_MASK;
    uint64_t  pupt  = ((uint64_t *)ppd)[2] & PHYS_MASK;
    uint64_t *pupte = (uint64_t *)pupt;

    uint64_t *pml4     = frame_alloc();
    uint64_t *pdpt_low = frame_alloc();
    uint64_t *upt      = frame_alloc();

    pml4[0]   = (uint64_t)pdpt_low | P | W | U;
    pml4[511] = shared_pdpt_high   | P | W;
    uint64_t *pd_low = build_low_map(pdpt_low);
    pd_low[2] = (uint64_t)upt | P | W | U;

    for (int i = 0; i < 512; i++) {                      /* copy each present user page */
        if (!(pupte[i] & P)) continue;
        uint8_t *src = (uint8_t *)(pupte[i] & PHYS_MASK);
        uint8_t *dst = (uint8_t *)frame_alloc();
        for (int b = 0; b < 0x1000; b++) dst[b] = src[b];
        upt[i] = (uint64_t)dst | (pupte[i] & 0xfff);     /* keep the parent's flags */
    }
    return (uint64_t)pml4;
}

/* Reclaim everything vmm_create_user allocated for a task: its private code,
 * stack and data frames, the identity-map page directories, an on-demand
 * framebuffer PD if any, and the PML4/PDPT. The shared higher half is left
 * untouched. MUST be called when this PML4 is no longer the active CR3. */
void vmm_destroy_user(uint64_t pml4_phys) {
    uint64_t *pml4     = (uint64_t *)pml4_phys;
    uint64_t  pdpt_low = pml4[0] & PHYS_MASK;
    uint64_t *pdpte    = (uint64_t *)pdpt_low;
    uint64_t  pd0      = pdpte[0] & PHYS_MASK;
    uint64_t  upt      = ((uint64_t *)pd0)[2] & PHYS_MASK;
    uint64_t *upte     = (uint64_t *)upt;

    for (int i = 0; i < 512; i++)                      /* code, stack and data frames */
        if (upte[i] & P) frame_free(upte[i] & PHYS_MASK);
    frame_free(upt);
    for (int pd = 0; pd < ident_pds; pd++)             /* the identity-map page directories */
        if (pdpte[pd] & P) frame_free(pdpte[pd] & PHYS_MASK);
    uint64_t fbpd = pdpte[fb_pdpt_slot];               /* the on-demand framebuffer PD (just past RAM) */
    if (fbpd & P) frame_free(fbpd & PHYS_MASK);        /* free the PD, not the MMIO it maps */
    uint64_t spd = pdpte[surf_pdpt_slot];              /* window-surface page tables for this AS */
    if (spd & P) {                                     /* free the PTs + the PD, NOT the shared */
        uint64_t *pd = (uint64_t *)(spd & PHYS_MASK);  /* surface frames they point at (the      */
        for (int i = 0; i < 512; i++)                  /* window subsystem owns/frees those)     */
            if (pd[i] & P) frame_free(pd[i] & PHYS_MASK);
        frame_free(spd & PHYS_MASK);
    }
    for (int s = anon_pdpt_slot; s < 512; s++) {       /* anonymous mmap region: PRIVATE frames, */
        uint64_t apd = pdpte[s];                       /* so free the mapped RAM too (it grows   */
        if (!(apd & P)) continue;                      /* across slots, so sweep them all)       */
        uint64_t *pd = (uint64_t *)(apd & PHYS_MASK);
        for (int i = 0; i < 512; i++) {
            if (!(pd[i] & P)) continue;
            uint64_t *pt = (uint64_t *)(pd[i] & PHYS_MASK);
            for (int j = 0; j < 512; j++)
                if (pt[j] & P) frame_free(pt[j] & PHYS_MASK);   /* the mapped pages themselves */
            frame_free(pd[i] & PHYS_MASK);
        }
        frame_free(apd & PHYS_MASK);
    }
    frame_free(pdpt_low);
    frame_free(pml4_phys);
}
