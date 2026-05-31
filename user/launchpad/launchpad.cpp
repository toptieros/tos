/* launchpad -- a macOS-style app grid. twm opens it as a near-fullscreen popup
 * when Super is tapped on its own: every installed app (a /Apps scan) is shown as
 * a labelled tile; click one to launch it (the Launchpad then closes). A search
 * field at the top filters the grid as you type and holds focus from the start, so
 * you can type to filter without clicking it first (#11); the tile block is centred
 * both horizontally and vertically. Esc or a click outside dismisses it. */
#include "ui.h"
#include "app.h"
#include "applist.h"

#define LP_CELL 150            /* fixed tile cell (so few apps stay centred, not stretched) */

/* A centred flow grid of app tiles. The app sets filt[]/nfilt (indices into the
 * shared apps[] that match the current search); the grid lays them out centred in
 * its rect, recomputed each draw so it tracks the filtered count live. */
struct Grid : ui::Widget {
    AppEntry *apps = nullptr;
    int      *filt = nullptr;
    int       nfilt = 0, cols = 5, fh = 0;
    int       gx0 = 0, gy0 = 0;                  /* centred origin of the tile block */
    void (*on_launch)(void *, int) = nullptr;    /* arg = filtered index */
    void *ctx = nullptr;
    int   hot_tile = -1;

    void recentre() {
        cols = r.w / LP_CELL; if (cols < 1) cols = 1;
        int shown = nfilt < cols ? nfilt : cols;     /* tiles on the widest row */
        if (shown < 1) shown = 1;
        int rows = (nfilt + cols - 1) / cols; if (rows < 1) rows = 1;
        int bw = shown * LP_CELL, bh = rows * LP_CELL;
        gx0 = r.x + (r.w - bw) / 2; if (gx0 < r.x) gx0 = r.x;
        gy0 = r.y + (r.h - bh) / 2; if (gy0 < r.y) gy0 = r.y;
    }
    void tile_at(int mx, int my, int &idx) {
        idx = -1;
        if (mx < gx0 || my < gy0) return;
        int gx = (mx - gx0) / LP_CELL, gy = (my - gy0) / LP_CELL;
        if (gx < 0 || gx >= cols) return;
        int k = gy * cols + gx;
        if (k >= 0 && k < nfilt) idx = k;
    }
    void draw() override {
        recentre();
        for (int k = 0; k < nfilt; k++) {
            AppEntry &a = apps[filt[k]];
            int gx = k % cols, gy = k / cols;
            int x = gx0 + gx * LP_CELL, y = gy0 + gy * LP_CELL;
            int pad = 22, ts = LP_CELL - 2 * pad;
            uint32_t tile = (k == hot_tile) ? ARGB(255, 70, 80, 110) : ARGB(255, 50, 58, 80);
            ugfx_rrect_a(x + pad, y + 12, ts, ts, 18, tile);
            ugfx_rrect_border(x + pad, y + 12, ts, ts, 18, 1, TH_BORDER);
            if (a.icon) {                                /* the app's real icon, centred in the tile */
                int is = ts - 28; if (is > 64) is = 64;
                ugfx_blit_scaled(x + pad + (ts - is) / 2, y + 12 + (ts - is) / 2, is, is, a.icon, a.iw, a.ih);
            } else {
                char init[2] = { a.name[0], 0 };         /* fall back to a big initial */
                ugfx_text(x + LP_CELL / 2 - ugfx_text_w(init) / 2, y + 12 + ts / 2 - fh / 2, init, RGB(235, 240, 250), UGFX_TRANSPARENT);
            }
            ugfx_text(x + (LP_CELL - ugfx_text_w(a.name)) / 2, y + 12 + ts + 6, a.name, TH_TEXT, UGFX_TRANSPARENT);
        }
    }
    bool on_mouse(int mx, int my, int btn) override {
        (void)btn; int i; tile_at(mx, my, i);
        if (i >= 0 && on_launch) on_launch(ctx, i);
        return true;
    }
    bool on_hover(int mx, int my) override {
        int i; tile_at(mx, my, i);
        if (i != hot_tile) { hot_tile = i; return true; }
        return false;
    }
    void on_leave() override { hot_tile = -1; }
};

struct Launchpad : ui::Window {
    ui::TextField query;
    Grid grid;
    AppEntry apps[16];
    int napp = 0, filt[16];

    void refilter() {
        grid.nfilt = 0;
        for (int i = 0; i < napp; i++)
            if (app_match(apps[i].name, query.text())) filt[grid.nfilt++] = i;
        grid.filt = filt;
        grid.hot_tile = -1;
        printf("[launchpad] filt=%d\r\n", grid.nfilt);   /* live filter count (also drives the test) */
        invalidate();
    }
    void layout() {
        int fh = ugfx_font_h();
        grid.fh = fh;
        int qw = 360; if (qw > w - 80) qw = w - 80;
        query.r = { (w - qw) / 2, 24, qw, fh + 16 };     /* centred search field at the top */
        int top = query.r.y + query.r.h + 18;
        grid.r = { 20, top, w - 40, h - top - 24 };
    }
    void on_resize(int, int) override { layout(); }
    bool build() {
        struct sysinfo si; sysinfo(&si);
        int cw = (int)si.fb_w - 80, ch = (int)si.fb_h - 80;
        popup = true; overlay = true;                /* drawn above the dock, dim scrim behind */
        if (!create(cw, ch, "Launchpad")) return false;
        bg = TH_FROST_KEY;          /* compositor frosts the backdrop where the panel is */
        napp = app_scan(apps, 16);
        query.bg = RGB(20, 23, 32); query.ctx = this;
        query.on_change = [](void *c) { ((Launchpad *)c)->refilter(); };
        grid.apps = apps; grid.ctx = this;
        grid.on_launch = [](void *c, int k) {        /* k is a filtered index */
            Launchpad *l = (Launchpad *)c;
            sys_launch(l->apps[l->filt[k]].exec);
            l->running = false;                      /* close after launching */
        };
        layout();
        add(&grid); add(&query);
        focus = &query;                              /* type-to-filter from the start (#11) */
        refilter();
        return true;
    }
};

int app_main() {
    Launchpad *app = new Launchpad();
    if (!app->build()) { print("[launchpad] needs the desktop\r\n"); proc_exit(); }
    print("[launchpad] up\r\n");
    return app->run();
}
