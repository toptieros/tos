/* twm -- the dock and the installed-app catalog.
 *
 * The centred, frosted dock of launchers: the leftmost Launchpad button, the
 * pinned apps, then a transient tile per running-but-unpinned window. Tiles are
 * built from the /Apps bundle catalog (loaded once at startup) plus the live
 * window set. This file owns catalog loading, the dock's layout + hit geometry,
 * and tile drawing; the click handling + auto-hide animation that drive it live in
 * twm.c, which calls layout_dock()/rebuild_dock()/draw_dock() here. */
#include "twm.h"

static int dock_runsep = -1;     /* first running-unpinned tile (the pinned|running boundary); -1 when none */

/* ------------------------------------------------------------------ dock tiles */
int title_is(const char *title, const char *label) {   /* window title starts with label */
    int i = 0; for (; label[i]; i++) if (title[i] != label[i]) return 0; return 1;
}
/* running state of the app behind a dock tile: bit0 = a window is open,
 * bit1 = its window is focused, bit2 = (only) minimized. */
static int app_state(const char *label) {
    int st = 0, fs = focus_slot();
    for (int i = 0; i < MAXW; i++) {
        if (!cw[i].used || !title_is(cw[i].title, label)) continue;
        st |= 1;
        if (i == fs) st |= 2;
        if (cw[i].min) st |= 4;
    }
    if (st & 2) st &= ~4;                           /* focused wins over the minimized hint */
    return st;
}
int dock_tile_for(const char *title) {              /* dock icon whose label matches a window */
    for (int i = 0; i < nicons; i++) if (title_is(title, icons[i].label)) return i;
    return -1;
}
int find_app_window(const char *label) {            /* a matching window slot (prefer minimized) */
    int any = -1;
    for (int i = 0; i < MAXW; i++)
        if (cw[i].used && title_is(cw[i].title, label)) { if (cw[i].min) return i; any = i; }
    return any;
}
int tile_hovered(struct icon *ic) {
    return cur_x >= ic->cx - TH_TILE / 2 && cur_x < ic->cx + TH_TILE / 2 &&
           cur_y >= ic->cy - TH_TILE / 2 && cur_y < ic->cy + TH_TILE / 2;
}
static void draw_tile(struct icon *ic) {
    int x = ic->cx - APPICON_SZ / 2, y = ic->cy - APPICON_SZ / 2;
    int st = app_state(ic->label);
    if (tile_hovered(ic))                           /* hover: soft lift */
        ugfx_rrect_a(x - 6, y - 6, APPICON_SZ + 12, APPICON_SZ + 12, TH_TILE_RAD + 4, ARGB(38, 255, 255, 255));
    if (st & 2)                                     /* focused: subtle Windows-style highlight */
        ugfx_rrect_a(x - 5, y - 5, APPICON_SZ + 10, APPICON_SZ + 10, TH_TILE_RAD + 3, ARGB(46, 255, 255, 255));
    if (ic->special) {                              /* Launchpad button: a card with a 3x3 grid */
        ugfx_rrect_a(x, y, APPICON_SZ, APPICON_SZ, 11, ARGB(255, 58, 64, 84));
        int pad = 9, gap = 4, cell = (APPICON_SZ - 2 * pad - 2 * gap) / 3;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                ugfx_rrect_a(x + pad + c * (cell + gap), y + pad + r * (cell + gap), cell, cell, 2,
                             ARGB(255, 150, 180, 232));
    }
    else if (ic->img) ugfx_blit_scaled(x, y, APPICON_SZ, APPICON_SZ, ic->img, ic->iw, ic->ih);  /* hi-res bundle icon -> tile */
    else              ugfx_blit_argb(x, y, APPICON_SZ, APPICON_SZ, appicons_argb[ICON_APP]);  /* generic fallback */
    int iy = y + APPICON_SZ + 3;                    /* running indicator under the tile */
    if (st & 2)        ugfx_rrect_a(ic->cx - 9, iy, 18, 3, 1, g_accent);   /* focused: accent bar */
    else if (st & 1)   ugfx_rrect_a(ic->cx - 2, iy, 4, 3, 1, ARGB(160, 200, 210, 230));     /* running: dot */
}
void draw_dock(void) {
    ugfx_elevation(dock_x, dock_y, dock_w, dock_h, TH_DOCK_RAD, DOCK_ELEVATION);  /* float it off the desktop */
    ugfx_frost(dock_x, dock_y, dock_w, dock_h, TH_DOCK_RAD, TH_DOCK_FROST);  /* frosted-glass panel      */
    ugfx_rrect_border(dock_x, dock_y, dock_w, dock_h, TH_DOCK_RAD, 1, TH_BORDER_DIM);  /* crisp edge     */
    ugfx_fill_a(dock_x + TH_DOCK_RAD, dock_y, dock_w - 2 * TH_DOCK_RAD, 1, TH_DOCK_HI_A);  /* top sheen   */
    if (dock_runsep >= 1 && dock_runsep < nicons) {  /* faint pinned | running separator in the gap before the first running tile */
        int sx = (icons[dock_runsep - 1].cx + icons[dock_runsep].cx) / 2;
        ugfx_fill_a(sx, dock_y + DOCK_PAD + 6, 1, TH_TILE - 12, TH_BORDER);
    }
    for (int i = 0; i < nicons; i++) draw_tile(&icons[i]);
}

/* ------------------------------------------------------------------ /Apps catalog */
/* dst = a + "/" + b  (a is a directory path, b a relative name) */
static void path_join(char *dst, const char *a, const char *b) {
    int i = 0; while (a[i]) { dst[i] = a[i]; i++; }
    if (i && dst[i - 1] != '/') dst[i++] = '/';
    for (int j = 0; b[j]; j++) dst[i++] = b[j];
    dst[i] = 0;
}
static int ends_app(const char *s) {                /* name ends in ".app" */
    int n = 0; while (s[n]) n++;
    return n >= 5 && s[n-4] == '.' && s[n-3] == 'a' && s[n-2] == 'p' && s[n-1] == 'p';
}

/* Load an icon.argb file (u32 w, u32 h, then w*h little-endian ARGB) into a
 * malloc'd buffer; 0 on any error (the dock then draws a generic tile). */
static uint32_t *load_icon(const char *path, int *w, int *h) {
    int fd = fopen(path, O_RDONLY);
    if (fd < 0) return 0;
    uint32_t hdr[2] = { 0, 0 };
    if (fread_(fd, (char *)hdr, 8) != 8 || !hdr[0] || !hdr[1] || hdr[0] > 256 || hdr[1] > 256) {
        fclose_(fd); return 0;
    }
    int need = (int)(hdr[0] * hdr[1] * 4), got = 0;
    uint32_t *px = (uint32_t *)malloc((unsigned)need);
    if (!px) { fclose_(fd); return 0; }
    while (got < need) { int r = fread_(fd, (char *)px + got, need - got); if (r <= 0) break; got += r; }
    fclose_(fd);
    if (got != need) { free(px); return 0; }
    *w = (int)hdr[0]; *h = (int)hdr[1];
    return px;
}

/* Catalog every /Apps/<Name>.app bundle once (design/app-package-format.md): its
 * display name, absolute exec path, icon, and whether the manifest pins it to the
 * dock (pinned != false). rebuild_dock() composes the visible dock from this plus
 * the running window set. Replaces the old flat shortcuts file + baked icon table. */
void load_apps(void) {
    struct dirent ents[2 * MAXAPPS];
    int n = readdir("/Apps", ents, 2 * MAXAPPS);
    for (int i = 0; i < n && napps < MAXAPPS; i++) {
        if (ents[i].type != FT_DIR || !ends_app(ents[i].name)) continue;
        char base[80]; path_join(base, "/Apps", ents[i].name);     /* /Apps/<Name>.app */
        char mpath[112]; path_join(mpath, base, "manifest");
        char buf[1024]; int fd = fopen(mpath, O_RDONLY);
        if (fd < 0) continue;
        int mn = fread_(fd, buf, sizeof buf - 1); fclose_(fd);
        if (mn <= 0) continue;
        buf[mn] = 0;

        char val[96];
        if (!manifest_get(buf, "name", val, sizeof val)) continue;
        struct app *a = &apps[napps];
        int j = 0; for (; val[j] && j < 23; j++) a->label[j] = val[j]; a->label[j] = 0;
        if (!manifest_get(buf, "exec", val, sizeof val)) continue;
        path_join(a->exec, base, val);                             /* absolute exec path */
        a->pinned = 1;
        if (manifest_get(buf, "pinned", val, sizeof val) &&
            (streq(val, "false") || streq(val, "0") || streq(val, "no"))) a->pinned = 0;
        a->img = 0; a->iw = APPICON_SZ; a->ih = APPICON_SZ;
        char iconrel[40];
        if (manifest_get(buf, "icon", iconrel, sizeof iconrel) && iconrel[0]) {
            char ipath[120]; path_join(ipath, base, iconrel);
            a->img = load_icon(ipath, &a->iw, &a->ih);
        }
        napps++;
    }
}
/* The catalog app whose name prefixes a window title (windows are titled by app
 * name), giving a running window its canonical label + icon; -1 if none. */
int app_for_title(const char *title) {
    for (int i = 0; i < napps; i++) if (title_is(title, apps[i].label)) return i;
    return -1;
}
/* Compose the visible dock: the leftmost Launchpad button, then every pinned app,
 * then a transient tile for each running (incl. minimized) non-popup window whose
 * app isn't already shown -- so an unpinned app like Notepad (opened from Files or
 * Spotlight) appears in the dock while it runs and drops off when it closes. */
void rebuild_dock(void) {
    nicons = 0;
    struct icon *lp = &icons[nicons++];                    /* Launchpad button (single click) */
    const char *lpl = "Launchpad";
    int k = 0; for (; lpl[k]; k++) lp->label[k] = lpl[k]; lp->label[k] = 0;
    lp->exec[0] = 0; lp->img = 0; lp->iw = lp->ih = APPICON_SZ; lp->special = 1;
    for (int i = 0; i < napps && nicons < MAXICON; i++) {  /* pinned apps, in catalog order */
        if (!apps[i].pinned) continue;
        struct icon *ic = &icons[nicons++];
        int j = 0; for (; (ic->label[j] = apps[i].label[j]); j++) ;
        j = 0;       for (; (ic->exec[j]  = apps[i].exec[j]);  j++) ;
        ic->img = apps[i].img; ic->iw = apps[i].iw; ic->ih = apps[i].ih; ic->special = 0;
    }
    int pin_end = nicons;                                  /* boundary: tiles [1..pin_end) are pinned, [pin_end..) run */
    for (int w = 0; w < MAXW && nicons < MAXICON; w++) {   /* running, not-yet-shown apps */
        if (!cw[w].used || cw[w].popup) continue;
        int dup = 0;
        for (int e = 0; e < nicons; e++) if (title_is(cw[w].title, icons[e].label)) { dup = 1; break; }
        if (dup) continue;
        struct icon *ic = &icons[nicons++];
        int ai = app_for_title(cw[w].title);
        const char *lab = ai >= 0 ? apps[ai].label : cw[w].title;
        int j = 0; for (; lab[j] && j < 23; j++) ic->label[j] = lab[j]; ic->label[j] = 0;
        ic->exec[0] = 0;
        ic->img = ai >= 0 ? apps[ai].img : 0;
        ic->iw  = ai >= 0 ? apps[ai].iw  : APPICON_SZ;
        ic->ih  = ai >= 0 ? apps[ai].ih  : APPICON_SZ;
        ic->special = 0;
    }
    dock_runsep = (nicons > pin_end) ? pin_end : -1;       /* divider only when >=1 running-unpinned tile exists */
}
/* A cheap hash of the running non-popup window set (by id); the dock rebuilds +
 * re-lays-out only when this changes, so layout_dock's serial trace and the
 * recentre fire on open/close, not every frame. */
unsigned running_sig(void) {
    unsigned h = 2166136261u ^ (unsigned)napps;
    for (int w = 0; w < MAXW; w++)
        if (cw[w].used && !cw[w].popup) h = (h ^ (unsigned)(cw[w].id + 1)) * 16777619u;
    return h;
}
/* Position the dock tiles for the current dock_y (recomputed as the dock slides). */
void place_dock_icons(void) {
    for (int i = 0; i < nicons; i++) {
        icons[i].cx = dock_x + DOCK_PAD + TH_TILE / 2 + i * (TH_TILE + DOCK_GAP);
        icons[i].cy = dock_y + DOCK_PAD + TH_TILE / 2;
    }
}
void layout_dock(void) {
    static const uint32_t pal[4] = { TH_TILE_0, TH_TILE_1, TH_TILE_2, TH_TILE_3 };
    if (nicons < 1) nicons = 1;
    dock_w = nicons * TH_TILE + (nicons - 1) * DOCK_GAP + 2 * DOCK_PAD;
    dock_h = TH_TILE + 2 * DOCK_PAD;
    dock_x = (W - dock_w) / 2;
    dock_y0 = H - dock_h - 18;
    dock_y = dock_y0;
    place_dock_icons();
    for (int i = 0; i < nicons; i++) {
        icons[i].tint = pal[i & 3];
        /* the test harness drives the dock by these coordinates (the shown base
         * position), so it never has to assume a layout or resolution. */
        print("[twm] icon "); print(icons[i].label);
        printc(' '); printu((unsigned)icons[i].cx); printc(' '); printu((unsigned)icons[i].cy);
        print("\r\n");
    }
    if (dock_runsep >= 1)                            /* the pinned|running boundary, when a running-unpinned app exists */
        { print("[twm] docksep "); printu((unsigned)dock_runsep); print("\r\n"); }
}
