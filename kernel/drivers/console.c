#include "console.h"
#include "cpu.h"
#include "sysfont.h"
#include "spinlock.h"

/* Blend a foreground colour over a background by 8-bit coverage (anti-aliasing
 * the system font). cov 0 -> bg, 255 -> fg. */
static inline uint32_t blend(uint32_t bg, uint32_t fg, uint8_t cov) {
    uint32_t a = cov, ia = 255 - cov;
    uint32_t r = (((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * ia) / 255;
    uint32_t g = (((fg >> 8)  & 0xff) * a + ((bg >> 8)  & 0xff) * ia) / 255;
    uint32_t b = (( fg        & 0xff) * a + ( bg        & 0xff) * ia) / 255;
    return (r << 16) | (g << 8) | b;
}

/* Serialises console output across CPUs so their lines don't interleave. */
static spinlock_t console_lock = SPINLOCK_INIT;

/* --- VGA text backend (BIOS) --------------------------------------------- */
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR 0x0f00                  /* white on black */
static volatile uint16_t *const vga = (volatile uint16_t *)0xb8000;
static int vga_pos = 0;

/* --- framebuffer backend (UEFI / GOP) ------------------------------------ */
static int fb_mode = 0;
static volatile uint32_t *fb;
static int fb_w, fb_h, fb_pitch, fb_cols, fb_rows, cx, cy;
/* The text console renders into a window (in glyph cells); by default the whole
 * screen, but the GUI confines it to a terminal window's client area via
 * console_set_window(). cx/cy are relative to the window's top-left. */
static int win_cx0, win_cy0, win_cols, win_rows;

static void serial_setup(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);                /* 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);                /* 8N1 (clears DLAB -> reg 1 is IER again) */
    outb(COM1 + 2, 0xc7);
    outb(COM1 + 4, 0x0b);                /* MCR: DTR|RTS|OUT2 (OUT2 gates the IRQ line) */
    outb(COM1 + 1, 0x01);                /* IER: received-data-available interrupt (IRQ4) -> serial input */
}

void console_init(struct boot_info *bi) {
    serial_setup();
    if (bi->console == BOOT_CONSOLE_FB) {
        fb_mode  = 1;
        fb       = (volatile uint32_t *)(FB_VBASE + (bi->fb_phys & 0x1fffff));
        fb_w     = bi->width;
        fb_h     = bi->height;
        fb_pitch = bi->pitch;
        fb_cols  = fb_w / SYSFONT_W;
        fb_rows  = fb_h / SYSFONT_H;
        win_cx0 = win_cy0 = 0;                /* console fills the screen until windowed */
        win_cols = fb_cols;
        win_rows = fb_rows;
        cx = cy  = 0;
        for (int i = 0; i < fb_h * fb_pitch; i++) fb[i] = 0;     /* clear black */
    } else {
        fb_mode = 0;
        for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) vga[i] = VGA_ATTR | ' ';
        vga_pos = 0;
    }
}

/* --- framebuffer glyph rendering (gx/gy are cells relative to the window) ---
 * Glyphs are anti-aliased: each pixel of the system font is an 8-bit coverage
 * value, blended between bg and fg. */
static void fb_glyph_c(int gx, int gy, char c, uint32_t fg, uint32_t bg) {
    if ((uint8_t)c < SYSFONT_FIRST || (uint8_t)c >= SYSFONT_FIRST + SYSFONT_COUNT) c = ' ';
    const uint8_t *g = sysfont[(uint8_t)c - SYSFONT_FIRST];
    int bx = (win_cx0 + gx) * SYSFONT_W, by = (win_cy0 + gy) * SYSFONT_H;
    for (int r = 0; r < SYSFONT_H; r++)
        for (int col = 0; col < SYSFONT_W; col++) {
            uint8_t cov = g[r * SYSFONT_W + col];
            fb[(by + r) * fb_pitch + (bx + col)] =
                cov == 0 ? bg : cov == 255 ? fg : blend(bg, fg, cov);
        }
}

static void fb_glyph(int gx, int gy, char c) {
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7f) return;
    fb_glyph_c(gx, gy, c, 0xffffffff, 0);
}

/* Scroll only the console window's rectangle up one text row, clearing the
 * newly exposed bottom row to black (so the surrounding desktop is untouched). */
static void fb_scroll(void) {
    int x0 = win_cx0 * SYSFONT_W, y0 = win_cy0 * SYSFONT_H;
    int w  = win_cols * SYSFONT_W, h = win_rows * SYSFONT_H;
    for (int r = 0; r < h - SYSFONT_H; r++)
        for (int x = 0; x < w; x++)
            fb[(y0 + r) * fb_pitch + (x0 + x)] = fb[(y0 + r + SYSFONT_H) * fb_pitch + (x0 + x)];
    for (int r = h - SYSFONT_H; r < h; r++)
        for (int x = 0; x < w; x++)
            fb[(y0 + r) * fb_pitch + (x0 + x)] = 0;
}

static void fb_putc(char c) {
    if (c == '\n')      { cx = 0; cy++; }
    else if (c == '\r') { cx = 0; }
    else if (c == '\b') { if (cx > 0) cx--; }   /* non-destructive, like VGA */
    else                { fb_glyph(cx, cy, c); if (++cx >= win_cols) { cx = 0; cy++; } }
    if (cy >= win_rows) { fb_scroll(); cy = win_rows - 1; }
}

static void vga_putc(char c) {
    if (c == '\n')      vga_pos += VGA_COLS - (vga_pos % VGA_COLS);
    else if (c == '\r') vga_pos -= (vga_pos % VGA_COLS);
    else if (c == '\b') { if (vga_pos > 0) vga_pos--; }
    else                vga[vga_pos++] = VGA_ATTR | (uint8_t)c;
    if (vga_pos >= VGA_COLS * VGA_ROWS) vga_pos = 0;
}

/* Once the compositor owns the framebuffer, kernel text would corrupt the
 * desktop, so it goes to serial only (the tests read serial, so they're fine).
 * VGA-text boots never set this. */
static int gui_mode = 0;
void console_set_gui(int on) { gui_mode = on; }

static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}
void console_serial_putc(char c) { serial_putc(c); }

static void putc_raw(char c) {
    serial_putc(c);                           /* always mirror to COM1 */
    if (fb_mode) { if (!gui_mode) fb_putc(c); }
    else         vga_putc(c);
}

static void puts_raw(const char *s) {
    for (; *s; ++s) putc_raw(*s);
}

void console_putc(char c) {
    uint64_t f = spin_lock_irqsave(&console_lock);
    putc_raw(c);
    spin_unlock_irqrestore(&console_lock, f);
}

void console_puts(const char *s) {
    uint64_t f = spin_lock_irqsave(&console_lock);
    puts_raw(s);
    spin_unlock_irqrestore(&console_lock, f);
}

void console_putdec(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    if (v == 0) { buf[i--] = '0'; }
    else while (v && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    uint64_t f = spin_lock_irqsave(&console_lock);
    puts_raw(&buf[i + 1]);
    spin_unlock_irqrestore(&console_lock, f);
}

void console_puthex(uint64_t v) {
    uint64_t f = spin_lock_irqsave(&console_lock);
    puts_raw("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        putc_raw("0123456789abcdef"[(v >> shift) & 0xf]);
    spin_unlock_irqrestore(&console_lock, f);
}

/* Paint a glyph at the *current* cursor cell without advancing it and without
 * mirroring to serial -- used by the shell to show a block cursor over the
 * character at the edit position. `inverse` swaps fg/bg (white block, black
 * glyph); painting normal restores the cell. Screen-only, so the serial stream
 * (and therefore the test harness) never sees it. */
void console_paint_cursor(char c, int inverse) {
    uint64_t f = spin_lock_irqsave(&console_lock);
    if ((uint8_t)c < 0x20) c = ' ';
    if (fb_mode) {
        uint32_t fg = inverse ? 0 : 0xffffffff;
        uint32_t bg = inverse ? 0xffffffff : 0;
        if (cx < win_cols && cy < win_rows) fb_glyph_c(cx, cy, c, fg, bg);
    } else {
        uint16_t attr = inverse ? 0x7000 : VGA_ATTR;   /* black-on-white block */
        if (vga_pos >= 0 && vga_pos < VGA_COLS * VGA_ROWS)
            vga[vga_pos] = attr | (uint8_t)c;
    }
    spin_unlock_irqrestore(&console_lock, f);
}

/* Confine the framebuffer text console to a window (pixel rect), so the GUI can
 * run the shell inside a terminal window while owning the rest of the screen.
 * w<=0 or h<=0 restores the full screen. No-op in VGA text mode (no GUI there).
 * Resets the cursor to the window's top-left. */
void console_set_window(int x, int y, int w, int h) {
    uint64_t f = spin_lock_irqsave(&console_lock);
    if (fb_mode) {
        if (w <= 0 || h <= 0) {
            win_cx0 = win_cy0 = 0;
            win_cols = fb_cols; win_rows = fb_rows;
        } else {
            win_cx0 = x / SYSFONT_W; win_cy0 = y / SYSFONT_H;
            win_cols = w / SYSFONT_W; win_rows = h / SYSFONT_H;
            if (win_cols < 1) win_cols = 1;
            if (win_rows < 1) win_rows = 1;
        }
        cx = cy = 0;
    }
    spin_unlock_irqrestore(&console_lock, f);
}
