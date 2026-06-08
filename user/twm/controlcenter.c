/* twm -- Control Center + the menu-bar status cluster layout.
 *
 * A quick-settings slide-over (design/ui.md) opened from a menu-bar status item:
 * live toggles for the dock/bar auto-hide (the registry keys update_chrome reads) +
 * a system summary + power actions. cc_layout() also lays out the right-side status
 * cluster (network/volume/battery/bell glyph slots + the CC button) and the
 * notification-center origin, right-to-left into fixed slots so nothing jitters as
 * the clock ticks. The bar (bar.c) draws the glyphs into the slots computed here. */
#include "twm.h"

#define CC_PAD 14

void cc_layout(void) {
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
void draw_cc(void) {
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
void draw_cc_button(int y) {                       /* the menu-bar status item (Lucide sliders) */
    int cy = y + bar_h / 2;
    draw_status_glyph(cc_btn_x + (cc_btn_w - STATUSICON_SZ) / 2, cy,
                      cc_open ? g_accent : SB_GLYPH, STATUSICON_CC);
}
void cc_click(int mx, int my) {                    /* a click inside the open panel */
    if (my >= cc_row1_y && my < cc_row1_y + 24) { cc_toggle("ui.dock.autohide"); return; }
    if (my >= cc_row2_y && my < cc_row2_y + 24) { cc_toggle("ui.bar.autohide");  return; }
    if (my >= cc_btn_yy && my < cc_btn_yy + 32) {
        int bw = (cc_w - 2 * CC_PAD - 10) / 2, bx = cc_x + CC_PAD;
        if (mx < bx + bw) reboot();
        else shutdown();
    }
}
