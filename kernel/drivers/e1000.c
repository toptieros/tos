/* Intel 8254x ("e1000") gigabit NIC. The ubiquitous emulated/real NIC -- QEMU's
 * `-device e1000` is an 82540EM (PCI 8086:100E) -- brought up behind the same netif
 * contract as virtio-net so the stack doesn't care which one it's talking to.
 *
 * Register block: a memory BAR (BAR0), mapped via vmm_map_mmio() (it lives in the
 * PCI hole, not the RAM identity map). Data path: two legacy descriptor rings
 * (16-byte descriptors) plus packet buffers, all from vmm_alloc_surface so they're
 * identity-mapped (phys == virt) -- a buffer pointer IS its DMA address, no bounce
 * buffer. Polled: TX waits on the descriptor's DD bit, RX scans for it. The MAC
 * comes from the Receive Address registers (the firmware loads them from the EEPROM
 * at reset); if those look unset we read the EEPROM directly. See design/virtio-net.md. */
#include "e1000.h"
#include "netif.h"
#include "pci.h"
#include "cpu.h"
#include "spinlock.h"
#include "console.h"
#include "vmm.h"

/* --- register offsets (from BAR0) ----------------------------------------- */
#define E_CTRL    0x0000    /* device control                  */
#define E_STATUS  0x0008    /* device status                   */
#define E_EERD    0x0014    /* EEPROM read                     */
#define E_ICR     0x00C0    /* interrupt cause read            */
#define E_IMS     0x00D0    /* interrupt mask set              */
#define E_IMC     0x00D8    /* interrupt mask clear            */
#define E_RCTL    0x0100    /* receive control                 */
#define E_TCTL    0x0400    /* transmit control                */
#define E_TIPG    0x0410    /* transmit inter-packet gap       */
#define E_RDBAL   0x2800    /* RX descriptor base (low)        */
#define E_RDBAH   0x2804    /* RX descriptor base (high)       */
#define E_RDLEN   0x2808    /* RX descriptor ring length       */
#define E_RDH     0x2810    /* RX descriptor head              */
#define E_RDT     0x2818    /* RX descriptor tail              */
#define E_TDBAL   0x3800
#define E_TDBAH   0x3804
#define E_TDLEN   0x3808
#define E_TDH     0x3810
#define E_TDT     0x3818
#define E_MTA     0x5200    /* multicast table array (128 dwords) */
#define E_RAL     0x5400    /* receive address low (MAC 0..3)  */
#define E_RAH     0x5404    /* receive address high (MAC 4..5 + AV) */

/* CTRL bits */
#define CTRL_SLU   0x00000040u   /* set link up           */
#define CTRL_ASDE  0x00000020u   /* auto-speed detect     */
#define CTRL_RST   0x04000000u   /* device reset          */
/* RCTL bits (BSIZE 2048 == bits clear, so RCTL has no size field set) */
#define RCTL_EN    0x00000002u   /* receiver enable       */
#define RCTL_BAM   0x00008000u   /* accept broadcast      */
#define RCTL_SECRC 0x04000000u   /* strip Ethernet CRC    */
/* TCTL bits */
#define TCTL_EN    0x00000002u   /* transmitter enable    */
#define TCTL_PSP   0x00000008u   /* pad short packets     */
#define TCTL_CT    0x000000F0u   /* collision threshold = 0x0F */
#define TCTL_COLD  0x00040000u   /* collision distance = 0x40 (full duplex) */
/* descriptor status / command bits */
#define RXS_DD     0x01          /* RX descriptor done    */
#define RXS_EOP    0x02          /* end of packet         */
#define TXC_EOP    0x01          /* end of packet         */
#define TXC_IFCS   0x02          /* insert FCS            */
#define TXC_RS     0x08          /* report status         */
#define TXS_DD     0x01          /* TX descriptor done    */

#define PAGE  4096u
#define NRX   32                 /* receive descriptors / buffers */
#define NTX   8                  /* transmit descriptors / buffers */
#define BUFSZ 2048u

struct rx_desc {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint16_t csum;
    volatile uint8_t  status;
    volatile uint8_t  errors;
    volatile uint16_t special;
} __attribute__((packed));

struct tx_desc {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint8_t  cso;
    volatile uint8_t  cmd;
    volatile uint8_t  status;
    volatile uint8_t  css;
    volatile uint16_t special;
} __attribute__((packed));

static spinlock_t e_lock = SPINLOCK_INIT;
static int        present = 0;
static volatile uint8_t *regs = 0;       /* mapped register block */
static uint8_t    mac[6];
static struct rx_desc *rx_ring;          /* NRX descriptors */
static struct tx_desc *tx_ring;          /* NTX descriptors */
static uint8_t   *rx_bufs, *tx_bufs;
static uint16_t   rx_cur = 0, tx_cur = 0;

static inline uint32_t er(uint32_t off)             { return *(volatile uint32_t *)(regs + off); }
static inline void     ew(uint32_t off, uint32_t v) { *(volatile uint32_t *)(regs + off) = v; }

static void bcopy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void bzero(void *d, uint32_t n) { uint8_t *p = (uint8_t *)d; for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static void put_hex2(uint8_t v) {
    console_putc("0123456789abcdef"[(v >> 4) & 0xF]);
    console_putc("0123456789abcdef"[v & 0xF]);
}
static void delay(int n) { for (volatile int i = 0; i < n * 1000; i++) { } }

/* EEPROM word read (82540EM format: START=bit0, DONE=bit4, ADDR=bits8-15, DATA hi). */
static uint16_t eeprom_read(uint16_t word) {
    ew(E_EERD, ((uint32_t)word << 8) | 1u);
    uint32_t v = 0;
    for (int i = 0; i < 100000; i++) { v = er(E_EERD); if (v & (1u << 4)) break; }
    return (uint16_t)(v >> 16);
}

static void read_mac(void) {
    uint32_t ral = er(E_RAL), rah = er(E_RAH);
    if (rah & 0x80000000u) {                          /* Address Valid -- firmware filled it in */
        mac[0] = (uint8_t)ral;        mac[1] = (uint8_t)(ral >> 8);
        mac[2] = (uint8_t)(ral >> 16); mac[3] = (uint8_t)(ral >> 24);
        mac[4] = (uint8_t)rah;        mac[5] = (uint8_t)(rah >> 8);
        return;
    }
    uint16_t w0 = eeprom_read(0), w1 = eeprom_read(1), w2 = eeprom_read(2);
    mac[0] = (uint8_t)w0; mac[1] = (uint8_t)(w0 >> 8);
    mac[2] = (uint8_t)w1; mac[3] = (uint8_t)(w1 >> 8);
    mac[4] = (uint8_t)w2; mac[5] = (uint8_t)(w2 >> 8);
    /* program it into the receive-address filter so unicast frames are accepted */
    ew(E_RAL, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
    ew(E_RAH, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | 0x80000000u);
}

static int find_device(uint8_t *slot_out, uint8_t *func_out) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_read32(0, slot, func, 0x00);
            uint16_t vendor = id & 0xFFFF, device = id >> 16;
            if (vendor == 0xFFFF) { if (func == 0) break; else continue; }
            /* Intel 8254x legacy NICs QEMU exposes: 82540EM (100E, the default
             * `-device e1000`), 82545EM (100F), 82544GC (1008). NOT e1000e (10D3),
             * which is a different register model. */
            if (vendor == 0x8086 && (device == 0x100E || device == 0x100F || device == 0x1008)) {
                *slot_out = slot; *func_out = func; return 1;
            }
            if (func == 0) {
                uint32_t hdr = pci_read32(0, slot, 0, 0x0C);
                if (!((hdr >> 16) & 0x80)) break;     /* not multifunction */
            }
        }
    }
    return 0;
}

int e1000_tx(const void *frame, uint32_t len) {
    if (!present || len == 0 || len > BUFSZ) return -1;
    uint64_t fl = spin_lock_irqsave(&e_lock);
    uint16_t i = tx_cur;
    bcopy(tx_bufs + (uint32_t)i * BUFSZ, frame, len);
    tx_ring[i].addr   = (uint64_t)(uintptr_t)(tx_bufs + (uint32_t)i * BUFSZ);
    tx_ring[i].length = (uint16_t)len;
    tx_ring[i].cmd    = TXC_EOP | TXC_IFCS | TXC_RS;
    tx_ring[i].status = 0;
    tx_cur = (uint16_t)((i + 1) % NTX);
    __sync_synchronize();
    ew(E_TDT, tx_cur);

    uint64_t spins = 0;
    while (!(tx_ring[i].status & TXS_DD)) {
        if (++spins > 200000000ULL) { spin_unlock_irqrestore(&e_lock, fl); return -1; }
        __asm__ volatile("pause");
    }
    spin_unlock_irqrestore(&e_lock, fl);
    return 0;
}

int e1000_rx(void *buf, uint32_t max) {
    if (!present) return -1;
    uint64_t fl = spin_lock_irqsave(&e_lock);
    uint16_t i = rx_cur;
    if (!(rx_ring[i].status & RXS_DD)) { spin_unlock_irqrestore(&e_lock, fl); return 0; }

    uint32_t len = rx_ring[i].length;                 /* CRC already stripped (RCTL_SECRC) */
    if (len > max) len = max;
    bcopy(buf, rx_bufs + (uint32_t)i * BUFSZ, len);

    rx_ring[i].status = 0;                             /* hand the descriptor back to the NIC */
    __sync_synchronize();
    ew(E_RDT, i);                                      /* tail = the descriptor we just freed */
    rx_cur = (uint16_t)((i + 1) % NRX);
    spin_unlock_irqrestore(&e_lock, fl);
    return (int)len;
}

int e1000_present(void)        { return present; }
const uint8_t *e1000_mac(void) { return mac; }

static const struct netif e1000_nif = {
    "e1000", e1000_present, e1000_mac, e1000_tx, e1000_rx,
};

/* Boot self-test: TX an ARP "who-has 10.0.2.2" and poll RX for the reply -- proves
 * the driver contract (a frame out AND a frame in) on this NIC specifically, before
 * the stack runs. Mirrors virtio-net's. Non-intrusive: one broadcast ARP. */
static void arp_selftest(void) {
    uint8_t f[42]; bzero(f, sizeof f);
    for (int i = 0; i < 6; i++) f[i] = 0xFF;
    for (int i = 0; i < 6; i++) f[6 + i] = mac[i];
    f[12] = 0x08; f[13] = 0x06;
    f[14] = 0x00; f[15] = 0x01; f[16] = 0x08; f[17] = 0x00;
    f[18] = 6; f[19] = 4; f[20] = 0x00; f[21] = 0x01;
    for (int i = 0; i < 6; i++) f[22 + i] = mac[i];
    f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;
    f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;

    if (e1000_tx(f, 42) < 0) { console_puts("[e1000] selftest SKIP (tx)\r\n"); return; }
    uint8_t r[BUFSZ];
    for (long tries = 0; tries < 4000000; tries++) {
        int n = e1000_rx(r, sizeof r);
        if (n >= 42 && r[12] == 0x08 && r[13] == 0x06 && r[21] == 0x02 &&
            r[28] == 10 && r[29] == 0 && r[30] == 2 && r[31] == 2) {
            console_puts("[e1000] ARP reply: 10.0.2.2 is at ");
            for (int i = 0; i < 6; i++) { if (i) console_putc(':'); put_hex2(r[22 + i]); }
            console_puts("\r\n[e1000] selftest OK\r\n");
            return;
        }
        if (n <= 0) for (volatile int d = 0; d < 2000; d++) { }
    }
    console_puts("[e1000] selftest FAIL (no ARP reply)\r\n");
}

void e1000_init(void) {
    uint8_t slot, func;
    if (!find_device(&slot, &func)) { console_puts("[e1000] none\r\n"); return; }

    uint32_t bar0 = pci_read32(0, slot, func, 0x10);
    if (bar0 & 1) { console_puts("[e1000] BAR0 is I/O space -- unsupported\r\n"); return; }
    uint64_t mmio_phys = bar0 & ~0xFu;
    if (((bar0 >> 1) & 3) == 2)                          /* 64-bit memory BAR */
        mmio_phys |= (uint64_t)pci_read32(0, slot, func, 0x14) << 32;

    uint32_t cmd = pci_read32(0, slot, func, 0x04);
    pci_write32(0, slot, func, 0x04, cmd | 0x2 | 0x4);  /* memory space + bus-master DMA */

    regs = (volatile uint8_t *)vmm_map_mmio(mmio_phys, 0x20000);
    if (!regs) { console_puts("[e1000] BAR0 map failed\r\n"); return; }

    ew(E_IMC, 0xFFFFFFFFu);                             /* mask all interrupts (we poll) */
    ew(E_CTRL, er(E_CTRL) | CTRL_RST);                  /* reset the device */
    delay(10);
    for (int i = 0; i < 1000 && (er(E_CTRL) & CTRL_RST); i++) delay(1);
    ew(E_IMC, 0xFFFFFFFFu);                             /* reset re-arms interrupts; re-mask */

    ew(E_CTRL, er(E_CTRL) | CTRL_SLU | CTRL_ASDE);      /* bring the link up */
    for (int i = 0; i < 128; i++) ew(E_MTA + (uint32_t)i * 4, 0);   /* clear multicast filter */

    read_mac();

    /* descriptor rings (both fit in one identity-mapped page) + packet buffers */
    uint64_t ring = vmm_alloc_surface(1);
    uint64_t rb   = vmm_alloc_surface((NRX * BUFSZ + PAGE - 1) / PAGE);
    uint64_t tb   = vmm_alloc_surface((NTX * BUFSZ + PAGE - 1) / PAGE);
    if (!ring || !rb || !tb) { console_puts("[e1000] DMA alloc failed\r\n"); return; }
    bzero((void *)(uintptr_t)ring, PAGE);
    rx_ring = (struct rx_desc *)(uintptr_t)ring;
    tx_ring = (struct tx_desc *)(uintptr_t)(ring + 512);   /* NRX*16=512 bytes of RX ring first */
    rx_bufs = (uint8_t *)(uintptr_t)rb;
    tx_bufs = (uint8_t *)(uintptr_t)tb;

    for (int i = 0; i < NRX; i++) {
        rx_ring[i].addr   = (uint64_t)(uintptr_t)(rx_bufs + (uint32_t)i * BUFSZ);
        rx_ring[i].status = 0;
    }
    ew(E_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    ew(E_RDBAH, (uint32_t)((uint64_t)(uintptr_t)rx_ring >> 32));
    ew(E_RDLEN, NRX * 16);
    ew(E_RDH, 0);
    ew(E_RDT, NRX - 1);                                  /* all descriptors owned by the NIC */
    ew(E_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);        /* BSIZE 2048 (size field 0) */

    for (int i = 0; i < NTX; i++) { tx_ring[i].status = TXS_DD; tx_ring[i].cmd = 0; }
    ew(E_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    ew(E_TDBAH, (uint32_t)((uint64_t)(uintptr_t)tx_ring >> 32));
    ew(E_TDLEN, NTX * 16);
    ew(E_TDH, 0);
    ew(E_TDT, 0);
    ew(E_TIPG, 0x0060200Au);                            /* IPGT=10, IPGR1=8, IPGR2=6 (copper) */
    ew(E_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    present = 1;
    console_puts("[e1000] up: MAC ");
    for (int i = 0; i < 6; i++) { if (i) console_putc(':'); put_hex2(mac[i]); }
    console_puts("\r\n");

    netif_register(&e1000_nif);                          /* offer this NIC to the stack */
    arp_selftest();
}
