/* A minimal single-connection TCP client (active open) over net.c's IPv4 layer:
 * 3-way handshake, push/recv with the mandatory pseudo-header checksum, and a FIN
 * close. No retransmission (the SLIRP/local link is reliable), no options, fixed
 * window, one connection at a time -- enough to fetch over the network and to prove
 * TCP works end-to-end. A sockets syscall layer + multi-connection state is next. */
#include "net.h"
#include "netif.h"
#include "console.h"

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

static uint8_t  t_dip[4];
static uint16_t t_sport, t_dport;
static uint32_t t_snd;          /* our next sequence number to send */
static uint32_t t_rcv;          /* next sequence number we expect from the peer */
static int      t_open = 0;
static int      t_peer_fin = 0; /* peer sent FIN (no more data is coming) */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static int ip_eq(const uint8_t *a, const uint8_t *b) { return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]; }

/* TCP checksum over the IPv4 pseudo-header + the segment. */
static uint16_t tcp_csum(const uint8_t *sip, const uint8_t *dip, const uint8_t *seg, int seglen) {
    uint32_t s = 0;
    s += get16(sip); s += get16(sip + 2);
    s += get16(dip); s += get16(dip + 2);
    s += 6;                                          /* protocol */
    s += (uint32_t)seglen;                           /* TCP length */
    for (int i = 0; i + 1 < seglen; i += 2) s += (uint32_t)((seg[i] << 8) | seg[i + 1]);
    if (seglen & 1) s += (uint32_t)(seg[seglen - 1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static int tcp_send_seg(uint8_t flags, const uint8_t *data, int dlen) {
    uint8_t seg[20 + 1400];
    put16(seg + 0, t_sport); put16(seg + 2, t_dport);
    put32(seg + 4, t_snd);   put32(seg + 8, t_rcv);
    seg[12] = 0x50;          seg[13] = flags;        /* data offset = 5 dwords */
    put16(seg + 14, 8192);                           /* window */
    put16(seg + 16, 0);                              /* checksum (filled below) */
    put16(seg + 18, 0);                              /* urgent pointer */
    for (int i = 0; i < dlen; i++) seg[20 + i] = data[i];
    put16(seg + 16, tcp_csum(net_my_ip(), t_dip, seg, 20 + dlen));
    return net_ip_tx(6, t_dip, seg, (uint32_t)(20 + dlen));
}

/* Poll (briefly) for one TCP segment on our connection. Returns 1 + fills the
 * out-params, or 0 if nothing arrived within the budget. Kept short so callers that
 * loop (recv waiting for more data / the FIN) spin quickly rather than blocking for
 * seconds per call; a SLIRP/local round-trip lands well inside this. */
static int tcp_recv_seg(uint8_t *flags, uint32_t *seq, uint32_t *ack,
                        const uint8_t **data, int *dlen, uint8_t *r, int rmax) {
    for (long tries = 0; tries < 600000; tries++) {
        int n = netif_rx(r, (uint32_t)rmax);
        if (n >= 14 + 20 + 20 && get16(r + 12) == 0x0800 && r[14 + 9] == 6) {
            const uint8_t *ip = r + 14;
            int ihl = (ip[0] & 0xF) * 4;
            const uint8_t *tcp = ip + ihl;
            if (!ip_eq(ip + 12, t_dip)) continue;
            if (get16(tcp + 0) != t_dport || get16(tcp + 2) != t_sport) continue;
            int thl = ((tcp[12] >> 4) & 0xF) * 4;
            int payload = (int)get16(ip + 2) - ihl - thl;
            *flags = tcp[13]; *seq = get32(tcp + 4); *ack = get32(tcp + 8);
            *data = tcp + thl; *dlen = payload > 0 ? payload : 0;
            return 1;
        }
        if (n <= 0) for (volatile int d = 0; d < 2000; d++) { }
    }
    return 0;
}

int net_tcp_connect(const uint8_t dip[4], uint16_t dport) {
    if (!netif_present()) return -1;
    for (int i = 0; i < 4; i++) t_dip[i] = dip[i];
    t_dport = dport; t_sport = 0xC000;
    t_snd = 0x12340000u; t_rcv = 0; t_open = 0; t_peer_fin = 0;

    if (tcp_send_seg(F_SYN, 0, 0) < 0) return -1;
    t_snd++;                                         /* SYN consumes one sequence number */

    uint8_t r[2048], fl; uint32_t seq, ack; const uint8_t *d; int dl;
    for (int tries = 0; ; tries++) {                 /* wait for the SYN-ACK (a few short polls) */
        if (tcp_recv_seg(&fl, &seq, &ack, &d, &dl, r, sizeof r)) {
            if ((fl & (F_SYN | F_ACK)) == (F_SYN | F_ACK)) break;   /* got it */
            if (fl & F_RST) return -1;               /* refused */
        }
        if (tries >= 12) return -1;                  /* gave up */
    }
    t_rcv = seq + 1;                                 /* peer ISN + 1 */
    if (tcp_send_seg(F_ACK, 0, 0) < 0) return -1;
    t_open = 1;
    return 0;
}

/* --- passive open (a one-connection server) -------------------------------
 * Reuses the same single-connection TCB as the active path: listen records the
 * local port, accept blocks polling for a SYN to it, replies SYN-ACK, and marks
 * the connection established (the client's bare ACK + the request data are then
 * read by net_tcp_recv, which ACKs them). One connection at a time -- enough to
 * answer an HTTP request and prove tOS can *serve*, not just fetch. */
static uint16_t t_listen_port = 0;
static int      t_listening = 0;

int net_tcp_listen(uint16_t port) {
    if (!netif_present()) return -1;
    t_listen_port = port; t_listening = 1;
    t_open = 0; t_peer_fin = 0;
    return 0;
}

int net_tcp_accept(void) {
    if (!t_listening) return -1;
    uint8_t r[2048];
    for (long tries = 0; tries < 30000000; tries++) {     /* wait (bounded) for a client */
        int n = netif_rx(r, sizeof r);
        if (n >= 14 + 20 + 20 && get16(r + 12) == 0x0800 && r[14 + 9] == 6) {
            const uint8_t *ip = r + 14;
            int ihl = (ip[0] & 0xF) * 4;
            const uint8_t *tcp = ip + ihl;
            if (get16(tcp + 2) == t_listen_port && (tcp[13] & F_SYN) && !(tcp[13] & F_ACK)) {
                for (int i = 0; i < 4; i++) t_dip[i] = ip[12 + i];   /* peer = the SYN's source */
                t_dport = get16(tcp + 0);
                t_sport = t_listen_port;
                t_rcv = get32(tcp + 4) + 1;                          /* peer ISN + 1 */
                t_snd = 0x12340000u;
                if (tcp_send_seg(F_SYN | F_ACK, 0, 0) < 0) return -1;
                t_snd++;                                            /* SYN-ACK consumes a seq */
                t_open = 1; t_listening = 0;
                return 0;
            }
        }
        if (n <= 0) for (volatile int d = 0; d < 2000; d++) { }
    }
    t_listening = 0;
    return -1;                                            /* no client connected in time */
}

int net_tcp_send(const uint8_t *data, uint32_t len) {
    if (!t_open) return -1;
    if (tcp_send_seg(F_PSH | F_ACK, data, (int)len) < 0) return -1;
    t_snd += len;
    return 0;
}

int net_tcp_recv(uint8_t *buf, uint32_t max) {
    if (!t_open) return -1;
    if (t_peer_fin) return -1;                       /* peer already closed; no more data */
    uint8_t r[2048], fl; uint32_t seq, ack; const uint8_t *d; int dl;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (!tcp_recv_seg(&fl, &seq, &ack, &d, &dl, r, sizeof r)) return 0;   /* nothing yet */
        if (fl & F_RST) { t_open = 0; return -1; }
        if (dl > 0 && seq == t_rcv) {                /* in-order data */
            int cp = dl > (int)max ? (int)max : dl;
            for (int i = 0; i < cp; i++) buf[i] = d[i];
            t_rcv += (uint32_t)dl;
            if (fl & F_FIN) { t_rcv++; t_peer_fin = 1; }   /* data + FIN in one segment */
            tcp_send_seg(F_ACK, 0, 0);               /* acknowledge data (and FIN, if any) */
            return cp;
        }
        if ((fl & F_FIN) && seq == t_rcv) {          /* clean close, no more data */
            t_rcv++; t_peer_fin = 1; tcp_send_seg(F_ACK, 0, 0);
            return -1;
        }
    }
    return 0;
}

void net_tcp_close(void) {
    if (!t_open) return;
    tcp_send_seg(F_FIN | F_ACK, 0, 0);
    t_snd++;
    uint8_t r[2048], fl; uint32_t seq, ack; const uint8_t *d; int dl;
    for (int i = 0; i < 4; i++) {
        if (!tcp_recv_seg(&fl, &seq, &ack, &d, &dl, r, sizeof r)) break;
        if (fl & F_FIN) { t_rcv = seq + 1; tcp_send_seg(F_ACK, 0, 0); break; }
    }
    t_open = 0;
}
