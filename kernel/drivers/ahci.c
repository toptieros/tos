/* AHCI 1.x SATA driver over the memory-mapped HBA register block (ABAR = BAR5).
 * Polled, one command slot in flight, DMA through identity-mapped command
 * structures and a bounce buffer. Clean-room from the public AHCI 1.3.1 spec and
 * the OSDev wiki -- no Linux code. Tested against QEMU's `-device ich9-ahci`.
 *
 * Memory model (same as virtio_blk.c): every structure here comes from
 * vmm_alloc_surface -> frame_alloc_contig, which returns identity-mapped low RAM
 * (phys == virt), so a kernel pointer doubles as the device-visible physical
 * address in CLB/FB/CTBA/PRDT. The caller's buffer may live in the higher half
 * (kernel .bss/stack, virt != phys), so the device DMAs only into the bounce
 * buffer and we copy to/from the caller. The ABAR itself is in the PCI hole and
 * isn't covered by the RAM identity map, so vmm_map_mmio() maps it explicitly. */
#include "ahci.h"
#include "pci.h"
#include "cpu.h"
#include "spinlock.h"
#include "console.h"
#include "vmm.h"
#include "blockdev.h"

/* ---- Generic Host Control registers (offsets from ABAR) ---- */
#define HBA_CAP   0x00      /* host capabilities                 */
#define HBA_GHC   0x04      /* global host control               */
#define HBA_IS    0x08      /* interrupt status (per-port bits)  */
#define HBA_PI    0x0C      /* ports implemented (bitmask)       */
#define HBA_VS    0x10      /* version                           */

#define GHC_AE    (1u << 31)   /* AHCI enable */
#define GHC_HR    (1u << 0)    /* HBA reset   */

/* ---- Per-port registers (offset = 0x100 + port*0x80 + reg) ---- */
#define PORT_BASE(p) (0x100u + (uint32_t)(p) * 0x80u)
#define P_CLB     0x00      /* command list base (lower 32)      */
#define P_CLBU    0x04      /* command list base (upper 32)      */
#define P_FB      0x08      /* FIS base (lower 32)               */
#define P_FBU     0x0C      /* FIS base (upper 32)               */
#define P_IS      0x10      /* interrupt status                  */
#define P_IE      0x14      /* interrupt enable                  */
#define P_CMD     0x18      /* command and status                */
#define P_TFD     0x20      /* task file data (status/error)     */
#define P_SIG     0x24      /* signature                         */
#define P_SSTS    0x28      /* SATA status (DET/IPM/SPD)         */
#define P_SCTL    0x2C      /* SATA control                      */
#define P_SERR    0x30      /* SATA error                        */
#define P_CI      0x38      /* command issue (one bit per slot)  */

#define CMD_ST    (1u << 0)    /* start (process the command list) */
#define CMD_FRE   (1u << 4)    /* FIS receive enable               */
#define CMD_FR    (1u << 14)   /* FIS receive running              */
#define CMD_CR    (1u << 15)   /* command list running             */

#define TFD_BSY   (1u << 7)    /* interface busy */
#define TFD_DRQ   (1u << 3)    /* data transfer requested */
#define TFD_ERR   (1u << 0)    /* error */

#define IS_TFES   (1u << 30)   /* task file error status */

#define SIG_SATA  0x00000101u  /* PxSIG for a plain SATA disk */

#define FIS_TYPE_H2D 0x27      /* host-to-device register FIS */

#define ATA_CMD_READ_DMA_EX  0x25
#define ATA_CMD_WRITE_DMA_EX 0x35
#define ATA_CMD_IDENTIFY     0xEC

#define PAGE 4096u
#define BOUNCE_SECTORS 64      /* 32 KiB max per device command (one PRDT entry) */

/* A command header (one of 32 in the command list); 32 bytes. */
struct cmd_header {
    uint16_t flags;        /* CFL[4:0] | A[5] | W[6] | P[7] | ... */
    uint16_t prdtl;        /* number of PRDT entries */
    volatile uint32_t prdbc; /* PRD byte count transferred (HBA-written) */
    uint32_t ctba, ctbau;  /* command table base (128-byte aligned) */
    uint32_t rsv[4];
} __attribute__((packed));

/* A PRDT entry; 16 bytes. */
struct prdt_entry {
    uint32_t dba, dbau;    /* data base address */
    uint32_t rsv;
    uint32_t dbc;          /* bits 21:0 = byte count - 1; bit 31 = interrupt-on-completion */
} __attribute__((packed));

/* The command table for slot 0: 64-byte CFIS, 16-byte ATAPI cmd, 48-byte pad,
 * then the PRDT. 128-byte aligned. We use a single PRDT entry. */
struct cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    struct prdt_entry prdt[1];
} __attribute__((packed));

static spinlock_t ahci_lock = SPINLOCK_INIT;
static int          present = 0;
static volatile uint8_t *abar;         /* mapped HBA register block */
static int          port = -1;         /* the bound port number */
static uint64_t     capacity = 0;      /* sectors */

static struct cmd_header *cmd_list;    /* 32 headers, 1 KiB, 1 KiB-aligned */
static uint8_t          *recv_fis;     /* 256-byte received-FIS area, 256-aligned */
static struct cmd_table *cmd_tbl;      /* slot-0 command table, 128-aligned */
static uint8_t          *bounce;       /* DMA bounce buffer */

static inline uint32_t mr(uint32_t off)            { return *(volatile uint32_t *)(abar + off); }
static inline void     mw(uint32_t off, uint32_t v){ *(volatile uint32_t *)(abar + off) = v; }
static inline uint32_t pr(uint32_t reg)            { return mr(PORT_BASE(port) + reg); }
static inline void     pw(uint32_t reg, uint32_t v){ mw(PORT_BASE(port) + reg, v); }

static void bzero(void *d, uint32_t n) { uint8_t *p = (uint8_t *)d; for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static void bcopy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}

/* Find an AHCI HBA (PCI class 0x01 mass-storage, subclass 0x06 SATA) on bus 0. */
static int find_device(uint8_t *slot_out, uint8_t *func_out) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_read32(0, slot, func, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) { if (func == 0) break; else continue; }
            uint32_t cls = pci_read32(0, slot, func, 0x08);
            if (((cls >> 24) & 0xFF) == 0x01 && ((cls >> 16) & 0xFF) == 0x06) {
                *slot_out = slot; *func_out = func; return 1;
            }
            if (func == 0) {
                uint32_t hdr = pci_read32(0, slot, 0, 0x0C);
                if (!((hdr >> 16) & 0x80)) break;       /* not multi-function */
            }
        }
    }
    return 0;
}

/* Stop the port's command-list + FIS engines and wait for them to quiesce. */
static void port_stop(void) {
    pw(P_CMD, pr(P_CMD) & ~CMD_ST);
    pw(P_CMD, pr(P_CMD) & ~CMD_FRE);
    for (uint64_t i = 0; i < 2000000ULL; i++) {
        if (!(pr(P_CMD) & (CMD_CR | CMD_FR))) break;
        __asm__ volatile("pause");
    }
}

/* Start the FIS-receive then the command-list engine. */
static void port_start(void) {
    while (pr(P_CMD) & CMD_CR) __asm__ volatile("pause");
    pw(P_CMD, pr(P_CMD) | CMD_FRE);
    pw(P_CMD, pr(P_CMD) | CMD_ST);
}

/* Issue one DMA command (slot 0) and spin until it retires. write=0 reads.
 * Data moves through the bounce buffer; returns 0 on success, -1 on error. */
static int issue(uint64_t lba, uint32_t count, int write, uint8_t ata_cmd) {
    /* wait for the port to be idle (not BSY/DRQ) */
    for (uint64_t i = 0; ; i++) {
        if (!(pr(P_TFD) & (TFD_BSY | TFD_DRQ))) break;
        if (i > 100000000ULL) return -1;
        __asm__ volatile("pause");
    }
    pw(P_IS, 0xFFFFFFFFu);                          /* clear pending interrupt status */

    uint32_t bytes = 512u * count;
    bzero(cmd_tbl, sizeof(struct cmd_table));
    cmd_tbl->prdt[0].dba  = (uint32_t)(uintptr_t)bounce;
    cmd_tbl->prdt[0].dbau = 0;
    cmd_tbl->prdt[0].dbc  = (bytes - 1) & 0x3FFFFF; /* byte count - 1, no interrupt */

    uint8_t *f = cmd_tbl->cfis;
    f[0] = FIS_TYPE_H2D;
    f[1] = 0x80;                                    /* C=1: this FIS carries a command */
    f[2] = ata_cmd;
    f[3] = 0;                                       /* features (low) */
    f[4] = (uint8_t)(lba);
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                                    /* device: LBA mode */
    f[8]  = (uint8_t)(lba >> 24);
    f[9]  = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[11] = 0;                                      /* features (high) */
    f[12] = (uint8_t)(count);
    f[13] = (uint8_t)(count >> 8);

    cmd_list[0].flags = (uint16_t)(5 | (write ? (1u << 6) : 0)); /* CFL = 5 dwords (H2D FIS) */
    cmd_list[0].prdtl = 1;
    cmd_list[0].prdbc = 0;
    cmd_list[0].ctba  = (uint32_t)(uintptr_t)cmd_tbl;
    cmd_list[0].ctbau = 0;

    pw(P_CI, 1u);                                   /* issue command on slot 0 */

    for (uint64_t i = 0; ; i++) {
        if (!(pr(P_CI) & 1u)) break;                /* slot cleared: command done */
        if (pr(P_IS) & IS_TFES) return -1;          /* task file error */
        if (i > 200000000ULL) return -1;
        __asm__ volatile("pause");
    }
    if (pr(P_TFD) & (TFD_ERR | (1u << 5)/*DF*/)) return -1;
    return 0;
}

static int do_io(uint64_t lba, uint32_t count, void *buf, int write) {
    if (!present || count == 0) return -1;
    uint8_t *p = (uint8_t *)buf;
    uint64_t fl = spin_lock_irqsave(&ahci_lock);
    int rc = 0;
    while (count) {
        uint32_t c = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        uint32_t bytes = c * 512u;
        if (write) bcopy(bounce, p, bytes);
        if (issue(lba, c, write, write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX) < 0) { rc = -1; break; }
        if (!write) bcopy(p, bounce, bytes);
        lba += c; p += bytes; count -= c;
    }
    spin_unlock_irqrestore(&ahci_lock, fl);
    return rc;
}

int ahci_read(uint64_t lba, uint32_t count, void *buf)        { return do_io(lba, count, buf, 0); }
int ahci_write(uint64_t lba, uint32_t count, const void *buf) { return do_io(lba, count, (void *)buf, 1); }
int ahci_present(void)       { return present; }
uint64_t ahci_capacity(void) { return capacity; }

/* IDENTIFY the bound port's drive into the bounce buffer; return total LBA48
 * sectors (words 100-103), falling back to the LBA28 count (words 60-61). */
static uint64_t identify_capacity(void) {
    if (issue(0, 1, 0, ATA_CMD_IDENTIFY) < 0) return 0;
    uint16_t *id = (uint16_t *)bounce;
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16)
                   | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    if (lba48) return lba48;
    return (uint64_t)id[60] | ((uint64_t)id[61] << 16);
}

/* Non-destructive last-sector round-trip through the block layer (proves the
 * bdev registration + dispatch, not just the raw driver). No data is corrupted. */
static void selftest(void) {
    int dev = bdev_find("ahci0");
    uint64_t page = vmm_alloc_surface(1);
    if (dev < 0 || !page) { console_puts("[ahci] selftest SKIP\r\n"); return; }
    uint8_t *save = (uint8_t *)page, *pat = save + 512, *back = save + 1024;
    uint64_t s = capacity ? capacity - 1 : 0;
    for (int i = 0; i < 512; i++) pat[i] = (uint8_t)(i * 11 + 0x2c);
    int ok = bdev_read(dev, s, 1, save) == 0
          && bdev_write(dev, s, 1, pat) == 0
          && bdev_read(dev, s, 1, back) == 0;
    if (ok) { for (int i = 0; i < 512; i++) if (back[i] != pat[i]) { ok = 0; break; } }
    bdev_write(dev, s, 1, save);                    /* restore */
    console_puts(ok ? "[ahci] selftest OK\r\n" : "[ahci] selftest FAIL\r\n");
}

void ahci_init(void) {
    uint8_t slot, func;
    if (!find_device(&slot, &func)) { console_puts("[ahci] none\r\n"); return; }

    uint32_t bar5 = pci_read32(0, slot, func, 0x24);
    if (bar5 & 1) { console_puts("[ahci] ABAR is I/O space -- unsupported\r\n"); return; }
    uint64_t abar_phys = bar5 & ~0xFu;

    uint32_t cmd = pci_read32(0, slot, func, 0x04);   /* enable memory space + bus-master DMA */
    pci_write32(0, slot, func, 0x04, cmd | 0x2 | 0x4);

    abar = (volatile uint8_t *)vmm_map_mmio(abar_phys, 0x2000);
    if (!abar) { console_puts("[ahci] ABAR map failed\r\n"); return; }

    mw(HBA_GHC, mr(HBA_GHC) | GHC_AE);                /* ensure AHCI mode */

    uint32_t pi = mr(HBA_PI);
    int found = -1;
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        uint32_t ssts = mr(PORT_BASE(p) + P_SSTS);
        uint32_t sig  = mr(PORT_BASE(p) + P_SIG);
        if ((ssts & 0xF) == 3 && sig == SIG_SATA) { found = p; break; }  /* DET=3 + SATA disk */
    }
    if (found < 0) { console_puts("[ahci] HBA present but no SATA disk\r\n"); return; }
    port = found;

    /* DMA structures: one page holds the command list (1 KiB) + received-FIS area
     * (256 B) + the slot-0 command table (128-aligned); plus a bounce buffer.
     * All identity-mapped, so the pointers are the device-visible phys addresses. */
    uint64_t pg = vmm_alloc_surface(1);
    uint64_t bb = vmm_alloc_surface((BOUNCE_SECTORS * 512) / PAGE);
    if (!pg || !bb) { console_puts("[ahci] DMA alloc failed\r\n"); return; }
    cmd_list = (struct cmd_header *)(uintptr_t)pg;          /* 4 KiB-aligned => 1 KiB-aligned */
    recv_fis = (uint8_t *)(uintptr_t)(pg + 1024);           /* 256-aligned */
    cmd_tbl  = (struct cmd_table *)(uintptr_t)(pg + 2048);  /* 128-aligned */
    bounce   = (uint8_t *)(uintptr_t)bb;
    bzero((void *)cmd_list, 1024);
    bzero(recv_fis, 256);

    port_stop();
    pw(P_CLB, (uint32_t)(uintptr_t)cmd_list); pw(P_CLBU, 0);
    pw(P_FB,  (uint32_t)(uintptr_t)recv_fis); pw(P_FBU, 0);
    pw(P_SERR, 0xFFFFFFFFu);                          /* clear any latched SATA errors */
    pw(P_IS,   0xFFFFFFFFu);
    port_start();

    present = 1;
    capacity = identify_capacity();
    bdev_register("ahci0", capacity, ahci_read, ahci_write);

    console_puts("[ahci] up: port ");
    console_putdec((uint64_t)port);
    console_puts(", ");
    console_putdec(capacity);
    console_puts(" sectors\r\n");

    selftest();
}
