/* ===========================================================================
 * tOS UEFI loader. Places the (embedded) kernel and user images where the
 * kernel expects them, queries GOP for the framebuffer, builds a page map
 * (4 GiB identity for the transition + the higher half + the framebuffer),
 * exits boot services, and jumps into the same kmain the BIOS path uses --
 * passing a boot_info in RDI so the kernel renders to the framebuffer.
 * =========================================================================== */
#include <stdint.h>
#include "bootinfo.h"                /* struct boot_info, FB_VBASE (from kernel/) */
#include "kernel_blob.h"             /* kernel_bin[], kernel_bin_len (programs embedded) */

typedef uint64_t EFI_STATUS;
typedef void    *EFI_HANDLE;

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;

typedef struct {
    char  Hdr[24];
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (*AllocatePages)(int, int, uint64_t, uint64_t *);      /* 40 */
    void *FreePages;
    EFI_STATUS (*GetMemoryMap)(uint64_t *, void *, uint64_t *, uint64_t *, uint32_t *); /* 56 */
    void *AllocatePool, *FreePool;
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    void *HandleProtocol, *Reserved, *RegisterProtocolNotify;
    void *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE, uint64_t);             /* 232 */
    void *GetNextMonotonicCount, *Stall, *SetWatchdogTimer;
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (*LocateProtocol)(EFI_GUID *, void *, void **);        /* 320 */
} EFI_BOOT_SERVICES;

typedef struct {
    char  Hdr[24];
    void *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle; void *ConIn;
    EFI_HANDLE ConsoleOutHandle; void *ConOut;
    EFI_HANDLE StandardErrorHandle; void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;                                 /* 96 */
    uint64_t NumberOfTableEntries;                                  /* 104 */
    void *ConfigurationTable;                                       /* 112 -> EFI_CONFIGURATION_TABLE[] */
} EFI_SYSTEM_TABLE;

/* Each firmware configuration table is a {GUID, pointer} pair; the ACPI ones
 * point straight at the RSDP. */
typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* Graphics Output Protocol (only the fields we read). */
typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution, PixelFormat;
    uint32_t PixelInformation[4];
    uint32_t PixelsPerScanLine;
} GOP_MODE_INFO;
typedef struct {
    uint32_t MaxMode, Mode;
    GOP_MODE_INFO *Info;
    uint64_t SizeOfInfo, FrameBufferBase, FrameBufferSize;
} GOP_MODE;
typedef struct {
    void *QueryMode, *SetMode, *Blt;
    GOP_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

static EFI_GUID GOP_GUID =
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a } };

/* ACPI RSDP configuration-table GUIDs (2.0+ preferred, 1.0 fallback). */
static EFI_GUID ACPI20_GUID =
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81 } };
static EFI_GUID ACPI10_GUID =
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a,0x16,0x00,0x23,0xa7,0x68,0x72,0xe2 } };

static int guid_eq(const EFI_GUID *x, const EFI_GUID *y){
    if (x->a != y->a || x->b != y->b || x->c != y->c) return 0;
    for (int i = 0; i < 8; i++) if (x->d[i] != y->d[i]) return 0;
    return 1;
}

/* Walk the firmware configuration tables for the ACPI RSDP and hand its physical
 * address to the kernel via boot_info, so MADT/FADT work under UEFI (where the
 * legacy 0..1MiB RSDP scan finds nothing). Prefer the ACPI 2.0 table; fall back
 * to 1.0. Done before ExitBootServices, while the system table is still valid. */
static uint64_t find_acpi_rsdp(EFI_SYSTEM_TABLE *st){
    EFI_CONFIGURATION_TABLE *ct = (EFI_CONFIGURATION_TABLE *)st->ConfigurationTable;
    if (!ct) return 0;
    uint64_t rsdp = 0;
    for (uint64_t i = 0; i < st->NumberOfTableEntries; i++){
        if (guid_eq(&ct[i].VendorGuid, &ACPI20_GUID)) return (uint64_t)ct[i].VendorTable;
        if (guid_eq(&ct[i].VendorGuid, &ACPI10_GUID)) rsdp = (uint64_t)ct[i].VendorTable;
    }
    return rsdp;
}

#define AllocateAnyPages 0
#define AllocateAddress  2
#define EfiLoaderData    2

#define POOL_PHYS    0x100000
#define KERNEL_PHYS  0x200000
#define KERNEL_VMA   0xFFFFFFFF80000000ULL
#define GIB          0x40000000ULL
#define MAX_RAM      (512ULL * GIB)   /* matches the kernel's e820 cap (vmm.c) */

#define COM1 0x3f8
static inline void outb(uint16_t p, uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t inb(uint16_t p){uint8_t r;__asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p));return r;}
static void serial_init(void){
    outb(COM1+1,0); outb(COM1+3,0x80); outb(COM1+0,3);
    outb(COM1+1,0); outb(COM1+3,3); outb(COM1+2,0xc7); outb(COM1+4,0x0b);
}
static void sputs(const char *s){ for(;*s;++s){ while((inb(COM1+5)&0x20)==0){} outb(COM1,(uint8_t)*s);} }
static void putu(uint64_t v){ char b[21]; int i=20; b[20]=0; if(!v){sputs("0");return;} while(v){b[--i]=(char)('0'+v%10); v/=10;} sputs(b+i); }

static void copy(uint64_t dst, const unsigned char *src, uint64_t n){
    volatile uint8_t *d=(volatile uint8_t*)dst;
    for(uint64_t i=0;i<n;i++) d[i]=src[i];
}
static void put(uint64_t t, int i, uint64_t v){ ((volatile uint64_t*)t)[i]=v; }
static void zero(uint64_t t){ for(int i=0;i<512;i++) ((volatile uint64_t*)t)[i]=0; }

static struct boot_info boot_info;

/* EFI memory-map descriptor (only the fields we read). */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* Highest physical RAM address the firmware reports, rounded up to a whole GiB.
 * OVMF loads this EFI app -- its code, its stack and the boot_info struct -- high
 * in RAM, ABOVE the 4 GiB PCI hole on big-memory machines (e.g. ~5 GiB at -m 8G).
 * The transition page tables must therefore identity-map ALL of RAM, not just the
 * low 4 GiB, or the very first instruction fetch after the CR3 switch (still at the
 * loader's high RIP) #PFs -- and the kernel's `*bi` read would too. Scales with the
 * machine; no fixed cap. Mirrors the kernel's own e820 map (kernel/mm/vmm.c). */
static uint64_t ram_top(EFI_BOOT_SERVICES *bs){
    static uint8_t mm[65536];
    uint64_t msize = sizeof(mm), mkey, dsize = 0; uint32_t dver;
    uint64_t top = 4 * GIB;                          /* always map at least the low 4 GiB */
    if (bs->GetMemoryMap(&msize, mm, &mkey, &dsize, &dver) == 0 && dsize){
        for (uint64_t off = 0; off + dsize <= msize; off += dsize){
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mm + off);
            uint32_t t = d->Type;                    /* usable-RAM types the app can live in */
            if (!(t==1||t==2||t==3||t==4||t==5||t==6||t==7||t==9||t==10||t==14)) continue;
            if (d->PhysicalStart >= MAX_RAM) continue;
            uint64_t end = d->PhysicalStart + d->NumberOfPages * 0x1000ULL;
            if (end > top) top = end;
        }
    }
    if (top > MAX_RAM) top = MAX_RAM;
    return (top + GIB - 1) & ~(GIB - 1);             /* round up to a whole GiB */
}

/* Bump allocator over a contiguous arena of (zeroed) page-table frames. */
static uint64_t arena_next;
static uint64_t pt_alloc(void){ uint64_t p = arena_next; arena_next += 0x1000; zero(p); return p; }

static uint64_t g_cr3;          /* PML4 physical address handed to the kernel */

/* Identity-map [0, top) with 2 MiB huge pages (one page directory per GiB), plus
 * the higher-half kernel at KERNEL_VMA and the framebuffer window at FB_VBASE. The
 * page tables come from a single contiguous AllocatePages arena (the MMU walks them
 * by physical address, so the arena can live anywhere in RAM). */
static void build_tables(EFI_BOOT_SERVICES *bs, uint64_t top){
    int npd = (int)(top / GIB);                       /* top is GiB-aligned */
    if (npd < 4)   npd = 4;
    if (npd > 512) npd = 512;                         /* one PDPT page == 512 GiB */

    uint64_t base = 0;
    bs->AllocatePages(AllocateAnyPages, EfiLoaderData, 5 + (uint64_t)npd, &base);
    arena_next = base;

    uint64_t pml4 = pt_alloc(), pdpt_lo = pt_alloc(), pdpt_hi = pt_alloc();
    put(pml4, 0,   pdpt_lo | 0x3);
    put(pml4, 511, pdpt_hi | 0x3);

    for (int g = 0; g < npd; g++){                    /* 0..top identity (covers the app + boot_info) */
        uint64_t pd = pt_alloc();
        put(pdpt_lo, g, pd | 0x3);
        for (int j = 0; j < 512; j++)
            put(pd, j, ((uint64_t)g * GIB + (uint64_t)j * 0x200000ULL) | 0x83);
    }

    uint64_t pd_high = pt_alloc();                    /* 0xFF..80000000 -> kernel */
    put(pdpt_hi, 510, pd_high | 0x3);
    put(pd_high, 0, KERNEL_PHYS | 0x83);

    if (boot_info.console == BOOT_CONSOLE_FB){         /* FB_VBASE -> framebuffer */
        uint64_t fb = boot_info.fb_phys & ~0x1fffffULL;
        uint64_t pd_fb = pt_alloc();
        put(pdpt_hi, 511, pd_fb | 0x3);
        for (int i = 0; i < 8; i++) put(pd_fb, i, (fb + (uint64_t)i * 0x200000ULL) | 0x83);
    }
    g_cr3 = pml4;
}

EFI_STATUS efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE *st){
    EFI_BOOT_SERVICES *bs = st->BootServices;
    uint64_t addr;

    serial_init();
    sputs("[uefi] tOS loader\r\n");

    addr = POOL_PHYS;   bs->AllocatePages(AllocateAddress, EfiLoaderData, 256, &addr);
    addr = KERNEL_PHYS; bs->AllocatePages(AllocateAddress, EfiLoaderData, 512, &addr);

    copy(KERNEL_PHYS, kernel_bin, kernel_bin_len);

    /* query GOP for the framebuffer */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    boot_info.console = BOOT_CONSOLE_VGA;
    if (bs->LocateProtocol(&GOP_GUID, 0, (void **)&gop) == 0 && gop){
        boot_info.console = BOOT_CONSOLE_FB;
        boot_info.width   = gop->Mode->Info->HorizontalResolution;
        boot_info.height  = gop->Mode->Info->VerticalResolution;
        boot_info.pitch   = gop->Mode->Info->PixelsPerScanLine;
        boot_info.fb_phys = gop->Mode->FrameBufferBase;
        sputs("[uefi] GOP framebuffer found\r\n");
    }

    boot_info.acpi_rsdp = find_acpi_rsdp(st);
    if (boot_info.acpi_rsdp){ sputs("[uefi] ACPI RSDP @ "); putu(boot_info.acpi_rsdp); sputs("\r\n"); }
    else                     sputs("[uefi] no ACPI RSDP in config tables\r\n");

    uint64_t top = ram_top(bs);
    sputs("[uefi] identity-mapping "); putu(top / GIB); sputs(" GiB of RAM\r\n");
    build_tables(bs, top);
    sputs("[uefi] exiting boot services\r\n");

    static uint8_t mmap[65536];
    uint64_t msize, mkey, dsize; uint32_t dver;
    for(int t=0; t<16; t++){
        msize = sizeof(mmap);
        bs->GetMemoryMap(&msize, mmap, &mkey, &dsize, &dver);
        if (bs->ExitBootServices(img, mkey) == 0) break;
    }

    uint64_t cr3 = g_cr3, bi = (uint64_t)&boot_info, entry = KERNEL_VMA;
    __asm__ volatile(
        "cli\n\t"
        "mov %0, %%cr3\n\t"
        "mov %1, %%rdi\n\t"
        "jmp *%2\n\t"
        : : "r"(cr3), "r"(bi), "r"(entry) : "rdi", "memory");

    for(;;) __asm__ volatile("hlt");
    return 0;
}
