/* A tiny indirection so the IPv4/ARP/ICMP/UDP/TCP stack isn't welded to one NIC.
 * Each link driver (virtio-net, e1000, ...) fills a `struct netif` with its
 * present/mac/tx/rx and registers it at init; the stack moves frames through
 * netif_tx/netif_rx and reads the MAC through netif_mac, never naming a driver.
 * The first NIC to come up wins (kmain probes virtio-net before e1000), so a box
 * with a paravirtual NIC prefers it and one with only an e1000 uses that -- the
 * same stack either way. Frames in, frames out, a MAC: the whole contract. */
#pragma once
#include <stdint.h>

struct netif {
    const char     *name;
    int           (*present)(void);
    const uint8_t *(*mac)(void);
    int           (*tx)(const void *frame, uint32_t len);
    int           (*rx)(void *buf, uint32_t max);
};

void           netif_register(const struct netif *nif);  /* first brought-up NIC wins */
int            netif_present(void);
const char    *netif_name(void);
const uint8_t *netif_mac(void);
int            netif_tx(const void *frame, uint32_t len);
int            netif_rx(void *buf, uint32_t max);
