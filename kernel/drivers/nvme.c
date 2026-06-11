/* NVMe 1.x block driver over the memory-mapped controller registers (BAR0/1).
 * Polled, one command in flight per queue, DMA through identity-mapped queues +
 * PRP lists + a bounce buffer. Clean-room from the public NVMe base spec and the
 * OSDev wiki -- no Linux code. Tested against QEMU's `-device nvme`.
 *
 * Memory model (same as ahci.c / virtio_blk.c): all queues, the PRP list, and the
 * bounce buffer come from vmm_alloc_surface -> identity-mapped low RAM (phys ==
 * virt), so a kernel pointer doubles as the device-visible physical address in
 * ASQ/ACQ/PRP. The caller's buffer may be higher-half (virt != phys), so the
 * device DMAs only into the bounce buffer and we copy to/from the caller. The
 * register block lives in the PCI hole, so vmm_map_mmio() maps it. */
#include "nvme.h"
#include "pci.h"
#include "cpu.h"
#include "spinlock.h"
#include "console.h"
#include "vmm.h"
#include "blockdev.h"

/* ---- controller registers (offsets from the mapped BAR) ---- */
#define REG_CAP   0x00      /* capabilities (64-bit) */
#define REG_CC    0x14      /* controller configuration */
#define REG_CSTS  0x1C      /* controller status */
#define REG_AQA   0x24      /* admin queue attributes */
#define REG_ASQ   0x28      /* admin submission queue base (64-bit) */
#define REG_ACQ   0x30      /* admin completion queue base (64-bit) */
#define REG_DB    0x1000    /* doorbell registers start here */

#define CC_EN     (1u << 0)
#define CSTS_RDY  (1u << 0)
#define CSTS_CFS  (1u << 1)

/* admin opcodes */
#define ADM_CREATE_SQ  0x01
#define ADM_CREATE_CQ  0x05
#define ADM_IDENTIFY   0x06
/* NVM I/O opcodes */
#define IO_WRITE       0x01
#define IO_READ        0x02

#define PAGE 4096u
#define ADM_Q 8             /* admin queue depth (entries) */
#define IO_Q  8             /* I/O queue depth (entries)   */
#define BOUNCE_SECTORS 64   /* 32 KiB == 8 pages -> PRP1 + a 7-entry PRP list */
#define NSID 1u             /* we use namespace 1 */

struct cqe { uint32_t dw0, dw1, dw2, dw3; } __attribute__((packed));

static spinlock_t nvme_lock = SPINLOCK_INIT;
static int          present = 0;
static volatile uint8_t *regs;          /* mapped controller register block */
static uint32_t     dstrd;              /* doorbell stride exponent */
static uint64_t     capacity = 0;       /* 512-byte sectors */

/* queue 0 = admin, queue 1 = I/O. */
static uint8_t  *sq[2];                 /* submission queues (64-byte entries) */
static struct cqe *cq[2];               /* completion queues (16-byte entries) */
static uint32_t  qdepth[2];
static uint32_t  sq_tail[2], cq_head[2], cq_phase[2];
static uint32_t  sq_db[2],  cq_db[2];   /* doorbell register offsets */
static uint32_t *prp_list;              /* one page: PRP entries for big transfers */
static uint8_t  *bounce;                /* DMA bounce buffer (BOUNCE_SECTORS) */
static uint16_t  cid = 0;               /* command identifier counter */

static inline uint32_t r32(uint32_t o)            { return *(volatile uint32_t *)(regs + o); }
static inline void     w32(uint32_t o, uint32_t v){ *(volatile uint32_t *)(regs + o) = v; }
static inline uint64_t r64(uint32_t o)            { return *(volatile uint64_t *)(regs + o); }
static inline void     w64(uint32_t o, uint64_t v){ *(volatile uint64_t *)(regs + o) = v; }

static void bzero(void *d, uint32_t n) { uint8_t *p = (uint8_t *)d; for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static void bcopy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}

/* Find an NVMe controller (PCI class 0x01 mass-storage, subclass 0x08 NVM). */
static int find_device(uint8_t *slot_out, uint8_t *func_out) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_read32(0, slot, func, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) { if (func == 0) break; else continue; }
            uint32_t cls = pci_read32(0, slot, func, 0x08);
            if (((cls >> 24) & 0xFF) == 0x01 && ((cls >> 16) & 0xFF) == 0x08) {
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

/* Push a 16-dword command onto queue `q`'s submission queue, ring the doorbell,
 * and spin until the matching completion arrives (phase-tag tracking). Returns 0
 * on success, -1 on error/timeout. One command in flight per queue. */
static int submit(int q, uint32_t *cmd) {
    uint32_t *slot = (uint32_t *)(sq[q] + sq_tail[q] * 64);
    for (int i = 0; i < 16; i++) slot[i] = cmd[i];
    sq_tail[q] = (sq_tail[q] + 1) % qdepth[q];
    w32(sq_db[q], sq_tail[q]);                          /* ring SQ tail doorbell */

    volatile struct cqe *e = (volatile struct cqe *)&cq[q][cq_head[q]];
    uint64_t spins = 0;
    while (((e->dw3 >> 16) & 1u) != cq_phase[q]) {       /* wait for the phase to flip */
        if (++spins > 200000000ULL) return -1;
        __asm__ volatile("pause");
    }
    uint16_t status = (uint16_t)((e->dw3 >> 17) & 0x7FFF);
    cq_head[q] = (cq_head[q] + 1) % qdepth[q];
    if (cq_head[q] == 0) cq_phase[q] ^= 1;               /* wrapped: flip expected phase */
    w32(cq_db[q], cq_head[q]);                           /* ring CQ head doorbell */
    return status ? -1 : 0;
}

/* Fill PRP1/PRP2 (cmd dwords 6..9) for a transfer of `bytes` from the bounce
 * buffer (page-aligned + contiguous, so page i = bounce + i*PAGE). */
static void set_prp(uint32_t *cmd, uint32_t bytes) {
    uint64_t p1 = (uint64_t)(uintptr_t)bounce;
    uint64_t p2 = 0;
    if (bytes > PAGE) {
        uint32_t npages = (bytes + PAGE - 1) / PAGE;
        if (npages == 2) {
            p2 = p1 + PAGE;
        } else {
            for (uint32_t i = 0; i + 1 < npages; i++) {
                prp_list[2 * i]     = (uint32_t)(p1 + (uint64_t)(i + 1) * PAGE);
                prp_list[2 * i + 1] = (uint32_t)((p1 + (uint64_t)(i + 1) * PAGE) >> 32);
            }
            p2 = (uint64_t)(uintptr_t)prp_list;
        }
    }
    cmd[6] = (uint32_t)p1;       cmd[7] = (uint32_t)(p1 >> 32);
    cmd[8] = (uint32_t)p2;       cmd[9] = (uint32_t)(p2 >> 32);
}

/* One I/O command (read or write) for up to BOUNCE_SECTORS sectors. */
static int issue(uint64_t lba, uint32_t count, int write) {
    uint32_t cmd[16]; bzero(cmd, sizeof(cmd));
    cmd[0] = (write ? IO_WRITE : IO_READ) | ((uint32_t)(++cid) << 16);
    cmd[1] = NSID;
    set_prp(cmd, count * 512u);
    cmd[10] = (uint32_t)lba;
    cmd[11] = (uint32_t)(lba >> 32);
    cmd[12] = (count - 1) & 0xFFFF;                      /* NLB = count - 1 (0-based) */
    return submit(1, cmd);
}

static int do_io(uint64_t lba, uint32_t count, void *buf, int write) {
    if (!present || count == 0) return -1;
    uint8_t *p = (uint8_t *)buf;
    uint64_t fl = spin_lock_irqsave(&nvme_lock);
    int rc = 0;
    while (count) {
        uint32_t c = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        uint32_t bytes = c * 512u;
        if (write) bcopy(bounce, p, bytes);
        if (issue(lba, c, write) < 0) { rc = -1; break; }
        if (!write) bcopy(p, bounce, bytes);
        lba += c; p += bytes; count -= c;
    }
    spin_unlock_irqrestore(&nvme_lock, fl);
    return rc;
}

int nvme_read(uint64_t lba, uint32_t count, void *buf)        { return do_io(lba, count, buf, 0); }
int nvme_write(uint64_t lba, uint32_t count, const void *buf) { return do_io(lba, count, (void *)buf, 1); }
int nvme_present(void)       { return present; }
uint64_t nvme_capacity(void) { return capacity; }

/* Non-destructive last-sector round-trip through the block layer. */
static void selftest(void) {
    int dev = bdev_find("nvme0");
    uint64_t page = vmm_alloc_surface(1);
    if (dev < 0 || !page) { console_puts("[nvme] selftest SKIP\r\n"); return; }
    uint8_t *save = (uint8_t *)page, *pat = save + 512, *back = save + 1024;
    uint64_t s = capacity ? capacity - 1 : 0;
    for (int i = 0; i < 512; i++) pat[i] = (uint8_t)(i * 13 + 0x91);
    int ok = bdev_read(dev, s, 1, save) == 0
          && bdev_write(dev, s, 1, pat) == 0
          && bdev_read(dev, s, 1, back) == 0;
    if (ok) { for (int i = 0; i < 512; i++) if (back[i] != pat[i]) { ok = 0; break; } }
    bdev_write(dev, s, 1, save);                         /* restore */
    console_puts(ok ? "[nvme] selftest OK\r\n" : "[nvme] selftest FAIL\r\n");
}

void nvme_init(void) {
    uint8_t slot, func;
    if (!find_device(&slot, &func)) { console_puts("[nvme] none\r\n"); return; }

    uint32_t bar0 = pci_read32(0, slot, func, 0x10);
    if (bar0 & 1) { console_puts("[nvme] BAR0 is I/O space -- unsupported\r\n"); return; }
    uint64_t base = bar0 & ~0xFu;
    if (((bar0 >> 1) & 3) == 2)                          /* 64-bit BAR: high half in BAR1 */
        base |= (uint64_t)pci_read32(0, slot, func, 0x14) << 32;

    uint32_t cmd = pci_read32(0, slot, func, 0x04);      /* enable memory space + bus-master */
    pci_write32(0, slot, func, 0x04, cmd | 0x2 | 0x4);

    regs = (volatile uint8_t *)vmm_map_mmio(base, 0x2000);
    if (!regs) { console_puts("[nvme] BAR map failed\r\n"); return; }

    uint64_t cap = r64(REG_CAP);
    dstrd = (uint32_t)((cap >> 32) & 0xF);
    uint32_t stride = 4u << dstrd;
    uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;        /* max queue entries */
    if (ADM_Q > mqes || IO_Q > mqes) { console_puts("[nvme] queue too deep for controller\r\n"); return; }

    /* disable the controller before (re)programming the admin queue */
    w32(REG_CC, r32(REG_CC) & ~CC_EN);
    for (uint64_t i = 0; (r32(REG_CSTS) & CSTS_RDY); i++) {
        if (i > 100000000ULL) { console_puts("[nvme] disable timeout\r\n"); return; }
        __asm__ volatile("pause");
    }

    /* DMA memory: 4 queue pages (admin SQ/CQ, I/O SQ/CQ) + 1 PRP-list page, and a
     * separate bounce buffer. Each queue is page-aligned (NVMe requires it). */
    uint64_t qm = vmm_alloc_surface(5);
    uint64_t bb = vmm_alloc_surface((BOUNCE_SECTORS * 512) / PAGE);
    if (!qm || !bb) { console_puts("[nvme] DMA alloc failed\r\n"); return; }
    sq[0]    = (uint8_t *)(uintptr_t)(qm + 0 * PAGE);
    cq[0]    = (struct cqe *)(uintptr_t)(qm + 1 * PAGE);
    sq[1]    = (uint8_t *)(uintptr_t)(qm + 2 * PAGE);
    cq[1]    = (struct cqe *)(uintptr_t)(qm + 3 * PAGE);
    prp_list = (uint32_t *)(uintptr_t)(qm + 4 * PAGE);
    bounce   = (uint8_t *)(uintptr_t)bb;
    bzero(sq[0], PAGE); bzero(cq[0], PAGE); bzero(sq[1], PAGE); bzero(cq[1], PAGE);

    qdepth[0] = ADM_Q; qdepth[1] = IO_Q;
    sq_tail[0] = cq_head[0] = 0; cq_phase[0] = 1;
    sq_tail[1] = cq_head[1] = 0; cq_phase[1] = 1;
    sq_db[0] = REG_DB + 0 * stride; cq_db[0] = REG_DB + 1 * stride;
    sq_db[1] = REG_DB + 2 * stride; cq_db[1] = REG_DB + 3 * stride;

    /* program the admin queue + enable the controller */
    w32(REG_AQA, (ADM_Q - 1) | ((ADM_Q - 1) << 16));
    w64(REG_ASQ, (uint64_t)(uintptr_t)sq[0]);
    w64(REG_ACQ, (uint64_t)(uintptr_t)cq[0]);
    w32(REG_CC, (4u << 20) | (6u << 16) | (0u << 7) | (0u << 4) | CC_EN); /* IOCQES=16B, IOSQES=64B, 4K, NVM, EN */
    for (uint64_t i = 0; !(r32(REG_CSTS) & CSTS_RDY); i++) {
        if (r32(REG_CSTS) & CSTS_CFS) { console_puts("[nvme] controller fatal status\r\n"); return; }
        if (i > 100000000ULL) { console_puts("[nvme] enable timeout\r\n"); return; }
        __asm__ volatile("pause");
    }

    /* create the I/O completion queue, then the I/O submission queue (admin cmds) */
    uint32_t c[16];
    bzero(c, sizeof(c));
    c[0]  = ADM_CREATE_CQ | ((uint32_t)(++cid) << 16);
    c[6]  = (uint32_t)(uintptr_t)cq[1]; c[7] = (uint32_t)((uint64_t)(uintptr_t)cq[1] >> 32);
    c[10] = 1u | ((IO_Q - 1) << 16);                    /* QID=1, QSIZE=depth-1 */
    c[11] = 0x1;                                        /* PC=1 (physically contiguous), no interrupts */
    if (submit(0, c) < 0) { console_puts("[nvme] create CQ failed\r\n"); return; }

    bzero(c, sizeof(c));
    c[0]  = ADM_CREATE_SQ | ((uint32_t)(++cid) << 16);
    c[6]  = (uint32_t)(uintptr_t)sq[1]; c[7] = (uint32_t)((uint64_t)(uintptr_t)sq[1] >> 32);
    c[10] = 1u | ((IO_Q - 1) << 16);                    /* QID=1, QSIZE=depth-1 */
    c[11] = (1u << 16) | 0x1;                           /* CQID=1, PC=1 */
    if (submit(0, c) < 0) { console_puts("[nvme] create SQ failed\r\n"); return; }

    /* IDENTIFY namespace 1 -> capacity + LBA size (into the bounce buffer) */
    bzero(c, sizeof(c));
    c[0]  = ADM_IDENTIFY | ((uint32_t)(++cid) << 16);
    c[1]  = NSID;
    c[6]  = (uint32_t)(uintptr_t)bounce; c[7] = (uint32_t)((uint64_t)(uintptr_t)bounce >> 32);
    c[10] = 0;                                          /* CNS=0: identify namespace */
    if (submit(0, c) < 0) { console_puts("[nvme] identify ns failed\r\n"); return; }

    uint64_t nsze = *(uint64_t *)(bounce + 0);          /* size in logical blocks */
    uint8_t  flbas = bounce[26] & 0xF;                  /* current LBA-format index */
    uint32_t lbaf  = *(uint32_t *)(bounce + 128 + 4u * flbas);
    uint32_t lbads = (lbaf >> 16) & 0xFF;               /* LBA data size = 2^lbads bytes */
    if (lbads != 9) {                                   /* only 512-byte LBAs map 1:1 to bdev sectors */
        console_puts("[nvme] namespace LBA size != 512 -- not registered\r\n");
        return;
    }
    capacity = nsze;
    present = 1;
    bdev_register("nvme0", capacity, nvme_read, nvme_write);

    console_puts("[nvme] up: ");
    console_putdec(capacity);
    console_puts(" sectors\r\n");

    selftest();
}
