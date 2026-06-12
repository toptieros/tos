/* The active network interface (see netif.h). A driver calls netif_register once
 * it has actually brought a device up; the first such registration wins and every
 * stack call routes through it. With no NIC, present() is false and tx/rx fail
 * cleanly (-1), so net_init simply skips -- exactly the old virtio-only behaviour. */
#include "netif.h"

static const struct netif *active = 0;

void netif_register(const struct netif *nif) {
    if (!active && nif && nif->present && nif->present()) active = nif;
}

int netif_present(void) { return active && active->present(); }
const char *netif_name(void) { return active ? active->name : "none"; }

const uint8_t *netif_mac(void) {
    static const uint8_t zero[6] = { 0,0,0,0,0,0 };
    return active ? active->mac() : zero;
}

int netif_tx(const void *frame, uint32_t len) { return active ? active->tx(frame, len) : -1; }
int netif_rx(void *buf, uint32_t max)         { return active ? active->rx(buf, max)   : -1; }
