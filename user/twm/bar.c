/* twm -- the top menu bar.
 *
 * The translucent frosted bar across the top: the clickable logo + focused-app
 * menu tiles, the app-declared File/Edit/... tiles (#6), the right-side status
 * cluster (network / volume / battery / bell placeholder glyphs) and a
 * registry-driven clock. The dropdown menus those tiles open live in menubar.c;
 * the control-center status item + cluster layout live in controlcenter.c. */
#include "twm.h"

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
void draw_status_glyph(int x, int cy, uint32_t col, int idx) {
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
    int h24 = streq(reg_get("clock.format", "24h"), "24h");
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
void draw_bar(void) {
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
    /* app-declared menu tiles (File / Edit / ... ) to the right of the app name (#6) */
    int mtx = app_hit_x + app_hit_w + 4;
    for (int i = 0; i < (int)cur_menu.nmenus && i < WINMENU_MAX; i++) {
        const char *mt = cur_menu.m[i].title;
        int mw = ugfx_text_w(mt) + 16;
        appmenu_x[i] = mtx; appmenu_w[i] = mw;
        int active = (menu_kind == 3 && menu_app_idx == i);
        int hot = cur_y >= y && cur_y < y + bar_h && cur_x >= mtx && cur_x < mtx + mw;
        if (active || hot)
            ugfx_rrect_a(mtx, y + 3, mw, bar_h - 6, TH_R_SM, active ? ARGB(40, 120, 170, 255) : ARGB(28, 255, 255, 255));
        ugfx_text(mtx + 8, y + (bar_h - fh) / 2, mt, TH_TEXT, UGFX_TRANSPARENT);
        mtx += mw + 2;
    }
    { static unsigned last_amsig = 0;                   /* report tile geometry for the harness, on change */
      unsigned s = (unsigned)cur_menu.nmenus;
      for (int i = 0; i < (int)cur_menu.nmenus && i < WINMENU_MAX; i++) s = s * 131u + (unsigned)appmenu_x[i] * 7u + (unsigned)appmenu_w[i];
      if (s != last_amsig) { last_amsig = s;
          for (int i = 0; i < (int)cur_menu.nmenus && i < WINMENU_MAX; i++) {
              print("[twm] appmenu "); printu((unsigned)i); printc(' '); print(cur_menu.m[i].title);
              printc(' '); printu((unsigned)appmenu_x[i]); printc(' '); printu((unsigned)appmenu_w[i]); print("\r\n"); } } }
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
