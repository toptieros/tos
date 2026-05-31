/* Text output to either the VGA text buffer (BIOS) or a GOP framebuffer
 * (UEFI), selected from the boot_info, plus a COM1 serial mirror. */
#pragma once
#include <stdint.h>
#include "bootinfo.h"

void console_init(struct boot_info *bi);
void console_putc(char c);
void console_puts(const char *s);
void console_putdec(uint64_t v);
void console_puthex(uint64_t v);
void console_paint_cursor(char c, int inverse);
void console_set_window(int x, int y, int w, int h);
void console_serial_putc(char c);                /* serial-only output (pty mirror) */
void console_set_gui(int on);                    /* compositor owns the FB: suppress fb text */
