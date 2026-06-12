/* A tiny DHCP client (DISCOVER -> OFFER -> REQUEST -> ACK) over the UDP/IP layer in
 * net.c. Leases an address from the network's DHCP server (QEMU SLIRP runs one at
 * 10.0.2.2 that hands out 10.0.2.15+), so the guest stops hardcoding its IP. Polled,
 * one exchange at boot. BOOTP fixed fields per RFC 951/2131; options per RFC 2132. */
#include "net.h"
#include "netif.h"
#include "console.h"

#define BOOTP_LEN  236              /* fixed BOOTP header bytes before the options   */
#define DHCP_MAGIC 0x63825363u      /* options magic cookie                          */

static const uint8_t bcast_mac[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t any_ip[4]    = { 0,0,0,0 };
static const uint8_t bcast_ip[4]  = { 255,255,255,255 };
static const uint32_t xid = 0x3903F326u;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p)    { return (uint16_t)((p[0] << 8) | p[1]); }

/* Fill the BOOTP fixed header + magic cookie for our MAC/xid; return offset 240
 * (where options start). `msgbuf` must be >= 240 bytes. */
static int dhcp_base(uint8_t *m) {
    for (int i = 0; i < 240; i++) m[i] = 0;
    m[0] = 1;                           /* op = BOOTREQUEST */
    m[1] = 1;                           /* htype = Ethernet */
    m[2] = 6;                           /* hlen */
    m[4] = (uint8_t)(xid >> 24); m[5] = (uint8_t)(xid >> 16);
    m[6] = (uint8_t)(xid >> 8);  m[7] = (uint8_t)xid;
    put16(m + 10, 0x8000);              /* flags: broadcast (we have no IP to unicast to yet) */
    const uint8_t *mac = netif_mac();
    for (int i = 0; i < 6; i++) m[28 + i] = mac[i];     /* chaddr */
    m[236] = 0x63; m[237] = 0x82; m[238] = 0x53; m[239] = 0x63;   /* magic cookie */
    return 240;
}

/* Find DHCP option `tag` in a received message; returns its data pointer + length. */
static const uint8_t *dhcp_opt(const uint8_t *m, int len, uint8_t tag, int *olen) {
    int i = 240;
    while (i + 1 < len) {
        uint8_t t = m[i];
        if (t == 255) break;            /* end */
        if (t == 0) { i++; continue; }  /* pad */
        int l = m[i + 1];
        if (t == tag) { *olen = l; return m + i + 2; }
        i += 2 + l;
    }
    return 0;
}

/* Receive a DHCP reply with our xid; return its message type (option 53) and copy
 * yiaddr + the server-id (option 54) out. 0 if none arrived in time. */
static int dhcp_recv(uint8_t yiaddr[4], uint8_t server_id[4]) {
    uint8_t r[2048];
    for (long tries = 0; tries < 6000000; tries++) {
        int n = netif_rx(r, sizeof r);
        if (n >= 14 + 20 + 8 + BOOTP_LEN && get16(r + 12) == 0x0800 && r[14 + 9] == 17) {  /* IPv4/UDP */
            int ihl = (r[14] & 0xF) * 4;
            const uint8_t *udp = r + 14 + ihl;
            if (get16(udp + 2) != 68) continue;          /* dst port 68 (DHCP client) */
            const uint8_t *m = udp + 8;
            int mlen = n - (int)(m - r);
            if (mlen < 240 || m[0] != 2) continue;       /* BOOTREPLY */
            if (!(m[4] == (uint8_t)(xid >> 24) && m[7] == (uint8_t)xid)) continue;  /* our xid */
            int ol = 0;
            const uint8_t *mt = dhcp_opt(m, mlen, 53, &ol);
            if (!mt) continue;
            for (int i = 0; i < 4; i++) yiaddr[i] = m[16 + i];
            const uint8_t *sid = dhcp_opt(m, mlen, 54, &ol);
            for (int i = 0; i < 4; i++) server_id[i] = sid ? sid[i] : 0;
            return mt[0];
        }
        if (n <= 0) for (volatile int d = 0; d < 2000; d++) { }
    }
    return 0;
}

int net_dhcp(uint8_t ip_out[4]) {
    if (!netif_present()) return -1;

    /* DISCOVER */
    uint8_t m[300];
    int o = dhcp_base(m);
    m[o++] = 53; m[o++] = 1; m[o++] = 1;                 /* DHCPDISCOVER */
    m[o++] = 55; m[o++] = 3; m[o++] = 1; m[o++] = 3; m[o++] = 6;   /* param req: mask, router, dns */
    m[o++] = 255;                                        /* end */
    if (net_udp_tx(bcast_mac, any_ip, bcast_ip, 68, 67, m, (uint32_t)o) < 0) return -1;

    uint8_t yip[4], sid[4];
    if (dhcp_recv(yip, sid) != 2) return -1;             /* expect OFFER */

    /* REQUEST the offered address */
    o = dhcp_base(m);
    m[o++] = 53; m[o++] = 1; m[o++] = 3;                 /* DHCPREQUEST */
    m[o++] = 50; m[o++] = 4; for (int i = 0; i < 4; i++) m[o++] = yip[i];   /* requested IP */
    m[o++] = 54; m[o++] = 4; for (int i = 0; i < 4; i++) m[o++] = sid[i];   /* server id   */
    m[o++] = 255;
    if (net_udp_tx(bcast_mac, any_ip, bcast_ip, 68, 67, m, (uint32_t)o) < 0) return -1;

    uint8_t aip[4], asid[4];
    if (dhcp_recv(aip, asid) != 5) return -1;            /* expect ACK */
    for (int i = 0; i < 4; i++) ip_out[i] = aip[i];
    return 0;
}
