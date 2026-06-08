/* twm -- the macOS-style Alt-Tab window switcher (#7).
 *
 * Alt+Tab opens a centred card of window tiles (icon + title) with an animated
 * selection highlight. It commits on Alt-release (after a genuine hold), a click on
 * a tile, Enter, or the linger timeout; ESC cancels. Holding Alt keeps it up on real
 * hardware; under the test harness (which can't hold a modifier) the linger timeout
 * commits, so the card is still visible.
 *
 * Because every focus change in this compositor also raises the window, the z-order
 * list (zo[], top == most recent) IS the MRU stack -- so a switch SESSION snapshots
 * that order once and steps a cursor through the snapshot, raising each window as it
 * goes. Only sw_overlay (is the card up) is shared with the rest of twm; the
 * session/animation state is private here, driven once a frame by switcher_tick(). */
#include "twm.h"

#define SWITCH_LINGER 80             /* frames a switch session stays "warm" (~1s) */
#define SW_CELL      108             /* per-tile cell width (icon + title)          */
#define SW_PAD       18
#define SW_SHADOW_SP 28
#define SW_SHADOW_DY 8

static int sw_order[MAXW], sw_n, sw_pos, sw_until;
static int sw_alt_frames;            /* consecutive frames Alt seen held this session */
static int sw_sel_x, sw_target_x;    /* animated highlight centre (px)              */
static int sw_px, sw_py, sw_pw, sw_ph;  /* card rect (set by switcher_layout)        */

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
void window_switch(int frame, int backward) {
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
void switch_commit(void) {
    if (!sw_overlay) return;
    int slot = sw_order[sw_pos];
    sw_overlay = 0; sw_until = 0;
    dirty_switcher();
    if (slot >= 0 && slot < MAXW && cw[slot].used && !cw[slot].min) focus_window(slot);
    print("[twm] altswitch commit "); print(slot >= 0 && slot < MAXW ? cw[slot].title : "?"); print("\r\n");
}
void switch_cancel(void) {
    if (!sw_overlay) return;
    sw_overlay = 0; sw_until = 0;
    dirty_switcher();
    print("[twm] altswitch cancel\r\n");
}
/* A click inside the card selects + commits the tile under the cursor; a click
 * outside cancels. Returns 1 if the click was consumed. */
int switch_click(int mx, int my) {
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
void draw_switcher(void) {
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
/* Once a frame (from the main loop): commit the card when Alt is released after a
 * genuine hold (or the linger lapses), and ease the selection highlight toward its
 * target tile. A no-op when the card isn't up. */
void switcher_tick(int frame) {
    if (!sw_overlay) return;
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
