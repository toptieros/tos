/* NVMe block driver (MMIO controller registers, admin + one I/O queue pair, DMA
 * via PRPs). Probes the controller on the PCI bus, sets up the queues, IDENTIFYs
 * namespace 1, and registers it with the block-device layer as "nvme0". Polled
 * (no IRQ), one command in flight per queue. Supports 512-byte-LBA namespaces
 * (the QEMU `-device nvme` default); other LBA sizes are reported and skipped. */
#pragma once
#include <stdint.h>

void     nvme_init(void);                                   /* probe + register "nvme0" if present */
int      nvme_present(void);
uint64_t nvme_capacity(void);                               /* 512-byte sectors (0 if none) */
int      nvme_read(uint64_t lba, uint32_t count, void *buf);
int      nvme_write(uint64_t lba, uint32_t count, const void *buf);
