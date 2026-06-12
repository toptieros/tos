/* virtio-net over the legacy virtio-pci transport (I/O BAR0), matching how
 * virtio_blk.c drives the transitional device. Two virtqueues: RX (queue 0) and
 * TX (queue 1). Offloads are all OFF (we negotiate only VIRTIO_NET_F_MAC), so every
 * frame carries a zeroed 10-byte virtio_net_hdr and we deal in plain Ethernet.
 * Polled. See design/virtio-net.md.
 *
 * Memory model: every ring + buffer comes from vmm_alloc_surface (identity-mapped,
 * phys == virt), so a buffer pointer IS its DMA address -- no bounce buffer needed
 * (the driver owns these buffers; nothing here lives in the higher half). */
#include "virtio_net.h"
#include "pci.h"
#include "cpu.h"
#include "spinlock.h"
#include "console.h"
#include "vmm.h"

/* legacy virtio-pci I/O register offsets (from BAR0, MSI-X disabled) */
#define VIO_HOST_FEATURES  0x00
#define VIO_GUEST_FEATURES 0x04
#define VIO_QUEUE_PFN      0x08
#define VIO_QUEUE_NUM      0x0C
#define VIO_QUEUE_SEL      0x0E
#define VIO_QUEUE_NOTIFY   0x10
#define VIO_STATUS         0x12
#define VIO_NET_CONFIG     0x14   /* device config: mac[6], status[2] */

#define VST_ACK       1
#define VST_DRIVER    2
#define VST_DRIVER_OK 4
#define VST_FAILED    0x80

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VNET_F_MAC (1u << 5)

#define PAGE 4096u
#define NRX  16                   /* pre-posted receive buffers */
#define BUFSZ 2048u               /* per buffer: virtio_net_hdr + up to 1514 frame */

struct vring_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct vring_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));

/* legacy virtio_net_hdr (10 bytes; no num_buffers without MRG_RXBUF) */
struct virtio_net_hdr {
    uint8_t  flags, gso_type;
    uint16_t hdr_len, gso_size, csum_start, csum_offset;
} __attribute__((packed));
#define NETHDR 10u

/* One split virtqueue. */
struct vq {
    uint16_t index, qsize, last_used;
    struct vring_desc *desc;
    volatile uint16_t *avail;     /* [0]=flags [1]=idx [2..]=ring[Q] */
    volatile uint16_t *used;      /* [0]=flags [1]=idx, then used_elem[Q] */
    struct vring_used_elem *used_ring;
};

static spinlock_t net_lock = SPINLOCK_INIT;
static int      present = 0;
static uint16_t io_base = 0;
static uint8_t  mac[6];
static struct vq rxq, txq;
static uint8_t *rx_bufs;          /* NRX contiguous BUFSZ buffers */
static uint8_t *tx_buf;           /* single TX buffer (hdr + frame) */

static inline uint16_t align_up(uint16_t v, uint16_t a) { return (uint16_t)((v + a - 1) & ~(a - 1)); }
static void bcopy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void bzero(void *d, uint32_t n) { uint8_t *p = (uint8_t *)d; for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static void put_hex2(uint8_t v) {            /* one byte as exactly two hex digits */
    console_putc("0123456789abcdef"[(v >> 4) & 0xF]);
    console_putc("0123456789abcdef"[v & 0xF]);
}

static int find_device(uint8_t *slot_out, uint8_t *func_out) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_read32(0, slot, func, 0x00);
            uint16_t vendor = id & 0xFFFF, device = id >> 16;
            if (vendor == 0xFFFF) { if (func == 0) break; else continue; }
            /* virtio vendor; transitional net = 0x1000, modern = 0x1041 */
            if (vendor == 0x1AF4 && (device == 0x1000 || device == 0x1041)) {
                *slot_out = slot; *func_out = func; return 1;
            }
            if (func == 0) {
                uint32_t hdr = pci_read32(0, slot, 0, 0x0C);
                if (!((hdr >> 16) & 0x80)) break;
            }
        }
    }
    return 0;
}

/* Allocate + register one split virtqueue (legacy layout: desc | avail | used). */
static int vq_setup(struct vq *q, uint16_t index) {
    outw(io_base + VIO_QUEUE_SEL, index);
    uint16_t qsize = inw(io_base + VIO_QUEUE_NUM);
    if (qsize == 0) return -1;
    q->index = index; q->qsize = qsize; q->last_used = 0;

    uint16_t desc_sz  = (uint16_t)(16 * qsize);
    uint16_t avail_sz = (uint16_t)(6 + 2 * qsize);
    uint16_t used_off = align_up((uint16_t)(desc_sz + avail_sz), (uint16_t)PAGE);
    uint32_t used_sz  = 6u + 8u * qsize;
    int nframes = (int)((used_off + used_sz + PAGE - 1) / PAGE);

    uint64_t vbase = vmm_alloc_surface(nframes);
    if (!vbase) return -1;
    bzero((void *)(uintptr_t)vbase, nframes * PAGE);
    q->desc      = (struct vring_desc *)(uintptr_t)vbase;
    q->avail     = (volatile uint16_t *)(uintptr_t)(vbase + desc_sz);
    q->used      = (volatile uint16_t *)(uintptr_t)(vbase + used_off);
    q->used_ring = (struct vring_used_elem *)(uintptr_t)(vbase + used_off + 4);

    outw(io_base + VIO_QUEUE_SEL, index);
    outl(io_base + VIO_QUEUE_PFN, (uint32_t)(vbase >> 12));
    return 0;
}

/* Post receive buffer `i` (a single device-writable descriptor) into the RX ring. */
static void rx_post(int i) {
    rxq.desc[i].addr = (uint64_t)(uintptr_t)(rx_bufs + (uint32_t)i * BUFSZ);
    rxq.desc[i].len  = BUFSZ;
    rxq.desc[i].flags = VRING_DESC_F_WRITE;
    rxq.desc[i].next = 0;
    rxq.avail[2 + (rxq.avail[1] % rxq.qsize)] = (uint16_t)i;
    __sync_synchronize();
    rxq.avail[1]++;
}

int virtio_net_tx(const void *frame, uint32_t len) {
    if (!present || len == 0 || len > BUFSZ - NETHDR) return -1;
    uint64_t fl = spin_lock_irqsave(&net_lock);
    bzero(tx_buf, NETHDR);                          /* zeroed virtio_net_hdr (offloads off) */
    bcopy(tx_buf + NETHDR, frame, len);

    uint16_t head = 0;                              /* TX uses descriptor 0, one at a time */
    txq.desc[head].addr = (uint64_t)(uintptr_t)tx_buf;
    txq.desc[head].len  = NETHDR + len;
    txq.desc[head].flags = 0;                       /* device-readable */
    txq.desc[head].next = 0;
    txq.avail[2 + (txq.avail[1] % txq.qsize)] = head;
    __sync_synchronize();
    txq.avail[1]++;
    __sync_synchronize();
    outw(io_base + VIO_QUEUE_NOTIFY, txq.index);

    uint64_t spins = 0;
    while (txq.used[1] == txq.last_used) {
        if (++spins > 200000000ULL) { spin_unlock_irqrestore(&net_lock, fl); return -1; }
        __asm__ volatile("pause");
    }
    txq.last_used = txq.used[1];
    spin_unlock_irqrestore(&net_lock, fl);
    return 0;
}

int virtio_net_rx(void *buf, uint32_t max) {
    if (!present) return -1;
    uint64_t fl = spin_lock_irqsave(&net_lock);
    if (rxq.used[1] == rxq.last_used) { spin_unlock_irqrestore(&net_lock, fl); return 0; }

    struct vring_used_elem *e = &rxq.used_ring[rxq.last_used % rxq.qsize];
    uint32_t id = e->id, total = e->len;
    uint32_t flen = total > NETHDR ? total - NETHDR : 0;   /* strip the virtio_net_hdr */
    if (flen > max) flen = max;
    bcopy(buf, rx_bufs + id * BUFSZ + NETHDR, flen);

    rxq.last_used++;
    rx_post((int)id);                              /* re-post the buffer */
    __sync_synchronize();
    outw(io_base + VIO_QUEUE_NOTIFY, rxq.index);
    spin_unlock_irqrestore(&net_lock, fl);
    return (int)flen;
}

int virtio_net_present(void)        { return present; }
const uint8_t *virtio_net_mac(void) { return mac; }

/* Boot self-test: TX an ARP "who-has 10.0.2.2" (the QEMU SLIRP gateway) and poll RX
 * for the reply -- proves the full driver contract (a frame out AND a frame in) plus
 * the device's MAC config. Non-intrusive: one broadcast ARP, no state changed. */
static void arp_selftest(void) {
    uint8_t f[42]; bzero(f, sizeof f);
    for (int i = 0; i < 6; i++) f[i] = 0xFF;          /* dst: broadcast */
    for (int i = 0; i < 6; i++) f[6 + i] = mac[i];    /* src: our MAC   */
    f[12] = 0x08; f[13] = 0x06;                        /* ethertype ARP  */
    f[14] = 0x00; f[15] = 0x01;                        /* htype Ethernet */
    f[16] = 0x08; f[17] = 0x00;                        /* ptype IPv4     */
    f[18] = 6; f[19] = 4;                              /* hlen, plen     */
    f[20] = 0x00; f[21] = 0x01;                        /* oper request   */
    for (int i = 0; i < 6; i++) f[22 + i] = mac[i];    /* sender MAC     */
    f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;      /* sender IP 10.0.2.15 (SLIRP guest) */
    f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;       /* target IP 10.0.2.2  (SLIRP gw)    */

    if (virtio_net_tx(f, 42) < 0) { console_puts("[virtio-net] selftest SKIP (tx)\r\n"); return; }

    uint8_t r[BUFSZ];
    for (long tries = 0; tries < 4000000; tries++) {
        int n = virtio_net_rx(r, sizeof r);
        if (n >= 42 && r[12] == 0x08 && r[13] == 0x06 && r[21] == 0x02 &&  /* ARP reply */
            r[28] == 10 && r[29] == 0 && r[30] == 2 && r[31] == 2) {       /* from 10.0.2.2 */
            console_puts("[virtio-net] ARP reply: 10.0.2.2 is at ");
            for (int i = 0; i < 6; i++) { if (i) console_putc(':'); put_hex2(r[22 + i]); }
            console_puts("\r\n[virtio-net] selftest OK\r\n");
            return;
        }
        if (n <= 0) for (volatile int d = 0; d < 2000; d++) { }            /* brief backoff */
    }
    console_puts("[virtio-net] selftest FAIL (no ARP reply)\r\n");
}

void virtio_net_init(void) {
    uint8_t slot, func;
    if (!find_device(&slot, &func)) { console_puts("[virtio-net] none\r\n"); return; }

    uint32_t bar0 = pci_read32(0, slot, func, 0x10);
    if (!(bar0 & 1)) { console_puts("[virtio-net] BAR0 is MMIO (modern-only) -- unsupported\r\n"); return; }
    io_base = (uint16_t)(bar0 & 0xFFFC);

    uint32_t cmd = pci_read32(0, slot, func, 0x04);
    pci_write32(0, slot, func, 0x04, cmd | 0x1 | 0x4);    /* I/O space + bus-master DMA */

    outb(io_base + VIO_STATUS, 0);                        /* reset */
    outb(io_base + VIO_STATUS, VST_ACK);
    outb(io_base + VIO_STATUS, VST_ACK | VST_DRIVER);
    uint32_t host = inl(io_base + VIO_HOST_FEATURES);
    outl(io_base + VIO_GUEST_FEATURES, host & VNET_F_MAC);  /* only MAC; offloads off */

    for (int i = 0; i < 6; i++) mac[i] = inb(io_base + VIO_NET_CONFIG + i);

    if (vq_setup(&rxq, 0) < 0 || vq_setup(&txq, 1) < 0) {
        console_puts("[virtio-net] queue setup failed\r\n");
        outb(io_base + VIO_STATUS, VST_FAILED); return;
    }

    uint64_t rb = vmm_alloc_surface((NRX * BUFSZ + PAGE - 1) / PAGE);
    uint64_t tb = vmm_alloc_surface(1);
    if (!rb || !tb) { console_puts("[virtio-net] buffer alloc failed\r\n"); outb(io_base + VIO_STATUS, VST_FAILED); return; }
    rx_bufs = (uint8_t *)(uintptr_t)rb;
    tx_buf  = (uint8_t *)(uintptr_t)tb;
    for (int i = 0; i < NRX; i++) rx_post(i);

    outb(io_base + VIO_STATUS, VST_ACK | VST_DRIVER | VST_DRIVER_OK);
    __sync_synchronize();
    outw(io_base + VIO_QUEUE_NOTIFY, rxq.index);          /* tell the device RX buffers are ready */
    present = 1;

    console_puts("[virtio-net] up: MAC ");
    for (int i = 0; i < 6; i++) { if (i) console_putc(':'); put_hex2(mac[i]); }
    console_puts("\r\n");

    arp_selftest();
}
