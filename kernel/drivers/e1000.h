/* Intel 8254x ("e1000" / 82540EM) gigabit NIC -- the second network driver, behind
 * the same netif contract as virtio-net. Memory-mapped register block (BAR0), legacy
 * RX/TX descriptor rings in identity-mapped DMA memory, MAC from RAL/RAH (EEPROM
 * fallback). Polled, no IRQ. The protocol stack is NIC-agnostic (see net/netif.h);
 * a box with only an e1000 leases/pings/fetches through the very same code path as
 * one with virtio-net. See design/virtio-net.md. */
#pragma once
#include <stdint.h>

void e1000_init(void);                      /* probe + bring up an 8254x if present */
int  e1000_present(void);
const uint8_t *e1000_mac(void);             /* 6-byte MAC, valid once present */
int  e1000_tx(const void *frame, uint32_t len);   /* one Ethernet frame; 0 / -1 */
int  e1000_rx(void *buf, uint32_t max);           /* one frame, or 0 if none, -1 absent */
