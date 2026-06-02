/* PS/2 keyboard driver (scancode set 1), interrupt-driven on IRQ1.
 *
 * A full keyboard: the main block, both Shift/Ctrl/Alt, Caps Lock and Num Lock
 * (with the LEDs), the function keys, the navigation cluster, and the keypad.
 * Output is a byte stream in the style of a real terminal:
 *   - printable keys      -> their ASCII (Shift/Caps applied)
 *   - Ctrl+letter         -> the C0 control code (^A=1 .. ^Z=26)
 *   - arrows / nav / F-keys -> ANSI escape sequences (ESC [ ... / ESC O ...)
 * The bytes go into a ring buffer that SYS_READ (or the compositor's SYS_GETKEY)
 * drains; a blocked reader is woken once per interrupt. The 8259 PIC is remapped
 * so hardware IRQs land at vectors 0x20+.
 */
#include "keyboard.h"
#include "syscall.h"
#include "sched.h"
#include "cpu.h"
#include <stdint.h>

/* --- scancode set 1 (make codes) -> ASCII, US layout --------------------- */
static const char base[128] = {
    [0x01]=27,  [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
    [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',
    [0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',
    [0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',[0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',
    [0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' ',
};
static const char shifted[128] = {
    [0x01]=27,  [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
    [0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',[0x0C]='_',[0x0D]='+',[0x0E]='\b',[0x0F]='\t',
    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',[0x16]='U',
    [0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',[0x24]='J',
    [0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',[0x2B]='|',
    [0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',
    [0x33]='<',[0x34]='>',[0x35]='?',[0x39]=' ',
};
/* keypad digits/operators when Num Lock is on (scancodes 0x47..0x53, 0x37) */
static const char keypad[128] = {
    [0x37]='*',[0x47]='7',[0x48]='8',[0x49]='9',[0x4A]='-',[0x4B]='4',[0x4C]='5',
    [0x4D]='6',[0x4E]='+',[0x4F]='1',[0x50]='2',[0x51]='3',[0x52]='0',[0x53]='.',
};

/* --- input ring buffer (written by the IRQ, drained by SYS_READ/GETKEY) --- */
#define KBUF 512
static volatile char kbuf[KBUF];
static volatile int khead = 0, ktail = 0;

/* modifier state */
static int lshift, rshift, lctrl, rctrl, lalt, ralt, lgui, rgui;
static int caps, num = 1, scroll;        /* Num Lock defaults on, like a PC */
static int gui_armed;                     /* Super pressed with no other key yet -> a lone tap */
static int extended = 0;                  /* saw an 0xE0 prefix */
static int pushed = 0;                    /* anything queued this IRQ? */

static void buf_push(char c) {
    int n = (khead + 1) % KBUF;
    if (n != ktail) { kbuf[khead] = c; khead = n; pushed = 1; }
}
static void emit(char c)            { buf_push(c); }
static void emit_str(const char *s) { for (; *s; s++) buf_push(*s); }

int kbd_getc(void) {
    if (khead == ktail) return -1;
    char c = kbuf[ktail];
    ktail = (ktail + 1) % KBUF;
    return (int)(uint8_t)c;
}

static int shift(void) { return lshift || rshift; }
static int ctrl(void)  { return lctrl  || rctrl;  }
static int alt(void)   { return lalt   || ralt;   }
static int gui(void)   { return lgui   || rgui;   }   /* Super / Windows key */

/* The live modifier bitmask (KMOD_*), surfaced to the compositor so apps can see
 * which modifiers are held -- the foundation for shift-select, chord routing, and
 * the Alt-Tab overlay. */
unsigned kbd_mods(void) {
    return (shift() ? KMOD_SHIFT : 0) | (ctrl() ? KMOD_CTRL : 0) |
           (alt()   ? KMOD_ALT   : 0) | (gui()  ? KMOD_SUPER : 0);
}
/* xterm modifier parameter for a nav-key CSI: 1 + Shift(1) + Alt(2) + Ctrl(4),
 * so 1 = no modifiers, 5 = Ctrl, 2 = Shift, 6 = Ctrl+Shift (matches the toolkit's
 * CSI decoder, which reads (param-1) as a bitmask). */
static int xterm_mod(void) {
    return 1 + (shift() ? 1 : 0) + (alt() ? 2 : 0) + (ctrl() ? 4 : 0);
}
/* Emit a cursor/nav CSI, adding the xterm modifier param when a modifier is held.
 * `lead` is the numeric prefix kept even when unmodified ("" for cursor/Home/End,
 * which gain an implicit "1" only once modified; "3" for Delete's ESC[3~). */
static void emit_nav(const char *lead, char final) {
    int m = xterm_mod();
    char seq[16]; int i = 0;
    seq[i++] = 0x1b; seq[i++] = '[';
    if (m > 1) {
        const char *l = lead[0] ? lead : "1";
        for (const char *p = l; *p; p++) seq[i++] = *p;
        seq[i++] = ';'; seq[i++] = (char)('0' + m);
    } else {
        for (const char *p = lead; *p; p++) seq[i++] = *p;
    }
    seq[i++] = final; seq[i] = 0;
    emit_str(seq);
}

/* PgUp/PgDn: plain keys forward the xterm page CSI to the app (the shell), but
 * Shift+PgUp/PgDn is an app-level intent the terminal emulator consumes to page
 * its own scrollback (like xterm) -- so emit a distinct byte the term intercepts. */
static void key_page(int up) {
    if (shift()) { emit((char)(up ? KEY_TERM_PGUP : KEY_TERM_PGDN)); return; }
    emit_str(up ? "\x1b[5~" : "\x1b[6~");
}

/* A main-block key was pressed: translate with the current modifiers. */
static void key_main(uint8_t sc) {
    char c = base[sc];
    if (!c) return;
    char raw = c;                                                  /* unshifted key, for chord matching */

    /* Global compositor chords (Super + key). Super+Tab is intentionally NOT here:
     * it is reserved for future desktop switching and falls through to a Tab. */
    if (gui()) {
        if (raw == 'q') { emit((char)(shift() ? KEY_SUPER_KILL : KEY_SUPER_Q)); return; }
        if (raw == 'v') { emit((char)KEY_SUPER_V);     return; }
        if (raw == ' ') { emit((char)KEY_SUPER_SPACE); return; }
    }
    if (alt() && raw == '\t') { emit((char)KEY_ALT_TAB); return; }  /* MRU window switcher */

    /* App-level clipboard edit chords, delivered to the focused window. */
    if (ctrl() && shift()) {
        if (raw == 'c') { emit((char)KEY_TERM_COPY);  return; }
        if (raw == 'x') { emit((char)KEY_TERM_CUT);   return; }
        if (raw == 'v') { emit((char)KEY_TERM_PASTE); return; }
    }

    if (ctrl() && c == '\b') { emit(0x17); return; }               /* Ctrl+Backspace: delete previous word (^W) */

    if (c >= 'a' && c <= 'z') {                                     /* letter: Shift XOR Caps */
        if (shift() ^ caps) c -= 32;
    } else if (shift() && shifted[sc]) {                            /* symbol: Shift only      */
        c = shifted[sc];
    }
    if (ctrl()) {
        if      (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);     /* ^A..^Z = 1..26 */
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else if (c == '[') c = 27; else if (c == '\\') c = 28;
        else if (c == ']') c = 29; else if (c == '`' ) c = 0;       /* ^@ */
        else if (c == ' ') c = 0;
        else if (c == '/') c = 31;
        /* other Ctrl combos: pass the char through unchanged */
    }
    emit(c);
}

static void key_function(uint8_t sc) {
    switch (sc) {                               /* xterm-style sequences */
    case 0x3B: emit_str("\x1b" "OP"); break;    /* F1  */
    case 0x3C: emit_str("\x1b" "OQ"); break;    /* F2  */
    case 0x3D: emit_str("\x1b" "OR"); break;    /* F3  */
    case 0x3E: emit_str("\x1b" "OS"); break;    /* F4  */
    case 0x3F: emit_str("\x1b[15~"); break;     /* F5  */
    case 0x40: emit_str("\x1b[17~"); break;     /* F6  */
    case 0x41: emit_str("\x1b[18~"); break;     /* F7  */
    case 0x42: emit_str("\x1b[19~"); break;     /* F8  */
    case 0x43: emit_str("\x1b[20~"); break;     /* F9  */
    case 0x44: emit_str("\x1b[21~"); break;     /* F10 */
    case 0x57: emit_str("\x1b[23~"); break;     /* F11 */
    case 0x58: emit_str("\x1b[24~"); break;     /* F12 */
    }
}

static void handle_normal(uint8_t code, int release) {
    if (!release) gui_armed = 0;             /* any normal key press cancels a lone-Super tap */
    switch (code) {
    case 0x2A: lshift = !release; return;
    case 0x36: rshift = !release; return;
    case 0x1D: lctrl  = !release; return;
    case 0x38: lalt   = !release; return;
    case 0x3A: if (!release) caps   = !caps;   return;   /* Caps Lock (LED is cosmetic, skipped) */
    case 0x45: if (!release) num    = !num;    return;   /* Num Lock  */
    case 0x46: if (!release) scroll = !scroll; return;   /* Scroll Lock */
    }
    if (release) return;

    if ((code >= 0x3B && code <= 0x44) || code == 0x57 || code == 0x58) { key_function(code); return; }

    if (keypad[code]) {                          /* keypad block */
        if (num) { emit(keypad[code]); return; }
        switch (code) {                          /* Num Lock off -> navigation */
        case 0x47: emit_str("\x1b[H"); return;   case 0x4F: emit_str("\x1b[F"); return;
        case 0x48: emit_str("\x1b[A"); return;   case 0x50: emit_str("\x1b[B"); return;
        case 0x4B: emit_str("\x1b[D"); return;   case 0x4D: emit_str("\x1b[C"); return;
        case 0x49: emit_str("\x1b[5~"); return;  case 0x51: emit_str("\x1b[6~"); return;
        case 0x52: emit_str("\x1b[2~"); return;  case 0x53: emit_str("\x1b[3~"); return;
        default: emit(keypad[code]); return;     /* '*','-','+' have no nav meaning */
        }
    }
    key_main(code);
}

/* A lone Super tap (press + release with no key in between) opens the Launchpad. */
static void gui_changed(int press) {
    if (press) gui_armed = 1;
    else { if (gui_armed) emit((char)KEY_LAUNCHPAD); gui_armed = 0; }
}
static void handle_extended(uint8_t code, int release) {
    switch (code) {
    case 0x1D: rctrl = !release; if (!release) gui_armed = 0; return;   /* right Ctrl */
    case 0x38: ralt  = !release; if (!release) gui_armed = 0; return;   /* right Alt  */
    case 0x5B: lgui  = !release; gui_changed(!release); return;         /* left  Super / Windows key */
    case 0x5C: rgui  = !release; gui_changed(!release); return;         /* right Super */
    }
    if (release) return;
    gui_armed = 0;                               /* any other extended key press cancels the tap */
    switch (code) {
    case 0x48: emit_nav("", 'A'); break;         /* Up    (+Shift: extend selection) */
    case 0x50: emit_nav("", 'B'); break;         /* Down  (+Shift: extend selection) */
    case 0x4D: emit_nav("", 'C'); break;         /* Right (Ctrl: word jump; Shift: extend) */
    case 0x4B: emit_nav("", 'D'); break;         /* Left  (Ctrl: word jump; Shift: extend) */
    case 0x47: emit_nav("", 'H'); break;         /* Home  (+Shift: extend selection) */
    case 0x4F: emit_nav("", 'F'); break;         /* End   (+Shift: extend selection) */
    case 0x49: key_page(1); break;               /* PgUp  (+Shift: term scrollback) */
    case 0x51: key_page(0); break;               /* PgDn  (+Shift: term scrollback) */
    case 0x52: emit_str("\x1b[2~"); break;       /* Insert */
    case 0x53: emit_nav("3", '~'); break;        /* Delete (Ctrl: delete word) */
    case 0x1C: emit('\n'); break;                /* keypad Enter */
    case 0x35: emit('/'); break;                 /* keypad /     */
    }
    (void)ralt;
}

void keyboard_irq(void) {
    uint8_t sc = inb(0x60);
    outb(0x20, 0x20);                            /* EOI to master PIC */
    pushed = 0;

    if (sc == 0xE0) { extended = 1; return; }    /* prefix: nav cluster, right modifiers */
    if (sc == 0xE1) { return; }                  /* Pause/Break prefix: ignore the burst */

    int release = sc & 0x80;
    uint8_t code = sc & 0x7f;
    if (extended) { extended = 0; handle_extended(code, release); }
    else          handle_normal(code, release);

    if (pushed) sched_wake_readers();            /* a key (or sequence) arrived */
}

static void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);   /* ICW1: begin init, expect ICW4 */
    outb(0x21, 0x20); outb(0xA1, 0x28);   /* ICW2: master->0x20, slave->0x28 */
    outb(0x21, 0x04); outb(0xA1, 0x02);   /* ICW3: cascade wiring */
    outb(0x21, 0x01); outb(0xA1, 0x01);   /* ICW4: 8086 mode */
    outb(0x21, 0xFC); outb(0xA1, 0xFF);   /* unmask IRQ0 (timer) + IRQ1 (kbd) */
}

void kbd_init(void) {
    pic_remap();
    while (inb(0x64) & 1) inb(0x60);       /* drain any pending byte */
}
