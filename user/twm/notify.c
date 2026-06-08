/* twm -- notifications: the toast + the notification center.
 *
 * Apps post via notify(); twm drains the kernel queue (poll_notifications),
 * ring-buffers the recent ones for the center, and slides the newest in as a
 * top-right toast (slide-in / hold / slide-out). The bell status item in the bar
 * toggles the center and carries an unseen badge; the click routing for both lives
 * in twm.c, which calls into draw/hit helpers here. */
#include "twm.h"

#define NC_PAD       14
#define NC_ROW       (2 * fh + 14)   /* a center row: title + body + padding          */
#define NC_SLIDE     8               /* frames to slide a new row into an open center */
#define NC_MAXLINES  6               /* max body lines when a center row is expanded  */
#define TOAST_W      300
#define TOAST_PAD    12
#define TOAST_IN     7               /* slide-in frames                               */
#define TOAST_HOLD   300             /* visible frames (~3.7s at 12ms/frame)          */
#define TOAST_OUT    9               /* slide-out frames                              */
#define TOAST_LIFE   (TOAST_IN + TOAST_HOLD + TOAST_OUT)
#define TOAST_MAXLINES 6             /* max body lines when a toast is expanded       */

/* ------------------------------------------------------------------ toast */
/* Greedy word-wrap of src to maxw px, capped at TOAST_MAXLINES. When draw, render
 * each line at (x, y0 + i*fh) in col; always returns the line count (>=1).
 * the wrap logic itself is the pure tu_wrap (textutil.h, unit-tested); here we
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
void toast_box(int *x, int *y, int *w, int *h) {
    *w = TOAST_W;
    *h = toast_height();
    int base_x = W - TOAST_W - 12, travel = TOAST_W + 24, off = 0;
    if (toast_age < TOAST_IN)
        off = travel * (TOAST_IN - toast_age) / TOAST_IN;
    else if (toast_age >= TOAST_IN + TOAST_HOLD)
        off = travel * (toast_age - (TOAST_IN + TOAST_HOLD)) / TOAST_OUT;
    *x = base_x + off; *y = bar_h + 10;
}
void dirty_toast(void) {                                /* the toast's full travel band + shadow halo */
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
void toast_kill(void) { toast_live = 0; toast_linger = 4; dirty_toast(); }
void draw_toast(void) {
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

/* A click on the live toast (macOS-style: consumed so it never leaks to a window
 * behind). The X dismisses, the chevron toggles expand, the body opens the sender.
 * `frame` routes a body click to its app. Returns 1 if the click landed on it. */
int toast_click(int mx, int my, int frame) {
    if (!toast_live) return 0;
    int tx, ty, tw, th2; toast_box(&tx, &ty, &tw, &th2);
    if (mx < tx || mx >= tx + tw || my < ty || my >= ty + th2) return 0;
    int xbtn = tx + tw - TOAST_PAD - STATUSICON_SZ;            /* dismiss (X) left edge   */
    int chev = xbtn - STATUSICON_SZ - 4;                       /* chevron left edge        */
    int rt = ty + TOAST_PAD - 4, rb = ty + TOAST_PAD + fh + 6;
    if (my >= rt && my < rb && mx >= xbtn - 3) {               /* X: dismiss instantly */
        toast_kill();
        print("[twm] toast dismiss\r\n");
    } else if (toast_collapsible && my >= rt && my < rb &&
               mx >= chev - 3 && mx < xbtn - 3) {              /* chevron: expand/collapse */
        toast_expanded = !toast_expanded; toast_paused = 1;
        dirty_toast();
        print("[twm] toast expand "); printu((unsigned)toast_expanded); print("\r\n");
    } else if (toast.target[0]) {                             /* body: open the sender, then dismiss */
        print("[twm] notif open "); print(toast.target); print("\r\n");
        notif_activate(toast.target, frame);
        toast_kill();
    }
    return 1;
}
/* Once a frame (from the main loop): slide the toast in, hold (paused while hovered),
 * then slide out and retire it; keep scrubbing the footprint for a few frames after. */
void toast_tick(void) {
    if (toast_live) {
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
}

/* ------------------------------------------------------------------ center */
/* Is (mx,my) on the notification center's Clear button (only live with notes)? */
int nc_clear_hit(int mx, int my) {
    if (!notes_n) return 0;
    int cwid = ugfx_text_w("Clear");
    int clx = nc_x + NC_W - NC_PAD - cwid, cly = nc_y + NC_PAD;
    return mx >= clx - 4 && mx < clx + cwid + 4 && my >= cly - 2 && my < cly + fh + 2;
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
void nc_layout(void) {
    int rows_h = 0;
    for (int k = 0; k < notes_n; k++)
        rows_h += nc_row_h((notes_head - 1 - k + 2 * NOTE_KEEP) % NOTE_KEEP);
    if (!notes_n) rows_h = NC_ROW;                       /* "No notifications" reserves one row */
    nc_h = NC_PAD + fh + 10 + rows_h + NC_PAD;
}
void draw_nc(void) {
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
int nc_click_row(int mx, int my) {
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
int nc_row_at(int mx, int my) {
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
int poll_notifications(void) {
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
