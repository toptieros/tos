/* Minimal ACPI: find the RSDP, walk the RSDT/XSDT, parse the MADT (CPU/APIC
 * topology) and the FADT (PM1a control block + _S5 sleep type for poweroff, and
 * the reset register). No AML interpreter -- just table walking plus the standard
 * tiny _S5 byte scan. Gives real-hardware-portable CPU discovery + shutdown/reset,
 * with the old QEMU magic ports kept as a fallback. See design/roadmap.md Phase 4. */
#pragma once
#include <stdint.h>

void acpi_init(uint64_t rsdp_phys);         /* scan tables; call after vmm_init, in kmain.
                                             * rsdp_phys: RSDP from the UEFI handoff, or 0 to
                                             * fall back to the legacy BIOS-area scan */
int  acpi_cpu_apic_ids(uint8_t *ids, int max);  /* fill APIC ids from the MADT; count, 0 if none */
int  acpi_poweroff(void);                   /* issue an ACPI S5 poweroff; -1 if unavailable */
int  acpi_reset(void);                      /* issue an ACPI reset; -1 if unavailable */
