/* virtio-blk: a DMA block device over the legacy virtio-pci transport (the QEMU
 * `if=virtio` / virtio-blk-pci transitional device). The cheapest real block
 * driver -- it replaces ATA PIO for the install target under QEMU (roadmap
 * Phase 4 #1). Polled (no IRQ): one request in flight, we spin on the used ring. */
#pragma once
#include <stdint.h>

/* Scan PCI bus 0 for a virtio-blk device and, if found, negotiate + set up its
 * request virtqueue. Prints its capacity. Safe to call when none is present
 * (it just reports "none"). */
void virtio_blk_init(void);

int  virtio_blk_present(void);     /* 1 if a device was set up */
uint64_t virtio_blk_capacity(void);/* device size in 512-byte sectors (0 if absent) */

/* Read/write `count` 512-byte sectors at LBA `lba` to/from a kernel buffer
 * (identity-mapped, i.e. phys == virt, like every kernel allocation). Returns 0
 * on success, -1 on error/no-device. */
int virtio_blk_read(uint64_t lba, uint32_t count, void *buf);
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);
