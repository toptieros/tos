/* twm -- the menu-bar dropdowns (#6/#8).
 *
 * The logo and the focused app's name are clickable menu tiles that open a
 * dropdown: the logo's system menu, the app tile's universal About/Quit, or an
 * app-declared File/Edit/... menu fetched from the focused window (refresh_app_menu
 * keeps cur_menu in sync once a frame). The bar (bar.c) draws the tiles; the click
 * routing that opens these lives in twm.c. */
#include "twm.h"

#define MENU_MAXI 8                  /* max items in a dropdown (>= WINMENU_ITEMS) */
#define MENU_ROW  (fh + 12)          /* dropdown row height                */
#define MENU_PAD  8
#define MENU_CHECK_W 18              /* left gutter for a check mark (#6)   */

static const char *menu_items[MENU_MAXI];
static unsigned char menu_iflags[MENU_MAXI];   /* WMI_* per visible dropdown row (#6)   */
static char menu_iaccel[MENU_MAXI];            /* Ctrl-accel letter per row, 0 = none   */
static char menu_about[40];                    /* dynamic "About <app>" label           */
static unsigned cur_menu_sig;                  /* signature so the bar repaints on change */

void menu_close(void) { if (menu_kind) { dirty_menu(); add_dirty(0, 0, W, bar_h); menu_kind = 0; } }
/* The dropdown row index under a y inside the open menu (the caller bounds-checks). */
int menu_row_at(int my) { return (my - (menu_y + MENU_PAD)) / MENU_ROW; }
/* Open the logo (system) menu or the focused app's menu, anchored at tile_x. */
void menu_open_kind(int kind, int tile_x) {
    if (cc_open) { cc_open = 0; dirty_cc(); }            /* the bar's panels are mutually exclusive */
    if (nc_open) { nc_open = 0; dirty_nc(); }
    menu_kind = kind; menu_nitems = 0;
    for (int i = 0; i < MENU_MAXI; i++) { menu_iflags[i] = 0; menu_iaccel[i] = 0; }
    const char *app = "tOS";
    if (kind == 1) {                                 /* logo: the system menu */
        menu_items[menu_nitems++] = "About This tOS";
        menu_items[menu_nitems++] = "Preferences...";
        menu_items[menu_nitems++] = "Restart";
        menu_items[menu_nitems++] = "Shut Down";
    } else if (kind == 3) {                          /* an app-declared menu (File/Edit/...) */
        if (menu_app_idx < 0 || menu_app_idx >= (int)cur_menu.nmenus) { menu_kind = 0; return; }
        app = cur_menu.m[menu_app_idx].title;
        for (unsigned k = 0; k < cur_menu.m[menu_app_idx].nitems && menu_nitems < MENU_MAXI; k++) {
            menu_iflags[menu_nitems] = cur_menu.m[menu_app_idx].flags[k];
            menu_iaccel[menu_nitems] = cur_menu.m[menu_app_idx].accel[k];
            menu_items[menu_nitems++] = cur_menu.m[menu_app_idx].items[k];
        }
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
    int wmax = 0, amax = 0;                          /* widen to the longest label + accel column */
    for (int i = 0; i < menu_nitems; i++) {
        int w = ugfx_text_w(menu_items[i]); if (w > wmax) wmax = w;
        if (menu_iaccel[i]) { char ab[3] = { '^', menu_iaccel[i], 0 }; int aw = ugfx_text_w(ab); if (aw > amax) amax = aw; }
    }
    menu_w = MENU_PAD + MENU_CHECK_W + wmax + (amax ? amax + 20 : 0) + MENU_PAD;
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
/* A small ✓ drawn from two strokes (no line primitive in ugfx); ~10px box at (x,y). */
static void draw_check(int x, int y, uint32_t col) {
    for (int i = 0; i <= 3; i++) ugfx_fill(x + i,     y + 3 + i, 2, 2, col);   /* short down-right */
    for (int i = 0; i <= 5; i++) ugfx_fill(x + 3 + i, y + 6 - i, 2, 2, col);   /* long up-right    */
}
void draw_menu(void) {
    if (!menu_kind) return;
    struct rect r = shadow_box(menu_x, menu_y, menu_w, menu_h, MENU_SHADOW_SP, MENU_SHADOW_DY);
    if (!rects_hit(cur_clip, r)) return;
    ugfx_shadow(menu_x, menu_y + MENU_SHADOW_DY, menu_w, menu_h, TH_R_MD, MENU_SHADOW_SP, TH_SHADOW, 130);
    ugfx_frost(menu_x, menu_y, menu_w, menu_h, TH_R_MD, TH_CC_FROST);
    ugfx_rrect_border(menu_x, menu_y, menu_w, menu_h, TH_R_MD, 1, TH_BORDER_DIM);
    for (int i = 0; i < menu_nitems; i++) {
        int ry = menu_y + MENU_PAD + i * MENU_ROW;
        int dis = (menu_iflags[i] & WMI_DISABLED) != 0;
        int hot = !dis && cur_x >= menu_x && cur_x < menu_x + menu_w && cur_y >= ry && cur_y < ry + MENU_ROW;
        if (hot) ugfx_rrect_a(menu_x + 4, ry, menu_w - 8, MENU_ROW, TH_R_SM, ARGB(235, 96, 152, 252));
        uint32_t col = dis ? TH_MUTED : (hot ? RGB(255, 255, 255) : TH_TEXT);
        int ty = ry + (MENU_ROW - fh) / 2;
        if (menu_iflags[i] & WMI_CHECKED) draw_check(menu_x + MENU_PAD + 2, ty + 1, col);
        ugfx_text(menu_x + MENU_PAD + MENU_CHECK_W, ty, menu_items[i], col, UGFX_TRANSPARENT);
        if (menu_iaccel[i]) {                        /* right-aligned Ctrl-accel hint, e.g. ^S */
            char ab[3] = { '^', menu_iaccel[i], 0 };
            uint32_t ac = dis ? TH_MUTED : (hot ? ARGB(210, 255, 255, 255) : TH_MUTED);
            ugfx_text(menu_x + menu_w - MENU_PAD - 4 - ugfx_text_w(ab), ty, ab, ac, UGFX_TRANSPARENT);
        }
    }
}
/* Run a menu item by index, then close. Reuses notify()/reboot()/shutdown(); "Quit"
 * sends WEV_CLOSE to the focused window (a real app action). */
void menu_click(int idx) {
    int kind = menu_kind;
    if (idx >= 0 && idx < menu_nitems && (menu_iflags[idx] & WMI_DISABLED)) { menu_close(); return; }  /* disabled: ignore */
    print("[twm] menuitem "); printu((unsigned)kind); printc(' '); printu((unsigned)idx); print("\r\n");
    menu_close();
    if (kind == 1) {                                 /* system menu */
        if (idx == 0)      notify("tOS", "tOS 1.0 -- a from-scratch hobby OS");
        else if (idx == 1) notify("Preferences", "Settings live in Control Center for now");
        else if (idx == 2) reboot();
        else if (idx == 3) shutdown();
    } else if (kind == 2) {                          /* app menu: universal About / Quit */
        int f = focus_slot();
        if (idx == 0 && f >= 0) notify(cw[f].title, "A tOS application");
        else if (idx == 1 && f >= 0) wm_post(cw[f].id, WEV_CLOSE, 0);   /* Quit */
    } else if (kind == 3) {                          /* app-declared menu -> WEV_MENU back to the app */
        int f = focus_slot();
        if (f >= 0) wm_post(cw[f].id, WEV_MENU, WEV_MENU_PACK(menu_app_idx, idx));
    }
}

/* A cheap signature of a menu spec, so the bar repaints only when the focused
 * app's declared menu actually changes (it is fetched every frame). */
static unsigned menu_sig(const struct winmenu *m) {
    unsigned h = 2166136261u ^ m->nmenus;
    for (unsigned i = 0; i < m->nmenus && i < WINMENU_MAX; i++) {
        for (const char *p = m->m[i].title; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
        h = (h ^ m->m[i].nitems) * 16777619u;
        for (unsigned k = 0; k < m->m[i].nitems && k < WINMENU_ITEMS; k++) {
            for (const char *p = m->m[i].items[k]; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
            h = (h ^ m->m[i].flags[k]) * 16777619u;          /* fold in check/disabled + accel (#6) */
            h = (h ^ (unsigned char)m->m[i].accel[k]) * 16777619u;
        }
    }
    return h;
}
/* Fetch the focused window's declared menu bar (#6) into cur_menu; repaint the bar
 * (and drop an open app dropdown) when it changes. Called once per frame. */
void refresh_app_menu(void) {
    int fwin = focus_slot();
    struct winmenu nm; nm.nmenus = 0;
    if (fwin >= 0 && !cw[fwin].popup) wm_getmenu(cw[fwin].id, &nm);
    unsigned sig = menu_sig(&nm);
    if (sig != cur_menu_sig) {
        cur_menu = nm; cur_menu_sig = sig;
        if (menu_kind == 3) menu_close();
        add_dirty(0, 0, W, bar_h);
    }
}
