/* AHCI/SATA block driver (MMIO ABAR, DMA via command list + PRDT). Probes the
 * HBA on the PCI bus, brings up the first port with a SATA disk, and registers it
 * with the block-device layer as "ahci0" so the fs/installer can use it like any
 * other disk. Polled (no IRQ), one command in flight. */
#pragma once
#include <stdint.h>

void     ahci_init(void);                                   /* probe + register "ahci0" if present */
int      ahci_present(void);
uint64_t ahci_capacity(void);                               /* sectors on the bound port (0 if none) */
int      ahci_read(uint64_t lba, uint32_t count, void *buf);
int      ahci_write(uint64_t lba, uint32_t count, const void *buf);
