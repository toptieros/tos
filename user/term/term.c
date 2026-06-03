/* term -- the tOS terminal emulator. An ordinary app: it asks twm for a window
 * (a shared-memory pixel surface), runs the shell as a piped child over a kernel
 * pty, and is the thing that actually knows what a terminal is. It maintains a
 * character grid, parses the shell's byte stream (newlines, backspace, tabs and
 * ANSI CSI/SGR escapes for colour), renders the grid into its surface, and
 * forwards the keystrokes twm routes to it back to the shell. Because term owns
 * the grid, twm can move/restack the window freely without losing any text. */
#include "ulib.h"
#include "ugfx.h"

#define MAXCOLS 220
#define MAXROWS 70

/* 16-colour ANSI palette (xRGB). */
static const uint32_t pal[16] = {
    RGB(28, 32, 42),  RGB(224, 96, 96),  RGB(126, 200, 126), RGB(222, 198, 110),
    RGB(98, 150, 222), RGB(200, 130, 200),RGB(110, 200, 212), RGB(206, 212, 222),
    RGB(110,120,138),  RGB(240,130,130),  RGB(150,224,150),   RGB(238,220,140),
    RGB(130,176,240),  RGB(220,160,220),  RGB(150,224,234),   RGB(238,242,248),
};
#define DEF_FG 7
#define DEF_BG 0
#define CUR_COL RGB(158, 206, 252)       /* I-beam cursor bar colour (soft accent blue) */

static uint32_t *surf;
static int sw, sh, fw, fh, cols, rows;
static int win;

/* grid + shadow (what is currently drawn), each cell = char/fg/bg */
static uint8_t gch[MAXROWS * MAXCOLS], gfg[MAXROWS * MAXCOLS], gbg[MAXROWS * MAXCOLS];
static uint8_t sch[MAXROWS * MAXCOLS], sfg[MAXROWS * MAXCOLS], sbg[MAXROWS * MAXCOLS];
static uint8_t ssel[MAXROWS * MAXCOLS];  /* shadow: was this cell drawn selected last render? */

/* Scrollback: rows that scrolled off the top are kept in a ring so the wheel can
 * page back through history. view_off = how many rows we've scrolled back from the
 * live bottom (0 = following the live grid). */
#define SBROWS 256
static uint8_t sbch[SBROWS * MAXCOLS], sbfg[SBROWS * MAXCOLS], sbbg[SBROWS * MAXCOLS];
static int sb_count, sb_head;            /* rows stored (<=SBROWS); head = oldest slot */
static int view_off;                     /* rows scrolled back from the live bottom (0 = live) */

static int cx, cy;                       /* cursor cell */
static int cur_fg = DEF_FG, cur_bg = DEF_BG, bold;
static int dirty;
static int blink_on = 1, blink_ctr;      /* the cursor blinks; solid while typing */

/* Mouse text selection (click-drag): an anchor cell and the current end cell, in
 * reading order (linear cell index). Copied to the clipboard on Ctrl+Shift+C. */
#define SEL_BG RGB(48, 84, 140)          /* highlight behind selected cells */
static int sel_on, sel_a, sel_b;         /* active flag + anchor/end linear cell indices */
static void clear_sel(void) { if (sel_on) { sel_on = 0; dirty = 1; } }
static int  in_sel(int r, int c) {
    if (!sel_on) return 0;
    int ci = r * cols + c, lo = sel_a < sel_b ? sel_a : sel_b, hi = sel_a < sel_b ? sel_b : sel_a;
    return ci >= lo && ci <= hi;
}

/* escape-sequence parser state */
static enum { S_NORMAL, S_ESC, S_CSI } st;
static int params[8], nparam, priv;

/* Cells are stored at a fixed stride (MAXCOLS), so changing cols/rows on resize
 * doesn't scramble existing content -- a cell keeps its (row,col). */
#define STRIDE MAXCOLS
static void cell_set(int r, int c, uint8_t ch, uint8_t fg, uint8_t bg) {
    int i = r * STRIDE + c; gch[i] = ch; gfg[i] = fg; gbg[i] = bg;
}
static void blank_row(int r) { for (int c = 0; c < cols; c++) cell_set(r, c, ' ', DEF_FG, DEF_BG); }
static void clear_grid(void) { for (int r = 0; r < rows; r++) blank_row(r); cx = cy = 0; }

/* Push a grid row into the scrollback ring (oldest is overwritten when full). */
static void sb_push(int srcrow) {
    int slot = (sb_head + sb_count) % SBROWS;
    if (sb_count == SBROWS) sb_head = (sb_head + 1) % SBROWS;   /* full: drop the oldest */
    else sb_count++;
    for (int c = 0; c < MAXCOLS; c++) {
        int s = srcrow * STRIDE + c, d = slot * MAXCOLS + c;
        sbch[d] = gch[s]; sbfg[d] = gfg[s]; sbbg[d] = gbg[s];
    }
}
/* The cell shown at visible (r,c) for the current scrollback offset: from the ring
 * when scrolled back into history, otherwise straight from the live grid. */
static void view_cell(int r, int c, uint8_t *ch, uint8_t *fg, uint8_t *bg) {
    int logical = (sb_count - view_off) + r;
    if (logical < sb_count) {
        int i = ((sb_head + logical) % SBROWS) * MAXCOLS + c;
        *ch = sbch[i]; *fg = sbfg[i]; *bg = sbbg[i];
    } else {
        int i = (logical - sb_count) * STRIDE + c;
        *ch = gch[i]; *fg = gfg[i]; *bg = gbg[i];
    }
}

static void scroll_up(void) {
    sb_push(0);                                  /* the top row scrolls into history */
    /* move the surface pixels up one text row, then the grid + shadow with them,
     * so only the freshly blanked bottom row differs on the next render. */
    int rowpx = fh * sw;
    for (int y = 0; y < (rows - 1) * fh * sw; y++) surf[y] = surf[y + rowpx];
    for (int y = (rows - 1) * fh * sw; y < rows * fh * sw; y++) surf[y] = pal[DEF_BG];
    for (int r = 0; r < rows - 1; r++)
        for (int c = 0; c < cols; c++) {
            int d = r * STRIDE + c, s = (r + 1) * STRIDE + c;
            gch[d] = gch[s]; gfg[d] = gfg[s]; gbg[d] = gbg[s];
            sch[d] = sch[s]; sfg[d] = sfg[s]; sbg[d] = sbg[s];
        }
    for (int c = 0; c < cols; c++) {
        int b = (rows - 1) * STRIDE + c;
        gch[b] = ' '; gfg[b] = DEF_FG; gbg[b] = DEF_BG;
        sch[b] = ' '; sfg[b] = DEF_FG; sbg[b] = DEF_BG;
    }
}

static void newline(void) { cx = 0; if (++cy >= rows) { clear_sel(); scroll_up(); cy = rows - 1; } }

static void put_char(uint8_t c) {
    if (cx >= cols) newline();
    int fg = cur_fg;
    if (bold && fg < 8) fg += 8;
    cell_set(cy, cx, c, (uint8_t)fg, (uint8_t)cur_bg);
    cx++;
}

static void sgr(void) {
    if (nparam == 0) { params[0] = 0; nparam = 1; }
    for (int i = 0; i < nparam; i++) {
        int p = params[i];
        if (p == 0)               { cur_fg = DEF_FG; cur_bg = DEF_BG; bold = 0; }
        else if (p == 1)          bold = 1;
        else if (p == 22)         bold = 0;
        else if (p >= 30 && p <= 37)   cur_fg = p - 30;
        else if (p == 39)         cur_fg = DEF_FG;
        else if (p >= 40 && p <= 47)   cur_bg = p - 40;
        else if (p == 49)         cur_bg = DEF_BG;
        else if (p >= 90 && p <= 97)   cur_fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) cur_bg = p - 100 + 8;
        else if (p == 38 && i + 2 < nparam && params[i+1] == 5) { cur_fg = params[i+2] & 15; i += 2; }
        else if (p == 48 && i + 2 < nparam && params[i+1] == 5) { cur_bg = params[i+2] & 15; i += 2; }
    }
}

static void csi(uint8_t f) {
    switch (f) {
    case 'm': sgr(); break;
    case 'H': case 'f': {                          /* cursor position (1-based) */
        int r = nparam > 0 && params[0] ? params[0] - 1 : 0;
        int c = nparam > 1 && params[1] ? params[1] - 1 : 0;
        cy = r < 0 ? 0 : r >= rows ? rows - 1 : r;
        cx = c < 0 ? 0 : c >= cols ? cols - 1 : c;
        break;
    }
    case 'A': cy -= nparam && params[0] ? params[0] : 1; if (cy < 0) cy = 0; break;
    case 'B': cy += nparam && params[0] ? params[0] : 1; if (cy >= rows) cy = rows - 1; break;
    case 'C': cx += nparam && params[0] ? params[0] : 1; if (cx >= cols) cx = cols - 1; break;
    case 'D': cx -= nparam && params[0] ? params[0] : 1; if (cx < 0) cx = 0; break;
    case 'J':                                       /* erase display (2 = all) */
        if (nparam == 0 || params[0] == 2 || params[0] == 0) { clear_grid(); }
        break;
    case 'K':                                       /* erase line to the right */
        for (int c = cx; c < cols; c++) cell_set(cy, c, ' ', DEF_FG, DEF_BG);
        break;
    default: break;
    }
}

static void feed(uint8_t c) {
    switch (st) {
    case S_NORMAL:
        if (c == 0x1b) { st = S_ESC; }
        else if (c == '\n') newline();
        else if (c == '\r') cx = 0;
        else if (c == '\b') { if (cx > 0) cx--; }
        else if (c == '\t') { cx = (cx + 8) & ~7; if (cx >= cols) cx = cols - 1; }
        else if (c == 7) { /* bell: ignore */ }
        else if (c >= 0x20) put_char(c);
        break;
    case S_ESC:
        if (c == '[') { st = S_CSI; nparam = 0; params[0] = 0; priv = 0; }
        else st = S_NORMAL;                         /* unsupported ESC x: drop */
        break;
    case S_CSI:
        if (c == '?') { priv = 1; }
        else if (c >= '0' && c <= '9') {
            if (nparam == 0) nparam = 1;
            if (nparam <= 8) params[nparam - 1] = params[nparam - 1] * 10 + (c - '0');
        } else if (c == ';') { if (nparam < 8) params[nparam++] = 0; }
        else { if (nparam == 0 && (c >= '@')) nparam = 0; csi(c); st = S_NORMAL; }
        break;
    }
}

/* The I-beam is an OVERLAY painted on top of a cell's glyph, not part of the
 * shadow grid -- so it must be erased explicitly (repaint that cell's glyph)
 * before anything that could move it. In particular it is hidden BEFORE feeding
 * shell output: a scroll memmoves the surface (which would otherwise drag a
 * burned-in bar up with the text) and a clear / erase-line leaves cells the glyph
 * diff considers unchanged (so it would never repaint over a stale bar). We track
 * exactly where the bar is painted (curx,cury) and hide it whenever it must move,
 * blink off, or the surface is cleared. */
static int cur_shown, curx = -1, cury = -1;

/* Erase the bar by repainting the glyph of the cell it sits on. Called before any
 * scroll/clear and whenever the cursor moves or blinks off. */
static void hide_cursor(void) {
    if (!cur_shown) return;
    if (curx >= 0 && curx < cols && cury >= 0 && cury < rows) {
        int i = cury * STRIDE + curx;
        ugfx_char(curx * fw, cury * fh, (char)gch[i], pal[gfg[i]], pal[gbg[i]]);
        dirty = 1;
    }
    cur_shown = 0;
}

/* A thin vertical I-beam bar at the left edge of the cursor cell -- the modern
 * terminal look. (The chunky inverse block stays in the no-UI text console, drawn
 * by the kernel.) */
static void show_cursor(void) {
    if (cur_shown || cx < 0 || cx >= cols || cy < 0 || cy >= rows) return;
    int bw = fw >= 8 ? 2 : 1;
    ugfx_fill(cx * fw, cy * fh + 1, bw, fh - 2, CUR_COL);
    curx = cx; cury = cy; cur_shown = 1; dirty = 1;
}

/* Pure glyph diff: repaint only cells whose char/fg/bg changed. The cursor is NOT
 * drawn here -- it is managed separately around the feed/render cycle. */
static void render(void) {
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int i = r * STRIDE + c;
            uint8_t ch, fg, bg; view_cell(r, c, &ch, &fg, &bg);
            uint8_t sl = (view_off == 0) ? (uint8_t)in_sel(r, c) : 0;   /* no selection while scrolled back */
            if (ch != sch[i] || fg != sfg[i] || bg != sbg[i] || sl != ssel[i]) {
                uint32_t bgc = sl ? SEL_BG : pal[bg];
                ugfx_char(c * fw, r * fh, (char)ch, pal[fg], bgc);
                sch[i] = ch; sfg[i] = fg; sbg[i] = bg; ssel[i] = sl;
                dirty = 1;
            }
        }
}

/* invalidate the shadow so the next render redraws every visible cell (used
 * after the surface is cleared or resized). */
static void invalidate(void) {
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) { int i = r * STRIDE + c; sch[i] = 0xff; sfg[i] = 0xff; sbg[i] = 0xff; ssel[i] = 0xff; }
}

/* (re)point ugfx at the surface, recompute the grid size, repaint from scratch.
 * The grid content is preserved across a resize (fixed stride); only cols/rows
 * and the cursor clamp change. */
static void setup_surface(int pitch) {
    ugfx_set_target(surf, sw, sh, pitch);
    cols = sw / fw; rows = sh / fh;
    if (cols > MAXCOLS) cols = MAXCOLS;
    if (rows > MAXROWS) rows = MAXROWS;
    if (cx >= cols) cx = cols - 1;
    if (cy >= rows) cy = rows - 1;
    ugfx_clear(pal[DEF_BG]);
    invalidate();
    cur_shown = 0;            /* the freshly-cleared surface holds no cursor bar */
}

/* Copy the current selection to the system clipboard (Ctrl+Shift+C): gather the
 * selected cells in reading order, one grid row at a time with trailing spaces
 * trimmed and rows joined by newlines, then clip_put it as text. */
static void term_copy(void) {
    if (!sel_on) return;
    int lo = sel_a < sel_b ? sel_a : sel_b, hi = sel_a < sel_b ? sel_b : sel_a;
    static char out[MAXROWS * (MAXCOLS + 1)];
    int n = 0;
    for (int r = lo / cols; r <= hi / cols && r < rows; r++) {
        int c0 = (r == lo / cols) ? lo % cols : 0;
        int c1 = (r == hi / cols) ? hi % cols : cols - 1;
        int line0 = n;
        for (int c = c0; c <= c1 && c < cols; c++) {
            char ch = (char)gch[r * STRIDE + c];
            if (n < (int)sizeof out - 1) out[n++] = ch ? ch : ' ';
        }
        while (n > line0 && out[n - 1] == ' ') n--;          /* trim trailing spaces */
        if (r < hi / cols && n < (int)sizeof out - 1) out[n++] = '\n';
    }
    if (n > 0) clip_put(CLIP_TEXT, "terminal", out, n);
    print("[term] copy "); printu((unsigned)n); print("\r\n");   /* harness hook */
}

/* Paste the active clipboard entry into the shell (Ctrl+Shift+V): feed its bytes
 * to the pty exactly as if they were typed. */
static void term_paste(int pty) {
    int idx = clip_active(-1);
    if (idx < 0) return;
    char b[1024];
    int n = clip_get(idx, b, sizeof b);
    if (n > 0) pty_write(pty, b, n);
}

static void mset(char *d, const char *s) { int i = 0; for (; s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* Declare the terminal's menu bar (app menus #6). Edit > Copy/Paste/Clear carry NO
 * Ctrl accelerators on purpose: terminal copy/paste are Ctrl+Shift+C/V, and a plain
 * ^C/^V accelerator would be intercepted by the compositor and stolen from the shell
 * (^C = interrupt). The items are reachable by clicking the menu. */
static void setup_menu(int wid) {
    struct winmenu mb = {0};
    mb.nmenus = 1;
    mset(mb.m[0].title, "Edit");
    mb.m[0].nitems = 3;
    mset(mb.m[0].items[0], "Copy");
    mset(mb.m[0].items[1], "Paste");
    mset(mb.m[0].items[2], "Clear");
    win_setmenu(wid, &mb);
}

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    struct sysinfo si; sysinfo(&si);
    int cw = (int)si.fb_w - 120, ch = (int)si.fb_h - 150;
    if (cw < 200) cw = 200;
    if (ch < 150) ch = 150;

    struct wininfo wi;
    wi.w = (uint32_t)cw; wi.h = (uint32_t)ch; wi.flags = 0;
    const char *t = "Terminal"; int q = 0; for (; t[q]; q++) wi.title[q] = t[q]; wi.title[q] = 0;
    win = win_create(&wi);
    if (win < 0) { print("[term] win_create failed\r\n"); proc_exit(); }
    setup_menu(win);                  /* Edit > Copy / Paste / Clear in the bar */
    surf = (uint32_t *)wi.vaddr; sw = (int)wi.w; sh = (int)wi.h;
    fw = ugfx_font_w(); fh = ugfx_font_h();
    setup_surface((int)wi.pitch);     /* sets cols/rows first ... */
    clear_grid();                     /* ... so this actually blanks the grid (was a no-op when rows==0) */
    print("[term] grid "); printu((unsigned)fw); printc(' '); printu((unsigned)fh);
    printc(' '); printu((unsigned)cols); printc(' '); printu((unsigned)rows); print("\r\n");  /* harness hook */
    render();
    win_present(win);

    int pty = pty_open();
    if (pty < 0) { print("[term] pty_open failed\r\n"); proc_exit(); }
    int pid = fork();
    if (pid == 0) { pty_attach(pty); exec("shell"); print("[term] exec(shell) failed\r\n"); proc_exit(); }

    char buf[512];
    struct winevent ev;
    int ind_shown = 0;                /* scrollback indicator currently painted in the right margin */
    for (;;) {
        dirty = 0;

        int n = pty_read(pty, buf, sizeof buf);          /* shell output -> grid */
        if (n > 0) {
            if (view_off) { view_off = 0; invalidate(); }   /* new output snaps back to the live bottom */
            hide_cursor();                               /* lift the bar BEFORE output scrolls/clears the surface */
            for (int i = 0; i < n; i++) feed((uint8_t)buf[i]);
            blink_on = 1; blink_ctr = 0;                 /* keep the cursor solid during output */
        }

        while (win_poll(win, &ev)) {                     /* events from the compositor */
            if (ev.type == WEV_KEY) {
                int k = (int)(ev.a & 0xff);
                if (k == KEY_TERM_PASTE)      { term_paste(pty); }
                else if (k == KEY_TERM_COPY || k == KEY_TERM_CUT) { term_copy(); clear_sel(); }
                else if (k == KEY_TERM_PGUP || k == KEY_TERM_PGDN) {   /* Shift+PgUp/PgDn: page scrollback */
                    int page = rows > 1 ? rows - 1 : 1;               /* keep one line of overlap, like less */
                    int nv = view_off + (k == KEY_TERM_PGUP ? page : -page);
                    if (nv < 0) nv = 0; if (nv > sb_count) nv = sb_count;
                    if (nv != view_off) { view_off = nv; hide_cursor(); invalidate(); dirty = 1; }
                }
                else { char c = (char)k; pty_write(pty, &c, 1); clear_sel(); blink_on = 1; blink_ctr = 0; }
            }
            else if (ev.type == WEV_MOUSE) {             /* click-drag selects grid text */
                int mx = (int)WEV_MOUSE_X(ev.a), my = (int)WEV_MOUSE_Y(ev.a), mb = (int)WEV_MOUSE_BTN(ev.a);
                if (mb & 1) {
                    int c = mx / fw, r = my / fh;
                    if (c < 0) c = 0; if (c >= cols) c = cols - 1;
                    if (r < 0) r = 0; if (r >= rows) r = rows - 1;
                    int ci = r * cols + c;
                    if (mb & WEV_MOUSE_DRAG) { sel_b = ci; }          /* extend */
                    else { sel_a = sel_b = ci; sel_on = 1; }          /* fresh press: anchor */
                    dirty = 1;
                }
            }
            else if (ev.type == WEV_SCROLL) {            /* wheel pages through scrollback */
                int d = (int)WEV_MOUSE_BTN(ev.a); if (d > 127) d -= 256;
                int nv = view_off + d * 3; if (nv < 0) nv = 0; if (nv > sb_count) nv = sb_count;
                if (nv != view_off) { view_off = nv; hide_cursor(); invalidate(); dirty = 1; }
            }
            else if (ev.type == WEV_MENU) {              /* Edit menu pick (app menus #6) */
                int it = WEV_MENU_I(ev.a);
                print("[term] menu "); printu((unsigned)it); print("\r\n");
                if (it == 0) { term_copy(); clear_sel(); }            /* Copy */
                else if (it == 1) { term_paste(pty); }               /* Paste */
                else if (it == 2) { clear_grid(); invalidate(); dirty = 1; }  /* Clear */
            }
            else if (ev.type == WEV_CLOSE) { goto done; }
            else if (ev.type == WEV_RESIZE) {
                int nw = (int)(ev.a >> 16), nh = (int)(ev.a & 0xffff);
                if (nw >= fw && nh >= fh && win_resize(win, nw, nh) == 0) {
                    sw = nw; sh = nh;
                    surf = (uint32_t *)wi.vaddr;          /* same vaddr after resize */
                    setup_surface(nw);                    /* pitch == width; clears cur_shown */
                }
            }
        }

        if (++blink_ctr >= 66) { blink_ctr = 0; blink_on = !blink_on; }   /* ~530ms blink */

        render();                                        /* repaint changed glyphs */
        if (view_off == 0) {                             /* live: manage the blinking I-beam */
            if (cur_shown && (curx != cx || cury != cy)) hide_cursor();
            if (blink_on) show_cursor(); else hide_cursor();
        } else if (cur_shown) hide_cursor();             /* scrolled back: no live cursor */
        if (dirty) {                                     /* scrollback position indicator on the right edge */
            if (view_off > 0 && sb_count > 0) {
                ugfx_fill(sw - 4, 0, 4, sh, pal[DEF_BG]);             /* clear the strip, then draw the shared thumb */
                ugfx_scroll_thumb(sw - 4, 0, 4, sh, sb_count - view_off, sb_count + rows, rows, 0);
                ind_shown = 1;
            } else if (ind_shown) { ugfx_fill(sw - 4, 0, 4, sh, pal[DEF_BG]); ind_shown = 0; }
            win_present(win);
        }

        if (trywait() < 0) break;                        /* shell exited */
        sleep_ms(8);
    }
done:
    win_close(win);
    pty_close(pty);
    proc_exit();
}
