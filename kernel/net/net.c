/* Minimal IPv4 + ARP + ICMP over virtio-net. Hand-built frames, internet checksum,
 * polled RX demux. The first layer of a native TCP/IP stack (design/virtio-net.md).
 * Everything is big-endian on the wire; we build/parse byte-by-byte to stay
 * endian-explicit. */
#include "net.h"
#include "virtio_net.h"
#include "console.h"

#define ETH_ARP  0x0806
#define ETH_IPV4 0x0800
#define IP_ICMP  1

static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static uint8_t my_ip[4]  = { 10,0,2,15 };           /* QEMU SLIRP default guest IP   */
static uint8_t gw_ip[4]  = { 10,0,2,2  };           /* QEMU SLIRP gateway            */
static uint16_t ip_ident = 0x1000;

/* one-entry ARP cache (gateway); grows to a table when routing needs it */
static uint8_t  arp_ip[4];
static uint8_t  arp_mac[6];
static int      arp_valid = 0;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p)    { return (uint16_t)((p[0] << 8) | p[1]); }
static int ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

/* Internet checksum: 16-bit one's-complement sum of the big-endian halfwords. */
static uint16_t csum16(const uint8_t *p, int n) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* Build an Ethernet header into f[0..13]; returns the payload offset (14). */
static int eth_hdr(uint8_t *f, const uint8_t dst[6], uint16_t ethertype) {
    const uint8_t *src = virtio_net_mac();
    for (int i = 0; i < 6; i++) { f[i] = dst[i]; f[6 + i] = src[i]; }
    put16(f + 12, ethertype);
    return 14;
}

/* Resolve `ip` to a MAC: send an ARP request, poll for the reply, cache it. */
int net_arp_resolve(const uint8_t ip[4], uint8_t mac_out[6]) {
    if (!virtio_net_present()) return -1;
    if (arp_valid && ip_eq(arp_ip, ip)) {
        for (int i = 0; i < 6; i++) mac_out[i] = arp_mac[i];
        return 0;
    }
    const uint8_t *src = virtio_net_mac();
    uint8_t f[42];
    int o = eth_hdr(f, bcast, ETH_ARP);
    put16(f + o + 0, 1);            /* htype Ethernet */
    put16(f + o + 2, ETH_IPV4);     /* ptype IPv4     */
    f[o + 4] = 6; f[o + 5] = 4;     /* hlen, plen     */
    put16(f + o + 6, 1);            /* oper request   */
    for (int i = 0; i < 6; i++) f[o + 8 + i]  = src[i];
    for (int i = 0; i < 4; i++) f[o + 14 + i] = my_ip[i];
    for (int i = 0; i < 6; i++) f[o + 18 + i] = 0;
    for (int i = 0; i < 4; i++) f[o + 24 + i] = ip[i];
    if (virtio_net_tx(f, 42) < 0) return -1;

    uint8_t r[2048];
    for (long tries = 0; tries < 4000000; tries++) {
        int n = virtio_net_rx(r, sizeof r);
        if (n >= 42 && get16(r + 12) == ETH_ARP && get16(r + 14 + 6) == 2 /* reply */
            && ip_eq(r + 14 + 14, ip)) {                                  /* sender IP == target */
            for (int i = 0; i < 6; i++) { arp_mac[i] = r[14 + 8 + i]; mac_out[i] = arp_mac[i]; }
            for (int i = 0; i < 4; i++) arp_ip[i] = ip[i];
            arp_valid = 1;
            return 0;
        }
        if (n <= 0) for (volatile int d = 0; d < 2000; d++) { }
    }
    return -1;
}

/* Send one ICMP echo request to `ip` (via the gateway MAC) and wait for the reply. */
int net_ping(const uint8_t ip[4]) {
    if (!virtio_net_present()) return -1;
    uint8_t gwmac[6];
    if (net_arp_resolve(gw_ip, gwmac) < 0) return -1;     /* next-hop is the gateway */

    uint8_t f[14 + 20 + 16];                              /* eth + IP + ICMP(8) + 8 payload */
    int o = eth_hdr(f, gwmac, ETH_IPV4);
    uint8_t *ip4 = f + o;
    uint8_t *icmp = ip4 + 20;
    int icmp_len = 8 + 8;                                  /* header + payload */

    /* ICMP echo request */
    icmp[0] = 8; icmp[1] = 0;                              /* type 8 (echo), code 0 */
    put16(icmp + 2, 0);                                    /* checksum (filled below) */
    put16(icmp + 4, 0xABCD);                               /* identifier */
    put16(icmp + 6, 1);                                    /* sequence   */
    for (int i = 0; i < 8; i++) icmp[8 + i] = (uint8_t)(0x30 + i);
    put16(icmp + 2, csum16(icmp, icmp_len));

    /* IPv4 header */
    ip4[0] = 0x45; ip4[1] = 0;
    put16(ip4 + 2, (uint16_t)(20 + icmp_len));            /* total length */
    put16(ip4 + 4, ip_ident++);
    put16(ip4 + 6, 0);                                    /* flags/frag */
    ip4[8] = 64; ip4[9] = IP_ICMP;                        /* TTL, protocol */
    put16(ip4 + 10, 0);                                   /* checksum (filled below) */
    for (int i = 0; i < 4; i++) { ip4[12 + i] = my_ip[i]; ip4[16 + i] = ip[i]; }
    put16(ip4 + 10, csum16(ip4, 20));

    int total = o + 20 + icmp_len;
    if (virtio_net_tx(f, (uint32_t)total) < 0) return -1;

    uint8_t r[2048];
    for (long tries = 0; tries < 4000000; tries++) {
        int n = virtio_net_rx(r, sizeof r);
        if (n >= 14 + 20 + 8 && get16(r + 12) == ETH_IPV4) {
            uint8_t *rip = r + 14;
            int ihl = (rip[0] & 0xF) * 4;
            uint8_t *ricmp = rip + ihl;
            if (rip[9] == IP_ICMP && ricmp[0] == 0 /* echo reply */
                && ip_eq(rip + 12, ip)             /* from the pinged host */
                && get16(ricmp + 4) == 0xABCD) {   /* our identifier */
                return 0;
            }
        }
        if (n <= 0) for (volatile int d = 0; d < 2000; d++) { }
    }
    return -1;
}

/* Send one UDP datagram. The caller supplies the dst MAC (gateway for off-link, or
 * broadcast for DHCP) and the src/dst IPs (src may be 0.0.0.0 before we have a lease). */
int net_udp_tx(const uint8_t dmac[6], const uint8_t sip[4], const uint8_t dip[4],
               uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len) {
    if (!virtio_net_present() || len > 1400) return -1;
    uint8_t f[14 + 20 + 8 + 1400];
    int o = eth_hdr(f, dmac, ETH_IPV4);
    uint8_t *ip4 = f + o, *udp = ip4 + 20, *payload = udp + 8;
    uint16_t ulen = (uint16_t)(8 + len);

    put16(udp + 0, sport); put16(udp + 2, dport);
    put16(udp + 4, ulen);  put16(udp + 6, 0);            /* checksum 0 = "not computed" (legal for IPv4) */
    for (uint32_t i = 0; i < len; i++) payload[i] = data[i];

    ip4[0] = 0x45; ip4[1] = 0;
    put16(ip4 + 2, (uint16_t)(20 + ulen));
    put16(ip4 + 4, ip_ident++);
    put16(ip4 + 6, 0);
    ip4[8] = 64; ip4[9] = 17;                            /* TTL, protocol UDP */
    put16(ip4 + 10, 0);
    for (int i = 0; i < 4; i++) { ip4[12 + i] = sip[i]; ip4[16 + i] = dip[i]; }
    put16(ip4 + 10, csum16(ip4, 20));

    return virtio_net_tx(f, (uint32_t)(o + 20 + ulen));
}

/* Send one IPv4 packet of protocol `proto` to `dip`, via the gateway as next hop
 * (SLIRP is a flat /24 reachable through 10.0.2.2). The caller supplies the L4
 * payload already built (and, for TCP/UDP, already checksummed). */
int net_ip_tx(uint8_t proto, const uint8_t dip[4], const uint8_t *payload, uint32_t len) {
    if (!virtio_net_present() || len > 1400) return -1;
    uint8_t gwmac[6];
    if (net_arp_resolve(gw_ip, gwmac) < 0) return -1;
    uint8_t f[14 + 20 + 1400];
    int o = eth_hdr(f, gwmac, ETH_IPV4);
    uint8_t *ip4 = f + o;
    ip4[0] = 0x45; ip4[1] = 0;
    put16(ip4 + 2, (uint16_t)(20 + len));
    put16(ip4 + 4, ip_ident++);
    put16(ip4 + 6, 0);
    ip4[8] = 64; ip4[9] = proto;
    put16(ip4 + 10, 0);
    for (int i = 0; i < 4; i++) { ip4[12 + i] = my_ip[i]; ip4[16 + i] = dip[i]; }
    put16(ip4 + 10, csum16(ip4, 20));
    for (uint32_t i = 0; i < len; i++) ip4[20 + i] = payload[i];
    return virtio_net_tx(f, (uint32_t)(o + 20 + len));
}

const uint8_t *net_my_ip(void) { return my_ip; }
void net_set_ip(const uint8_t ip[4]) { for (int i = 0; i < 4; i++) my_ip[i] = ip[i]; arp_valid = 0; }

static void print_ip(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) { if (i) console_putc('.'); console_putdec(ip[i]); }
}

void net_init(void) {
    if (!virtio_net_present()) return;

    uint8_t leased[4];
    if (net_dhcp(leased) == 0) {
        net_set_ip(leased);
        console_puts("[net] DHCP lease ");  print_ip(leased);  console_puts("\r\n");
    } else {
        console_puts("[net] DHCP: no lease, using static ");  print_ip(my_ip);  console_puts("\r\n");
    }

    if (net_ping(gw_ip) == 0) {
        console_puts("[net] ping 10.0.2.2: reply\r\n[net] selftest OK\r\n");
    } else {
        console_puts("[net] ping 10.0.2.2: no reply\r\n[net] selftest FAIL\r\n");
    }

    /* TCP echo round-trip against a host server reachable at 10.0.2.2:7777 (SLIRP
     * aliases the host). Only the dedicated probe starts that server; with none
     * listening the connect gets a fast RST and we skip (not a failure). */
    uint8_t srv[4] = { 10,0,2,2 };
    if (net_tcp_connect(srv, 7777) == 0) {
        const char *msg = "tOS-tcp";
        net_tcp_send((const uint8_t *)msg, 7);
        uint8_t buf[64];
        int n = net_tcp_recv(buf, sizeof buf);
        int ok = (n == 7);
        for (int i = 0; i < 7 && ok; i++) if (buf[i] != (uint8_t)msg[i]) ok = 0;
        net_tcp_close();
        console_puts(ok ? "[net] TCP echo OK\r\n" : "[net] TCP echo FAIL\r\n");
    } else {
        console_puts("[net] TCP: no echo server (skipped)\r\n");
    }
}
