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
#include "statusicons.h"
#include "manifest.h"
#include "registry.h"
#include "textutil.h"

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
static int dock_runsep = -1;     /* tile index of the first running-unpinned app (the pinned|running boundary); -1 when no running-unpinned app exists, so no divider is drawn */
static unsigned dock_sig;        /* signature of the running set; the dock rebuilds when it changes */
static int dock_x, dock_y, dock_w, dock_h;
static int dock_y0;              /* dock's shown (base) y; dock_y is the current (animated) y */
static int bar_y;                /* bar's current top: 0 = shown, -bar_h = hidden            */
static int bar_linger, dock_linger;   /* reveal-linger counters for auto-hide               */

struct rect { int x, y, w, h; };
#define WIN_SHADOW_DY   6   /* window drop-shadow downward offset (>= the 5/6 used in draw) */
#define DOCK_ELEVATION  3   /* the ugfx_elevation level the dock floats at                  */
/* THE single source for every drop-shadow halo (invalidations AND culls): a rect grown
 * to include a shadow that feathers `spread`px outward and rides `dy`px downward. Keeps
 * the full `spread` on top and adds `dy` to the bottom for the offset, so the box never
 * under-covers the shadow regardless of the offset. `spread` matches ugfx_shadow's
 * feather; ugfx_elevation_extent supplies (spread, dy) for an elevation level. */
static struct rect shadow_box(int x, int y, int w, int h, int spread, int dy) {
    struct rect r = { x - spread, y - spread, w + 2 * spread, h + 2 * spread + dy };
    return r;
}
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
/* macOS-style switcher overlay (#7): Alt+Tab opens a centred card of window tiles
 * (icon + title) with an animated selection highlight. It commits on Alt-release
 * (after a genuine hold), a click on a tile, Enter, or the linger timeout; ESC
 * cancels. Holding Alt keeps it up on real hardware; under the test harness (which
 * can't hold a modifier) the linger timeout commits, so the card is still visible. */
#define SW_CELL      108             /* per-tile cell width (icon + title)          */
#define SW_PAD       18
#define SW_SHADOW_SP 28
#define SW_SHADOW_DY 8
static int sw_overlay;               /* 1 while the switcher card is shown          */
static int sw_alt_frames;            /* consecutive frames Alt seen held this session */
static int sw_sel_x, sw_target_x;    /* animated highlight centre (px)              */
static int sw_px, sw_py, sw_pw, sw_ph;  /* card rect (set by switcher_layout)        */

/* Control Center: a quick-settings slide-over opened from a menu-bar status item
 * (design/ui.md). Live toggles for the dock/bar auto-hide (the registry keys
 * update_chrome already reads) + a system summary + power actions. */
#define CC_PAD 14
#define CC_RAD       16     /* control-center panel corner radius          */
#define CC_SHADOW_SP 24     /* control-center drop-shadow feather (px)      */
#define CC_SHADOW_DY 6      /* control-center shadow vertical offset (px)   */
/* dirty the whole panel INCLUDING its shadow halo (so opening/closing it leaves no
 * shadow residue) via the single shadow_box() extent. */
#define dirty_cc() do { struct rect _r = shadow_box(cc_x, cc_y, cc_w, cc_h, CC_SHADOW_SP, CC_SHADOW_DY); \
                        add_dirty(_r.x, _r.y, _r.w, _r.h); } while (0)
static int cc_open;
static int cc_x, cc_y, cc_w = 268, cc_h;          /* panel rect (h computed in cc_layout) */
static int cc_btn_x, cc_btn_w = 24;               /* the bar status-item hit box           */
static int cc_row1_y, cc_row2_y, cc_sep_y, cc_info_y, cc_btn_yy;
static void draw_cc_button(int y);                /* defined below; called from draw_bar */
static void draw_switcher(void);                  /* Alt-Tab overlay; called from compose */

/* Status cluster (top bar, right side; design/ui.md phase 2): placeholder system
 * glyphs (network / volume / battery) + a registry-driven clock. The glyphs are
 * honest placeholders -- there are no battery/audio/net drivers yet -- so they
 * read as iconic indicators, not as fake readings. Laid out right-to-left in
 * cc_layout() into fixed slots so the cluster never jitters as the clock ticks. */
#define SB_GLYPH   ARGB(235, 222, 228, 240)   /* status-glyph ink (alpha-aware)      */
#define SB_NET_W   STATUSICON_SZ              /* all glyphs are the same square Lucide box */
#define SB_VOL_W   STATUSICON_SZ
#define SB_BAT_W   STATUSICON_SZ
#define SB_BELL_W  STATUSICON_SZ
#define SB_GAP     12
static int sb_clk_w;                          /* reserved (worst-case) clock width   */
static int sb_net_x, sb_vol_x, sb_bat_x, sb_bell_x;   /* glyph left edges            */

/* Notifications (design/ui.md phase 3): apps post via notify(); twm drains the
 * kernel queue, ring-buffers the recent ones for the notification center, and
 * slides the newest in as a top-right toast (slide-in / hold / slide-out, no
 * alpha so the dirty-rect math stays simple). The bell status item toggles the
 * center and carries an unseen badge. */
#define NOTE_KEEP   8                /* recent notifications kept for the center      */
#define NC_W        300              /* notification-center panel width               */
#define NC_PAD      14
#define NC_ROW      (2 * fh + 14)    /* a center row: title + body + padding          */
#define NC_SHADOW_SP 24
#define NC_SHADOW_DY 6
#define TOAST_W     300
#define TOAST_PAD   12
#define TOAST_IN    7                /* slide-in frames                               */
#define TOAST_HOLD  300              /* visible frames (~3.7s at 12ms/frame)          */
#define TOAST_OUT   9                /* slide-out frames                              */
#define TOAST_LIFE  (TOAST_IN + TOAST_HOLD + TOAST_OUT)
#define TOAST_MAXLINES 6             /* max body lines when a toast is expanded       */
#define NC_SLIDE    8                /* frames to slide a new row into an open center */
#define NC_MAXLINES 6                /* max body lines when a center row is expanded  */
static struct notif notes[NOTE_KEEP];
static unsigned char note_exp[NOTE_KEEP];        /* per-row expanded flag (ring-indexed) */
static int notes_n, notes_head, notes_unseen;   /* ring of kept notifications        */
static int nc_open, nc_x, nc_y, nc_h;            /* notification-center panel          */
static int nc_slide;                             /* >0: animate the newest center row */
static struct notif toast;                       /* the active toast                  */
static int toast_live, toast_age;                /* toast_live=0 -> none; else age 0.. */
static int toast_linger;                          /* frames to keep cleaning the footprint after it dies */
static int toast_expanded;                       /* body expanded to full (wrapped)   */
static int toast_paused;                         /* hover/click froze the auto-dismiss */
static int toast_collapsible;                    /* body was truncated -> chevron      */

/* Menu bar (#6/#8): the logo and the focused app's name are clickable menu tiles
 * that open a dropdown. Today the menus are compositor-owned (the logo's system
 * menu + the app tile's universal About/Quit); an app->WM protocol for apps to add
 * their own File/Edit/Help tiles is the remaining half of #6. */
#define MENU_MAXI 5                  /* max items in a dropdown            */
#define MENU_ROW  (fh + 12)          /* dropdown row height                */
#define MENU_PAD  8
#define MENU_SHADOW_SP 22
#define MENU_SHADOW_DY 6
static int menu_kind;                /* 0 none, 1 logo (system), 2 app     */
static int menu_x, menu_y, menu_w, menu_h;
static const char *menu_items[MENU_MAXI];
static int menu_nitems;
static char menu_about[40];          /* dynamic "About <app>" label        */
static int logo_hit_w;               /* logo click region width (set in draw_bar) */
static int app_hit_x, app_hit_w;     /* focused-app name click region (set in draw_bar) */

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

/* --- status-cluster glyphs: crisp Lucide line icons (tools/genstatus.py), blitted
 * as alpha masks recoloured to `col`, centred vertically on cy with left edge x.
 * Replaces the old hand-stacked 1px-rectangle glyphs, which had no anti-aliasing. */
static void draw_status_glyph(int x, int cy, uint32_t col, int idx) {
    ugfx_blit_tint(x, cy - STATUSICON_SZ / 2, STATUSICON_SZ, STATUSICON_SZ,
                   statusicons_argb[idx], col);
}
static void draw_net_glyph(int x, int cy, uint32_t col) { draw_status_glyph(x, cy, col, STATUSICON_WIFI); }
static void draw_vol_glyph(int x, int cy, uint32_t col) { draw_status_glyph(x, cy, col, STATUSICON_VOL); }
static void draw_bat_glyph(int x, int cy, uint32_t col) { draw_status_glyph(x, cy, col, STATUSICON_BATT); }
static void draw_bell_glyph(int x, int cy, uint32_t col, int badge) {
    draw_status_glyph(x, cy, col, STATUSICON_BELL);
    if (badge) ugfx_rrect_a(x + SB_BELL_W - 4, cy - STATUSICON_SZ / 2 - 1, 6, 6, 3, g_accent);  /* unseen badge */
}

/* Build the menu-bar clock from the registry: clock.format (24h|12h),
 * clock.seconds (bool), clock.weekday (bool). Read live so settings drive the
 * format. Emits the formatted string to serial once, for the harness. */
static void build_clock(char *clk, struct rtctime *t) {
    int sec = reg_bool("clock.seconds", 1);
    int h24 = streqz(reg_get("clock.format", "24h"), "24h");
    int wd  = reg_bool("clock.weekday", 1);
    char *p = clk;
    if (wd) { const char *w = weekday(t->year, t->month, t->day);
              *p++ = w[0]; *p++ = w[1]; *p++ = w[2]; *p++ = ' '; }
    int hh = t->hour;
    if (!h24) { hh %= 12; if (hh == 0) hh = 12; }
    two(p, (unsigned)hh); p += 2; *p++ = ':'; two(p, t->min); p += 2;
    if (sec) { *p++ = ':'; two(p, t->sec); p += 2; }
    if (!h24) { *p++ = ' '; *p++ = (t->hour < 12) ? 'A' : 'P'; *p++ = 'M'; }
    *p = 0;
    static int traced;
    if (!traced) { traced = 1; print("[twm] clk \""); print(clk); print("\"\r\n"); }
}

/* The bar is drawn at its current slide offset bar_y (0 shown, -bar_h hidden). The
 * desktop/window behind it is already painted by compose(), so the translucent
 * glass blends over it -- no internal draw_desk needed. */
static void draw_bar(void) {
    int y = bar_y;
    ugfx_frost(0, y, W, bar_h, 0, TH_BAR_FROST);        /* frosted-glass bar         */
    ugfx_fill_a(0, y, W, 1, ARGB(26, 255, 255, 255));   /* lit top edge (glass)      */
    ugfx_fill_a(0, y + bar_h - 1, W, 1, TH_BARLINE_A);  /* hairline                  */
    /* the logo is a menu tile: a hover/active state layer marks it clickable */
    logo_hit_w = LOGO_W + 16;
    int logo_hot = cur_y >= y && cur_y < y + bar_h && cur_x >= 8 && cur_x < 8 + logo_hit_w;
    if (menu_kind == 1 || logo_hot)
        ugfx_rrect_a(8, y + 3, logo_hit_w, bar_h - 6, TH_R_SM,
                     menu_kind == 1 ? ARGB(40, 120, 170, 255) : ARGB(28, 255, 255, 255));
    ugfx_blit_argb(14, y + (bar_h - LOGO_H) / 2, LOGO_W, LOGO_H, logo_argb);
    int f = focus_slot();
    const char *app = (f >= 0) ? cw[f].title : "tOS";
    app_hit_x = 14 + LOGO_W + 12 - 6; app_hit_w = ugfx_text_w(app) + 12;   /* app-name menu tile */
    { static int last_ax = -1, last_aw = -1;            /* report tile geometry for the harness */
      if (app_hit_x != last_ax || app_hit_w != last_aw) {
          last_ax = app_hit_x; last_aw = app_hit_w;
          print("[twm] menubar logo 8 "); printu((unsigned)logo_hit_w);
          print(" app "); printu((unsigned)app_hit_x); printc(' '); printu((unsigned)app_hit_w); print("\r\n"); } }
    int app_hot = (f >= 0) && cur_y >= y && cur_y < y + bar_h && cur_x >= app_hit_x && cur_x < app_hit_x + app_hit_w;
    if (menu_kind == 2 || app_hot)
        ugfx_rrect_a(app_hit_x, y + 3, app_hit_w, bar_h - 6, TH_R_SM,
                     menu_kind == 2 ? ARGB(40, 120, 170, 255) : ARGB(28, 255, 255, 255));
    ugfx_text(14 + LOGO_W + 12, y + (bar_h - fh) / 2, app, TH_TEXT, UGFX_TRANSPARENT);
    int cy = y + bar_h / 2;                             /* status cluster: glyphs + clock */
    draw_net_glyph(sb_net_x, cy, SB_GLYPH);
    draw_vol_glyph(sb_vol_x, cy, SB_GLYPH);
    draw_bat_glyph(sb_bat_x, cy, SB_GLYPH);
    draw_bell_glyph(sb_bell_x, cy, nc_open ? g_accent : SB_GLYPH, notes_unseen);
    struct rtctime t; rtc_time(&t);
    char clk[20]; build_clock(clk, &t);                 /* registry-driven menu-bar clock */
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
    else if (ic->img) ugfx_blit_scaled(x, y, APPICON_SZ, APPICON_SZ, ic->img, ic->iw, ic->ih);  /* hi-res bundle icon -> tile */
    else              ugfx_blit_argb(x, y, APPICON_SZ, APPICON_SZ, appicons_argb[ICON_APP]);  /* generic fallback */
    int iy = y + APPICON_SZ + 3;                    /* running indicator under the tile */
    if (st & 2)        ugfx_rrect_a(ic->cx - 9, iy, 18, 3, 1, g_accent);   /* focused: accent bar */
    else if (st & 1)   ugfx_rrect_a(ic->cx - 2, iy, 4, 3, 1, ARGB(160, 200, 210, 230));     /* running: dot */
}
static void draw_dock(void) {
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
    /* Right-side status cluster, laid out right-to-left into fixed slots so the
     * glyphs + CC button don't jitter as the clock ticks. Reserve the worst-case
     * clock width ("Ddd 00:00:00 PM": 12h + weekday + seconds) so a format change
     * can't push glyphs under the clock. Order L->R: [CC][net][vol][bat][clock]. */
    sb_clk_w = ugfx_text_w("Ddd 00:00:00 PM");
    int rx = W - 16 - sb_clk_w - SB_GAP;          /* right edge of the glyph cluster */
    sb_bell_x = rx - SB_BELL_W; rx = sb_bell_x - SB_GAP;
    sb_bat_x  = rx - SB_BAT_W;  rx = sb_bat_x - SB_GAP;
    sb_vol_x  = rx - SB_VOL_W;  rx = sb_vol_x - SB_GAP;
    sb_net_x  = rx - SB_NET_W;  rx = sb_net_x - SB_GAP;
    cc_btn_x  = rx - cc_btn_w;                    /* CC button, left of the cluster  */
    nc_x = W - NC_W - 12; nc_y = bar_h + 8;       /* notification center, right edge */
    print("[twm] statusbar net "); printu((unsigned)sb_net_x);
    print(" vol "); printu((unsigned)sb_vol_x);
    print(" bat "); printu((unsigned)sb_bat_x);
    print(" bell "); printu((unsigned)sb_bell_x);
    print(" cc ");  printu((unsigned)cc_btn_x); print("\r\n");
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
    struct rect r = shadow_box(cc_x, cc_y, cc_w, cc_h, CC_SHADOW_SP, CC_SHADOW_DY);
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
static void draw_cc_button(int y) {               /* the menu-bar status item (Lucide sliders) */
    int cy = y + bar_h / 2;
    draw_status_glyph(cc_btn_x + (cc_btn_w - STATUSICON_SZ) / 2, cy,
                      cc_open ? g_accent : SB_GLYPH, STATUSICON_CC);
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

/* ------------------------------------------------------------ notifications */
/* Copy src into dst, hard-truncating with a trailing ".." so it fits maxw px. */
static void fit_text(char *dst, int cap, const char *src, int maxw) {
    int n = 0; while (src[n] && n < cap - 1) n++;
    for (int len = n; len >= 0; len--) {
        int i = 0; for (; i < len && i < cap - 3; i++) dst[i] = src[i];
        if (len < n) { dst[i++] = '.'; dst[i++] = '.'; }
        dst[i] = 0;
        if (ugfx_text_w(dst) <= maxw) return;
    }
    dst[0] = 0;
}
/* Greedy word-wrap of src to maxw px, capped at TOAST_MAXLINES. When draw, render
 * each line at (x, y0 + i*fh) in col; always returns the line count (>=1). */
/* the wrap logic itself is the pure tu_wrap (textutil.h, unit-tested); here we
 * only supply the pixel-width measurer and a draw-emit callback. */
struct toast_emit_ctx { int x, y0; uint32_t col; };
static void toast_emit(void *vc, const char *line, int idx) {
    struct toast_emit_ctx *c = (struct toast_emit_ctx *)vc;
    ugfx_text(c->x, c->y0 + idx * fh, line, c->col, UGFX_TRANSPARENT);
}
static int toast_wrap(const char *src, int maxw, int x, int y0, uint32_t col, int draw) {
    if (!draw) return tu_wrap(src, maxw, TOAST_MAXLINES, ugfx_text_w, 0, 0);
    struct toast_emit_ctx c = { x, y0, col };
    return tu_wrap(src, maxw, TOAST_MAXLINES, ugfx_text_w, toast_emit, &c);
}
static int toast_body_lines(void) {
    return toast_expanded ? toast_wrap(toast.body, TOAST_W - 2 * TOAST_PAD, 0, 0, 0, 0) : 1;
}
static int toast_height(void) { return TOAST_PAD * 2 + fh + 4 + toast_body_lines() * fh; }
/* The active toast slides in from the right edge, holds, then slides back out. */
static void toast_box(int *x, int *y, int *w, int *h) {
    *w = TOAST_W;
    *h = toast_height();
    int base_x = W - TOAST_W - 12, travel = TOAST_W + 24, off = 0;
    if (toast_age < TOAST_IN)
        off = travel * (TOAST_IN - toast_age) / TOAST_IN;
    else if (toast_age >= TOAST_IN + TOAST_HOLD)
        off = travel * (toast_age - (TOAST_IN + TOAST_HOLD)) / TOAST_OUT;
    *x = base_x + off; *y = bar_h + 10;
}
static void dirty_toast(void) {                         /* the toast's full travel band + shadow halo */
    int base_x = W - TOAST_W - 12, y = bar_h + 10;
    int hmax = TOAST_PAD * 2 + fh + 4 + TOAST_MAXLINES * fh;   /* worst-case expanded height */
    /* the drop-shadow (draw_toast: ugfx_shadow spread 18, dy +4) bleeds ~18px LEFT
     * and ~14px ABOVE the card -- the old 8/4px margins left that strip uncleaned, so
     * a hover smeared a shadow ghost that lingered after the toast was gone. Cover the
     * full halo on every side (the band already runs to the right screen edge). */
    int sp = 20;
    add_dirty(base_x - sp, y - sp, (W - base_x) + sp, hmax + 2 * sp + 8);
}
/* Retire the toast, then keep re-cleaning its footprint for a few frames. The
 * soft drop-shadow is the one thing that has repeatedly smeared (the halo bleeds
 * past the card and any single dirty rect that misses it leaves a ghost); lingering
 * the footprint in the dirty set for several frames after death makes residue
 * impossible regardless of how the cursor was moving when it vanished. */
static void toast_kill(void) { toast_live = 0; toast_linger = 4; dirty_toast(); }
static void draw_toast(void) {
    if (!toast_live) return;
    int x, y, w, h; toast_box(&x, &y, &w, &h);
    char buf[96];
    ugfx_shadow(x, y + 4, w, h, TH_R_MD, 18, TH_SHADOW, 120);   /* solid card (animates safely) */
    ugfx_rrect_aa(x, y, w, h, TH_R_MD, TH_SURF_3);
    ugfx_rrect_border(x, y, w, h, TH_R_MD, 1, TH_BORDER);
    ugfx_rrect_a(x + TOAST_PAD, y + TOAST_PAD + 2, 5, fh - 2, 2, g_accent);   /* accent stripe */
    int xbtn_x = x + w - TOAST_PAD - STATUSICON_SZ;         /* dismiss (X), top-right corner */
    int chev_x = xbtn_x - STATUSICON_SZ - 4;                /* expand chevron, left of the X */
    int btns = STATUSICON_SZ + 6 + (toast_collapsible ? STATUSICON_SZ + 4 : 0);
    fit_text(buf, sizeof buf, toast.title, w - 2 * TOAST_PAD - 12 - btns);
    ugfx_text(x + TOAST_PAD + 12, y + TOAST_PAD, buf, TH_TEXT, UGFX_TRANSPARENT);
    int gy = y + TOAST_PAD + fh / 2;
    if (toast_collapsible)                                  /* baked Lucide expand/collapse chevron */
        draw_status_glyph(chev_x, gy, TH_MUTED, toast_expanded ? STATUSICON_CHEVRON_UP : STATUSICON_CHEVRON_DOWN);
    draw_status_glyph(xbtn_x, gy, TH_MUTED, STATUSICON_X);  /* dismiss button -- instant close */
    int by = y + TOAST_PAD + fh + 4;
    if (toast_expanded) {
        toast_wrap(toast.body, w - 2 * TOAST_PAD, x + TOAST_PAD, by, TH_MUTED, 1);
    } else {
        fit_text(buf, sizeof buf, toast.body, w - 2 * TOAST_PAD);
        ugfx_text(x + TOAST_PAD, by, buf, TH_MUTED, UGFX_TRANSPARENT);
    }
}
/* A center row is "collapsible" when its body is too wide to fit one line, so it
 * earns a chevron; expanding it wraps the body and grows the row (and the panel). */
static int nc_collapsible(int idx) { return ugfx_text_w(notes[idx].body) > NC_W - 2 * NC_PAD - 4; }
static int nc_row_h(int idx) {
    if (note_exp[idx] && nc_collapsible(idx)) {
        int n = tu_wrap(notes[idx].body, NC_W - 2 * NC_PAD, NC_MAXLINES, ugfx_text_w, 0, 0);
        return NC_ROW + (n - 1) * fh;                   /* extra wrapped body lines */
    }
    return NC_ROW;
}
static void nc_layout(void) {
    int rows_h = 0;
    for (int k = 0; k < notes_n; k++)
        rows_h += nc_row_h((notes_head - 1 - k + 2 * NOTE_KEEP) % NOTE_KEEP);
    if (!notes_n) rows_h = NC_ROW;                       /* "No notifications" reserves one row */
    nc_h = NC_PAD + fh + 10 + rows_h + NC_PAD;
}
#define dirty_nc() do { nc_layout(); struct rect _r = shadow_box(nc_x, nc_y, NC_W, nc_h, NC_SHADOW_SP, NC_SHADOW_DY); \
                        add_dirty(_r.x, _r.y, _r.w, _r.h); } while (0)
static void draw_nc(void) {
    if (!nc_open) return;
    nc_layout();
    struct rect r = shadow_box(nc_x, nc_y, NC_W, nc_h, NC_SHADOW_SP, NC_SHADOW_DY);
    if (!rects_hit(cur_clip, r)) return;
    ugfx_shadow(nc_x, nc_y + NC_SHADOW_DY, NC_W, nc_h, CC_RAD, NC_SHADOW_SP, TH_SHADOW, 130);
    ugfx_frost(nc_x, nc_y, NC_W, nc_h, CC_RAD, TH_CC_FROST);
    ugfx_rrect_border(nc_x, nc_y, NC_W, nc_h, CC_RAD, 1, TH_BORDER_DIM);
    ugfx_fill_a(nc_x + CC_RAD, nc_y, NC_W - 2 * CC_RAD, 1, ARGB(40, 255, 255, 255));   /* top sheen */
    ugfx_text(nc_x + NC_PAD, nc_y + NC_PAD, "Notifications", TH_TEXT, UGFX_TRANSPARENT);
    if (notes_n) {                                      /* Clear button, header right */
        int cwid = ugfx_text_w("Clear");
        ugfx_text(nc_x + NC_W - NC_PAD - cwid, nc_y + NC_PAD, "Clear", g_accent, UGFX_TRANSPARENT);
    }
    int y = nc_y + NC_PAD + fh + 10;
    char buf[96];
    if (!notes_n) { ugfx_text(nc_x + NC_PAD, y, "No notifications", TH_MUTED, UGFX_TRANSPARENT); return; }
    for (int k = 0; k < notes_n; k++) {                 /* newest first */
        int idx = (notes_head - 1 - k + 2 * NOTE_KEEP) % NOTE_KEEP;
        struct notif *nn = &notes[idx];
        int rh = nc_row_h(idx), coll = nc_collapsible(idx), exp = note_exp[idx] && coll;
        int sx = (k == 0 && nc_slide > 0) ? (NC_W - 2 * NC_PAD) * nc_slide / NC_SLIDE : 0;  /* slide newest in */
        ugfx_rrect_a(nc_x + NC_PAD - 4 + sx, y - 3, NC_W - 2 * NC_PAD + 8, rh - 6, TH_R_SM, ARGB(40, 60, 68, 90));
        ugfx_rrect_a(nc_x + NC_PAD + 2 + sx, y + 2, 4, fh - 2, 2, g_accent);
        fit_text(buf, sizeof buf, nn->title, NC_W - 2 * NC_PAD - 14 - (coll ? STATUSICON_SZ + 4 : 0));
        ugfx_text(nc_x + NC_PAD + 12 + sx, y, buf, TH_TEXT, UGFX_TRANSPARENT);
        if (coll)                                       /* per-row expand chevron */
            draw_status_glyph(nc_x + NC_W - NC_PAD - STATUSICON_SZ + sx, y + fh / 2,
                              TH_MUTED, exp ? STATUSICON_CHEVRON_UP : STATUSICON_CHEVRON_DOWN);
        if (exp) {                                       /* wrapped full body */
            struct toast_emit_ctx ec = { nc_x + NC_PAD + sx, y + fh + 2, TH_MUTED };
            tu_wrap(nn->body, NC_W - 2 * NC_PAD, NC_MAXLINES, ugfx_text_w, toast_emit, &ec);
        } else {
            fit_text(buf, sizeof buf, nn->body, NC_W - 2 * NC_PAD - 4);
            ugfx_text(nc_x + NC_PAD + sx, y + fh + 2, buf, TH_MUTED, UGFX_TRANSPARENT);
        }
        y += rh;
    }
}
/* A click inside the open center: toggle a row's expand if its chevron was hit.
 * Returns 1 if a chevron toggled (caller keeps the center open), else 0. */
static int nc_click_row(int mx, int my) {
    int y = nc_y + NC_PAD + fh + 10;
    for (int k = 0; k < notes_n; k++) {
        int idx = (notes_head - 1 - k + 2 * NOTE_KEEP) % NOTE_KEEP;
        int rh = nc_row_h(idx);
        if (nc_collapsible(idx)) {
            int cxr = nc_x + NC_W - NC_PAD, cxl = cxr - STATUSICON_SZ - 6;
            if (mx >= cxl && mx < cxr + 2 && my >= y - 2 && my < y + fh + 4) {
                dirty_nc();                             /* OLD panel + halo before the height changes */
                note_exp[idx] = !note_exp[idx];
                dirty_nc();                             /* then the NEW (taller/shorter) panel */
                print("[twm] nc row expand "); printu((unsigned)note_exp[idx]); print("\r\n");
                return 1;
            }
        }
        y += rh;
    }
    return 0;
}
/* The note index under a click in the open center body, or -1. Used to route a
 * row click to its sender (the row background spans the panel width). */
static int nc_row_at(int mx, int my) {
    if (mx < nc_x + NC_PAD - 4 || mx >= nc_x + NC_W - NC_PAD + 4) return -1;
    int y = nc_y + NC_PAD + fh + 10;
    for (int k = 0; k < notes_n; k++) {
        int idx = (notes_head - 1 - k + 2 * NOTE_KEEP) % NOTE_KEEP;
        int rh = nc_row_h(idx);
        if (my >= y - 3 && my < y - 3 + rh - 6) return idx;
        y += rh;
    }
    return -1;
}
/* Drain the kernel notification queue: ring-buffer each for the center and start
 * a toast for the newest. Returns 1 if anything arrived (so the loop can repaint). */
static int poll_notifications(void) {
    struct notif nn; int got = 0;
    while (wm_poll_notify(&nn)) {
        notes[notes_head] = nn;
        note_exp[notes_head] = 0;                       /* a fresh note starts collapsed */
        notes_head = (notes_head + 1) % NOTE_KEEP;
        if (notes_n < NOTE_KEEP) notes_n++;
        print("[twm] notify "); print(nn.title); print("\r\n");   /* harness hook */
        if (nc_open) {                                  /* center open: slide into the list, no toast */
            nc_slide = NC_SLIDE;
            print("[twm] notif slide\r\n");
        } else {                                        /* otherwise pop a top-right toast */
            notes_unseen++;
            toast = nn; toast_live = 1; toast_age = 0;
            toast_expanded = 0; toast_paused = 0;
            toast_collapsible = ugfx_text_w(nn.body) > TOAST_W - 2 * TOAST_PAD;
            print("[twm] toast at "); printu((unsigned)(W - TOAST_W - 12)); print(" ");
            printu((unsigned)(bar_h + 10)); print(" "); printu(TOAST_W); print(" ");
            printu((unsigned)(TOAST_PAD * 2 + fh + 4 + fh)); print("\r\n");
        }
        got = 1;
    }
    if (got) {
        nc_layout();
        if (nc_open) dirty_nc(); else dirty_toast();
        add_dirty(sb_bell_x - 4, 0, SB_BELL_W + 12, bar_h);
    }
    return got;
}

/* ------------------------------------------------------------ menu bar */
#define dirty_menu() do { struct rect _r = shadow_box(menu_x, menu_y, menu_w, menu_h, MENU_SHADOW_SP, MENU_SHADOW_DY); \
                          add_dirty(_r.x, _r.y, _r.w, _r.h); } while (0)
static void menu_close(void) { if (menu_kind) { dirty_menu(); add_dirty(0, 0, W, bar_h); menu_kind = 0; } }
/* Open the logo (system) menu or the focused app's menu, anchored at tile_x. */
static void menu_open_kind(int kind, int tile_x) {
    if (cc_open) { cc_open = 0; dirty_cc(); }            /* the bar's panels are mutually exclusive */
    if (nc_open) { nc_open = 0; dirty_nc(); }
    menu_kind = kind; menu_nitems = 0;
    const char *app = "tOS";
    if (kind == 1) {                                 /* logo: the system menu */
        menu_items[menu_nitems++] = "About This tOS";
        menu_items[menu_nitems++] = "Preferences...";
        menu_items[menu_nitems++] = "Restart";
        menu_items[menu_nitems++] = "Shut Down";
    } else {                                         /* app: universal About / Quit */
        int f = focus_slot();
        app = (f >= 0) ? cw[f].title : "tOS";
        int i = 0; const char *p = "About ";
        while (*p && i < (int)sizeof(menu_about) - 1) menu_about[i++] = *p++;
        for (int j = 0; app[j] && i < (int)sizeof(menu_about) - 1; j++) menu_about[i++] = app[j];
        menu_about[i] = 0;
        menu_items[menu_nitems++] = menu_about;
        menu_items[menu_nitems++] = "Quit";
    }
    int wmax = 0;                                    /* widen to the longest label */
    for (int i = 0; i < menu_nitems; i++) { int w = ugfx_text_w(menu_items[i]); if (w > wmax) wmax = w; }
    menu_w = wmax + 2 * MENU_PAD + 16;
    if (menu_w < 168) menu_w = 168;
    menu_h = menu_nitems * MENU_ROW + 2 * MENU_PAD;
    menu_x = tile_x; if (menu_x + menu_w > W - 6) menu_x = W - 6 - menu_w;
    menu_y = bar_h + 2;
    /* trace geometry so the harness can click a row: rows start at menu_y+MENU_PAD,
     * each MENU_ROW tall. Keeps the "[twm] menu logo" / "[twm] menu app <t>" prefixes. */
    print("[twm] menu "); if (kind == 1) print("logo"); else { print("app "); print(app); }
    print(" y "); printu((unsigned)(menu_y + MENU_PAD)); print(" row "); printu((unsigned)MENU_ROW);
    print(" x "); printu((unsigned)menu_x); print("\r\n");
    dirty_menu(); add_dirty(0, 0, W, bar_h);
}
static void draw_menu(void) {
    if (!menu_kind) return;
    struct rect r = shadow_box(menu_x, menu_y, menu_w, menu_h, MENU_SHADOW_SP, MENU_SHADOW_DY);
    if (!rects_hit(cur_clip, r)) return;
    ugfx_shadow(menu_x, menu_y + MENU_SHADOW_DY, menu_w, menu_h, TH_R_MD, MENU_SHADOW_SP, TH_SHADOW, 130);
    ugfx_frost(menu_x, menu_y, menu_w, menu_h, TH_R_MD, TH_CC_FROST);
    ugfx_rrect_border(menu_x, menu_y, menu_w, menu_h, TH_R_MD, 1, TH_BORDER_DIM);
    for (int i = 0; i < menu_nitems; i++) {
        int ry = menu_y + MENU_PAD + i * MENU_ROW;
        int hot = cur_x >= menu_x && cur_x < menu_x + menu_w && cur_y >= ry && cur_y < ry + MENU_ROW;
        if (hot) ugfx_rrect_a(menu_x + 4, ry, menu_w - 8, MENU_ROW, TH_R_SM, ARGB(235, 96, 152, 252));
        uint32_t col = hot ? RGB(255, 255, 255) : TH_TEXT;
        ugfx_text(menu_x + MENU_PAD + 8, ry + (MENU_ROW - fh) / 2, menu_items[i], col, UGFX_TRANSPARENT);
    }
}
/* Run a menu item by index, then close. Reuses notify()/reboot()/shutdown(); "Quit"
 * sends WEV_CLOSE to the focused window (a real app action). */
static void menu_click(int idx) {
    int kind = menu_kind;
    print("[twm] menuitem "); printu((unsigned)kind); printc(' '); printu((unsigned)idx); print("\r\n");
    menu_close();
    if (kind == 1) {                                 /* system menu */
        if (idx == 0)      notify("tOS", "tOS 1.0 -- a from-scratch hobby OS");
        else if (idx == 1) notify("Preferences", "Settings live in Control Center for now");
        else if (idx == 2) reboot();
        else if (idx == 3) shutdown();
    } else if (kind == 2) {                          /* app menu */
        int f = focus_slot();
        if (idx == 0 && f >= 0) notify(cw[f].title, "A tOS application");
        else if (idx == 1 && f >= 0) wm_post(cw[f].id, WEV_CLOSE, 0);   /* Quit */
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
    if (dock_runsep >= 1)                            /* the pinned|running boundary, when a running-unpinned app exists */
        { print("[twm] docksep "); printu((unsigned)dock_runsep); print("\r\n"); }
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
static void notif_activate(const char *target, int frame) {
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

/* --- macOS-style Alt-Tab switcher overlay (#7) ------------------------------- */
static void switcher_layout(void) {
    int n = sw_n > 0 ? sw_n : 1;
    sw_pw = n * SW_CELL + 2 * SW_PAD;
    if (sw_pw > W - 40) sw_pw = W - 40;
    sw_ph = SW_PAD + APPICON_SZ + 8 + fh + SW_PAD;
    sw_px = (W - sw_pw) / 2;
    sw_py = (H - sw_ph) / 2;
}
static int sw_tile_cx(int i) { return sw_px + SW_PAD + i * SW_CELL + SW_CELL / 2; }
static void dirty_switcher(void) {
    switcher_layout();
    struct rect r = shadow_box(sw_px, sw_py, sw_pw, sw_ph, SW_SHADOW_SP, SW_SHADOW_DY);
    add_dirty(r.x, r.y, r.w, r.h);
}
static void switch_report_sel(void) {
    int slot = sw_order[sw_pos];
    print("[twm] altswitch sel "); printu((unsigned)sw_pos); printc(' ');
    print(slot >= 0 && slot < MAXW ? cw[slot].title : "?"); print("\r\n");
}
/* Alt+Tab: open the switcher card (first press) or step the selection; `backward`
 * (Shift held) walks the other way. Focus does NOT change until commit. */
static void window_switch(int frame, int backward) {
    if (sw_until == 0 || frame >= sw_until || !sw_overlay) {   /* (re)start a session */
        sw_n = 0;
        for (int i = nz - 1; i >= 0; i--) {                    /* [0]=top (current), MRU order */
            int s = zo[i];
            if (!cw[s].popup && cw[s].used && !cw[s].min) sw_order[sw_n++] = s;
        }
        sw_pos = 0;
        if (sw_n >= 2) {
            sw_overlay = 1; sw_alt_frames = 0;
            switcher_layout();
            sw_sel_x = sw_target_x = sw_tile_cx(0);
            print("[twm] altswitch open "); printu((unsigned)sw_n); printc(' ');
            printu((unsigned)sw_px); printc(' '); printu((unsigned)sw_py); print("\r\n");
        }
    }
    sw_until = frame + SWITCH_LINGER;
    if (sw_n < 2) { sw_overlay = 0; return; }
    sw_pos = (sw_pos + (backward ? sw_n - 1 : 1)) % sw_n;      /* step one tile */
    sw_target_x = sw_tile_cx(sw_pos);
    dirty_switcher();
    switch_report_sel();
}
static void switch_commit(void) {
    if (!sw_overlay) return;
    int slot = sw_order[sw_pos];
    sw_overlay = 0; sw_until = 0;
    dirty_switcher();
    if (slot >= 0 && slot < MAXW && cw[slot].used && !cw[slot].min) focus_window(slot);
    print("[twm] altswitch commit "); print(slot >= 0 && slot < MAXW ? cw[slot].title : "?"); print("\r\n");
}
static void switch_cancel(void) {
    if (!sw_overlay) return;
    sw_overlay = 0; sw_until = 0;
    dirty_switcher();
    print("[twm] altswitch cancel\r\n");
}
/* A click inside the card selects + commits the tile under the cursor; a click
 * outside cancels. Returns 1 if the click was consumed. */
static int switch_click(int mx, int my) {
    if (!sw_overlay) return 0;
    switcher_layout();
    if (mx >= sw_px && mx < sw_px + sw_pw && my >= sw_py && my < sw_py + sw_ph) {
        int i = (mx - sw_px - SW_PAD) / SW_CELL;
        if (i >= 0 && i < sw_n) { sw_pos = i; switch_report_sel(); }
        switch_commit();
    } else {
        switch_cancel();
    }
    return 1;
}
static void draw_switcher(void) {
    if (!sw_overlay) return;
    switcher_layout();
    struct rect r = shadow_box(sw_px, sw_py, sw_pw, sw_ph, SW_SHADOW_SP, SW_SHADOW_DY);
    if (!rects_hit(cur_clip, r)) return;
    ugfx_shadow(sw_px, sw_py + SW_SHADOW_DY, sw_pw, sw_ph, CC_RAD, SW_SHADOW_SP, TH_SHADOW, 150);
    ugfx_frost(sw_px, sw_py, sw_pw, sw_ph, CC_RAD, TH_CC_FROST);
    ugfx_rrect_border(sw_px, sw_py, sw_pw, sw_ph, CC_RAD, 1, TH_BORDER_DIM);
    int hh = APPICON_SZ + 14, hy = sw_py + SW_PAD - 7;          /* animated selection highlight */
    ugfx_rrect_a(sw_sel_x - SW_CELL / 2 + 6, hy, SW_CELL - 12, hh, TH_R_MD, ARGB(70, 122, 152, 222));
    char buf[32];
    for (int i = 0; i < sw_n; i++) {
        int slot = sw_order[i]; if (slot < 0 || slot >= MAXW) continue;
        int cx = sw_tile_cx(i), ix = cx - APPICON_SZ / 2, iy = sw_py + SW_PAD;
        int ai = app_for_title(cw[slot].title);
        if (ai >= 0 && apps[ai].img) ugfx_blit_scaled(ix, iy, APPICON_SZ, APPICON_SZ, apps[ai].img, apps[ai].iw, apps[ai].ih);
        else                         ugfx_blit_argb(ix, iy, APPICON_SZ, APPICON_SZ, appicons_argb[ICON_APP]);
        fit_text(buf, sizeof buf, cw[slot].title, SW_CELL - 10);
        int tw = ugfx_text_w(buf);
        ugfx_text(cx - tw / 2, sw_py + SW_PAD + APPICON_SZ + 6, buf, i == sw_pos ? TH_TEXT : TH_MUTED, UGFX_TRANSPARENT);
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

        /* --- input: keys to the focused window ------------------------------- */
        if (pending_until && frame >= pending_until) pending_until = 0;   /* in-flight launch gave up */
        int key, f = focus_slot();
        while ((key = twm_getkey()) >= 0) {
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
            if (menu_kind && !handled) {
                handled = 1;
                if (ms.x >= menu_x && ms.x < menu_x + menu_w && ms.y >= menu_y && ms.y < menu_y + menu_h) {
                    int idx = (ms.y - (menu_y + MENU_PAD)) / MENU_ROW;
                    if (idx >= 0 && idx < menu_nitems) menu_click(idx); else menu_close();
                } else menu_close();
            } else if (!handled && on_logo) { menu_open_kind(1, 8);          handled = 1; }
              else if (!handled && on_appm) { menu_open_kind(2, app_hit_x);  handled = 1; }
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
                int cwid = ugfx_text_w("Clear");                  /* Clear button, header right */
                int clx = nc_x + NC_W - NC_PAD - cwid, cly = nc_y + NC_PAD;
                if (notes_n && ms.x >= clx - 4 && ms.x < clx + cwid + 4 &&
                    ms.y >= cly - 2 && ms.y < cly + fh + 2) {     /* Clear: empty the ring, stay open */
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
             * leaks to a window behind), but only the chevron button toggles expand. */
            if (toast_live && !handled) {
                int tx, ty, tw, th2; toast_box(&tx, &ty, &tw, &th2);
                if (ms.x >= tx && ms.x < tx + tw && ms.y >= ty && ms.y < ty + th2) {
                    handled = 1;
                    int xbtn = tx + tw - TOAST_PAD - STATUSICON_SZ;  /* dismiss (X) left edge   */
                    int chev = xbtn - STATUSICON_SZ - 4;             /* chevron left edge        */
                    int rt = ty + TOAST_PAD - 4, rb = ty + TOAST_PAD + fh + 6;
                    if (ms.y >= rt && ms.y < rb && ms.x >= xbtn - 3) {           /* X: dismiss instantly */
                        toast_kill();
                        print("[twm] toast dismiss\r\n");
                    } else if (toast_collapsible && ms.y >= rt && ms.y < rb &&
                               ms.x >= chev - 3 && ms.x < xbtn - 3) {            /* chevron: expand/collapse */
                        toast_expanded = !toast_expanded; toast_paused = 1;
                        dirty_toast();
                        print("[twm] toast expand "); printu((unsigned)toast_expanded); print("\r\n");
                    } else if (toast.target[0]) {                               /* body: open the sender, then dismiss */
                        print("[twm] notif open "); print(toast.target); print("\r\n");
                        notif_activate(toast.target, frame);
                        toast_kill();
                    }
                }
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
        if (sw_overlay) {                               /* Alt-Tab card: commit + slide animation */
            unsigned m = kbd_mods();
            if (m & KMOD_ALT) { if (sw_alt_frames < 1000) sw_alt_frames++; }
            else if (sw_alt_frames >= 3) switch_commit();   /* Alt was genuinely held, now released */
            else if (frame >= sw_until)  switch_commit();   /* linger lapsed (no hold detected) */
            if (sw_overlay && sw_sel_x != sw_target_x) {    /* ease the highlight toward its tile */
                int d = sw_target_x - sw_sel_x;
                sw_sel_x += d > 0 ? (d + 2) / 3 : (d - 2) / 3;
                dirty_switcher();
            }
        }
        if (nc_slide > 0) { nc_slide--; dirty_nc(); }   /* animate a row sliding into the center */
        if (toast_live) {                               /* animate the toast (slide in/hold/out) */
            int tx, ty, tw, th2; toast_box(&tx, &ty, &tw, &th2);
            int hov = cur_x >= tx && cur_x < tx + tw && cur_y >= ty && cur_y < ty + th2;
            if (hov) {                                  /* hover pauses the auto-dismiss timer */
                if (toast_age < TOAST_IN) { toast_age++; dirty_toast(); }            /* finish sliding in */
                else if (toast_age != TOAST_IN) { toast_age = TOAST_IN; dirty_toast(); }  /* snap fully open */
                if (!toast_paused) { toast_paused = 1; print("[twm] toast pause\r\n"); }
            } else {
                toast_paused = 0;
                dirty_toast();
                if (++toast_age >= TOAST_LIFE) toast_kill();
            }
        } else if (toast_linger > 0) {                  /* belt-and-suspenders: scrub any shadow ghost */
            toast_linger--; dirty_toast();
        }

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
