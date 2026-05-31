/* clipboard -- the tOS clipboard manager (Windows Win+V style). twm launches it
 * on Super+V. It lists the system clipboard ring (text + file-byte entries,
 * newest first); single-click an entry to make it the active paste target, and
 * Clear empties the ring. The store itself lives in the kernel (SYS_CLIP_*), so
 * any app can copy into it (the shell `copy` command, Files' "Copy"). */
#include "ui.h"
#include "app.h"

struct ClipApp : ui::Window {
    ui::Panel    header;
    ui::Label    title, hint;
    ui::Button   clearbtn;
    ui::ListView list;
    int fh = 0, TBH = 0;

    static void render_row(void *ctx, int i, ui::Rect cell, bool sel) {
        ClipApp *a = (ClipApp *)ctx;
        if (!sel && (i & 1)) ugfx_fill(cell.x, cell.y, cell.w, cell.h, RGB(33, 37, 49));
        struct clipinfo ci;
        if (clip_info(i, &ci) != 0) return;
        int ty = cell.y + (cell.h - a->fh) / 2, ix = cell.x + 12;
        /* type badge */
        uint32_t col = ci.type == CLIP_FILE ? ARGB(255, 224, 162, 58) : ARGB(255, 96, 152, 232);
        ugfx_rrect_a(ix, cell.y + (cell.h - 20) / 2, 20, 20, 5, col);
        ugfx_text(ix + 6, ty, ci.type == CLIP_FILE ? "F" : "T", RGB(255, 255, 255), UGFX_TRANSPARENT);
        /* preview */
        char prev[80];
        if (ci.type == CLIP_FILE) {
            snprintf(prev, sizeof prev, "%s  (%u bytes)", ci.name[0] ? ci.name : "file", ci.len);
        } else {
            char buf[72]; int n = clip_get(i, buf, sizeof buf - 1); if (n < 0) n = 0;
            for (int k = 0; k < n; k++) if (buf[k] == '\n' || buf[k] == '\r' || buf[k] == '\t') buf[k] = ' ';
            buf[n] = 0;
            strncpy(prev, buf, sizeof prev - 1); prev[sizeof prev - 1] = 0;
        }
        ugfx_text(ix + 30, ty, prev, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
        if (ci.active) {
            const char *m = "active";
            ugfx_text(cell.x + cell.w - ugfx_text_w(m) - 12, ty, m, TH_ACCENT, UGFX_TRANSPARENT);
        }
    }
    void refresh() { list.count = clip_count(); if (list.count == 0) list.sel = -1; invalidate(); }
    void layout() {
        fh = ugfx_font_h(); TBH = fh + 16;
        header.r = { 0, 0, w, TBH };
        title.r = { 12, (TBH - fh) / 2, 160, fh };
        int bw = ugfx_text_w("Clear") + 20;
        clearbtn.r = { w - bw - 8, (TBH - (fh + 8)) / 2, bw, fh + 8 };
        list.r = { 0, TBH, w, h - TBH };
        list.row_h = fh + 14;
    }
    void on_resize(int, int) override { layout(); }
    bool build() {
        struct sysinfo si; sysinfo(&si);
        int cw = 440, ch = 320;
        if (cw > (int)si.fb_w - 40) cw = (int)si.fb_w - 40;
        if (ch > (int)si.fb_h - 80) ch = (int)si.fb_h - 80;
        popup = true;                       /* borderless centred overlay: Esc / click-away closes it */
        if (!create(cw, ch, "Clipboard")) return false;
        header.color = RGB(34, 39, 52); header.sep_bottom = true;
        title.text = "Clipboard"; title.fg = TH_TEXT;
        list.bg = RGB(24, 27, 37); list.ctx = this; list.render_row = render_row;
        list.on_select   = [](void *c, int i) { clip_active(i); ((ClipApp *)c)->invalidate(); };
        list.on_activate = [](void *c, int i) { clip_active(i); ((ClipApp *)c)->invalidate(); };
        clearbtn.text = "Clear"; clearbtn.ctx = this;
        clearbtn.on_click = [](void *c) { clip_clear(); ((ClipApp *)c)->refresh(); };
        layout();
        add(&header); add(&title); add(&clearbtn); add(&list);
        focus = &list;
        refresh();
        return true;
    }
};

int app_main() {
    ClipApp *app = new ClipApp();
    if (!app->build()) { print("[clipboard] needs the desktop\r\n"); proc_exit(); }
    print("[clipboard] up\r\n");
    return app->run();
}
