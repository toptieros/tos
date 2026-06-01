/* spotlight -- a Spotlight-style quick launcher. twm opens it on Super+Space as a
 * borderless popup: type to filter the installed apps (a /Apps scan), Up/Down to
 * pick, Enter or double-click to launch (and the popup closes). Esc or a click
 * outside dismisses it (the compositor handles that for any popup window). */
#include "ui.h"
#include "app.h"
#include "applist.h"

struct Spotlight : ui::Window {
    ui::TextField query;
    ui::ListView  list;
    AppEntry apps[16];
    int      napp = 0, filt[16], nfilt = 0, fh = 0;

    static void render_row(void *ctx, int i, ui::Rect cell, bool sel) {
        Spotlight *s = (Spotlight *)ctx;
        if (i < 0 || i >= s->nfilt) return;
        AppEntry &a = s->apps[s->filt[i]];
        int is = cell.h - 8; if (is > 24) is = 24;
        int tx = cell.x + 14;
        if (a.icon) {                                  /* the app's real icon at the row's left */
            ugfx_blit_scaled(cell.x + 10, cell.y + (cell.h - is) / 2, is, is, a.icon, a.iw, a.ih);
            tx = cell.x + 10 + is + 8;
        }
        int ty = cell.y + (cell.h - s->fh) / 2;
        ugfx_text(tx, ty, a.name, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
    }
    void refilter() {
        nfilt = 0;
        for (int i = 0; i < napp; i++) if (app_match(apps[i].name, query.text())) filt[nfilt++] = i;
        list.count = nfilt; list.sel = nfilt ? 0 : -1; list.top = 0;
        invalidate();
    }
    void launch_sel() {
        if (list.sel < 0 || list.sel >= nfilt) return;
        sys_launch(apps[filt[list.sel]].exec);
        running = false;                       /* close the launcher after launching */
    }
    void layout() {
        fh = ugfx_font_h();
        int qh = fh + 26;                                /* taller, Google-search-box proportions */
        query.r = { 14, 14, w - 28, qh };
        list.r  = { 8, 14 + qh + 10, w - 16, h - (14 + qh + 10) - 10 };
        list.row_h = fh + 14;
    }
    void on_resize(int, int) override { layout(); }
    void on_key(int key) override {            /* query holds focus; arrows + Tab drive the result list */
        /* #13: navigation keys walk the filtered results rather than doing nothing
         * (or, in older builds, dismissing the popup). They're consumed here so they
         * never fall through to a focus change that would close the launcher. */
        if (nfilt <= 0) return;
        int prev = list.sel;
        if (key == ui::UK_UP || key == ui::UK_DOWN) list.on_key(key);
        else if (key == '\t') { int s = list.sel + 1; if (s >= nfilt) s = 0; list.sel = s; }  /* Tab cycles, wrapping */
        else return;
        list.ensure_visible(list.sel);
        invalidate();
        if (list.sel != prev) printf("[spotlight] sel=%d\r\n", list.sel);   /* nav telemetry (also drives the test) */
    }
    bool build() {
        struct sysinfo si; sysinfo(&si);
        int cw = 460, ch = 360;
        if (cw > (int)si.fb_w - 60)  cw = (int)si.fb_w - 60;
        if (ch > (int)si.fb_h - 120) ch = (int)si.fb_h - 120;
        popup = true;
        if (!create(cw, ch, "Spotlight")) return false;
        bg = RGB(28, 32, 44);
        napp = app_scan(apps, 16);
        query.bg = RGB(20, 23, 32); query.ctx = this; query.radius = TH_R_PILL;
        query.on_change = [](void *c) { ((Spotlight *)c)->refilter(); };
        query.on_submit = [](void *c) { ((Spotlight *)c)->launch_sel(); };
        list.bg = RGB(24, 27, 37); list.ctx = this; list.render_row = render_row;
        list.on_activate = [](void *c, int) { ((Spotlight *)c)->launch_sel(); };
        layout();
        add(&query); add(&list);
        focus = &query;
        refilter();
        return true;
    }
};

int app_main() {
    Spotlight *app = new Spotlight();
    if (!app->build()) { print("[spotlight] needs the desktop\r\n"); proc_exit(); }
    print("[spotlight] up\r\n");
    return app->run();
}
