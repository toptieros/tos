/* twm -- the tOS window manager / compositor.
 *
 * It owns the framebuffer and does only window management: a desktop, a
 * translucent top bar (logo, focused-app title, clock), and a centred dock of
 * launchers read from a shortcuts file (double-click to open an app). It
 * composites app windows -- each app draws into its own shared-memory surface
 * (SYS_WIN_*) -- with rounded corners, soft drop shadows, focus, drag, resize
 * and close. It knows nothing about what an app draws.
 *
 * Rendering is BACK-BUFFERED: everything is composited into an off-screen RAM
 * buffer (sized to the real resolution via SYS_MMAP, drawn from the whole frame
 * pool -- no fixed cap), then only the damaged rectangles are copied to the
 * framebuffer. That gives flicker-free motion, lets shadows/translucency blend
 * cheaply, and means there is a single framebuffer writer, so the old
 * cursor-smear is gone.
 *
 * With no framebuffer it just exec()s the shell, so a text boot is an ordinary
 * TTY. */
#include "ulib.h"
#include "ugfx.h"
#include "theme.h"
#include "logo.h"
#include "winbtns.h"
#include "cursors.h"
#include "icons.h"
#include "manifest.h"
#include "registry.h"

#define MAXW      8
#define MAXICON   16             /* dock tiles: launchpad + pinned + running, see rebuild_dock */
#define MAXAPPS   8              /* catalog of installed /Apps bundles                          */
#define MAXDIRTY  32
#define DBL_FRAMES 40            /* double-click window, in event-loop frames */
#define GRIP      18             /* bottom-right resize-grip hit box          */
#define CURW      14             /* cursor damage box                         */
#define CURH      21
#define DOCK_GAP  16
#define DOCK_PAD  14
#define DOCK_SH   20   /* dock drop-shadow margin: every dock damage rect must include  *
                        * this halo or the soft shadow leaves residue when it animates.  */
#define EDGE         4    /* screen-edge reveal zone for auto-hidden chrome (px)   */
#define HIDE_LINGER  16   /* frames the chrome lingers after the cursor leaves     */
#define SLIDE        6    /* chrome slide speed (px/frame)                         */

static uint32_t *bb;             /* back buffer (mmap'd, fb_w*fb_h, tightly packed) */
static uint32_t *wall;           /* precomputed wallpaper (gradient+glow+vignette), W*H */
static int W, H, fh, bar_h, TH;  /* screen, font height, bar height, title height  */

/* desktop gradient endpoints, split into channels for the per-row lerp */
static int gtr, gtg, gtb, gbr, gbg, gbb;

struct cwin { int used, id, wx, wy; uint32_t w, h, seq; uint64_t vaddr; char title[32];
              int min, maxed, sx, sy; uint32_t sw, sh;     /* min/max flags + pre-maximize geometry */
              int popup, overlay; };                       /* WIN_POPUP / WIN_OVERLAY (dim, above dock) */
static struct cwin cw[MAXW];
static int zo[MAXW], nz;         /* z-order: zo[nz-1] is topmost == focused */

/* The installed-app catalog: every /Apps/<Name>.app bundle (name, absolute exec
 * path, loaded icon, and whether its manifest pins it to the dock). Loaded once at
 * startup; the dock and the launchpad button draw from it. */
struct app { char label[24], exec[120]; uint32_t *img; int iw, ih; int pinned; };
static struct app apps[MAXAPPS];
static int napps;

/* a dock tile: display name, the absolute exec path, its icon (img==0 -> a generic
 * or, for the launchpad button, a grid glyph), and its on-screen centre. `special`
 * marks the leftmost Launchpad button (single-click summons the launchpad grid).
 * The visible set is rebuilt by rebuild_dock(): launchpad + pinned + running apps. */
struct icon { char label[24], exec[120]; uint32_t *img; int iw, ih; int cx, cy; uint32_t tint; int special; };
static struct icon icons[MAXICON];
static int nicons;
static unsigned dock_sig;        /* signature of the running set; the dock rebuilds when it changes */
static int dock_x, dock_y, dock_w, dock_h;
static int dock_y0;              /* dock's shown (base) y; dock_y is the current (animated) y */
static int bar_y;                /* bar's current top: 0 = shown, -bar_h = hidden            */
static int bar_linger, dock_linger;   /* reveal-linger counters for auto-hide               */

struct rect { int x, y, w, h; };
static struct rect dirty[MAXDIRTY];
static int ndirty;
static struct rect cur_clip;     /* the rect compose() is currently painting */
static int cur_x, cur_y;
static int cur_id = CUR_ARROW;   /* context-aware cursor shape (CUR_*) */
static uint32_t g_accent;        /* focus accent colour, from the registry (theme.accent) */
static int busy_until, busy_frame, launch_busy;  /* spinner cursor while an app launches */

/* A single in-flight window animation: a translucent rounded-rect "ghost" that
 * scales between a window's geometry and its dock tile -- minimize collapses the
 * window into the dock, clicking the tile restores it. The real window is hidden
 * (min flag) while its ghost animates. */
enum { AN_NONE, AN_MIN, AN_RESTORE };
static struct { int kind, slot, frame, nframes; struct rect from, to, cur, prev; } anim;

/* Single-instance "summon": when a launch is in flight (the window hasn't mapped
 * yet) we remember which app so a second Super-chord doesn't fork a duplicate. */
static int  pending_until;           /* frame deadline for the in-flight launch */
static char pending_title[32];       /* the window title we expect it to map     */

/* Alt-Tab-style window switcher (interim Super+Tab). A press cycles focus through
 * the open windows in most-recently-used order. Because every focus change in this
 * compositor also raises the window, the z-order list (zo[], top == most recent)
 * IS the MRU stack -- so a switch SESSION snapshots that order once and steps a
 * cursor through the snapshot, raising each window as it goes. Snapshotting keeps
 * the cycle stable while we reorder zo underneath it; the session ends (and the
 * landed window becomes MRU-top for free) once the deadline lapses with no press. */
#define SWITCH_LINGER 80             /* frames a switch session stays "warm" (~1s) */
static int sw_order[MAXW], sw_n, sw_pos, sw_until;

/* Control Center: a quick-settings slide-over opened from a menu-bar status item
 * (design/ui.md). Live toggles for the dock/bar auto-hide (the registry keys
 * update_chrome already reads) + a system summary + power actions. */
#define CC_PAD 14
#define CC_RAD       16     /* control-center panel corner radius          */
#define CC_SHADOW_SP 24     /* control-center drop-shadow feather (px)      */
#define CC_SHADOW_DY 6      /* control-center shadow vertical offset (px)   */
/* dirty the whole panel INCLUDING its shadow halo (so opening/closing it leaves
 * no shadow residue) -- the +DY accounts for the downward shadow offset. */
#define dirty_cc() add_dirty(cc_x - CC_SHADOW_SP, cc_y - CC_SHADOW_SP, \
                             cc_w + 2 * CC_SHADOW_SP, cc_h + 2 * CC_SHADOW_SP + CC_SHADOW_DY)
static int cc_open;
static int cc_x, cc_y, cc_w = 268, cc_h;          /* panel rect (h computed in cc_layout) */
static int cc_btn_x, cc_btn_w = 24;               /* the bar status-item hit box           */
static int cc_row1_y, cc_row2_y, cc_sep_y, cc_info_y, cc_btn_yy;
static void draw_cc_button(int y);                /* defined below; called from draw_bar */

/* ------------------------------------------------------------------ helpers */
static int streqz(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int wth(struct cwin *c) { return c->popup ? 0 : TH; }   /* title-bar height (popups have none) */
static int owf(struct cwin *c) { return (int)c->w; }
static int ohf(struct cwin *c) { return wth(c) + (int)c->h; }
static int find(int id) { for (int i = 0; i < MAXW; i++) if (cw[i].used && cw[i].id == id) return i; return -1; }
static int wslot(void)  { for (int i = 0; i < MAXW; i++) if (!cw[i].used) return i; return -1; }
static int find_snap(struct wmwin *s, int n, int id) { for (int i = 0; i < n; i++) if (s[i].id == id) return i; return -1; }

static int rects_hit(struct rect a, struct rect b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

/* z-order list ---------------------------------------------------------------*/
static int focus_slot(void) { return nz ? zo[nz - 1] : -1; }
static void zo_remove(int slot) {
    int k = -1; for (int i = 0; i < nz; i++) if (zo[i] == slot) { k = i; break; }
    if (k < 0) return;
    for (int i = k; i < nz - 1; i++) zo[i] = zo[i + 1];
    nz--;
}
static void zo_raise(int slot) { zo_remove(slot); zo[nz++] = slot; }   /* to the top */

/* dirty rectangles -----------------------------------------------------------*/
static int overlay_slot(void);                         /* defined near compose() */
static int box_hit(int x, int y, int w, int h, int bx, int by, int bw, int bh) {
    return x < bx + bw && bx < x + w && y < by + bh && by < y + h;
}
static void union_box(int *x, int *y, int *w, int *h, int bx, int by, int bw, int bh) {
    int x0 = *x < bx ? *x : bx, y0 = *y < by ? *y : by;
    int x1 = *x + *w > bx + bw ? *x + *w : bx + bw;
    int y1 = *y + *h > by + bh ? *y + *h : by + bh;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}
/* Frosted panels (bar/dock/control-center) must be invalidated WHOLE: ugfx_frost
 * re-blurs the entire panel region read from the back buffer, so a partial dirty
 * rect would re-blur the already-frosted pixels outside it and smear (the "drawing
 * happens when hovering the bar/dock" bug). Grow any rect that touches a frosted
 * panel to cover that whole panel (+ its shadow halo), so the frost always reads a
 * freshly-painted, un-frosted backdrop. Single source for these extents. */
static void expand_to_panels(int *x, int *y, int *w, int *h) {
    if (bar_y > -bar_h && box_hit(*x, *y, *w, *h, 0, bar_y, W, bar_h))
        union_box(x, y, w, h, 0, bar_y, W, bar_h);
    int dx = dock_x - DOCK_SH, dy = dock_y - DOCK_SH, dw = dock_w + 2 * DOCK_SH, dh = dock_h + 2 * DOCK_SH;
    if (dock_w > 0 && box_hit(*x, *y, *w, *h, dx, dy, dw, dh))
        union_box(x, y, w, h, dx, dy, dw, dh);
    if (cc_open) {
        int cx = cc_x - CC_SHADOW_SP, cy = cc_y - CC_SHADOW_SP;
        int cw = cc_w + 2 * CC_SHADOW_SP, ch = cc_h + 2 * CC_SHADOW_SP + CC_SHADOW_DY;
        if (box_hit(*x, *y, *w, *h, cx, cy, cw, ch)) union_box(x, y, w, h, cx, cy, cw, ch);
    }
    int ov = overlay_slot();                            /* Launchpad: also frosted */
    if (ov >= 0) {
        struct cwin *o = &cw[ov];
        int ox = o->wx - TH_SHADOW_SP, oy = o->wy - TH_SHADOW_SP;
        int ow = owf(o) + 2 * TH_SHADOW_SP, oh = ohf(o) + 2 * TH_SHADOW_SP;
        if (box_hit(*x, *y, *w, *h, ox, oy, ow, oh)) union_box(x, y, w, h, ox, oy, ow, oh);
    }
}
static void add_dirty(int x, int y, int w, int h) {
    expand_to_panels(&x, &y, &w, &h);                  /* frosted panels invalidate whole */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    struct rect r = { x, y, w, h };
    for (int i = 0; i < ndirty; i++) {                 /* merge into an overlapping rect */
        if (rects_hit(dirty[i], r)) {
            int x0 = dirty[i].x < x ? dirty[i].x : x;
            int y0 = dirty[i].y < y ? dirty[i].y : y;
            int x1 = dirty[i].x + dirty[i].w > x + w ? dirty[i].x + dirty[i].w : x + w;
            int y1 = dirty[i].y + dirty[i].h > y + h ? dirty[i].y + dirty[i].h : y + h;
            dirty[i].x = x0; dirty[i].y = y0; dirty[i].w = x1 - x0; dirty[i].h = y1 - y0;
            return;
        }
    }
    if (ndirty < MAXDIRTY) { dirty[ndirty++] = r; return; }
    for (int i = 1; i < ndirty; i++) {                 /* full: collapse everything into [0] */
        int x0 = dirty[0].x < dirty[i].x ? dirty[0].x : dirty[i].x;
        int y0 = dirty[0].y < dirty[i].y ? dirty[0].y : dirty[i].y;
        int x1 = dirty[0].x + dirty[0].w > dirty[i].x + dirty[i].w ? dirty[0].x + dirty[0].w : dirty[i].x + dirty[i].w;
        int y1 = dirty[0].y + dirty[0].h > dirty[i].y + dirty[i].h ? dirty[0].y + dirty[0].h : dirty[i].y + dirty[i].h;
        dirty[0].x = x0; dirty[0].y = y0; dirty[0].w = x1 - x0; dirty[0].h = y1 - y0;
    }
    ndirty = 1;
    add_dirty(x, y, w, h);
}
static void dirty_win(struct cwin *c) {                /* a window incl. its shadow ring */
    add_dirty(c->wx - TH_SHADOW_SP, c->wy - TH_SHADOW_SP,
              owf(c) + 2 * TH_SHADOW_SP, ohf(c) + 2 * TH_SHADOW_SP);
}

/* ------------------------------------------------------------------ desktop */
static uint32_t desk_color(int y) {
    int den = H > 1 ? H - 1 : 1;
    int r = gtr + (gbr - gtr) * y / den;
    int g = gtg + (gbg - gtg) * y / den;
    int b = gtb + (gbb - gtb) * y / den;
    return RGB(r, g, b);
}
/* The wallpaper is precomputed once into `wall` (below) so draw_desk is a fast
 * copy; if that alloc failed we fall back to the plain per-row gradient. */
static void draw_desk(int x, int y, int w, int h) {
    if (wall) { ugfx_blit(x, y, wall + (long)y * W + x, w, h, W); return; }
    for (int j = y; j < y + h; j++) ugfx_fill(x, j, w, 1, desk_color(j));  /* clipped */
}

static int clampb(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Render the desktop once: the vertical slate gradient, plus a soft cool glow in
 * the upper third and a gentle corner vignette for depth -- the "floating" look
 * without leaving our palette. Quadratic falloffs, integer only (no FPU). */
static void gen_wallpaper(void) {
    wall = (uint32_t *)mmap_((unsigned long)W * (unsigned long)H * 4);
    if (!wall) return;
    int gx = W / 2, gy = (H * 9) / 25;                       /* glow centre (~upper third) */
    int gr = (H * 6) / 10; long gR2 = (long)gr * gr;         /* glow radius^2 */
    int cx = W / 2, cy = H / 2; long vR2 = (long)cx * cx + (long)cy * cy;   /* vignette */
    for (int py = 0; py < H; py++) {
        uint32_t base = desk_color(py);
        int br = (base >> 16) & 0xff, bg = (base >> 8) & 0xff, bbl = base & 0xff;
        for (int px = 0; px < W; px++) {
            long dx = px - gx, dy = py - gy, d2 = dx * dx + dy * dy;
            int glow = d2 < gR2 ? (int)((gR2 - d2) * 48 / gR2) : 0;     /* 0..48 dome   */
            long vx = px - cx, vy = py - cy;
            int vig = (int)(((long)vx * vx + (long)vy * vy) * 26 / vR2); /* 0..26 darken */
            int r = clampb(br + glow * 6 / 10 - vig);
            int g = clampb(bg + glow * 8 / 10 - vig);
            int b = clampb(bbl + glow + glow / 3 - vig);                 /* glow leans blue */
            wall[(long)py * W + px] = RGB(r, g, b);
        }
    }
}

/* ------------------------------------------------------------------ top bar */
static void two(char *p, unsigned v) { p[0] = '0' + (v / 10) % 10; p[1] = '0' + v % 10; }

/* Day-of-week from a date, via Zeller's congruence (no calendar table). */
static const char *weekday(int y, int m, int d) {
    static const char *names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    if (m < 3) { m += 12; y -= 1; }                 /* Jan/Feb count as months 13/14 of the prior year */
    int k = y % 100, j = y / 100;
    int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;  /* 0=Sat .. 6=Fri */
    return names[(h + 6) % 7];                       /* remap to 0=Sun .. 6=Sat */
}

/* The bar is drawn at its current slide offset bar_y (0 shown, -bar_h hidden). The
 * desktop/window behind it is already painted by compose(), so the translucent
 * glass blends over it -- no internal draw_desk needed. */
static void draw_bar(void) {
    int y = bar_y;
    ugfx_frost(0, y, W, bar_h, 0, TH_BAR_FROST);        /* frosted-glass bar         */
    ugfx_fill_a(0, y, W, 1, ARGB(26, 255, 255, 255));   /* lit top edge (glass)      */
    ugfx_fill_a(0, y + bar_h - 1, W, 1, TH_BARLINE_A);  /* hairline                  */
    ugfx_blit_argb(14, y + (bar_h - LOGO_H) / 2, LOGO_W, LOGO_H, logo_argb);
    int f = focus_slot();
    const char *app = (f >= 0) ? cw[f].title : "tOS";
    ugfx_text(14 + LOGO_W + 12, y + (bar_h - fh) / 2, app, TH_TEXT, UGFX_TRANSPARENT);
    struct rtctime t; rtc_time(&t);
    char clk[16];                                       /* "Ddd HH:MM:SS" -- a macOS-style menu-bar clock */
    const char *wd = weekday(t.year, t.month, t.day);
    clk[0] = wd[0]; clk[1] = wd[1]; clk[2] = wd[2]; clk[3] = ' ';
    two(clk + 4, t.hour); clk[6] = ':'; two(clk + 7, t.min); clk[9] = ':'; two(clk + 10, t.sec); clk[12] = 0;
    ugfx_text(W - ugfx_text_w(clk) - 16, y + (bar_h - fh) / 2, clk, TH_TEXT, UGFX_TRANSPARENT);
    draw_cc_button(y);                                  /* control-center status item */
}

/* ------------------------------------------------------------------ dock */
static int title_is(const char *title, const char *label) {   /* window title starts with label */
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
static int dock_tile_for(const char *title) {       /* dock icon whose label matches a window */
    for (int i = 0; i < nicons; i++) if (title_is(title, icons[i].label)) return i;
    return -1;
}
static int find_app_window(const char *label) {     /* a matching window slot (prefer minimized) */
    int any = -1;
    for (int i = 0; i < MAXW; i++)
        if (cw[i].used && title_is(cw[i].title, label)) { if (cw[i].min) return i; any = i; }
    return any;
}
static int tile_hovered(struct icon *ic) {
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
    else if (ic->img) ugfx_blit_argb(x, y, ic->iw, ic->ih, ic->img);   /* the bundle's own icon */
    else              ugfx_blit_argb(x, y, APPICON_SZ, APPICON_SZ, appicons_argb[ICON_APP]);  /* generic fallback */
    int iy = y + APPICON_SZ + 3;                    /* running indicator under the tile */
    if (st & 2)        ugfx_rrect_a(ic->cx - 9, iy, 18, 3, 1, g_accent);   /* focused: accent bar */
    else if (st & 1)   ugfx_rrect_a(ic->cx - 2, iy, 4, 3, 1, ARGB(160, 200, 210, 230));     /* running: dot */
}
static void draw_dock(void) {
    ugfx_elevation(dock_x, dock_y, dock_w, dock_h, TH_DOCK_RAD, 3);          /* float it off the desktop */
    ugfx_frost(dock_x, dock_y, dock_w, dock_h, TH_DOCK_RAD, TH_DOCK_FROST);  /* frosted-glass panel      */
    ugfx_rrect_border(dock_x, dock_y, dock_w, dock_h, TH_DOCK_RAD, 1, TH_BORDER_DIM);  /* crisp edge     */
    ugfx_fill_a(dock_x + TH_DOCK_RAD, dock_y, dock_w - 2 * TH_DOCK_RAD, 1, TH_DOCK_HI_A);  /* top sheen   */
    for (int i = 0; i < nicons; i++) draw_tile(&icons[i]);
}

/* ------------------------------------------------------------------ windows */
/* blit a window-control button (baked macOS-style traffic light) centred at (cx,cy) */
static void blit_btn(int cx, int cy, int idx) {
    ugfx_blit_argb(cx - WINBTN_W / 2, cy - WINBTN_H / 2, WINBTN_W, WINBTN_H, winbtns_argb[idx]);
}

static void draw_window(int slot) {
    struct cwin *c = &cw[slot];
    int ow = owf(c), oh = ohf(c), wx = c->wx, wy = c->wy;
    struct rect wb = { wx - TH_SHADOW_SP, wy - TH_SHADOW_SP, ow + 2 * TH_SHADOW_SP, oh + 2 * TH_SHADOW_SP };
    if (!rects_hit(cur_clip, wb)) return;               /* nothing of this window in the rect */
    int foc = (focus_slot() == slot);
    if (c->popup) {                                     /* borderless overlay: surface + shadow only */
        ugfx_shadow(wx, wy + 6, ow, oh, TH_RADIUS, TH_SHADOW_SP, TH_SHADOW, TH_SHADOW_A);
        ugfx_blit_round(wx, wy, (const uint32_t *)c->vaddr, (int)c->w, (int)c->h, (int)c->w, TH_RADIUS);
        ugfx_rrect_border(wx, wy, ow, oh, TH_RADIUS, 1, TH_BORDER);
        return;
    }
    ugfx_shadow(wx, wy + 5, ow, oh, TH_RADIUS, TH_SHADOW_SP, TH_SHADOW, foc ? TH_SHADOW_A : TH_SHADOW_A2);
    ugfx_rrect_aa(wx, wy, ow, oh, TH_RADIUS, TH_CHROME);                       /* frame + title bar */
    ugfx_fill_a(wx + TH_RADIUS, wy, ow - 2 * TH_RADIUS, 1, ARGB(40, 255, 255, 255));  /* title sheen */
    ugfx_blit_round_bottom(wx, wy + TH, (const uint32_t *)c->vaddr,            /* client surface    */
                           (int)c->w, (int)c->h, (int)c->w, TH_RADIUS);
    ugfx_fill_a(wx, wy + TH - 1, ow, 1, ARGB(70, 0, 0, 0));                    /* title divider     */
    int ly = wy + TH / 2;                                                     /* controls, top-right */
    int cxc = wx + ow - 18, cxx = cxc - 22, cxm = cxx - 22;                    /* close / max / min */
    blit_btn(cxm, ly, foc ? WB_MIN   : WB_INACTIVE);
    blit_btn(cxx, ly, foc ? WB_MAX   : WB_INACTIVE);
    blit_btn(cxc, ly, foc ? WB_CLOSE : WB_INACTIVE);
    int tw = ugfx_text_w(c->title);
    int tx = wx + (ow - tw) / 2;                                              /* centred, kept clear of the buttons */
    if (tx + tw > cxm - WINBTN_W / 2 - 10) tx = cxm - WINBTN_W / 2 - 10 - tw;
    if (tx < wx + 14) tx = wx + 14;
    ugfx_text(tx, wy + (TH - fh) / 2, c->title, foc ? TH_TEXT : TH_MUTED, UGFX_TRANSPARENT);
    ugfx_rrect_border(wx, wy, ow, oh, TH_RADIUS, 1, foc ? TH_BORDER : TH_BORDER_DIM);  /* crisp frame ring */
}
static void clampw(struct cwin *c) {
    if (c->maxed) { c->wx = 0; c->wy = 0; return; }   /* fullscreen owns the whole screen */
    int ow = owf(c), oh = ohf(c);
    if (c->wx + ow > W) c->wx = W - ow;
    if (c->wy + oh > H) c->wy = H - oh;
    if (c->wx < 0) c->wx = 0;
    if (c->wy < bar_h + 8) c->wy = bar_h + 8;
}

/* macOS-style fullscreen/restore: the green control fills the WHOLE screen (the
 * bar + dock auto-hide via update_chrome) and toggles back to the floating
 * geometry. The app owns its surface, so we ask it to resize (WEV_RESIZE); twm
 * only positions the window. We dirty both the old and new spot so the move shows
 * even before the app resizes next frame. */
static void toggle_max(int slot) {
    struct cwin *c = &cw[slot];
    dirty_win(c);
    if (!c->maxed) {
        c->sx = c->wx; c->sy = c->wy; c->sw = c->w; c->sh = c->h;   /* remember the floating geometry */
        c->maxed = 1;
        c->wx = 0; c->wy = 0;                           /* fullscreen: fill the screen */
        int nw = W, nh = H - TH;                        /* client below the window's own title bar */
        wm_post(c->id, WEV_RESIZE, ((unsigned)nw << 16) | (unsigned)nh);
    } else {
        c->maxed = 0;
        c->wx = c->sx; c->wy = c->sy;
        wm_post(c->id, WEV_RESIZE, ((unsigned)c->sw << 16) | (unsigned)c->sh);
    }
    dirty_win(c);
}

/* ------------------------------------------------------------ animation */
static void anim_start(int kind, int slot, struct rect from, struct rect to, int n) {
    anim.kind = kind; anim.slot = slot; anim.frame = 0; anim.nframes = n;
    anim.from = from; anim.to = to; anim.cur = from; anim.prev = from;
}
static struct rect anim_ghost_box(void) {           /* ghost rect padded for its border */
    struct rect g = anim.cur;
    g.x -= 3; g.y -= 3; g.w += 6; g.h += 6; return g;
}
static void draw_ghost(void) {
    if (!anim.kind) return;
    struct rect g = anim.cur;
    if (!rects_hit(cur_clip, g)) return;
    int rad = g.h < 24 ? (g.h / 3) : TH_RADIUS;
    ugfx_rrect_a(g.x, g.y, g.w, g.h, rad, ARGB(175, 56, 62, 82));         /* translucent chrome */
    ugfx_fill_a(g.x + rad, g.y, g.w - 2 * rad, 1, ARGB(120, 150, 170, 230));  /* lit top edge */
}
/* advance the genie animation one frame; dirties the regions it touches */
static void anim_step(void) {
    if (!anim.kind) return;
    anim.prev = anim.cur;
    anim.frame++;
    if (anim.frame >= anim.nframes) {                /* land */
        if (anim.kind == AN_RESTORE) {
            cw[anim.slot].min = 0; zo_raise(anim.slot);
            dirty_win(&cw[anim.slot]); add_dirty(0, 0, W, bar_h);
        }
        struct rect b = anim_ghost_box(); add_dirty(b.x, b.y, b.w, b.h);
        anim.kind = AN_NONE;
        return;
    }
    int num = anim.frame, den = anim.nframes;
    anim.cur.x = anim.from.x + (anim.to.x - anim.from.x) * num / den;
    anim.cur.y = anim.from.y + (anim.to.y - anim.from.y) * num / den;
    anim.cur.w = anim.from.w + (anim.to.w - anim.from.w) * num / den;
    anim.cur.h = anim.from.h + (anim.to.h - anim.from.h) * num / den;
    struct rect b = { anim.prev.x - 3, anim.prev.y - 3, anim.prev.w + 6, anim.prev.h + 6 };
    add_dirty(b.x, b.y, b.w, b.h);
    b = anim_ghost_box(); add_dirty(b.x, b.y, b.w, b.h);
}

/* ------------------------------------------------------------ control center */
static void cc_layout(void) {
    cc_x = W - cc_w - 12;
    cc_y = bar_h + 8;
    int yy = cc_y + 14 + fh + 12;                 /* below the "Control Center" title */
    cc_row1_y = yy;
    cc_row2_y = yy + 36;
    cc_sep_y  = yy + 72;
    cc_info_y = cc_sep_y + 12;
    cc_btn_yy = cc_info_y + 3 * (fh + 4) + 12;
    cc_h = (cc_btn_yy - cc_y) + 32 + CC_PAD;
    cc_btn_x = W - 168;                           /* status item, left of the clock */
}
static void cc_toggle(const char *key) {
    int v = reg_bool(key, 0);
    reg_set(key, v ? "false" : "true");           /* update_chrome reads reg_bool live */
    reg_save();
}
static int cc_pt_in(int x, int y, int w, int h) {   /* is the cursor inside this rect? */
    return cur_x >= x && cur_x < x + w && cur_y >= y && cur_y < y + h;
}
static void draw_switch(int rowy, const char *label, int on) {
    int rx = cc_x + CC_PAD - 6, rw = cc_w - 2 * CC_PAD + 12;     /* full-width hit/hover row */
    if (cc_pt_in(rx, rowy - 2, rw, 28)) ugfx_state_layer(rx, rowy - 2, rw, 28, TH_R_SM, TH_HOVER_A);
    ugfx_text(cc_x + CC_PAD, rowy + (24 - fh) / 2, label, TH_TEXT, UGFX_TRANSPARENT);
    /* An iOS-style switch: an opaque pill track (accent when on, a clearly LIT slate
     * track when off so it reads as a flippable control, not a flat dark blob) with a
     * white knob that slides + pops via a soft shadow and a thin rim. */
    int pw = 46, ph = 24, px = cc_x + cc_w - CC_PAD - pw, py = rowy;
    ugfx_rrect_aa(px, py, pw, ph, ph / 2, on ? (g_accent & 0xffffff) : RGB(88, 96, 116));
    ugfx_rrect_border(px, py, pw, ph, ph / 2, 1, ARGB(85, 0, 0, 0));          /* recessed rim */
    int kr = ph - 6, kx = on ? px + pw - kr - 3 : px + 3, ky = py + 3;
    ugfx_shadow(kx, ky + 1, kr, kr, kr / 2, 4, TH_SHADOW, 100);               /* knob lift */
    ugfx_rrect_aa(kx, ky, kr, kr, kr / 2, RGB(255, 255, 255));
    ugfx_rrect_border(kx, ky, kr, kr, kr / 2, 1, ARGB(45, 0, 0, 0));
}
static void cc_button(int bx, int by, int bw, int bh, const char *label, uint32_t col) {
    ugfx_rrect_a(bx, by, bw, bh, TH_R_SM, col);
    if (cc_pt_in(bx, by, bw, bh)) ugfx_state_layer(bx, by, bw, bh, TH_R_SM, TH_HOVER_A);
    ugfx_rrect_border(bx, by, bw, bh, TH_R_SM, 1, TH_BORDER);
    ugfx_text(bx + (bw - ugfx_text_w(label)) / 2, by + (bh - fh) / 2, label, TH_TEXT, UGFX_TRANSPARENT);
}
static void draw_cc(void) {
    if (!cc_open) return;
    /* The early-out must cover the SHADOW halo, not just the panel: the shadow
     * feathers CC_SHADOW_SP px outside the panel (offset CC_SHADOW_DY down), so a
     * damage rect that clips only the halo (e.g. the cursor gliding past an edge)
     * still has to repaint it -- otherwise compose() paints the desktop over the
     * shadow and we never redraw it, leaving a moving "hole" trailing the cursor.
     * (Same fix draw_window() already applies via its TH_SHADOW_SP-padded box.) */
    struct rect r = { cc_x - CC_SHADOW_SP, cc_y - CC_SHADOW_SP,
                      cc_w + 2 * CC_SHADOW_SP, cc_h + 2 * CC_SHADOW_SP + CC_SHADOW_DY };
    if (!rects_hit(cur_clip, r)) return;
    ugfx_shadow(cc_x, cc_y + CC_SHADOW_DY, cc_w, cc_h, CC_RAD, CC_SHADOW_SP, TH_SHADOW, 130);
    ugfx_frost(cc_x, cc_y, cc_w, cc_h, CC_RAD, TH_CC_FROST);             /* frosted-glass panel */
    ugfx_rrect_border(cc_x, cc_y, cc_w, cc_h, CC_RAD, 1, TH_BORDER_DIM);                /* crisp edge */
    ugfx_fill_a(cc_x + CC_RAD, cc_y, cc_w - 2 * CC_RAD, 1, ARGB(40, 255, 255, 255));    /* top sheen  */
    ugfx_text(cc_x + CC_PAD, cc_y + 14, "Control Center", TH_TEXT, UGFX_TRANSPARENT);
    draw_switch(cc_row1_y, "Auto-hide Dock", reg_bool("ui.dock.autohide", 0));
    draw_switch(cc_row2_y, "Auto-hide Bar",  reg_bool("ui.bar.autohide", 0));
    ugfx_fill_a(cc_x + CC_PAD, cc_sep_y, cc_w - 2 * CC_PAD, 1, ARGB(46, 150, 170, 230));
    struct sysinfo si; sysinfo(&si);
    char buf[48]; int y = cc_info_y;
    snprintf(buf, sizeof buf, "CPUs: %u", si.ncpu);                      ugfx_text(cc_x + CC_PAD, y, buf, TH_MUTED, UGFX_TRANSPARENT); y += fh + 4;
    snprintf(buf, sizeof buf, "RAM: %u MB", (unsigned)(si.ram_bytes >> 20)); ugfx_text(cc_x + CC_PAD, y, buf, TH_MUTED, UGFX_TRANSPARENT); y += fh + 4;
    snprintf(buf, sizeof buf, "Uptime: %us", (unsigned)(si.uptime_ticks / TIMER_HZ)); ugfx_text(cc_x + CC_PAD, y, buf, TH_MUTED, UGFX_TRANSPARENT);
    int bw = (cc_w - 2 * CC_PAD - 10) / 2, bx = cc_x + CC_PAD, bh = 32;
    cc_button(bx, cc_btn_yy, bw, bh, "Restart", ARGB(255, 60, 66, 84));
    cc_button(bx + bw + 10, cc_btn_yy, bw, bh, "Shut Down", ARGB(255, 200, 76, 70));
}
static void draw_cc_button(int y) {               /* the menu-bar status item (two toggle pills) */
    int cy = y + bar_h / 2;
    for (int i = 0; i < 2; i++) {
        int py = cy - 5 + i * 7, on = i == 0;
        ugfx_rrect_a(cc_btn_x, py, 18, 5, 2, cc_open ? g_accent : ARGB(150, 200, 210, 230));
        ugfx_fill_a(on ? cc_btn_x + 11 : cc_btn_x + 2, py, 5, 5, RGB(235, 240, 248));
    }
}
static void cc_click(int mx, int my) {            /* a click inside the open panel */
    if (my >= cc_row1_y && my < cc_row1_y + 24) { cc_toggle("ui.dock.autohide"); return; }
    if (my >= cc_row2_y && my < cc_row2_y + 24) { cc_toggle("ui.bar.autohide");  return; }
    if (my >= cc_btn_yy && my < cc_btn_yy + 32) {
        int bw = (cc_w - 2 * CC_PAD - 10) / 2, bx = cc_x + CC_PAD;
        if (mx < bx + bw) reboot();
        else shutdown();
    }
}

/* ------------------------------------------------------------ compose/present */
/* A visible WIN_OVERLAY window (Launchpad) -> dimmed full-screen, drawn above the
 * dock. -1 if none. */
static int overlay_slot(void) {
    for (int i = 0; i < MAXW; i++) if (cw[i].used && cw[i].overlay && !cw[i].min) return i;
    return -1;
}
static void compose(struct rect r) {
    cur_clip = r;
    ugfx_set_clip(r.x, r.y, r.w, r.h);
    draw_desk(r.x, r.y, r.w, r.h);
    int ov = overlay_slot();
    for (int i = 0; i < nz; i++) if (zo[i] != ov) draw_window(zo[i]);   /* back to front (overlay drawn last) */
    struct rect barr = { 0, 0, W, bar_h };
    /* the dock hit-test must include its shadow halo, else a rect that clips only
     * the halo (a window edge or the cursor sliding past) repaints the desktop over
     * the shadow without redrawing it -- the same residue bug fixed for draw_cc. */
    struct rect dckr = { dock_x - DOCK_SH, dock_y - DOCK_SH, dock_w + 2 * DOCK_SH, dock_h + 2 * DOCK_SH };
    if (bar_y > -bar_h && rects_hit(r, barr)) draw_bar();
    if (rects_hit(r, dckr)) draw_dock();
    draw_cc();                                          /* control-center panel, over windows + dock */
    if (ov >= 0) {                                      /* Launchpad: dim the screen, frosted glass panel */
        struct cwin *o = &cw[ov];
        int ox = o->wx, oy = o->wy, ow = owf(o), oh = ohf(o);
        ugfx_fill_a(r.x, r.y, r.w, r.h, ARGB(120, 6, 8, 12));            /* dim scrim (lighter, so the blur has content) */
        ugfx_shadow(ox, oy + 6, ow, oh, TH_RADIUS, TH_SHADOW_SP, TH_SHADOW, TH_SHADOW_A);
        ugfx_frost(ox, oy, ow, oh, TH_RADIUS, TH_OV_FROST);             /* frosted backdrop behind the grid */
        ugfx_blit_round_key(ox, oy, (const uint32_t *)o->vaddr, (int)o->w, (int)o->h,
                            (int)o->w, TH_RADIUS, TH_FROST_KEY);        /* content; sentinel bg lets the frost show */
        ugfx_rrect_border(ox, oy, ow, oh, TH_RADIUS, 1, TH_BORDER);
    }
    draw_ghost();                                       /* minimize/restore genie, over everything */
    ugfx_blit_argb(cur_x - cursor_hotspot[cur_id][0], cur_y - cursor_hotspot[cur_id][1],
                   CURSOR_W, CURSOR_H, cursors_argb[cur_id]);   /* always on top, clipped */
    ugfx_clip_none();
}
static void flush_dirty(void) {
    for (int i = 0; i < ndirty; i++) {
        compose(dirty[i]);
        ugfx_present_rect(bb, dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h);
    }
    ndirty = 0;
}

/* ------------------------------------------------------------------ /Apps */
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
static void load_apps(void) {
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
            (streqz(val, "false") || streqz(val, "0") || streqz(val, "no"))) a->pinned = 0;
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
static int app_for_title(const char *title) {
    for (int i = 0; i < napps; i++) if (title_is(title, apps[i].label)) return i;
    return -1;
}
/* Compose the visible dock: the leftmost Launchpad button, then every pinned app,
 * then a transient tile for each running (incl. minimized) non-popup window whose
 * app isn't already shown -- so an unpinned app like Notepad (opened from Files or
 * Spotlight) appears in the dock while it runs and drops off when it closes. */
static void rebuild_dock(void) {
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
}
/* A cheap hash of the running non-popup window set (by id); the dock rebuilds +
 * re-lays-out only when this changes, so layout_dock's serial trace and the
 * recentre fire on open/close, not every frame. */
static unsigned running_sig(void) {
    unsigned h = 2166136261u ^ (unsigned)napps;
    for (int w = 0; w < MAXW; w++)
        if (cw[w].used && !cw[w].popup) h = (h ^ (unsigned)(cw[w].id + 1)) * 16777619u;
    return h;
}
/* Position the dock tiles for the current dock_y (recomputed as the dock slides). */
static void place_dock_icons(void) {
    for (int i = 0; i < nicons; i++) {
        icons[i].cx = dock_x + DOCK_PAD + TH_TILE / 2 + i * (TH_TILE + DOCK_GAP);
        icons[i].cy = dock_y + DOCK_PAD + TH_TILE / 2;
    }
}
static void layout_dock(void) {
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
}

static void launch(const char *prog) {
    int pid = fork();
    if (pid == 0) { exec(prog); proc_exit(); }
    launch_busy = 1;                                    /* show the spinner cursor until the window appears */
}

static int any_fullscreen(void) {
    for (int i = 0; i < MAXW; i++) if (cw[i].used && cw[i].maxed && !cw[i].min) return 1;
    return 0;
}

/* Raise + focus a window (no-op if it is already focused), dirtying both the old
 * and new window plus the bar so their focused/unfocused chrome updates at once.
 * Shared by the dock, the window switcher and summon. */
static void focus_window(int slot) {
    int prev = focus_slot();
    if (prev == slot) return;
    zo_raise(slot);
    dirty_win(&cw[slot]);
    if (prev >= 0 && cw[prev].used) dirty_win(&cw[prev]);
    add_dirty(0, 0, W, bar_h);
}

/* Begin the genie that floats a minimized window back out of its dock tile. */
static void restore_window(int slot) {
    int ti = dock_tile_for(cw[slot].title);
    struct rect to   = { cw[slot].wx, cw[slot].wy, owf(&cw[slot]), ohf(&cw[slot]) };
    struct rect from = (ti >= 0)
        ? (struct rect){ icons[ti].cx - 18, icons[ti].cy - 14, 36, 28 }
        : (struct rect){ cw[slot].wx + (int)cw[slot].w / 2 - 18, cw[slot].wy, 36, 28 };
    anim_start(AN_RESTORE, slot, from, to, 12);
}

/* Single-instance launch: if a window of this app already exists, raise/restore
 * it; otherwise fork+exec the program. A short pending guard keyed on the title
 * stops a second summon (e.g. Super+V pressed twice) from forking a duplicate in
 * the gap before the first window maps. Match is by title for now (the cleaner key
 * later is a stable per-app id in the window metadata -- see NEXT_STEPS). */
static void summon(const char *title, const char *prog, int frame) {
    int ws = find_app_window(title);          /* prefers a minimized match */
    if (ws >= 0) {
        if (cw[ws].min) restore_window(ws);
        else            focus_window(ws);
        return;
    }
    if (pending_until && title_is(pending_title, title)) return;   /* launch already in flight */
    launch(prog);
    int i = 0; for (; title[i] && i < 31; i++) pending_title[i] = title[i]; pending_title[i] = 0;
    pending_until = frame + 120;              /* ~1.5s to let the window map */
}

/* getkey() with a one-byte pushback, so the input loop can peek the byte after an
 * ESC -- to tell a standalone Esc (dismiss popup) from the start of a CSI/SS3
 * nav-key escape sequence (arrows etc.), which must reach the focused app. */
static int twm_key_pb = -1;
static int twm_getkey(void) {
    if (twm_key_pb >= 0) { int k = twm_key_pb; twm_key_pb = -1; return k; }
    return getkey();
}
static void twm_ungetkey(int k) { twm_key_pb = k; }

/* Super+Tab: step the MRU window switcher (see the sw_* note up top). */
static void window_switch(int frame) {
    if (sw_until == 0 || frame >= sw_until) {     /* (re)start a session: snapshot MRU = reverse z */
        sw_n = 0;
        for (int i = nz - 1; i >= 0; i--) if (!cw[zo[i]].popup) sw_order[sw_n++] = zo[i];  /* [0]=top, skip overlays */
        sw_pos = 0;
    }
    sw_until = frame + SWITCH_LINGER;
    if (sw_n < 2) return;                          /* nothing to switch to */
    for (int step = 0; step < sw_n; step++) {      /* advance to the next still-valid window */
        sw_pos = (sw_pos + 1) % sw_n;
        int slot = sw_order[sw_pos];
        if (slot >= 0 && slot < MAXW && cw[slot].used && !cw[slot].min && !cw[slot].popup) { focus_window(slot); return; }
    }
}

/* macOS-style auto-hide. When a window is fullscreen -- or the user enabled
 * ui.bar.autohide / ui.dock.autohide -- the bar and dock slide off-screen and
 * reveal when the cursor reaches the top/bottom edge, retracting a moment after it
 * leaves (HIDE_LINGER). Each animates an offset (bar_y, dock_y) and dirties its
 * band so the compositor repaints only the moving strip. Default (no fullscreen,
 * autohide off) keeps both permanently shown -- no behaviour change. */
static void update_chrome(int mx, int my) {
    int fs = any_fullscreen();
    int auto_bar  = fs || reg_bool("ui.bar.autohide", 0);
    int auto_dock = fs || reg_bool("ui.dock.autohide", 0);

    int want_bar;
    if (!auto_bar) want_bar = 1;
    else {
        /* Peek only while the cursor is at the very top edge (then linger briefly).
         * We deliberately do NOT hold it open while the cursor sits just below the
         * edge, so a fullscreen window's own title-bar controls (just under the
         * bar) stay clickable. */
        if (my < EDGE) bar_linger = HIDE_LINGER;
        else if (bar_linger > 0) bar_linger--;
        want_bar = bar_linger > 0;
    }
    int bt = want_bar ? 0 : -bar_h;
    if (bar_y < bt)      { bar_y += SLIDE; if (bar_y > bt) bar_y = bt; add_dirty(0, 0, W, bar_h); }
    else if (bar_y > bt) { bar_y -= SLIDE; if (bar_y < bt) bar_y = bt; add_dirty(0, 0, W, bar_h); }

    int want_dock;
    if (!auto_dock) want_dock = 1;
    else {
        if (my >= H - EDGE) dock_linger = HIDE_LINGER;           /* at the bottom edge: reveal */
        else if (dock_y == dock_y0 && my >= dock_y0 && mx >= dock_x && mx < dock_x + dock_w)
            dock_linger = HIDE_LINGER;                           /* cursor on the shown dock */
        else if (dock_linger > 0) dock_linger--;
        want_dock = dock_linger > 0;
    }
    int dt = want_dock ? dock_y0 : H;
    if (dock_y != dt) {
        if (dock_y < dt) { dock_y += SLIDE; if (dock_y > dt) dock_y = dt; }
        else             { dock_y -= SLIDE; if (dock_y < dt) dock_y = dt; }
        place_dock_icons();
        add_dirty(dock_x - DOCK_SH, dock_y0 - DOCK_SH,           /* travel band incl. shadow halo */
                  dock_w + 2 * DOCK_SH, H - dock_y0 + DOCK_SH);
    }
}

/* ------------------------------------------------------------------ main */
__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    if (ugfx_init() < 0) { print("[twm] no framebuffer\r\n"); exec("shell"); proc_exit(); }
    W = ugfx_width(); H = ugfx_height(); fh = ugfx_font_h();
    bar_h = fh + 14; TH = fh + 12;
    gtr = (TH_DESK_TOP >> 16) & 0xff; gtg = (TH_DESK_TOP >> 8) & 0xff; gtb = TH_DESK_TOP & 0xff;
    gbr = (TH_DESK_BOT >> 16) & 0xff; gbg = (TH_DESK_BOT >> 8) & 0xff; gbb = TH_DESK_BOT & 0xff;

    bb = (uint32_t *)mmap_((unsigned long)W * (unsigned long)H * 4);
    if (!bb) { print("[twm] back buffer alloc failed\r\n"); exec("shell"); proc_exit(); }
    ugfx_set_target(bb, W, H, W);
    gen_wallpaper();                                    /* precompute the desktop once */

    wm_register();
    reg_load();                                         /* system + per-user settings */
    g_accent = reg_color("theme.accent", ARGB(235, 120, 170, 255));
    load_apps();
    rebuild_dock();
    layout_dock();
    dock_sig = running_sig();
    cc_layout();

    add_dirty(0, 0, W, H);                              /* initial full paint */
    flush_dirty();
    print("[twm] desktop ready\r\n");                   /* the harness waits for this */

    struct mousestate ms;
    int last_b = 0, last_rb = 0, last_back = 0, last_fwd = 0, last_sec = -1, frame = 0;
    int last_focus = -1;
    int overlay_on = 0;              /* Launchpad-overlay presence; a full repaint on each edge paints/clears the scrim */
    int drag = -1, drag_dx = 0, drag_dy = 0;
    int rsz = -1, rsz_ox = 0, rsz_oy = 0;
    int cdrag = -1;                  /* a button-held drag inside a window's client area */
    int last_click_icon = -1, last_click_frame = -1000;
    int hover_id = -1;               /* window currently receiving hover-move events */
    unsigned last_dock_sig = 0;
    struct wmwin snap[MAXW];
    struct rtctime t;

    for (;;) {
        frame++;
        if (launch_busy) { busy_until = frame + 80; launch_busy = 0; }   /* ~1s spinner */
        if (busy_until && frame >= busy_until) busy_until = 0;

        /* --- reconcile the live window set ----------------------------------- */
        int n = wm_windows(snap, MAXW);
        for (int i = 0; i < MAXW; i++) {                /* windows that went away */
            if (!cw[i].used) continue;
            if (find_snap(snap, n, cw[i].id) < 0) {
                dirty_win(&cw[i]);
                zo_remove(i);
                cw[i].used = 0;
                add_dirty(0, 0, W, bar_h);              /* title text may change */
            }
        }
        for (int j = 0; j < n; j++) {
            int k = find(snap[j].id);
            if (k < 0) {                                /* a new window */
                k = wslot(); if (k < 0) continue;
                cw[k].used = 1; cw[k].id = snap[j].id; cw[k].seq = snap[j].seq;
                cw[k].w = snap[j].w; cw[k].h = snap[j].h; cw[k].vaddr = snap[j].vaddr;
                cw[k].min = 0; cw[k].maxed = 0;                  /* a reused slot must start clean */
                cw[k].popup = (snap[j].flags & WIN_POPUP) != 0;  /* borderless centred overlay */
                cw[k].overlay = (snap[j].flags & WIN_OVERLAY) != 0;  /* Launchpad: dim, above dock */
                for (int q = 0; q < 32; q++) cw[k].title[q] = snap[j].title[q];
                cw[k].wx = (W - owf(&cw[k])) / 2;
                cw[k].wy = bar_h + (H - bar_h - ohf(&cw[k])) / 2;
                clampw(&cw[k]);
                zo_raise(k);
                busy_until = 0;                         /* the launched app's window appeared */
                if (pending_until && title_is(cw[k].title, pending_title)) pending_until = 0;  /* summon landed */
                /* report the placed client rect so tests can drive a window by its
                 * real on-screen geometry (same idea as the "[twm] icon" dock trace);
                 * x/y are the client-area top-left (below the title bar). */
                print("[twm] win "); print(cw[k].title);
                printc(' '); printu((unsigned)cw[k].wx);
                printc(' '); printu((unsigned)(cw[k].wy + wth(&cw[k])));
                printc(' '); printu(cw[k].w); printc(' '); printu(cw[k].h);
                print("\r\n");
                dirty_win(&cw[k]);
                add_dirty(0, 0, W, bar_h);
            } else {
                cw[k].vaddr = snap[j].vaddr;
                if (snap[j].w != cw[k].w || snap[j].h != cw[k].h) {   /* resized */
                    dirty_win(&cw[k]);
                    cw[k].w = snap[j].w; cw[k].h = snap[j].h;
                    clampw(&cw[k]);
                    dirty_win(&cw[k]);
                    cw[k].seq = snap[j].seq;
                } else {
                    int tch = !streqz(cw[k].title, snap[j].title);
                    for (int q = 0; q < 32; q++) cw[k].title[q] = snap[j].title[q];
                    if (tch) { dirty_win(&cw[k]); add_dirty(0, 0, W, bar_h); }
                    if (snap[j].seq != cw[k].seq) {     /* the app redrew its surface */
                        cw[k].seq = snap[j].seq;
                        if (!cw[k].min)                 /* a minimized window is off-screen */
                            add_dirty(cw[k].wx, cw[k].wy + wth(&cw[k]), (int)cw[k].w, (int)cw[k].h);
                    }
                }
            }
        }

        /* --- dock: rebuild + recentre when the running set changes ----------- */
        unsigned rsig = running_sig();
        if (rsig != dock_sig) {
            dock_sig = rsig;
            int oldtop = dock_y - DOCK_SH;
            rebuild_dock();
            layout_dock();
            int newtop = dock_y - DOCK_SH;
            int top = oldtop < newtop ? oldtop : newtop;
            add_dirty(0, top, W, H - top);          /* repaint the dock band over old + new extent */
        }

        /* --- Launchpad overlay: full repaint when it opens/closes so the dim scrim
         * is painted over (or cleared from) the whole screen, not just its rect --- */
        int ov_now = overlay_slot() >= 0;
        if (ov_now != overlay_on) { overlay_on = ov_now; add_dirty(0, 0, W, H); }

        /* --- input: keys to the focused window ------------------------------- */
        if (pending_until && frame >= pending_until) pending_until = 0;   /* in-flight launch gave up */
        int key, f = focus_slot();
        while ((key = twm_getkey()) >= 0) {
            if (key == KEY_SUPER_V)     { summon("Clipboard", "clipboard", frame); continue; }  /* clipboard manager */
            if (key == KEY_SUPER_SPACE) { summon("Spotlight", "spotlight", frame); continue; }  /* Spotlight search  */
            if (key == KEY_LAUNCHPAD) {                 /* Launchpad: toggle -- a second Super tap dismisses it */
                int lp = find_app_window("Launchpad");
                if (lp >= 0) wm_post(cw[lp].id, WEV_CLOSE, 0);
                else         summon("Launchpad", "launchpad", frame);
                continue;
            }
            if (key == KEY_ALT_TAB)     { window_switch(frame); continue; }                     /* window switcher   */
            f = focus_slot();                                            /* a chord above may have changed focus */
            if (key == KEY_SUPER_Q)   { if (f >= 0) wm_post(cw[f].id, WEV_CLOSE, 0); continue; }  /* close focused */
            if (key == KEY_SUPER_KILL){ if (f >= 0) wm_kill(cw[f].id);              continue; }  /* force-kill focused process */
            if (key == 27) {
                /* ESC is ambiguous: a lone Esc dismisses a popup, but it also
                 * leads every CSI/SS3 nav-key sequence (arrows = ESC [ A..D),
                 * which the keyboard driver queues atomically. Peek the next
                 * byte: '[' or 'O' means it's a nav/function key -> forward the
                 * whole sequence to the app; anything else is a standalone Esc. */
                int nx = twm_getkey();
                if (nx == '[' || nx == 'O') {
                    if (f >= 0) { wm_send_key(cw[f].id, 27); wm_send_key(cw[f].id, nx); }
                    continue;
                }
                if (nx >= 0) twm_ungetkey(nx);                            /* unrelated key follows; handle it next */
                if (f >= 0 && cw[f].popup) { wm_post(cw[f].id, WEV_CLOSE, 0); continue; }  /* lone Esc dismisses a popup */
                if (f >= 0) wm_send_key(cw[f].id, 27);                    /* bare Esc to a non-popup app */
                continue;
            }
            if (f >= 0) wm_send_key(cw[f].id, key);
        }

        /* --- input: mouse ---------------------------------------------------- */
        mouse(&ms);
        int moved = (ms.x != cur_x || ms.y != cur_y);
        if (moved) {                                     /* cursor moved: damage old + new */
            add_dirty(cur_x - 12, cur_y - 12, CURSOR_W + 24, CURSOR_H + 24);
            cur_x = ms.x; cur_y = ms.y;
            add_dirty(cur_x - 12, cur_y - 12, CURSOR_W + 24, CURSOR_H + 24);
        }
        update_chrome(ms.x, ms.y);                       /* fullscreen / edge-reveal auto-hide */
        if (cc_open && moved) add_dirty(cc_x, cc_y, cc_w, cc_h);   /* repaint CC hover states fully */
        /* scroll wheel -> the top-most window under the cursor, client-relative (WEV_SCROLL) */
        if (ms.wheel) {
            for (int zi = nz - 1; zi >= 0; zi--) {
                struct cwin *c = &cw[zo[zi]];
                if (c->min) continue;
                int ow = owf(c), oh = ohf(c);
                if (ms.x < c->wx || ms.x >= c->wx + ow || ms.y < c->wy || ms.y >= c->wy + oh) continue;
                int rx = ms.x - c->wx, ry = ms.y - c->wy - wth(c); if (ry < 0) ry = 0;
                wm_post(c->id, WEV_SCROLL, WEV_MOUSE_PACK(rx, ry, (unsigned)(ms.wheel & 0xff)));
                break;
            }
        }
        int down = ms.buttons & 1;
        if (down && !last_b) {                          /* press edge */
            int handled = 0;
            /* A focused popup overlay (clipboard/Spotlight) is modal-lite: a click
             * OUTSIDE it dismisses it and is consumed; a click inside falls through
             * to the window loop, which forwards it to the popup's client. */
            {
                int pf = focus_slot();
                if (pf >= 0 && cw[pf].popup) {
                    struct cwin *c = &cw[pf];
                    int inside = ms.x >= c->wx && ms.x < c->wx + owf(c) &&
                                 ms.y >= c->wy && ms.y < c->wy + ohf(c);
                    if (!inside) { wm_post(c->id, WEV_CLOSE, 0); handled = 1; }
                }
            }
            /* Control Center: the bar status item toggles it; while open, a click
             * inside acts on a row, a click anywhere else dismisses it. */
            int on_ccbtn = bar_y > -bar_h && ms.y >= 0 && ms.y < bar_h &&
                           ms.x >= cc_btn_x - 4 && ms.x < cc_btn_x + cc_btn_w;
            if (cc_open) {
                handled = 1;
                if (!on_ccbtn && ms.x >= cc_x && ms.x < cc_x + cc_w && ms.y >= cc_y && ms.y < cc_y + cc_h)
                    cc_click(ms.x, ms.y);
                cc_open = 0;
                dirty_cc(); add_dirty(0, 0, W, bar_h);
            } else if (on_ccbtn) {
                cc_open = 1; handled = 1;
                dirty_cc(); add_dirty(0, 0, W, bar_h);
            }
            /* The dock is a top-most overlay, so it must get the click before any
             * window it overlaps -- otherwise a window behind the dock swallows the
             * click and the dock becomes unusable. Clicks anywhere on the dock panel
             * are consumed (never leak through to a window underneath). */
            struct rect dckr = { dock_x, dock_y, dock_w, dock_h };
            if (ms.x >= dckr.x && ms.x < dckr.x + dckr.w && ms.y >= dckr.y && ms.y < dckr.y + dckr.h) {
                handled = 1;
                for (int i = 0; i < nicons; i++) {
                    int x = icons[i].cx - TH_TILE / 2, y = icons[i].cy - TH_TILE / 2;
                    if (ms.x < x || ms.x >= x + TH_TILE || ms.y < y || ms.y >= y + TH_TILE) continue;
                    if (icons[i].special) { summon("Launchpad", "launchpad", frame); break; }  /* single click */
                    int ws = find_app_window(icons[i].label);
                    if (ws >= 0 && cw[ws].min) {        /* restore a minimized window (animate) */
                        struct rect to   = { cw[ws].wx, cw[ws].wy, owf(&cw[ws]), ohf(&cw[ws]) };
                        struct rect from = { icons[i].cx - 18, icons[i].cy - 14, 36, 28 };
                        anim_start(AN_RESTORE, ws, from, to, 12);
                    } else if (ws >= 0) {               /* focus an already-open window */
                        int prev = focus_slot();
                        zo_raise(ws); dirty_win(&cw[ws]);
                        if (prev >= 0 && prev != ws) dirty_win(&cw[prev]);
                        add_dirty(0, 0, W, bar_h);
                    } else {                            /* not running: double-click launches */
                        if (last_click_icon == i && frame - last_click_frame <= DBL_FRAMES)
                            { launch(icons[i].exec); last_click_icon = -1; }
                        else { last_click_icon = i; last_click_frame = frame; }
                    }
                    break;
                }
            }
            for (int zi = nz - 1; zi >= 0 && !handled; zi--) {
                int slot = zo[zi]; struct cwin *c = &cw[slot];
                int ow = owf(c), oh = ohf(c);
                if (ms.x < c->wx || ms.x >= c->wx + ow || ms.y < c->wy || ms.y >= c->wy + oh) continue;
                if (focus_slot() != slot) {             /* raise + refocus */
                    int prev = focus_slot();
                    zo_raise(slot);
                    dirty_win(c);
                    if (prev >= 0) dirty_win(&cw[prev]);
                    add_dirty(0, 0, W, bar_h);
                }
                if (c->popup) {                         /* no chrome: the whole surface is client */
                    wm_post(c->id, WEV_MOUSE, WEV_MOUSE_PACK(ms.x - c->wx, ms.y - c->wy, ms.buttons & 1));
                    cdrag = c->id;                      /* allow drag-select inside the overlay */
                    handled = 1;
                    continue;
                }
                int ly = c->wy + TH / 2, dy = ms.y - ly, hit = 9 * 9;  /* control buttons, top-right */
                int cxc = c->wx + ow - 18, cxx = cxc - 22, cxm = cxx - 22;
                if ((ms.x - cxc) * (ms.x - cxc) + dy * dy <= hit) {        /* close            */
                    wm_post(c->id, WEV_CLOSE, 0);
                } else if ((ms.x - cxx) * (ms.x - cxx) + dy * dy <= hit) { /* maximize/restore */
                    toggle_max(slot);
                } else if ((ms.x - cxm) * (ms.x - cxm) + dy * dy <= hit) { /* minimize -> dock */
                    struct rect from = { c->wx, c->wy, ow, oh };
                    int ti = dock_tile_for(c->title);
                    struct rect to = (ti >= 0)
                        ? (struct rect){ icons[ti].cx - 18, icons[ti].cy - 14, 36, 28 }
                        : (struct rect){ dock_x + dock_w / 2 - 18, dock_y, 36, 28 };
                    dirty_win(c); c->min = 1; zo_remove(slot);
                    anim_start(AN_MIN, slot, from, to, 12);
                    add_dirty(0, 0, W, bar_h);
                } else if (!c->maxed && ms.x >= c->wx + ow - GRIP && ms.y >= c->wy + oh - GRIP) {
                    rsz = c->id; rsz_ox = c->wx; rsz_oy = c->wy;           /* resize grip (off when maxed) */
                } else if (ms.y < c->wy + TH) {                            /* title bar -> drag */
                    if (!c->maxed) { drag = c->id; drag_dx = ms.x - c->wx; drag_dy = ms.y - c->wy; }
                } else {                                                   /* client -> forward */
                    wm_post(c->id, WEV_MOUSE, WEV_MOUSE_PACK(ms.x - c->wx, ms.y - (c->wy + TH), ms.buttons & 1));
                    cdrag = c->id;                                         /* enable drag-select forwarding */
                }
                handled = 1;
            }
        } else if (!down) {
            if (rsz >= 0) {                             /* finish a resize: snap to the exact size */
                int k = find(rsz);
                if (k >= 0) {
                    int nw = ms.x - rsz_ox, nh = ms.y - rsz_oy - TH;
                    if (nw < 200) nw = 200;
                    if (nh < 120) nh = 120;
                    wm_post(rsz, WEV_RESIZE, ((unsigned)nw << 16) | (unsigned)nh);
                }
            }
            drag = -1; rsz = -1; cdrag = -1;
        }
        /* Client drag-select: while a button-held drag that started in a client area
         * continues, forward the (clamped, client-relative) position to that window
         * with the WEV_MOUSE_DRAG marker so it can extend a selection (terminal grid
         * select, Files rubber-band). Suppressed while moving/resizing the window. */
        if (cdrag >= 0 && down && moved && drag < 0 && rsz < 0) {
            int k = find(cdrag);
            if (k >= 0) {
                struct cwin *c = &cw[k];
                int ow = owf(c);
                int rx = ms.x - c->wx, ry = ms.y - (c->wy + wth(c));
                if (rx < 0) rx = 0; if (rx >= ow) rx = ow - 1;
                if (ry < 0) ry = 0; if (ry >= (int)c->h) ry = (int)c->h - 1;
                wm_post(cdrag, WEV_MOUSE, WEV_MOUSE_PACK(rx, ry, (ms.buttons & 1) | WEV_MOUSE_DRAG));
            }
        }
        /* Live resize: while the grip is held, push size changes to the app as the
         * pointer moves (throttled to >7px so we don't reallocate the surface every
         * pixel) -- the content resizes as you drag, not just on release. */
        if (rsz >= 0 && down) {
            int k = find(rsz);
            if (k >= 0) {
                int nw = ms.x - rsz_ox, nh = ms.y - rsz_oy - TH;
                if (nw < 200) nw = 200;
                if (nh < 120) nh = 120;
                int dw = nw - (int)cw[k].w, dh = nh - (int)cw[k].h;
                if (dw > 7 || dw < -7 || dh > 7 || dh < -7)
                    wm_post(rsz, WEV_RESIZE, ((unsigned)nw << 16) | (unsigned)nh);
            }
        }
        if (drag >= 0) {                                /* dragging a window */
            int k = find(drag);
            if (k >= 0) {
                int ox = cw[k].wx, oy = cw[k].wy;
                cw[k].wx = ms.x - drag_dx; cw[k].wy = ms.y - drag_dy;
                clampw(&cw[k]);
                if (cw[k].wx != ox || cw[k].wy != oy) {
                    add_dirty(ox - TH_SHADOW_SP, oy - TH_SHADOW_SP,
                              owf(&cw[k]) + 2 * TH_SHADOW_SP, ohf(&cw[k]) + 2 * TH_SHADOW_SP);
                    dirty_win(&cw[k]);
                }
            }
        }
        last_b = down;

        /* Hover feedback: forward pointer moves (no button held) to the window under
         * the cursor as a WEV_MOUSE with btn==0, so its toolkit can light up the
         * widget beneath -- the single biggest "feels alive" cue. Suppressed while
         * dragging/resizing/pressing or over the dock/bar/open control center. When
         * the pointer leaves a window we send one out-of-range packet (0xfff,0xfff)
         * so the app clears its hover. Legacy clients (term) read only the button
         * bits, so a btn==0 move is a harmless no-op for them. */
        {
            int hov = -1, hx = 0, hy = 0;
            if (!down && drag < 0 && rsz < 0 && !cc_open) {
                struct rect dr = { dock_x, dock_y, dock_w, dock_h };
                int over_dock = ms.x >= dr.x && ms.x < dr.x + dr.w && ms.y >= dr.y && ms.y < dr.y + dr.h;
                int over_bar  = bar_y > -bar_h && ms.y >= bar_y && ms.y < bar_y + bar_h;
                if (!over_dock && !over_bar)
                    for (int zi = nz - 1; zi >= 0; zi--) {       /* topmost window under the pointer */
                        struct cwin *c = &cw[zo[zi]];
                        int ow = owf(c), oh = ohf(c);
                        if (ms.x < c->wx || ms.x >= c->wx + ow || ms.y < c->wy || ms.y >= c->wy + oh) continue;
                        if (!c->min && ms.y >= c->wy + wth(c)) { /* over its client area (below any title) */
                            hov = c->id; hx = ms.x - c->wx; hy = ms.y - (c->wy + wth(c));
                        }
                        break;
                    }
            }
            if (hov != hover_id) {
                if (hover_id >= 0) wm_post(hover_id, WEV_MOUSE, WEV_MOUSE_PACK(0xfff, 0xfff, 0));
                hover_id = hov;
                if (hov >= 0) wm_post(hov, WEV_MOUSE, WEV_MOUSE_PACK(hx, hy, 0));
            } else if (hov >= 0 && moved) {
                wm_post(hov, WEV_MOUSE, WEV_MOUSE_PACK(hx, hy, 0));
            }
        }

        /* right-click in a window's client area -> forward as a context-menu request
         * (button bit 1 set in the packed mouse event); chrome/title/dock ignore it */
        {
            int rb = ms.buttons & 2;
            if (rb && !last_rb) {
                for (int zi = nz - 1; zi >= 0; zi--) {
                    struct cwin *c = &cw[zo[zi]];
                    int ow = owf(c), oh = ohf(c);
                    if (ms.x < c->wx || ms.x >= c->wx + ow || ms.y < c->wy || ms.y >= c->wy + oh) continue;
                    if (ms.y >= c->wy + wth(c))
                        wm_post(c->id, WEV_MOUSE, WEV_MOUSE_PACK(ms.x - c->wx, ms.y - (c->wy + wth(c)), 2));
                    break;
                }
            }
            last_rb = rb;
        }

        /* mouse side buttons -> back/forward navigation gestures for the focused app */
        {
            int back = ms.buttons & 0x08, fwd = ms.buttons & 0x10, ff = focus_slot();
            if (back && !last_back && ff >= 0) wm_post(cw[ff].id, WEV_NAV, 0);
            if (fwd  && !last_fwd  && ff >= 0) wm_post(cw[ff].id, WEV_NAV, 1);
            last_back = back; last_fwd = fwd;
        }

        /* A focus change that wasn't a direct click on the new window (closing or
         * minimizing the window above, a new window stealing focus) must repaint
         * BOTH the old and new windows -- otherwise their stale traffic-light
         * colours, title-text colour and shadow alpha linger in the back buffer
         * until the cursor's per-move damage paints them in piecemeal. */
        {
            int curf = focus_slot();
            if (curf != last_focus) {
                if (last_focus >= 0 && cw[last_focus].used) dirty_win(&cw[last_focus]);
                if (curf >= 0 && cw[curf].used) dirty_win(&cw[curf]);
                add_dirty(0, 0, W, bar_h);
                last_focus = curf;
                print("[twm] focus ");                  /* the harness watches this to assert focus moves */
                print(curf >= 0 ? cw[curf].title : "desktop");
                print("\r\n");
            }
        }

        rtc_time(&t);                                   /* clock ticks once a second */
        if (t.sec != last_sec) { last_sec = t.sec; add_dirty(W - 200, 0, 200, bar_h); }

        /* repaint the dock when the running/focus/minimized set changes, so its
         * indicators + active highlight stay in sync (cheap signature check). */
        unsigned sig = (unsigned)(focus_slot() + 1);
        for (int i = 0; i < MAXW; i++)
            if (cw[i].used) sig = sig * 31u + (unsigned)(i * 4 + (cw[i].min ? 1 : 0) + 1);
        int hovi = -1;                                  /* hovered dock tile -> repaint its lift */
        for (int i = 0; i < nicons; i++) if (tile_hovered(&icons[i])) hovi = i;
        sig = sig * 131u + (unsigned)(hovi + 1);
        if (sig != last_dock_sig) { last_dock_sig = sig;
            add_dirty(dock_x - DOCK_SH, dock_y - DOCK_SH, dock_w + 2 * DOCK_SH, dock_h + 2 * DOCK_SH); }

        /* --- context-aware cursor shape -------------------------------------- */
        {
            int want = CUR_ARROW;
            struct rect dr = { dock_x, dock_y, dock_w, dock_h };
            if (rsz >= 0) {                              /* mid resize-drag: keep the resize cursor */
                want = CUR_RESIZE_NWSE;
            } else if (busy_until > frame) {            /* an app is launching */
                want = CUR_BUSY0 + (busy_frame / 4) % CUR_BUSY_FRAMES;
                busy_frame++;
                add_dirty(cur_x - 12, cur_y - 12, CURSOR_W + 24, CURSOR_H + 24);  /* animate */
            } else if (ms.x >= dr.x && ms.x < dr.x + dr.w && ms.y >= dr.y && ms.y < dr.y + dr.h) {
                want = CUR_HAND;                        /* over the dock */
            } else {
                for (int zi = nz - 1; zi >= 0; zi--) {  /* topmost window under the pointer */
                    struct cwin *c = &cw[zo[zi]];
                    int ow = owf(c), oh = ohf(c);
                    if (ms.x < c->wx || ms.x >= c->wx + ow || ms.y < c->wy || ms.y >= c->wy + oh) continue;
                    if (!c->maxed && ms.x >= c->wx + ow - GRIP && ms.y >= c->wy + oh - GRIP)
                        want = CUR_RESIZE_NWSE;         /* resize affordance at the grip */
                    else if (ms.y >= c->wy + TH && streqz(c->title, "Terminal"))
                        want = CUR_IBEAM;               /* text area */
                    break;
                }
            }
            if (want != cur_id) { add_dirty(cur_x - 12, cur_y - 12, CURSOR_W + 24, CURSOR_H + 24); cur_id = want; }
        }

        anim_step();                                    /* advance the minimize/restore genie */
        flush_dirty();
        while (trywait() > 0) { }                       /* reap launched apps; desktop stays up */
        sleep_ms(12);
    }
}
