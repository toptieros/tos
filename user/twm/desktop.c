/* desktop.c -- the ~/Desktop layer, part of the compositor.
 *
 * The desktop is NOT a separate app: it is a feature of twm, a peer of the dock
 * (dock.c) and the control center (controlcenter.c). twm already owns the
 * wallpaper, the bar and the dock; the desktop -- the field of icons drawn over
 * the wallpaper -- belongs in the same shell, exactly as Plasma's *desktop
 * containment* lives inside plasmashell alongside the *panel containment* (the
 * task bar), rather than as a program you launch (see inspiration/plasma-desktop,
 * containments/desktop). KWin composites; plasmashell draws panels + desktop. In
 * tOS twm is both, so the desktop lives here.
 *
 * draw_desktop() paints the contents of ~/Desktop as an auto-grid of labelled
 * icons directly onto the back buffer, right after the wallpaper and below every
 * window -- so the wallpaper simply shows wherever there is no icon (no window,
 * no colour-key, no IPC). A single click selects an icon; a double click opens it
 * (a folder opens navigated-to in Files). The set is re-scanned once a second so
 * files dropped in from the shell or Files appear live. */
#include "twm.h"

#define DESKDIR    "/Users/user/Desktop"
#define FILES_EXEC "/Apps/Files.app/bin/files"
#define OPEN_DOC   "/tmp/.open-doc"        /* the hand-off Files reads at startup (sys_open_arg) */
#define DMAX       64
#define DCELL_W    96
#define DCELL_H    92
#define DICON      FILEICON_SZ             /* 64px file icons, blitted at native size */
#define DGRID_X    24                      /* icon grid inset from the work-area edges */
#define DGRID_Y    18

struct deskitem { char name[64]; int isdir; };
static struct deskitem ditems[DMAX];
static int      dn = 0;                    /* visible item count */
static int      dsel = -1;                 /* selected icon index, -1 none */
static int      dcols = 1;                 /* columns at the current width (set when laying out) */
static unsigned dsig = 0;                  /* content signature -> rescans repaint only on change */
static int      d_last_i = -1, d_last_frame = -1000;   /* double-click tracking */

static int ext_is(const char *name, const char *ext) {
    int L = 0; while (name[L]) L++;
    const char *e = name + L; while (e > name && *(e - 1) != '.') e--;
    if (e == name) return 0;
    for (int i = 0; ext[i] || e[i]; i++) if (e[i] != ext[i]) return 0;
    return 1;
}
static int desk_icon_for(int i) {
    if (ditems[i].isdir) return FILEICON_FOLDER;
    const char *n = ditems[i].name;
    if (ext_is(n, "txt") || ext_is(n, "md"))   return FILEICON_TEXT;
    if (ext_is(n, "argb") || ext_is(n, "ppm")) return FILEICON_IMAGE;
    if (ext_is(n, "elf"))                       return FILEICON_EXEC;
    return FILEICON_FILE;
}

/* Re-read ~/Desktop into ditems[]; returns a cheap content signature (FNV over the
 * names + dir flags + count) so tick can repaint only when the set actually changes. */
static unsigned desk_scan(void) {
    struct dirent e[DMAX]; int rn = readdir(DESKDIR, e, DMAX);
    dn = 0; unsigned h = 2166136261u;
    for (int i = 0; i < rn && dn < DMAX; i++) {
        if (e[i].name[0] == '.') continue;               /* hide dotfiles */
        int j = 0; for (; e[i].name[j] && j < 63; j++) ditems[dn].name[j] = e[i].name[j];
        ditems[dn].name[j] = 0;
        ditems[dn].isdir = (e[i].type == FT_DIR);
        for (int k = 0; ditems[dn].name[k]; k++) h = (h ^ (unsigned)ditems[dn].name[k]) * 16777619u;
        h = (h ^ (unsigned)ditems[dn].isdir) * 16777619u;
        dn++;
    }
    if (dsel >= dn) dsel = -1;
    return h ^ (unsigned)dn;
}

/* a label centred under the icon, truncated to the cell with a faint shadow so it
 * reads on any wallpaper (white ink, 1px dark offset). */
static void desk_label(const char *name, int cx, int y) {
    char lbl[24]; int L = 0;
    for (; name[L] && L < 22; L++) lbl[L] = name[L];
    lbl[L] = 0;
    if (name[L] && L > 2) { lbl[L - 1] = '.'; lbl[L - 2] = '.'; }    /* truncated */
    int tw = ugfx_text_w(lbl), tx = cx - tw / 2;
    ugfx_text(tx + 1, y + 1, lbl, ARGB(180, 0, 0, 0), UGFX_TRANSPARENT);   /* shadow */
    ugfx_text(tx, y, lbl, RGB(245, 247, 252), UGFX_TRANSPARENT);
}

static int desk_index_at(int x, int y) {
    int gx = DGRID_X, gy = bar_h + DGRID_Y;
    dcols = (W - 2 * DGRID_X) / DCELL_W; if (dcols < 1) dcols = 1;
    if (x < gx || y < gy) return -1;
    int c = (x - gx) / DCELL_W, ro = (y - gy) / DCELL_H;
    if (c < 0 || c >= dcols) return -1;
    int i = ro * dcols + c;
    return (i >= 0 && i < dn) ? i : -1;
}
static void desk_dirty_cell(int i) {                     /* repaint one icon cell (selection change) */
    if (i < 0 || i >= dn) return;
    int gx = DGRID_X, gy = bar_h + DGRID_Y;
    int c = i % dcols, ro = i / dcols;
    add_dirty(gx + c * DCELL_W, gy + ro * DCELL_H, DCELL_W, DCELL_H);
}

/* Open a desktop item in Files: a folder opens navigated-to (the absolute path is
 * handed off so Files lands there); a file opens Files at the Desktop folder. Uses
 * twm's launch(), so Files is dropped to its manifest caps like any launched app. */
static void desk_open(int i) {
    char full[256]; int p = 0;
    const char *d = DESKDIR; for (; d[p]; p++) full[p] = d[p];
    full[p++] = '/';
    for (int j = 0; ditems[i].name[j] && p < 255; j++) full[p++] = ditems[i].name[j];
    full[p] = 0;
    const char *arg = ditems[i].isdir ? full : DESKDIR;
    int n = 0; while (arg[n]) n++;
    sys_spit(OPEN_DOC, arg, n);                          /* the path Files reads at startup */
    launch(FILES_EXEC);
    print("[twm] desktop open "); print(ditems[i].name); print("\r\n");
}

/* ---- the contract in twm.h ------------------------------------------------- */
void desktop_init(void) {
    dsig = desk_scan();
    print("[twm] desktop items "); printu((unsigned)dn); print("\r\n");
}

void draw_desktop(void) {                                /* called by compose(), over the wallpaper */
    int gx = DGRID_X, gy = bar_h + DGRID_Y;
    dcols = (W - 2 * DGRID_X) / DCELL_W; if (dcols < 1) dcols = 1;
    for (int i = 0; i < dn; i++) {
        int c = i % dcols, ro = i / dcols;
        int x = gx + c * DCELL_W, y = gy + ro * DCELL_H;
        struct rect cell = { x, y, DCELL_W, DCELL_H };
        if (!rects_hit(cur_clip, cell)) continue;        /* nothing of this icon in the dirty rect */
        if (i == dsel) ugfx_rrect_a(x + 4, y + 2, DCELL_W - 8, DCELL_H - 4, 10, ARGB(96, 120, 150, 215));
        int ix = x + (DCELL_W - DICON) / 2;
        ugfx_blit_argb(ix, y + 6, DICON, DICON, fileicons_argb[desk_icon_for(i)]);
        desk_label(ditems[i].name, x + DCELL_W / 2, y + 6 + DICON + 2);
    }
}

void desktop_tick(void) {                                /* once a second: pick up files dropped in */
    unsigned ns = desk_scan();
    if (ns != dsig) {
        dsig = ns;
        add_dirty(0, bar_h, W, H - bar_h);               /* repaint the icon field */
        print("[twm] desktop reload "); printu((unsigned)dn); print("\r\n");
    }
}

/* A left press the chrome + windows didn't claim landed on the wallpaper: select the
 * icon under it (or deselect on empty space); a double click opens it. Returns 1 if it
 * hit an icon. The desktop is the bottom layer, so it only ever sees background clicks. */
int desktop_click(int mx, int my, int frame) {
    int i = desk_index_at(mx, my);
    if (i != dsel) { desk_dirty_cell(dsel); desk_dirty_cell(i); dsel = i; }
    if (i >= 0) {
        if (d_last_i == i && frame - d_last_frame <= DBL_FRAMES) { desk_open(i); d_last_i = -1; }
        else { d_last_i = i; d_last_frame = frame; }
        return 1;
    }
    return 0;
}
