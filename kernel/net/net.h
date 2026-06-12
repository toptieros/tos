/* The bottom of the TCP/IP stack, built on the virtio-net driver: an ARP resolver
 * + cache, IPv4 framing, and ICMP echo (ping). This is the native minimal stack
 * sketched in design/virtio-net.md (ARP -> IPv4 -> ICMP -> ... ); UDP/DHCP/TCP and a
 * sockets syscall layer build on these primitives later. Polled, single-homed.
 *
 * Static config for now (QEMU SLIRP defaults): guest 10.0.2.15, gateway 10.0.2.2.
 * DHCP will replace the static address. */
#pragma once
#include <stdint.h>

void net_init(void);                                /* bring up the stack; call after virtio_net_init */
int  net_arp_resolve(const uint8_t ip[4], uint8_t mac_out[6]);   /* 0 + MAC, or -1 */
int  net_ping(const uint8_t ip[4]);                 /* one ICMP echo round-trip; 0 if a reply came back */

/* Send one UDP datagram (builds Ethernet + IPv4 + UDP; checksum 0, valid for IPv4). */
int  net_udp_tx(const uint8_t dmac[6], const uint8_t sip[4], const uint8_t dip[4],
                uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len);

/* Send one IPv4 packet (protocol `proto`) to `dip` via the gateway; the L4 payload
 * must already be built + checksummed by the caller. Shared by TCP. */
int  net_ip_tx(uint8_t proto, const uint8_t dip[4], const uint8_t *payload, uint32_t len);

const uint8_t *net_my_ip(void);                     /* our current IPv4 address */
void           net_set_ip(const uint8_t ip[4]);     /* set it (e.g. from a DHCP lease) */

/* DHCP client (DISCOVER/OFFER/REQUEST/ACK) — leases an address; 0 + IP, or -1. */
int  net_dhcp(uint8_t ip_out[4]);

/* Minimal single-connection TCP client (active open, no retransmit/options). */
int  net_tcp_connect(const uint8_t dip[4], uint16_t dport);   /* 0 once established */
int  net_tcp_send(const uint8_t *data, uint32_t len);         /* 0 on send */
int  net_tcp_recv(uint8_t *buf, uint32_t max);                /* bytes copied, 0 none, -1 reset */
void net_tcp_close(void);
