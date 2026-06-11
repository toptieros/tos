/* virtio-blk over the legacy virtio-pci transport (I/O BAR0). Polled, one request
 * in flight. Works against QEMU's transitional virtio-blk-pci (the `if=virtio`
 * default): we drive the legacy register window and never set VIRTIO_F_VERSION_1,
 * so the device stays in legacy mode. See the virtio 0.9.5 spec / OSDev wiki.
 *
 * Memory model: every kernel frame here is identity-mapped (phys == virt, see
 * kernel/mm/vmm.c), so the same address serves as a CPU pointer AND the device-
 * visible physical address in a descriptor -- no separate phys translation. */
#include "virtio_blk.h"
#include "pci.h"
#include "cpu.h"
#include "spinlock.h"
#include "console.h"
#include "vmm.h"
#include "blockdev.h"

/* legacy virtio-pci I/O register offsets (from BAR0, MSI-X disabled) */
#define VIO_HOST_FEATURES  0x00   /* RO 32 */
#define VIO_GUEST_FEATURES 0x04   /* RW 32 */
#define VIO_QUEUE_PFN      0x08   /* RW 32 -- vring physical page-frame number */
#define VIO_QUEUE_NUM      0x0C   /* RO 16 -- device's max queue size          */
#define VIO_QUEUE_SEL      0x0E   /* RW 16 */
#define VIO_QUEUE_NOTIFY   0x10   /* RW 16 */
#define VIO_STATUS         0x12   /* RW  8 */
#define VIO_ISR            0x13   /* RC  8 */
#define VIO_BLK_CONFIG     0x14   /* device config: capacity (u64 sectors)      */

#define VST_ACK     1
#define VST_DRIVER  2
#define VST_DRIVER_OK 4
#define VST_FAILED  0x80

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2      /* buffer is device-write (read path / status) */

#define VBLK_T_IN   0             /* read from device  */
#define VBLK_T_OUT  1             /* write to device   */

#define PAGE 4096u

struct vring_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct vring_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));

struct virtio_blk_req { uint32_t type; uint32_t reserved; uint64_t sector; } __attribute__((packed));

static spinlock_t vblk_lock = SPINLOCK_INIT;
static int        present = 0;
static uint16_t   io_base = 0;
static uint16_t   qsize = 0;            /* queue size Q */
static uint64_t   capacity = 0;         /* sectors */

/* vring sub-structures (raw pointers into the contiguous DMA region) */
static struct vring_desc *desc;
static volatile uint16_t *avail;        /* [0]=flags [1]=idx [2..]=ring[Q]  */
static volatile uint16_t *used;         /* [0]=flags [1]=idx, then used_elem[Q] */
static struct vring_used_elem *used_ring;
static uint16_t   last_used = 0;

/* request scratch (header + status), one identity-mapped page */
static struct virtio_blk_req *req_hdr;
static volatile uint8_t      *req_status;

static inline uint16_t align_up(uint16_t v, uint16_t a) { return (uint16_t)((v + a - 1) & ~(a - 1)); }

/* Find a virtio-blk function on bus 0. Returns 1 and fills slot/func, else 0. */
static int find_device(uint8_t *slot_out, uint8_t *func_out) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_read32(0, slot, func, 0x00);
            uint16_t vendor = id & 0xFFFF, device = id >> 16;
            if (vendor == 0xFFFF) { if (func == 0) break; else continue; }
            /* Red Hat / virtio vendor; transitional blk = 0x1001, modern = 0x1042. */
            if (vendor == 0x1AF4 && (device == 0x1001 || device == 0x1042)) {
                *slot_out = slot; *func_out = func; return 1;
            }
            if (func == 0) {
                uint32_t hdr = pci_read32(0, slot, 0, 0x0C);
                if (!((hdr >> 16) & 0x80)) break;   /* not multi-function */
            }
        }
    }
    return 0;
}

/* Submit the head descriptor (index 0) and spin until the device retires it.
 * Returns the device status byte (0 == OK), or 0xff on timeout. */
static uint8_t submit_and_wait(void) {
    avail[2 + (avail[1] % qsize)] = 0;     /* avail.ring[idx % Q] = head desc 0 */
    __sync_synchronize();
    avail[1]++;                            /* publish: bump avail.idx           */
    __sync_synchronize();
    outw(io_base + VIO_QUEUE_NOTIFY, 0);   /* kick queue 0                       */

    uint64_t spins = 0;
    while (used[1] == last_used) {         /* spin on used.idx                   */
        if (++spins > 200000000ULL) return 0xff;
        __asm__ volatile("pause");
    }
    last_used = used[1];
    __sync_synchronize();
    return *req_status;
}

static int do_io(uint64_t lba, uint32_t count, void *buf, int write) {
    if (!present || count == 0) return -1;
    uint64_t fl = spin_lock_irqsave(&vblk_lock);
    req_hdr->type = write ? VBLK_T_OUT : VBLK_T_IN;
    req_hdr->reserved = 0;
    req_hdr->sector = lba;
    *req_status = 0xff;

    desc[0].addr = (uint64_t)(uintptr_t)req_hdr;  desc[0].len = sizeof(struct virtio_blk_req);
    desc[0].flags = VRING_DESC_F_NEXT;            desc[0].next = 1;
    desc[1].addr = (uint64_t)(uintptr_t)buf;      desc[1].len = 512u * count;
    desc[1].flags = VRING_DESC_F_NEXT | (write ? 0 : VRING_DESC_F_WRITE); desc[1].next = 2;
    desc[2].addr = (uint64_t)(uintptr_t)req_status; desc[2].len = 1;
    desc[2].flags = VRING_DESC_F_WRITE;           desc[2].next = 0;

    uint8_t st = submit_and_wait();
    spin_unlock_irqrestore(&vblk_lock, fl);
    return st == 0 ? 0 : -1;
}

int virtio_blk_read(uint64_t lba, uint32_t count, void *buf)        { return do_io(lba, count, buf, 0); }
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf) { return do_io(lba, count, (void *)buf, 1); }
int virtio_blk_present(void)      { return present; }
uint64_t virtio_blk_capacity(void){ return capacity; }

/* Non-destructive round-trip on the LAST sector: save -> write a pattern ->
 * read back + compare -> restore. Driven through the generic block layer (find
 * "virtio0", bdev_read/write), so it also proves the bdev registration + dispatch
 * -- not just the raw driver. No data on the disk is corrupted. */
static void selftest(void) {
    int dev = bdev_find("virtio0");
    uint64_t page = vmm_alloc_surface(1);
    if (dev < 0 || !page) { console_puts("[virtio-blk] selftest SKIP\r\n"); return; }
    uint8_t *save = (uint8_t *)page, *pat = save + 512, *back = save + 1024;
    uint64_t s = capacity ? capacity - 1 : 0;
    for (int i = 0; i < 512; i++) pat[i] = (uint8_t)(i * 7 + 0x5a);
    int ok = bdev_read(dev, s, 1, save) == 0
          && bdev_write(dev, s, 1, pat) == 0
          && bdev_read(dev, s, 1, back) == 0;
    if (ok) { for (int i = 0; i < 512; i++) if (back[i] != pat[i]) { ok = 0; break; } }
    bdev_write(dev, s, 1, save);                   /* restore the original bytes */
    console_puts(ok ? "[virtio-blk] selftest OK\r\n" : "[virtio-blk] selftest FAIL\r\n");
}

void virtio_blk_init(void) {
    uint8_t slot, func;
    if (!find_device(&slot, &func)) { console_puts("[virtio-blk] none\r\n"); return; }

    uint32_t bar0 = pci_read32(0, slot, func, 0x10);
    if (!(bar0 & 1)) { console_puts("[virtio-blk] BAR0 is MMIO (modern-only) -- unsupported\r\n"); return; }
    io_base = (uint16_t)(bar0 & 0xFFFC);

    uint32_t cmd = pci_read32(0, slot, func, 0x04);  /* enable I/O space + bus-master DMA */
    pci_write32(0, slot, func, 0x04, cmd | 0x1 | 0x4);

    outb(io_base + VIO_STATUS, 0);                                   /* reset            */
    outb(io_base + VIO_STATUS, VST_ACK);
    outb(io_base + VIO_STATUS, VST_ACK | VST_DRIVER);
    (void)inl(io_base + VIO_HOST_FEATURES);
    outl(io_base + VIO_GUEST_FEATURES, 0);                           /* legacy, no opts  */

    outw(io_base + VIO_QUEUE_SEL, 0);
    qsize = inw(io_base + VIO_QUEUE_NUM);
    if (qsize == 0) { console_puts("[virtio-blk] queue 0 missing\r\n"); outb(io_base + VIO_STATUS, VST_FAILED); return; }

    /* legacy split-vring layout: desc | avail | (page-align) used */
    uint16_t desc_sz  = (uint16_t)(16 * qsize);
    uint16_t avail_sz = (uint16_t)(6 + 2 * qsize);
    uint16_t used_off = align_up((uint16_t)(desc_sz + avail_sz), (uint16_t)PAGE);
    uint32_t used_sz  = 6u + 8u * qsize;
    uint32_t total    = used_off + used_sz;
    int nframes = (int)((total + PAGE - 1) / PAGE);

    uint64_t vbase = vmm_alloc_surface(nframes);
    if (!vbase) { console_puts("[virtio-blk] vring alloc failed\r\n"); outb(io_base + VIO_STATUS, VST_FAILED); return; }
    desc      = (struct vring_desc *)(uintptr_t)vbase;
    avail     = (volatile uint16_t *)(uintptr_t)(vbase + desc_sz);
    used      = (volatile uint16_t *)(uintptr_t)(vbase + used_off);
    used_ring = (struct vring_used_elem *)(uintptr_t)(vbase + used_off + 4);
    (void)used_ring;
    last_used = 0;

    uint64_t scratch = vmm_alloc_surface(1);
    if (!scratch) { console_puts("[virtio-blk] req page alloc failed\r\n"); outb(io_base + VIO_STATUS, VST_FAILED); return; }
    req_hdr    = (struct virtio_blk_req *)(uintptr_t)scratch;
    req_status = (volatile uint8_t *)(uintptr_t)(scratch + sizeof(struct virtio_blk_req));

    outl(io_base + VIO_QUEUE_PFN, (uint32_t)(vbase >> 12));           /* hand the device the ring */

    uint32_t cap_lo = inl(io_base + VIO_BLK_CONFIG);
    uint32_t cap_hi = inl(io_base + VIO_BLK_CONFIG + 4);
    capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    outb(io_base + VIO_STATUS, VST_ACK | VST_DRIVER | VST_DRIVER_OK); /* go */
    present = 1;
    bdev_register("virtio0", capacity, virtio_blk_read, virtio_blk_write);

    console_puts("[virtio-blk] up: ");
    console_putdec(capacity);
    console_puts(" sectors, queue ");
    console_putdec(qsize);
    console_puts("\r\n");

    selftest();
}
