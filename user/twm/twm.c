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
#include "twm.h"

/* ----------------------------------------------------------- core-only state
 * (state nothing outside twm.c touches stays private here; the shared compositor
 * state is DEFINED below and declared for the feature files in twm.h). */
#define MAXDIRTY  32
#define GRIP      18             /* bottom-right resize-grip hit box          */
#define CURW      14             /* cursor damage box                         */
#define CURH      21

static uint32_t *wall;           /* precomputed wallpaper (gradient+glow+vignette), W*H */
/* desktop gradient endpoints, split into channels for the per-row lerp */
static int gtr, gtg, gtb, gbr, gbg, gbb;

static struct rect dirty[MAXDIRTY];
static int ndirty;
static int cur_id = CUR_ARROW;   /* context-aware cursor shape (CUR_*) */
static int busy_until, busy_frame, launch_busy;  /* spinner cursor while an app launches */
static unsigned dock_sig;        /* signature of the running set; the dock rebuilds when it changes */
static int bar_linger, dock_linger;   /* reveal-linger counters for auto-hide */

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

/* ------------------------------------------------------------- shared state
 * (the contract in twm.h; defined here, used across the feature files). */
uint32_t *bb;                    /* back buffer (mmap'd, fb_w*fb_h, tightly packed) */
int W, H, fh, bar_h, TH;         /* screen, font height, bar height, title height  */
struct cwin cw[MAXW];
int zo[MAXW], nz;                /* z-order: zo[nz-1] is topmost == focused */
struct app apps[MAXAPPS];
int napps;
struct icon icons[MAXICON];
int nicons;
int dock_x, dock_y, dock_w, dock_h;
int dock_y0;                     /* dock's shown (base) y; dock_y is the current (animated) y */
int bar_y;                       /* bar's current top: 0 = shown, -bar_h = hidden */
struct rect cur_clip;            /* the rect compose() is currently painting */
int cur_x, cur_y;
uint32_t g_accent;               /* focus accent colour, from the registry (theme.accent) */

/* status cluster slot positions (computed by cc_layout, drawn by draw_bar) */
int sb_clk_w;
int sb_net_x, sb_vol_x, sb_bat_x, sb_bell_x;

/* control center panel */
int cc_open;
int cc_x, cc_y, cc_w = 268, cc_h;          /* panel rect (h computed in cc_layout) */
int cc_btn_x, cc_btn_w = 24;               /* the bar status-item hit box           */
int cc_row1_y, cc_row2_y, cc_sep_y, cc_info_y, cc_btn_yy;

/* notifications: the kept ring, the live toast, the center */
struct notif notes[NOTE_KEEP];
unsigned char note_exp[NOTE_KEEP];               /* per-row expanded flag (ring-indexed) */
int notes_n, notes_head, notes_unseen;          /* ring of kept notifications */
struct notif toast;                              /* the active toast */
int toast_live, toast_age;                       /* toast_live=0 -> none; else age 0.. */
int toast_linger;                                /* frames to keep cleaning the footprint after it dies */
int toast_expanded;                              /* body expanded to full (wrapped) */
int toast_paused;                                /* hover/click froze the auto-dismiss */
int toast_collapsible;                           /* body was truncated -> chevron */
int nc_open, nc_x, nc_y, nc_h;                   /* notification-center panel */
int nc_slide;                                    /* >0: animate the newest center row */

/* Alt-Tab switcher: only "is the card up" is shared (the rest is private in switcher.c) */
int sw_overlay;

/* menu bar: the open dropdown + the focused window's declared menu tiles */
int menu_kind;                   /* 0 none, 1 logo (system), 2 app (About/Quit), 3 app-declared */
int menu_x, menu_y, menu_w, menu_h;
int menu_nitems;
struct winmenu cur_menu;         /* the focused window's declared menu bar (#6) */
int menu_app_idx;                /* which cur_menu.m[] the kind-3 dropdown shows */
int appmenu_x[WINMENU_MAX], appmenu_w[WINMENU_MAX];  /* per-menu tile hit regions (set in draw_bar) */
int logo_hit_w;                  /* logo click region width (set in draw_bar) */
int app_hit_x, app_hit_w;        /* focused-app name click region (set in draw_bar) */

/* THE single source for every drop-shadow halo (invalidations AND culls): a rect grown
 * to include a shadow that feathers `spread`px outward and rides `dy`px downward. Keeps
 * the full `spread` on top and adds `dy` to the bottom for the offset, so the box never
 * under-covers the shadow regardless of the offset. `spread` matches ugfx_shadow's
 * feather; ugfx_elevation_extent supplies (spread, dy) for an elevation level. */
struct rect shadow_box(int x, int y, int w, int h, int spread, int dy) {
    struct rect r = { x - spread, y - spread, w + 2 * spread, h + 2 * spread + dy };
    return r;
}


/* ------------------------------------------------------------------ helpers */
static int streqz(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int wth(struct cwin *c) { return c->popup ? 0 : TH; }   /* title-bar height (popups have none) */
static int owf(struct cwin *c) { return (int)c->w; }
static int ohf(struct cwin *c) { return wth(c) + (int)c->h; }
/* A fullscreen window: client fills the whole screen and its title bar becomes a
 * sliding overlay (see fs_titlebar_y) -- distinct from a floating/maximized-to-work-area
 * window whose title bar sits at wy. Popups are never fullscreen. */
static int is_fs(struct cwin *c) { return c->maxed && !c->popup; }
/* The on-screen client rectangle (where the surface is blitted). Fullscreen owns
 * the whole screen at (0,0); a normal window's client sits just below its title. */
static void client_rect(struct cwin *c, int *x, int *y, int *w, int *h) {
    *w = (int)c->w; *h = (int)c->h;
    if (is_fs(c)) { *x = 0; *y = 0; }
    else          { *x = c->wx; *y = c->wy + wth(c); }
}
/* The outer rectangle for z-order hit-testing (which window is under the cursor).
 * Fullscreen: the whole screen (its title bar overlays the client, not above it). */
static void outer_rect(struct cwin *c, int *x, int *y, int *w, int *h) {
    if (is_fs(c)) { *x = 0; *y = 0; *w = (int)c->w; *h = (int)c->h; }
    else          { *x = c->wx; *y = c->wy; *w = owf(c); *h = ohf(c); }
}
static int in_client(struct cwin *c, int px, int py) {
    int x, y, w, h; client_rect(c, &x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}
/* The y of a fullscreen window's sliding title bar, coupled to the menu bar reveal
 * (bar_y): fully hidden (bar_y=-bar_h) -> -TH (just off the top edge); fully shown
 * (bar_y=0) -> bar_h (docked right under the menu bar). The menu bar + this title
 * bar reveal/retract together as one "top group" (design/ui.md). */
static int fs_titlebar_y(void) { return -TH + (bar_y + bar_h) * (bar_h + TH) / bar_h; }
static int find(int id) { for (int i = 0; i < MAXW; i++) if (cw[i].used && cw[i].id == id) return i; return -1; }
static int wslot(void)  { for (int i = 0; i < MAXW; i++) if (!cw[i].used) return i; return -1; }
static int find_snap(struct wmwin *s, int n, int id) { for (int i = 0; i < n; i++) if (s[i].id == id) return i; return -1; }

int rects_hit(struct rect a, struct rect b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

/* z-order list ---------------------------------------------------------------*/
int focus_slot(void) { return nz ? zo[nz - 1] : -1; }
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
    int dsp, ddy; ugfx_elevation_extent(DOCK_ELEVATION, &dsp, &ddy);   /* dock floats at this elevation */
    struct rect d = shadow_box(dock_x, dock_y, dock_w, dock_h, dsp, ddy);
    if (dock_w > 0 && box_hit(*x, *y, *w, *h, d.x, d.y, d.w, d.h))
        union_box(x, y, w, h, d.x, d.y, d.w, d.h);
    if (cc_open) {
        struct rect c = shadow_box(cc_x, cc_y, cc_w, cc_h, CC_SHADOW_SP, CC_SHADOW_DY);
        if (box_hit(*x, *y, *w, *h, c.x, c.y, c.w, c.h)) union_box(x, y, w, h, c.x, c.y, c.w, c.h);
    }
    if (nc_open) {                                      /* notification center: also frosted */
        struct rect c = shadow_box(nc_x, nc_y, NC_W, nc_h, NC_SHADOW_SP, NC_SHADOW_DY);
        if (box_hit(*x, *y, *w, *h, c.x, c.y, c.w, c.h)) union_box(x, y, w, h, c.x, c.y, c.w, c.h);
    }
    if (menu_kind) {                                    /* menu-bar dropdown: also frosted */
        struct rect c = shadow_box(menu_x, menu_y, menu_w, menu_h, MENU_SHADOW_SP, MENU_SHADOW_DY);
        if (box_hit(*x, *y, *w, *h, c.x, c.y, c.w, c.h)) union_box(x, y, w, h, c.x, c.y, c.w, c.h);
    }
    int ov = overlay_slot();                            /* Launchpad: also frosted */
    if (ov >= 0) {
        struct cwin *o = &cw[ov];
        struct rect ob = shadow_box(o->wx, o->wy, owf(o), ohf(o), TH_SHADOW_SP, WIN_SHADOW_DY);
        if (box_hit(*x, *y, *w, *h, ob.x, ob.y, ob.w, ob.h)) union_box(x, y, w, h, ob.x, ob.y, ob.w, ob.h);
    }
}
void add_dirty(int x, int y, int w, int h) {
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
    struct rect r = shadow_box(c->wx, c->wy, owf(c), ohf(c), TH_SHADOW_SP, WIN_SHADOW_DY);
    add_dirty(r.x, r.y, r.w, r.h);
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
/* Wallpaper presets: set the vertical-gradient endpoints from the desktop.wallpaper
 * registry key (the Settings app cycles it). Unknown / "slate" / "gradient" keep the
 * theme default. The glow + vignette in gen_wallpaper() ride on top of whatever this
 * picks, so each preset stays in the "floating" house style. */
static void set_wallpaper_palette(void) {
    uint32_t top = TH_DESK_TOP, bot = TH_DESK_BOT;
    const char *w = reg_get("desktop.wallpaper", "slate");
    if      (streqz(w, "midnight")) { top = RGB(22, 26, 44); bot = RGB(7,  9,  18); }
    else if (streqz(w, "forest"))   { top = RGB(26, 52, 44); bot = RGB(10, 24, 20); }
    else if (streqz(w, "plum"))     { top = RGB(46, 28, 54); bot = RGB(20, 11, 28); }
    else if (streqz(w, "graphite")) { top = RGB(50, 52, 58); bot = RGB(22, 23, 27); }
    gtr = (top >> 16) & 0xff; gtg = (top >> 8) & 0xff; gtb = top & 0xff;
    gbr = (bot >> 16) & 0xff; gbg = (bot >> 8) & 0xff; gbb = bot & 0xff;
}
static void gen_wallpaper(void) {
    if (!wall) wall = (uint32_t *)mmap_((unsigned long)W * (unsigned long)H * 4);   /* reuse on regen */
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

/* The Settings app persists changes to the registry on disk; there's no change-bus
 * yet, so re-read it once a second (off the clock tick) and apply anything that needs
 * more than the live reg_bool() reads update_chrome()/build_clock() already do --
 * shadows, accent, and the wallpaper gradient. Cheap: a tiny file, once a second. */
static void apply_settings_live(void) {
    static int      sh = -1; static uint32_t ac = 0; static char wp[24] = {0};
    reg_load();
    int      nsh = reg_bool("ui.shadows", 1);
    uint32_t nac = reg_color("theme.accent", ARGB(235, 120, 170, 255));
    const char *nw = reg_get("desktop.wallpaper", "slate");
    int changed = 0;
    if (nsh != sh) { sh = nsh; ugfx_set_shadows(nsh); changed = 1; }
    if (nac != ac) { ac = nac; g_accent = nac; changed = 1; }
    if (!streqz(nw, wp)) { int i = 0; for (; nw[i] && i < 23; i++) wp[i] = nw[i]; wp[i] = 0;
                           set_wallpaper_palette(); gen_wallpaper(); changed = 1; }
    if (changed) add_dirty(0, 0, W, H);                 /* repaint the whole desktop with the new look */
}


/* ----- shared text helper (used by the notification + switcher cards) -------- */
/* Copy src into dst, hard-truncating with a trailing ".." so it fits maxw px. */
void fit_text(char *dst, int cap, const char *src, int maxw) {
    int n = 0; while (src[n] && n < cap - 1) n++;
    for (int len = n; len >= 0; len--) {
        int i = 0; for (; i < len && i < cap - 3; i++) dst[i] = src[i];
        if (len < n) { dst[i++] = '.'; dst[i++] = '.'; }
        dst[i] = 0;
        if (ugfx_text_w(dst) <= maxw) return;
    }
    dst[0] = 0;
}

/* ------------------------------------------------------------------ windows */
/* blit a window-control button (baked macOS-style traffic light) centred at (cx,cy) */
static void blit_btn(int cx, int cy, int idx) {
    ugfx_blit_argb(cx - WINBTN_W / 2, cy - WINBTN_H / 2, WINBTN_W, WINBTN_H, winbtns_argb[idx]);
}

static void draw_window(int slot) {
    struct cwin *c = &cw[slot];
    int ow = owf(c), oh = ohf(c), wx = c->wx, wy = c->wy;
    struct rect wb = shadow_box(wx, wy, ow, oh, TH_SHADOW_SP, WIN_SHADOW_DY);
    if (!rects_hit(cur_clip, wb)) return;               /* nothing of this window in the rect */
    int foc = (focus_slot() == slot);
    if (c->popup) {                                     /* borderless overlay: surface + shadow only */
        ugfx_shadow(wx, wy + 6, ow, oh, TH_RADIUS, TH_SHADOW_SP, TH_SHADOW, TH_SHADOW_A);
        ugfx_blit_round(wx, wy, (const uint32_t *)c->vaddr, (int)c->w, (int)c->h, (int)c->w, TH_RADIUS);
        ugfx_rrect_border(wx, wy, ow, oh, TH_RADIUS, 1, TH_BORDER);
        return;
    }
    if (is_fs(c)) {                                     /* fullscreen: client fills the screen, title bar slides */
        ugfx_blit(0, 0, (const uint32_t *)c->vaddr, (int)c->w, (int)c->h, (int)c->w);
        int ty = fs_titlebar_y();
        if (ty + TH > 0) {                             /* the sliding title-bar overlay (with traffic lights) */
            ugfx_fill(0, ty, W, TH, TH_CHROME);
            ugfx_fill_a(0, ty + TH - 1, W, 1, ARGB(70, 0, 0, 0));
            int ly = ty + TH / 2, cxc = W - 18, cxx = cxc - 22, cxm = cxx - 22;
            blit_btn(cxm, ly, foc ? WB_MIN   : WB_INACTIVE);
            blit_btn(cxx, ly, foc ? WB_MAX   : WB_INACTIVE);
            blit_btn(cxc, ly, foc ? WB_CLOSE : WB_INACTIVE);
            int tw = ugfx_text_w(c->title), tx = (W - tw) / 2;
            if (tx + tw > cxm - WINBTN_W / 2 - 10) tx = cxm - WINBTN_W / 2 - 10 - tw;
            if (tx < 14) tx = 14;
            ugfx_text(tx, ty + (TH - fh) / 2, c->title, foc ? TH_TEXT : TH_MUTED, UGFX_TRANSPARENT);
        }
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
        c->wx = 0; c->wy = 0;                           /* fullscreen: client fills the WHOLE screen */
        wm_post(c->id, WEV_RESIZE, ((unsigned)W << 16) | (unsigned)H);  /* full height; title bar auto-hides */
        add_dirty(0, 0, W, H);                          /* the bar + dock + whole desktop give way */
    } else {
        c->maxed = 0;
        c->wx = c->sx; c->wy = c->sy;
        wm_post(c->id, WEV_RESIZE, ((unsigned)c->sw << 16) | (unsigned)c->sh);
        add_dirty(0, 0, W, H);                          /* repaint the desktop/bar/dock around the restored window */
    }
    dirty_win(c);
    print("[twm] fullscreen "); print(c->title); print(c->maxed ? " 1\r\n" : " 0\r\n");
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

/* ------------------------------------------------------------ compose/present */
/* A visible WIN_OVERLAY window (Launchpad) -> dimmed full-screen, drawn above the
 * dock. -1 if none. */
static int overlay_slot(void) {
    for (int i = 0; i < MAXW; i++) if (cw[i].used && cw[i].overlay && !cw[i].min) return i;
    return -1;
}
/* A visible WIN_MODAL window (the Files Open/Save picker) -> kept topmost + focused with
 * a dim scrim behind it; input outside it is swallowed. -1 if none. */
static int modal_slot(void) {
    for (int i = 0; i < MAXW; i++) if (cw[i].used && cw[i].modal && !cw[i].min) return i;
    return -1;
}
static void compose(struct rect r) {
    cur_clip = r;
    ugfx_set_clip(r.x, r.y, r.w, r.h);
    draw_desk(r.x, r.y, r.w, r.h);
    int ov = overlay_slot();
    int md = modal_slot();
    for (int i = 0; i < nz; i++) if (zo[i] != ov && zo[i] != md) draw_window(zo[i]);   /* back to front (overlay + modal drawn last) */
    struct rect barr = { 0, 0, W, bar_h };
    /* the dock hit-test must include its shadow halo, else a rect that clips only
     * the halo (a window edge or the cursor sliding past) repaints the desktop over
     * the shadow without redrawing it -- the same residue bug fixed for draw_cc. */
    int dsp, ddy; ugfx_elevation_extent(DOCK_ELEVATION, &dsp, &ddy);
    struct rect dckr = shadow_box(dock_x, dock_y, dock_w, dock_h, dsp, ddy);
    if (bar_y > -bar_h && rects_hit(r, barr)) draw_bar();
    if (rects_hit(r, dckr)) draw_dock();
    draw_cc();                                          /* control-center panel, over windows + dock */
    draw_nc();                                           /* notification center, same tier as CC */
    draw_menu();                                          /* menu-bar dropdown (logo / app menus) */
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
    if (md >= 0) {                                      /* modal dialog (picker): dim everything, dialog on top */
        ugfx_fill_a(r.x, r.y, r.w, r.h, ARGB(120, 6, 8, 12));           /* scrim over windows + bar + dock */
        draw_window(md);                                                /* the decorated dialog, full chrome, above the scrim */
    }
    draw_switcher();                                    /* Alt-Tab switcher card, above windows + dock */
    draw_toast();                                       /* notification toast, above windows/dock */
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
void focus_window(int slot) {
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

/* The three transient launchers (Spotlight, Launchpad, Clipboard) are a
 * single-instance group: only one may be up at a time, so summoning any one
 * first closes the others -- you can never have Spotlight floating over the
 * Launchpad. `except` is the launcher being summoned (left untouched); the rest
 * get a WEV_CLOSE if they're currently mapped. Called at the top of every
 * launcher summon path (the three Super hotkeys + the dock Launchpad button). */
static void dismiss_launchers(const char *except) {
    static const char *const launchers[] = { "Spotlight", "Launchpad", "Clipboard" };
    for (int i = 0; i < 3; i++) {
        if (title_is(except, launchers[i])) continue;
        int s = find_app_window(launchers[i]);
        if (s >= 0) wm_post(cw[s].id, WEV_CLOSE, 0);
    }
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

/* Route a notification click to its sender (design/ui.md): if the target app has
 * a window, focus it (restoring a minimized one); otherwise launch it from the
 * app catalog. An empty target is a no-op (notify() with no declared target). */
void notif_activate(const char *target, int frame) {
    if (!target || !target[0]) return;
    int ws = find_app_window(target);
    if (ws >= 0) { if (cw[ws].min) restore_window(ws); else focus_window(ws); return; }
    int ai = app_for_title(target);
    if (ai >= 0) summon(apps[ai].label, apps[ai].exec, frame);
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

/* Forward a key byte to a window, packing the live modifier mask into the high
 * bits of WEV_KEY (legacy readers mask `a & 0xff`; mod-aware ones read the flags). */
static void send_key(int id, int byte) {
    wm_post(id, WEV_KEY, WEV_KEY_PACK(byte, kbd_mods()));
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

    /* The "top group" = the menu bar, plus (in fullscreen) the focused window's own
     * title bar sliding right beneath it. They reveal/retract as one band. */
    int top_band = fs ? bar_h + TH : bar_h;
    int want_bar;
    if (!auto_bar) want_bar = 1;
    else {
        /* Top-edge peek arms the reveal; once shown, HOLD it while the cursor stays
         * anywhere in the revealed band (so you can travel down onto the title bar's
         * traffic lights). It retracts only when the cursor dives below the band into
         * the content, after a short linger. */
        if (my < EDGE) bar_linger = HIDE_LINGER;
        else if (bar_y > -bar_h && my < top_band) bar_linger = HIDE_LINGER;
        else if (bar_linger > 0) bar_linger--;
        want_bar = bar_linger > 0;
    }
    int bt = want_bar ? 0 : -bar_h;
    if (bar_y != bt) {
        if (bar_y < bt) { bar_y += SLIDE; if (bar_y > bt) bar_y = bt; }
        else            { bar_y -= SLIDE; if (bar_y < bt) bar_y = bt; }
        add_dirty(0, 0, W, top_band);   /* repaint the moving menu bar + the sliding fullscreen title bar */
    }
    {   /* telemetry: the top group reached a rest state while a window is fullscreen */
        static int last_top = -2;
        int st = bar_y == 0 ? 1 : (bar_y == -bar_h ? 0 : -1);
        if (fs) { if (st >= 0 && st != last_top) { last_top = st;
                    print(st ? "[twm] topbar shown\r\n" : "[twm] topbar hidden\r\n"); } }
        else last_top = -2;
    }

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
        int dsp, ddy; ugfx_elevation_extent(DOCK_ELEVATION, &dsp, &ddy);
        add_dirty(dock_x - dsp, dock_y0 - dsp,                   /* travel band incl. shadow halo */
                  dock_w + 2 * dsp, H - dock_y0 + dsp + ddy);
    }
}

/* ------------------------------------------------------------------ main */
__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    if (ugfx_init() < 0) { print("[twm] no framebuffer\r\n"); exec("shell"); proc_exit(); }
    W = ugfx_width(); H = ugfx_height(); fh = ugfx_font_h();
    bar_h = fh + 14; TH = fh + 12;
    bb = (uint32_t *)mmap_((unsigned long)W * (unsigned long)H * 4);
    if (!bb) { print("[twm] back buffer alloc failed\r\n"); exec("shell"); proc_exit(); }
    ugfx_set_target(bb, W, H, W);

    wm_register();
    reg_load();                                         /* settings BEFORE we paint the desktop */
    ugfx_set_shadows(reg_bool("ui.shadows", 1));         /* `reg set ui.shadows false` kills ALL drop shadows */
    g_accent = reg_color("theme.accent", ARGB(235, 120, 170, 255));
    set_wallpaper_palette();                             /* gradient endpoints from desktop.wallpaper */
    gen_wallpaper();                                    /* precompute the desktop once */
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
    unsigned last_kmods = 0;         /* modifier mask last frame, to detect key-UP transitions */
    int bar_hover = 0;               /* 0 none / 1 logo / 2 app: repaint the bar on hover changes */
    int last_focus = -1;
    int last_focus_id = -1;          /* focus tracked by id too: a freed slot reused by a new window in the same frame keeps the index but changes id */
    int overlay_on = 0;              /* Launchpad-overlay presence; a full repaint on each edge paints/clears the scrim */
    int modal_on = 0;                /* modal-dialog (picker) presence; same paint/clear-the-scrim repaint on each edge */
    int drag = -1, drag_dx = 0, drag_dy = 0;
    int rsz = -1, rsz_ox = 0, rsz_oy = 0;
    int cdrag = -1;                  /* a button-held drag inside a window's client area */
    int last_click_icon = -1, last_click_frame = -1000;
    int last_click_win = -1, last_click_wframe = -1000;   /* double-click a title bar -> toggle fullscreen */
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
                print("[twm] unmap "); print(cw[i].title); print("\r\n");  /* telemetry: a window went away */
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
                cw[k].modal = (snap[j].flags & WIN_MODAL) != 0;  /* picker: input-locked dialog, scrim behind */
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
                int rcx, rcy, rcw, rch; client_rect(&cw[k], &rcx, &rcy, &rcw, &rch);   /* fullscreen-aware */
                print("[twm] win "); print(cw[k].title);
                printc(' '); printu((unsigned)rcx);
                printc(' '); printu((unsigned)rcy);
                printc(' '); printu((unsigned)rcw); printc(' '); printu((unsigned)rch);
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
                        if (!cw[k].min) {               /* a minimized window is off-screen */
                            /* map the surface-relative damage to the screen via the
                             * fullscreen-aware client origin: a maximized window's client
                             * is blitted at (0,0) with NO title offset, so adding wth here
                             * (as the old code did) shoved the dirty band a title-bar's
                             * worth too low -- the real change (e.g. the top ".." row's
                             * hover) never got repainted and the offset band smeared. */
                            int sx, sy, scw, sch; client_rect(&cw[k], &sx, &sy, &scw, &sch);
                            int dw = (int)snap[j].dmgw, dh = (int)snap[j].dmgh;
                            int full = dw <= 0 || dh <= 0 ||                 /* no partial info */
                                       (snap[j].dmgx == 0 && snap[j].dmgy == 0 &&
                                        dw >= scw && dh >= sch);
                            if (full) add_dirty(sx, sy, scw, sch);
                            else {                                            /* composite only the damaged sub-rect */
                                int dx = (int)snap[j].dmgx, dy = (int)snap[j].dmgy;
                                if (dx + dw > scw) dw = scw - dx;
                                if (dy + dh > sch) dh = sch - dy;
                                if (dw > 0 && dh > 0) add_dirty(sx + dx, sy + dy, dw, dh);
                            }
                        }
                    }
                }
            }
        }

        /* --- dock: rebuild + recentre when the running set changes ----------- */
        unsigned rsig = running_sig();
        if (rsig != dock_sig) {
            dock_sig = rsig;
            int dsp; ugfx_elevation_extent(DOCK_ELEVATION, &dsp, 0);
            int oldtop = dock_y - dsp;
            rebuild_dock();
            layout_dock();
            int newtop = dock_y - dsp;
            int top = oldtop < newtop ? oldtop : newtop;
            add_dirty(0, top, W, H - top);          /* repaint the dock band over old + new extent */
        }

        /* --- Launchpad overlay: full repaint when it opens/closes so the dim scrim
         * is painted over (or cleared from) the whole screen, not just its rect --- */
        int ov_now = overlay_slot() >= 0;
        if (ov_now != overlay_on) { overlay_on = ov_now; add_dirty(0, 0, W, H); }
        int md_now = modal_slot() >= 0;
        if (md_now != modal_on) { modal_on = md_now; add_dirty(0, 0, W, H); }   /* paint/clear the modal scrim */

        /* --- input: keys to the focused window ------------------------------- */
        if (pending_until && frame >= pending_until) pending_until = 0;   /* in-flight launch gave up */
        int key, f = focus_slot();
        while ((key = twm_getkey()) >= 0) {
            /* While a WIN_MODAL dialog (picker) is up, swallow the keys that would steal
             * its focus or stack another overlay over it (the launchers + Alt-Tab), so it
             * stays truly modal. Keys that act on the focused window (Esc/menu accels) and
             * Super+Q (= cancel the dialog) still pass through to it below. */
            if (modal_slot() >= 0 &&
                (key == KEY_SUPER_V || key == KEY_SUPER_SPACE || key == KEY_LAUNCHPAD || key == KEY_ALT_TAB))
                continue;
            if (key == KEY_SUPER_V)     { dismiss_launchers("Clipboard"); summon("Clipboard", "clipboard", frame); continue; }  /* clipboard manager */
            if (key == KEY_SUPER_SPACE) { dismiss_launchers("Spotlight"); summon("Spotlight", "spotlight", frame); continue; }  /* Spotlight search  */
            if (key == KEY_LAUNCHPAD) {                 /* Launchpad: toggle -- a second Super tap dismisses it */
                int lp = find_app_window("Launchpad");
                if (lp >= 0) wm_post(cw[lp].id, WEV_CLOSE, 0);
                else { dismiss_launchers("Launchpad"); summon("Launchpad", "launchpad", frame); }
                continue;
            }
            if (key == KEY_ALT_TAB)     { window_switch(frame, kbd_mods() & KMOD_SHIFT); continue; }  /* switcher */
            if (sw_overlay) {                            /* keys while the switcher card is up */
                if (key == 27) {                         /* ESC, or a nav-key CSI -> peek the next byte */
                    int nx = twm_getkey();
                    if (nx == '[' || nx == 'O') {
                        int c2 = twm_getkey();
                        if (c2 == 'C' || c2 == 'B')      window_switch(frame, 0);   /* Right/Down -> next */
                        else if (c2 == 'D' || c2 == 'A') window_switch(frame, 1);   /* Left/Up   -> prev */
                        continue;
                    }
                    switch_cancel(); continue;           /* a lone Esc cancels the switch */
                }
                if (key == '\n' || key == '\r') { switch_commit(); continue; }      /* Enter commits */
                switch_commit();                         /* any other key commits, then is delivered */
            }                                            /* ...to the now-focused window (fall through) */
            f = focus_slot();                                            /* a chord above may have changed focus */
            if (key == KEY_SUPER_Q)   { if (f >= 0) wm_post(cw[f].id, WEV_CLOSE, 0); continue; }  /* close focused */
            if (key == KEY_SUPER_KILL){ if (f >= 0) wm_kill(cw[f].id);              continue; }  /* force-kill focused process */
            if (key == KEY_SUPER_F)   { if (f >= 0 && !cw[f].popup) toggle_max(f);  continue; }  /* toggle fullscreen */
            if (key == 27) {
                /* ESC is ambiguous: a lone Esc dismisses a popup, but it also
                 * leads every CSI/SS3 nav-key sequence (arrows = ESC [ A..D),
                 * which the keyboard driver queues atomically. Peek the next
                 * byte: '[' or 'O' means it's a nav/function key -> forward the
                 * whole sequence to the app; anything else is a standalone Esc. */
                int nx = twm_getkey();
                if (nx == '[' || nx == 'O') {
                    if (f >= 0) { send_key(cw[f].id, 27); send_key(cw[f].id, nx); }
                    continue;
                }
                if (nx >= 0) twm_ungetkey(nx);                            /* unrelated key follows; handle it next */
                if (f >= 0 && cw[f].popup) { wm_post(cw[f].id, WEV_CLOSE, 0); continue; }  /* lone Esc dismisses a popup */
                if (f >= 0) send_key(cw[f].id, 27);                       /* bare Esc to a non-popup app */
                continue;
            }
            /* App-menu keyboard accelerators (#6): Ctrl+<letter> while a window with a
             * declared menu is focused fires the matching enabled item as a WEV_MENU
             * (the same path a click takes), instead of forwarding the control byte.
             * Backspace/Tab/Enter/Esc arrive without Ctrl held, so they never match. */
            if (f >= 0 && (kbd_mods() & KMOD_CTRL) && key >= 1 && key <= 26) {
                char want = (char)('A' + key - 1);
                int am = -1, ai = -1;
                for (int mi = 0; mi < (int)cur_menu.nmenus && am < 0; mi++)
                    for (int ii = 0; ii < (int)cur_menu.m[mi].nitems; ii++)
                        if (cur_menu.m[mi].accel[ii] == want && !(cur_menu.m[mi].flags[ii] & WMI_DISABLED)) { am = mi; ai = ii; break; }
                if (am >= 0) {
                    wm_post(cw[f].id, WEV_MENU, WEV_MENU_PACK(am, ai));
                    print("[twm] accel "); printc(want); printc(' '); printu((unsigned)am); printc(' '); printu((unsigned)ai); print("\r\n");
                    continue;
                }
            }
            if (f >= 0) send_key(cw[f].id, key);
        }
        /* Modifier key-UP: the byte stream is down-only, so watch the live mask and
         * post WEV_KEYUP to the focused window when a modifier is released (the new
         * mask still held). Unblocks release-to-commit gestures (e.g. Alt-Tab). */
        {
            unsigned m = kbd_mods(); int kf = focus_slot();
            if (m != last_kmods) {
                if ((last_kmods & ~m) && kf >= 0) {      /* a modifier was released */
                    wm_post(cw[kf].id, WEV_KEYUP, m);
                    print("[twm] keyup "); printu(m); print("\r\n");
                }
                last_kmods = m;
            }
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
        refresh_app_menu();                              /* pick up the focused app's declared menu (#6) */
        if (cc_open && moved) dirty_cc();                          /* repaint CC hover states + shadow halo (no smear) */
        if (nc_open && moved) dirty_nc();                          /* notification center, ditto    */
        if (toast_live && moved) dirty_toast();                    /* live toast: keep its drop-shadow halo clean as the cursor sweeps it */
        if (menu_kind && moved) dirty_menu();                      /* dropdown row hover            */
        {   /* menu-bar tile hover: repaint the bar only when the hovered tile changes */
            int bh = 0;
            if (bar_y > -bar_h && cur_y >= 0 && cur_y < bar_h) {
                if (cur_x >= 8 && cur_x < 8 + logo_hit_w) bh = 1;
                else if (focus_slot() >= 0 && cur_x >= app_hit_x && cur_x < app_hit_x + app_hit_w) bh = 2;
            }
            if (bh != bar_hover) { bar_hover = bh; add_dirty(0, 0, W, bar_h); }
        }
        /* scroll wheel -> the top-most window under the cursor, client-relative (WEV_SCROLL) */
        if (ms.wheel) {
            for (int zi = nz - 1; zi >= 0; zi--) {
                struct cwin *c = &cw[zo[zi]];
                if (c->min) continue;
                int ox, oy, ow, oh; outer_rect(c, &ox, &oy, &ow, &oh);
                if (ms.x < ox || ms.x >= ox + ow || ms.y < oy || ms.y >= oy + oh) continue;
                int cx, cy, cwd, chd; client_rect(c, &cx, &cy, &cwd, &chd);
                int rx = ms.x - cx, ry = ms.y - cy; if (ry < 0) ry = 0;
                wm_post(c->id, WEV_SCROLL, WEV_MOUSE_PACK(rx, ry, (unsigned)(ms.wheel & 0xff)));
                break;
            }
        }
        int down = ms.buttons & 1;
        if (down && !last_b) {                          /* press edge */
            int handled = 0;
            if (sw_overlay) { switch_click(ms.x, ms.y); handled = 1; }   /* Alt-Tab card grabs the click */
            /* A WIN_MODAL dialog (the picker) locks input: a click anywhere OUTSIDE it is
             * swallowed (the dialog stays up + topmost), so the dimmed windows behind it
             * are inert -- a real modal. A click inside falls through to the window loop
             * below, which forwards it to the (topmost) dialog. */
            if (!handled) {
                int md = modal_slot();
                if (md >= 0) {
                    int ox, oy, ow, oh; outer_rect(&cw[md], &ox, &oy, &ow, &oh);
                    int inside = ms.x >= ox && ms.x < ox + ow && ms.y >= oy && ms.y < oy + oh;
                    if (!inside) {
                        if (focus_slot() != md) { zo_raise(md); dirty_win(&cw[md]); add_dirty(0, 0, W, bar_h); }
                        handled = 1;                    /* swallow: no bar/dock/other-window interaction */
                    }
                }
            }
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
            /* Menu bar (#6/#8): the logo (system menu) and the focused app's name are
             * clickable menu tiles. The dropdown is modal-lite like Control Center: a
             * click on an item runs it, a click anywhere else dismisses it. */
            int on_logo = bar_y > -bar_h && ms.y >= 0 && ms.y < bar_h && ms.x >= 8 && ms.x < 8 + logo_hit_w;
            int on_appm = bar_y > -bar_h && ms.y >= 0 && ms.y < bar_h && focus_slot() >= 0 &&
                          ms.x >= app_hit_x && ms.x < app_hit_x + app_hit_w;
            int on_amenu = -1;                          /* an app-declared menu tile (#6) */
            if (bar_y > -bar_h && ms.y >= 0 && ms.y < bar_h)
                for (int i = 0; i < (int)cur_menu.nmenus && i < WINMENU_MAX; i++)
                    if (ms.x >= appmenu_x[i] && ms.x < appmenu_x[i] + appmenu_w[i]) { on_amenu = i; break; }
            if (menu_kind && !handled) {
                handled = 1;
                if (ms.x >= menu_x && ms.x < menu_x + menu_w && ms.y >= menu_y && ms.y < menu_y + menu_h) {
                    int idx = menu_row_at(ms.y);
                    if (idx >= 0 && idx < menu_nitems) menu_click(idx); else menu_close();
                } else if (on_amenu >= 0) {             /* clicked another tile -> switch menus */
                    menu_close(); menu_app_idx = on_amenu; menu_open_kind(3, appmenu_x[on_amenu]);
                } else menu_close();
            } else if (!handled && on_logo) { menu_open_kind(1, 8);          handled = 1; }
              else if (!handled && on_appm) { menu_open_kind(2, app_hit_x);  handled = 1; }
              else if (!handled && on_amenu >= 0) { menu_app_idx = on_amenu; menu_open_kind(3, appmenu_x[on_amenu]); handled = 1; }
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
                if (nc_open) { nc_open = 0; dirty_nc(); }   /* the bar's panels are mutually exclusive */
                if (menu_kind) menu_close();
                dirty_cc(); add_dirty(0, 0, W, bar_h);
            }
            /* Notification center: the bell status item toggles it; opening it clears
             * the unseen badge. Same dismiss model as CC (a click outside closes it). */
            int on_bell = bar_y > -bar_h && ms.y >= 0 && ms.y < bar_h &&
                          ms.x >= sb_bell_x - 4 && ms.x < sb_bell_x + SB_BELL_W + 4;
            if (nc_open) {
                handled = 1;
                if (nc_clear_hit(ms.x, ms.y)) {                   /* Clear: empty the ring, stay open */
                    dirty_nc();                                   /* invalidate the OLD (tall) panel + halo BEFORE the height shrinks */
                    notes_n = 0; notes_head = 0; notes_unseen = 0; nc_slide = 0;
                    for (int i = 0; i < NOTE_KEEP; i++) note_exp[i] = 0;
                    dirty_nc(); add_dirty(0, 0, W, bar_h);        /* then the NEW (short) empty panel -- union repaints the ghost rows */
                    print("[twm] notifcenter clear\r\n");
                } else if (nc_click_row(ms.x, ms.y)) {            /* a row chevron: expand/collapse, stay open */
                    add_dirty(0, 0, W, bar_h);
                } else {                                          /* any other click closes the center... */
                    int ri = nc_row_at(ms.x, ms.y);               /* ...routing a row click to its sender first */
                    if (ri >= 0 && notes[ri].target[0]) {
                        print("[twm] notif open "); print(notes[ri].target); print("\r\n");
                        notif_activate(notes[ri].target, frame);
                    }
                    nc_open = 0;
                    dirty_nc(); add_dirty(0, 0, W, bar_h);
                }
            } else if (on_bell) {
                nc_open = 1; handled = 1; notes_unseen = 0;
                if (cc_open) { cc_open = 0; dirty_cc(); }
                if (menu_kind) menu_close();
                if (toast_live) toast_kill();                       /* center supersedes the toast */
                dirty_nc(); add_dirty(0, 0, W, bar_h);
                print("[twm] notifcenter open "); printu((unsigned)notes_n);
                print(" "); printu((unsigned)nc_x); print(" "); printu((unsigned)nc_y); print("\r\n");
            }
            /* The live toast: a click anywhere on it is consumed (macOS-style, never
             * leaks to a window behind); toast_click routes the X/chevron/body. */
            if (toast_live && !handled && toast_click(ms.x, ms.y, frame)) handled = 1;
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
                    if (icons[i].special) { dismiss_launchers("Launchpad"); summon("Launchpad", "launchpad", frame); break; }  /* single click */
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
                int ox, oy, ow, oh; outer_rect(c, &ox, &oy, &ow, &oh);
                if (ms.x < ox || ms.x >= ox + ow || ms.y < oy || ms.y >= oy + oh) continue;
                int shiftbit = (kbd_mods() & KMOD_SHIFT) ? WEV_MOUSE_SHIFT : 0;
                if (focus_slot() != slot) {             /* raise + refocus */
                    int prev = focus_slot();
                    zo_raise(slot);
                    dirty_win(c);
                    if (prev >= 0) dirty_win(&cw[prev]);
                    add_dirty(0, 0, W, bar_h);
                }
                if (c->popup) {                         /* no chrome: the whole surface is client */
                    wm_post(c->id, WEV_MOUSE, WEV_MOUSE_PACK(ms.x - c->wx, ms.y - c->wy, (ms.buttons & 1) | shiftbit));
                    cdrag = c->id;                      /* allow drag-select inside the overlay */
                    handled = 1;
                    continue;
                }
                if (is_fs(c)) {                         /* fullscreen: client fills the screen, title bar slides */
                    int ty = fs_titlebar_y();
                    if (ty + TH > 0 && ms.y >= ty && ms.y < ty + TH) {     /* on the revealed title bar */
                        int ly = ty + TH / 2, dy = ms.y - ly, hit = 9 * 9;
                        int cxc = W - 18, cxx = cxc - 22, cxm = cxx - 22;
                        if ((ms.x - cxc) * (ms.x - cxc) + dy * dy <= hit)      wm_post(c->id, WEV_CLOSE, 0);
                        else if ((ms.x - cxx) * (ms.x - cxx) + dy * dy <= hit) toggle_max(slot);   /* green: restore */
                        else if ((ms.x - cxm) * (ms.x - cxm) + dy * dy <= hit) {                   /* minimize -> dock */
                            int ti = dock_tile_for(c->title);
                            struct rect from = { 0, ty, W, TH };
                            struct rect to = (ti >= 0)
                                ? (struct rect){ icons[ti].cx - 18, icons[ti].cy - 14, 36, 28 }
                                : (struct rect){ dock_x + dock_w / 2 - 18, dock_y, 36, 28 };
                            dirty_win(c); c->min = 1; zo_remove(slot);
                            anim_start(AN_MIN, slot, from, to, 12);
                            add_dirty(0, 0, W, H);
                        } else {                                                  /* double-click title: restore */
                            if (last_click_win == slot && frame - last_click_wframe <= DBL_FRAMES)
                                { toggle_max(slot); last_click_win = -1; }
                            else { last_click_win = slot; last_click_wframe = frame; }
                        }
                    } else {                            /* the full-screen client area */
                        wm_post(c->id, WEV_MOUSE, WEV_MOUSE_PACK(ms.x, ms.y, (ms.buttons & 1) | shiftbit));
                        cdrag = c->id;
                    }
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
                } else if (ms.x >= c->wx + ow - GRIP && ms.y >= c->wy + oh - GRIP) {
                    rsz = c->id; rsz_ox = c->wx; rsz_oy = c->wy;           /* resize grip */
                } else if (ms.y < c->wy + TH) {                            /* title bar -> drag, or double-click to fullscreen */
                    if (last_click_win == slot && frame - last_click_wframe <= DBL_FRAMES)
                        { toggle_max(slot); last_click_win = -1; }
                    else { last_click_win = slot; last_click_wframe = frame;
                           drag = c->id; drag_dx = ms.x - c->wx; drag_dy = ms.y - c->wy; }
                } else {                                                   /* client -> forward */
                    wm_post(c->id, WEV_MOUSE, WEV_MOUSE_PACK(ms.x - c->wx, ms.y - (c->wy + TH), (ms.buttons & 1) | shiftbit));
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
                int cx, cy, cwd, chd; client_rect(c, &cx, &cy, &cwd, &chd);
                int rx = ms.x - cx, ry = ms.y - cy;
                if (rx < 0) rx = 0; if (rx >= cwd) rx = cwd - 1;
                if (ry < 0) ry = 0; if (ry >= chd) ry = chd - 1;
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
                        int ox, oy, ow, oh; outer_rect(c, &ox, &oy, &ow, &oh);
                        if (ms.x < ox || ms.x >= ox + ow || ms.y < oy || ms.y >= oy + oh) continue;
                        if (!c->min && in_client(c, ms.x, ms.y)) {  /* over its client area (below any title) */
                            int cx, cy, cwd, chd; client_rect(c, &cx, &cy, &cwd, &chd);
                            hov = c->id; hx = ms.x - cx; hy = ms.y - cy;
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
                    int ox, oy, ow, oh; outer_rect(c, &ox, &oy, &ow, &oh);
                    if (ms.x < ox || ms.x >= ox + ow || ms.y < oy || ms.y >= oy + oh) continue;
                    if (!c->min && in_client(c, ms.x, ms.y)) {
                        int cx, cy, cwd, chd; client_rect(c, &cx, &cy, &cwd, &chd);
                        wm_post(c->id, WEV_MOUSE, WEV_MOUSE_PACK(ms.x - cx, ms.y - cy, 2));
                    }
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
            int curid = curf >= 0 ? cw[curf].id : -1;
            if (curf != last_focus || curid != last_focus_id) {
                if (last_focus >= 0 && cw[last_focus].used) dirty_win(&cw[last_focus]);
                if (curf >= 0 && cw[curf].used) dirty_win(&cw[curf]);
                add_dirty(0, 0, W, bar_h);
                last_focus = curf;
                last_focus_id = curid;
                print("[twm] focus ");                  /* the harness watches this to assert focus moves */
                print(curf >= 0 ? cw[curf].title : "desktop");
                print("\r\n");
            }
        }

        rtc_time(&t);                                   /* clock ticks once a second */
        if (t.sec != last_sec) { last_sec = t.sec;     /* repaint the whole status cluster */
            int cx = cc_btn_x - 8; add_dirty(cx, 0, W - cx, bar_h);
            apply_settings_live(); }                    /* pick up Settings-app changes from disk */

        poll_notifications();                           /* drain notify() posts -> toast + center */
        switcher_tick(frame);                           /* Alt-Tab card: commit-on-release + highlight ease */
        if (nc_slide > 0) { nc_slide--; dirty_nc(); }   /* animate a row sliding into the center */
        toast_tick();                                   /* animate the toast (slide in/hold/out) */

        /* repaint the dock when the running/focus/minimized set changes, so its
         * indicators + active highlight stay in sync (cheap signature check). */
        unsigned sig = (unsigned)(focus_slot() + 1);
        for (int i = 0; i < MAXW; i++)
            if (cw[i].used) sig = sig * 31u + (unsigned)(i * 4 + (cw[i].min ? 1 : 0) + 1);
        int hovi = -1;                                  /* hovered dock tile -> repaint its lift */
        for (int i = 0; i < nicons; i++) if (tile_hovered(&icons[i])) hovi = i;
        sig = sig * 131u + (unsigned)(hovi + 1);
        if (sig != last_dock_sig) { last_dock_sig = sig;
            int dsp, ddy; ugfx_elevation_extent(DOCK_ELEVATION, &dsp, &ddy);
            struct rect db = shadow_box(dock_x, dock_y, dock_w, dock_h, dsp, ddy);
            add_dirty(db.x, db.y, db.w, db.h); }

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
