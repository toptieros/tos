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
 * It behaves like Files rooted at ~/Desktop: draw_desktop() paints the folder as an
 * auto-grid of labelled icons directly onto the back buffer (right after the wallpaper,
 * below every window -- the wallpaper shows wherever there is no icon). Click selects,
 * Ctrl/Shift extend the selection, an empty-space drag rubber-band-selects (the same
 * translucent accent band Files draws -- ugfx_rubberband), and a double click opens an
 * item (a folder opens navigated-to in Files). The set is re-scanned once a second so
 * files dropped in from the shell or Files appear live. Context menus + rename + copy
 * are layered on in later slices. */
#include "twm.h"
#include "filesel.h"      /* the shared selection-set algebra (also used by Files) */

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
static struct filesel dsel;                /* the multi-selection over [0,dn) */
static int      dcols = 1;                 /* columns at the current width (set when laying out) */
static unsigned dsig = 0;                  /* content signature -> rescans repaint only on change */
static int      d_last_i = -1, d_last_frame = -1000;   /* double-click tracking */

/* the rubber-band marquee, in screen coords (anchor -> cursor). marq_on while a drag
 * over empty desktop is in flight; marq_base is the selection at the drag's start so an
 * additive (Ctrl/Shift) marquee unions onto it. marq_pr is the previously-drawn band
 * rect, dirtied as the band grows so no smear is left behind. */
static int marq_on = 0, marq_add = 0;
static int marq_x0, marq_y0, marq_x1, marq_y1;
static struct filesel marq_base;
static struct rect marq_pr;

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
    fsel_init(&dsel, dn);                                 /* a fresh scan resets the selection */
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

/* the screen rect of icon i's cell (the grid is laid out in screen coords) */
static struct rect desk_cell(int i) {
    int gx = DGRID_X, gy = bar_h + DGRID_Y;
    int c = i % dcols, ro = i / dcols;
    struct rect r = { gx + c * DCELL_W, gy + ro * DCELL_H, DCELL_W, DCELL_H };
    return r;
}
static void desk_cols(void) { dcols = (W - 2 * DGRID_X) / DCELL_W; if (dcols < 1) dcols = 1; }

static int desk_index_at(int x, int y) {
    int gx = DGRID_X, gy = bar_h + DGRID_Y;
    desk_cols();
    if (x < gx || y < gy) return -1;
    int c = (x - gx) / DCELL_W, ro = (y - gy) / DCELL_H;
    if (c < 0 || c >= dcols) return -1;
    int i = ro * dcols + c;
    return (i >= 0 && i < dn) ? i : -1;
}
static void desk_dirty_cell(int i) {                     /* repaint one icon cell (selection change) */
    if (i < 0 || i >= dn) return;
    struct rect c = desk_cell(i);
    add_dirty(c.x, c.y, c.w, c.h);
}
static void desk_dirty_all(void) { add_dirty(0, bar_h, W, H - bar_h); }

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

/* the marquee band as a normalised screen rect */
static struct rect marq_rect(void) {
    int x0 = marq_x0 < marq_x1 ? marq_x0 : marq_x1;
    int y0 = marq_y0 < marq_y1 ? marq_y0 : marq_y1;
    int x1 = marq_x0 > marq_x1 ? marq_x0 : marq_x1;
    int y1 = marq_y0 > marq_y1 ? marq_y0 : marq_y1;
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

/* ---- the contract in twm.h ------------------------------------------------- */
void desktop_init(void) {
    dsig = desk_scan();
    print("[twm] desktop items "); printu((unsigned)dn); print("\r\n");
}

void draw_desktop(void) {                                /* called by compose(), over the wallpaper */
    desk_cols();
    for (int i = 0; i < dn; i++) {
        struct rect cell = desk_cell(i);
        if (!rects_hit(cur_clip, cell)) continue;        /* nothing of this icon in the dirty rect */
        if (fsel_has(&dsel, i))
            ugfx_rrect_a(cell.x + 4, cell.y + 2, DCELL_W - 8, DCELL_H - 4, 10, ARGB(96, 120, 150, 215));
        int ix = cell.x + (DCELL_W - DICON) / 2;
        ugfx_blit_argb(ix, cell.y + 6, DICON, DICON, fileicons_argb[desk_icon_for(i)]);
        desk_label(ditems[i].name, cell.x + DCELL_W / 2, cell.y + 6 + DICON + 2);
    }
    if (marq_on) {                                       /* the Dolphin-style rubber band, over the icons */
        struct rect b = marq_rect();
        ugfx_rubberband(b.x, b.y, b.w, b.h, g_accent);
    }
}

void desktop_tick(void) {                                /* once a second: pick up files dropped in */
    if (marq_on) return;                                 /* don't reshuffle the set mid-drag */
    unsigned ns = desk_scan();
    if (ns != dsig) {
        dsig = ns; desk_dirty_all();
        print("[twm] desktop reload "); printu((unsigned)dn); print("\r\n");
    }
}

/* A left press the chrome + windows didn't claim landed on the wallpaper. On an icon:
 * select it (Ctrl toggles, Shift ranges from the anchor), double-click opens it. On
 * empty space: clear (unless a modifier is held) and arm a rubber-band marquee a drag
 * extends. Returns 1 when it hit an icon. The desktop is the bottom layer, so it only
 * ever sees background clicks. */
int desktop_click(int mx, int my, int frame) {
    unsigned m = kbd_mods();
    int ctrl = (m & (KMOD_CTRL | KMOD_SUPER)) != 0, shift = (m & KMOD_SHIFT) != 0;
    int i = desk_index_at(mx, my);
    if (i >= 0) {
        if (shift)      fsel_range(&dsel, i, ctrl);
        else if (ctrl)  fsel_toggle(&dsel, i);
        else            fsel_click(&dsel, i);            /* clears others, selects i */
        desk_dirty_all();
        if (!ctrl && !shift && d_last_i == i && frame - d_last_frame <= DBL_FRAMES) {
            desk_open(i); d_last_i = -1;
        } else { d_last_i = i; d_last_frame = frame; }
        return 1;
    }
    /* empty space: arm the marquee (additive when a modifier is held) */
    marq_add = ctrl || shift;
    marq_base = dsel;
    if (!marq_add) { fsel_clear(&dsel); desk_dirty_all(); }
    marq_on = 1; marq_x0 = marq_x1 = mx; marq_y0 = marq_y1 = my;
    marq_pr = marq_rect();
    return 0;
}

int desktop_marquee_active(void) { return marq_on; }

void desktop_drag(int mx, int my) {                      /* extend the band; reselect intersecting icons */
    if (!marq_on) return;
    marq_x1 = mx; marq_y1 = my;
    struct rect b = marq_rect();
    dsel = marq_base;                                    /* rebuild from the drag-start set */
    desk_cols();
    for (int i = 0; i < dn; i++) {                       /* a 2D hit: any icon cell the band touches */
        struct rect c = desk_cell(i);
        if (rects_hit(b, c)) fsel_band(&dsel, i, i, 1);
    }
    /* repaint the band's travel (old + new, grown for the 1px hairline) + the icon field */
    add_dirty(marq_pr.x - 2, marq_pr.y - 2, marq_pr.w + 4, marq_pr.h + 4);
    add_dirty(b.x - 2, b.y - 2, b.w + 4, b.h + 4);
    marq_pr = b;
}

void desktop_release(void) {
    if (!marq_on) return;
    struct rect b = marq_rect();
    marq_on = 0;
    add_dirty(b.x - 2, b.y - 2, b.w + 4, b.h + 4);
    print("[twm] desktop marquee "); printu((unsigned)fsel_count(&dsel)); print("\r\n");
}
