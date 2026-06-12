/* Handoff structure the boot front-end passes to the kernel (in RDI).
 * BIOS passes NULL (defaults to VGA text); the UEFI loader fills in the GOP
 * framebuffer. Must stay byte-compatible between uefi/uefi.c and the kernel. */
#pragma once
#include <stdint.h>

#define BOOT_CONSOLE_VGA 0
#define BOOT_CONSOLE_FB  1

/* Higher-half window the kernel maps the framebuffer into (shared by every
 * address space, so console output survives per-process CR3 switches). */
#define FB_VBASE 0xFFFFFFFFC0000000ULL

struct boot_info {
    uint32_t console;     /* BOOT_CONSOLE_VGA | BOOT_CONSOLE_FB */
    uint32_t width;       /* pixels */
    uint32_t height;      /* pixels */
    uint32_t pitch;       /* pixels per scanline */
    uint32_t _pad;
    uint64_t fb_phys;     /* framebuffer physical base */
    uint64_t acpi_rsdp;   /* RSDP physical addr from the UEFI ACPI config table; 0 if unknown (BIOS) */
};
