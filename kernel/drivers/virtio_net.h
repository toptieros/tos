/* virtio-net (legacy virtio-pci) — the first network driver. Brings up the device,
 * reads its MAC, and moves raw Ethernet frames in/out over a transmit + receive
 * virtqueue. Polled (no IRQ yet). The protocol stack (ARP/IP/...) lives above this;
 * see design/virtio-net.md. Frames in, frames out, plus a MAC -- that's the whole
 * driver contract. */
#pragma once
#include <stdint.h>

void virtio_net_init(void);                 /* probe + bring up the NIC if present */
int  virtio_net_present(void);
const uint8_t *virtio_net_mac(void);        /* 6-byte MAC, valid once present */

/* Transmit one Ethernet frame (no virtio_net_hdr -- the driver prepends it).
 * Returns 0 on success, -1 if absent/failed. */
int  virtio_net_tx(const void *frame, uint32_t len);

/* Poll for one received Ethernet frame. Copies up to `max` bytes into `buf` and
 * returns the frame length, 0 if nothing is pending, -1 if absent. */
int  virtio_net_rx(void *buf, uint32_t max);
