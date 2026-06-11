#include "pci.h"
#include "console.h"
#include "cpu.h"
#include <stdint.h>

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

/* Read a 32-bit dword from a device's configuration space. */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

/* Write a 32-bit dword into a device's configuration space (e.g. the command
 * register to enable I/O space + bus-master DMA for a driver). */
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, val);
}

static void hex4(uint16_t v) {           /* 4 hex digits, no 0x prefix */
    for (int s = 12; s >= 0; s -= 4)
        console_putc("0123456789abcdef"[(v >> s) & 0xF]);
}

/* Brute-force scan of bus 0 (enough for QEMU's i440fx machine): print each
 * present function as bus:slot.func vendor:device class.subclass. */
void pci_list(void) {
    for (int slot = 0; slot < 32; slot++) {
        for (int func = 0; func < 8; func++) {
            uint32_t id = pci_read32(0, (uint8_t)slot, (uint8_t)func, 0x00);
            uint16_t vendor = id & 0xFFFF;
            if (vendor == 0xFFFF) { if (func == 0) break; else continue; }
            uint16_t device = id >> 16;
            uint32_t cls    = pci_read32(0, (uint8_t)slot, (uint8_t)func, 0x08);
            uint8_t  class  = (cls >> 24) & 0xFF;
            uint8_t  subcls = (cls >> 16) & 0xFF;

            console_puts("00:");
            console_putc("0123456789abcdef"[(slot >> 4) & 0xF]);
            console_putc("0123456789abcdef"[slot & 0xF]);
            console_putc('.');
            console_putc("01234567"[func & 7]);
            console_puts("  ");
            hex4(vendor); console_putc(':'); hex4(device);
            console_puts("  class ");
            hex4(class);  console_putc('.'); hex4(subcls);
            console_puts("\r\n");

            if (func == 0) {                      /* multi-function? bit 7 of header type */
                uint32_t hdr = pci_read32(0, (uint8_t)slot, 0, 0x0C);
                if (!((hdr >> 16) & 0x80)) break; /* single-function device */
            }
        }
    }
}
