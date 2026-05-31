/* PCI configuration-space access (legacy 0xCF8/0xCFC ports) + a bus scan. A
 * foundation for real device drivers (storage, NIC, ...) later on. */
#pragma once
#include <stdint.h>

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_list(void);     /* enumerate the bus, printing each device to the console */
