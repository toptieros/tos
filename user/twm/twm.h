/* twm -- internal shared header for the tOS compositor.
 *
 * twm is one program split across several translation units (twm.c plus the
 * feature files bar.c / dock.c / controlcenter.c / notify.c / switcher.c /
 * menubar.c). They all share one pile of compositor state -- the window list, the
 * dirty-rect set, the panel geometry -- so that state is DEFINED once in twm.c and
 * declared here for everyone else. Each feature's drawing + logic lives in its own
 * file; this header is the contract between them.
 *
 * Every twm .c file includes only this header (which pulls in the runtime + asset
 * headers below), so the include set stays uniform across the program. */
#ifndef TWM_H
#define TWM_H

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

/* ---------------------------------------------------------------- limits */
#define MAXW      8              /* concurrent windows                                          */
#define MAXICON   16             /* dock tiles: launchpad + pinned + running, see rebuild_dock  */
#define MAXAPPS   8              /* catalog of installed /Apps bundles                          */
#define DBL_FRAMES 40            /* double-click window, in event-loop frames                   */

/* ---------------------------------------------------------------- geometry */
#define WIN_SHADOW_DY   6        /* window drop-shadow downward offset (>= the 5/6 used in draw) */
#define DOCK_ELEVATION  3        /* the ugfx_elevation level the dock floats at                  */
#define DOCK_GAP  16
#define DOCK_PAD  14
#define EDGE         4           /* screen-edge reveal zone for auto-hidden chrome (px)   */
#define HIDE_LINGER  16          /* frames the chrome lingers after the cursor leaves     */
#define SLIDE        6           /* chrome slide speed (px/frame)                         */

/* --- status cluster (top bar, right side): square Lucide glyphs + a clock. Laid
 * out right-to-left by cc_layout() so the cluster never jitters as the clock ticks. */
#define SB_GLYPH   ARGB(235, 222, 228, 240)   /* status-glyph ink (alpha-aware)            */
#define SB_NET_W   STATUSICON_SZ              /* all glyphs are the same square Lucide box */
#define SB_VOL_W   STATUSICON_SZ
#define SB_BAT_W   STATUSICON_SZ
#define SB_BELL_W  STATUSICON_SZ
#define SB_GAP     12

/* --- control center panel + its drop shadow (the SHADOW extents are needed by the
 * compositor core's expand_to_panels, so they live here; the rest is in controlcenter.c). */
#define CC_RAD       16
#define CC_SHADOW_SP 24
#define CC_SHADOW_DY 6

/* --- notification center / toast: NC_W + the center's shadow extents are referenced
 * by expand_to_panels + the dirty_nc macro, so they live here. */
#define NOTE_KEEP    8           /* recent notifications kept for the center               */
#define NC_W         300         /* notification-center panel width                        */
#define NC_SHADOW_SP 24
#define NC_SHADOW_DY 6

/* --- menu-bar dropdown shadow extents (used by expand_to_panels + dirty_menu). */
#define MENU_SHADOW_SP 22
#define MENU_SHADOW_DY 6

/* ---------------------------------------------------------------- types */
struct rect { int x, y, w, h; };

struct cwin { int used, id, wx, wy; uint32_t w, h, seq; uint64_t vaddr; char title[32];
              int min, maxed, sx, sy; uint32_t sw, sh;     /* min/max flags + pre-maximize geometry */
              int popup, overlay, modal;                   /* WIN_POPUP / WIN_OVERLAY (dim, above dock) / WIN_MODAL (input-locked dialog) */
              int curs; };                                 /* app-declared cursor hint (SYS_WIN_SETCURSOR, CUR_*) */

/* The installed-app catalog entry: a /Apps/<Name>.app bundle (name, absolute exec
 * path, loaded icon, and whether its manifest pins it to the dock). */
struct app { char label[24], exec[120]; uint32_t *img; int iw, ih; int pinned; };

/* a dock tile: display name, absolute exec path, icon (img==0 -> generic or, for the
 * launchpad button, a grid glyph), and on-screen centre. `special` marks the
 * leftmost Launchpad button (single-click summons the launchpad grid). */
struct icon { char label[24], exec[120]; uint32_t *img; int iw, ih; int cx, cy; uint32_t tint; int special; };

/* ---------------------------------------------------------------- shared state
 * (all DEFINED in twm.c; the feature files see them through these externs). */
extern uint32_t *bb;                              /* back buffer                                */
extern int W, H, fh, bar_h, TH;                  /* screen, font height, bar height, title height */
extern struct cwin cw[MAXW];
extern int zo[MAXW], nz;                          /* z-order: zo[nz-1] is topmost == focused      */
extern struct app apps[MAXAPPS];
extern int napps;
extern struct icon icons[MAXICON];
extern int nicons;
extern int dock_x, dock_y, dock_w, dock_h, dock_y0;
extern int bar_y;                                 /* bar's current top: 0 shown, -bar_h hidden    */
extern struct rect cur_clip;                      /* the rect compose() is currently painting     */
extern int cur_x, cur_y;
extern uint32_t g_accent;                         /* focus accent colour (theme.accent)           */

/* status cluster glyph + clock slot positions (computed by cc_layout) */
extern int sb_clk_w, sb_net_x, sb_vol_x, sb_bat_x, sb_bell_x;

/* control center panel */
extern int cc_open;
extern int cc_x, cc_y, cc_w, cc_h;
extern int cc_btn_x, cc_btn_w;
extern int cc_row1_y, cc_row2_y, cc_sep_y, cc_info_y, cc_btn_yy;

/* notifications: the kept ring, the live toast, the center panel */
extern struct notif notes[NOTE_KEEP];
extern unsigned char note_exp[NOTE_KEEP];
extern int notes_n, notes_head, notes_unseen;
extern struct notif toast;
extern int toast_live, toast_age, toast_linger, toast_expanded, toast_paused, toast_collapsible;
extern int nc_open, nc_x, nc_y, nc_h, nc_slide;

/* Alt-Tab switcher: only the "is the card up" flag is shared with the core. */
extern int sw_overlay;

/* menu bar: the open dropdown + the focused window's declared menu tiles */
extern int menu_kind;                             /* 0 none, 1 logo, 2 app About/Quit, 3 app-declared */
extern int menu_x, menu_y, menu_w, menu_h, menu_nitems;
extern struct winmenu cur_menu;                   /* focused window's declared menu bar           */
extern int menu_app_idx;                          /* which cur_menu.m[] the kind-3 dropdown shows  */
extern int appmenu_x[WINMENU_MAX], appmenu_w[WINMENU_MAX];  /* per-menu tile hit regions          */
extern int logo_hit_w;                            /* logo click region width (set in draw_bar)     */
extern int app_hit_x, app_hit_w;                  /* focused-app name click region (set in draw_bar) */

/* ---------------------------------------------------------------- core helpers
 * (defined in twm.c, used everywhere). */
struct rect shadow_box(int x, int y, int w, int h, int spread, int dy);
void add_dirty(int x, int y, int w, int h);
int  rects_hit(struct rect a, struct rect b);
int  focus_slot(void);
void focus_window(int slot);
void fit_text(char *dst, int cap, const char *src, int maxw);   /* truncate with ".." to maxw px */
void notif_activate(const char *target, int frame);            /* focus/launch a notification's sender */

/* dirty the whole panel INCLUDING its shadow halo, so opening/closing one leaves no
 * residue. Kept as macros (shared across files) over the single shadow_box() extent. */
#define dirty_cc() do { struct rect _r = shadow_box(cc_x, cc_y, cc_w, cc_h, CC_SHADOW_SP, CC_SHADOW_DY); \
                        add_dirty(_r.x, _r.y, _r.w, _r.h); } while (0)
#define dirty_nc() do { nc_layout(); struct rect _r = shadow_box(nc_x, nc_y, NC_W, nc_h, NC_SHADOW_SP, NC_SHADOW_DY); \
                        add_dirty(_r.x, _r.y, _r.w, _r.h); } while (0)
#define dirty_menu() do { struct rect _r = shadow_box(menu_x, menu_y, menu_w, menu_h, MENU_SHADOW_SP, MENU_SHADOW_DY); \
                          add_dirty(_r.x, _r.y, _r.w, _r.h); } while (0)

/* ---------------------------------------------------------------- bar.c */
void draw_bar(void);
void draw_status_glyph(int x, int cy, uint32_t col, int idx);

/* ---------------------------------------------------------------- dock.c */
int  title_is(const char *title, const char *label);   /* window title starts with label   */
int  find_app_window(const char *label);               /* a matching window slot (prefer min) */
int  dock_tile_for(const char *title);                 /* dock icon whose label matches      */
int  app_for_title(const char *title);                 /* catalog app a window title belongs to */
int  tile_hovered(struct icon *ic);
void draw_dock(void);
void rebuild_dock(void);
void layout_dock(void);
void place_dock_icons(void);
unsigned running_sig(void);
void load_apps(void);

/* ---------------------------------------------------------------- controlcenter.c */
void cc_layout(void);
void draw_cc(void);
void draw_cc_button(int y);
void cc_click(int mx, int my);

/* ---------------------------------------------------------------- notify.c */
int  poll_notifications(void);
void draw_toast(void);
void draw_nc(void);
void toast_kill(void);
void dirty_toast(void);
void toast_tick(void);                       /* per-frame slide/hold/dismiss animation        */
int  toast_click(int mx, int my, int frame); /* click on the live toast; 1 if consumed         */
void nc_layout(void);
int  nc_click_row(int mx, int my);
int  nc_row_at(int mx, int my);
int  nc_clear_hit(int mx, int my);           /* is (mx,my) on the center's Clear button?        */
void toast_box(int *x, int *y, int *w, int *h);

/* ---------------------------------------------------------------- switcher.c */
void window_switch(int frame, int backward);
void switch_commit(void);
void switch_cancel(void);
int  switch_click(int mx, int my);
void draw_switcher(void);
void switcher_tick(int frame);   /* per-frame commit-on-release + highlight ease */

/* ---------------------------------------------------------------- menubar.c */
void menu_close(void);
void menu_open_kind(int kind, int tile_x);
void draw_menu(void);
void menu_click(int idx);
int  menu_row_at(int my);        /* dropdown row index under a y inside the menu */
void refresh_app_menu(void);

#endif /* TWM_H */
