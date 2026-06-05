/* files -- the tOS file manager (macOS Finder-like) on the C++ widget toolkit.
 *
 * Layout: a left SIDEBAR of pinned locations, an icon TOOLBAR (back / forward /
 * up | new-folder / delete), the directory LISTVIEW, and a right DETAILS panel
 * (single-click an item to inspect it). Files no longer opens documents itself --
 * double-clicking a file hands it to another app ("Open With"); .app bundles show
 * without the extension, with their own icon, and launch directly. New Folder /
 * Delete / Open With are also on a right-click CONTEXT MENU. Back/forward (the
 * toolbar arrows or the mouse side buttons) walk a navigation history. */
#include "ui.h"
#include "app.h"
#include "icons.h"
#include "manifest.h"
#include "registry.h"
#include "textutil.h"      /* tu_ci_contains: the filter-bar substring matcher */

#define NMAX    256
#define HISTN   32
#define MAXAPPS 16
#define HOMEDIR "/Users/user"

static const uint32_t C_ZEBRA   = RGB(33, 37, 49);
static const uint32_t C_SIDEBAR = RGB(27, 31, 43);
static const uint32_t C_DETAILS = RGB(30, 34, 46);
static const uint32_t C_LIST    = RGB(24, 27, 37);
static const uint32_t C_TOOLBAR = RGB(34, 39, 52);
static const uint32_t C_SELROW  = ARGB(255, 70, 116, 200);

/* ---------------------------------------------------------------- small helpers */
static int eqn(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int endsw(const char *s, const char *suf) {
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}
static int hasdot(const char *s) { for (; *s; s++) if (*s == '.') return 1; return 0; }
static int is_app_dir(int type, const char *n) { return type == FT_DIR && endsw(n, ".app"); }

static int file_icon_for(int type, const char *n) {
    if (type == FT_DIR) return FILEICON_FOLDER;
    if (endsw(n, ".txt") || eqn(n, "readme") || eqn(n, "motd") ||
        eqn(n, "guide") || eqn(n, "notes") || eqn(n, "shortcuts")) return FILEICON_TEXT;
    if (endsw(n, ".img") || endsw(n, ".png") || endsw(n, ".bmp")) return FILEICON_IMAGE;
    if (!hasdot(n)) return FILEICON_EXEC;
    return FILEICON_FILE;
}
static const char *kind_for(int type, const char *n) {
    if (is_app_dir(type, n)) return "Application";
    if (type == FT_DIR) return "Folder";
    if (endsw(n, ".txt") || eqn(n, "readme") || eqn(n, "motd") ||
        eqn(n, "guide") || eqn(n, "notes")) return "Text Document";
    if (endsw(n, ".img") || endsw(n, ".png") || endsw(n, ".bmp")) return "Image";
    if (!hasdot(n)) return "Executable";
    return "Document";
}
static void disp_name(const char *name, char *out, int cap) {       /* strip a trailing ".app" */
    int n = (int)strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".app") == 0) n -= 4;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = name[i];
    out[n] = 0;
}
static void ext_of(const char *name, char *out, int cap) {          /* lowercased extension, or "" */
    int dot = -1; for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    int j = 0;
    if (dot >= 0) for (int i = dot + 1; name[i] && j < cap - 1; i++) {
        char c = name[i]; if (c >= 'A' && c <= 'Z') c += 32; out[j++] = c;
    }
    out[j] = 0;
}
static void join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (int i = 0; dir[i] && n < cap - 1; i++) out[n++] = dir[i];
    if (n == 0 || out[n - 1] != '/') { if (n < cap - 1) out[n++] = '/'; }
    for (int i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = 0;
}
/* smooth (area/bilinear) alpha-blended scale of a sw*sh ARGB image into a dst box;
 * forwards to the shared ugfx resampler so file-list icons get the same crisp scaling */
static void blit_scaled(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh) {
    ugfx_blit_scaled(dx, dy, dw, dh, src, sw, sh);
}
static uint32_t *load_icon_argb(const char *path, int *w, int *h) {
    int fd = fopen(path, O_RDONLY);
    if (fd < 0) return 0;
    uint32_t hdr[2] = { 0, 0 };
    if (fread_(fd, (char *)hdr, 8) != 8 || !hdr[0] || !hdr[1] || hdr[0] > 256 || hdr[1] > 256) { fclose_(fd); return 0; }
    int need = (int)(hdr[0] * hdr[1] * 4), got = 0;
    uint32_t *px = (uint32_t *)malloc((unsigned)need);
    if (!px) { fclose_(fd); return 0; }
    while (got < need) { int r = fread_(fd, (char *)px + got, need - got); if (r <= 0) break; got += r; }
    fclose_(fd);
    if (got != need) { free(px); return 0; }
    *w = (int)hdr[0]; *h = (int)hdr[1];
    return px;
}

/* thin vector strokes for the toolbar glyphs (ugfx has no line primitive) */
static void plot(int x, int y, int t, uint32_t c) { ugfx_fill(x - t / 2, y - t / 2, t, t, c); }
static void vline_(int x0, int y0, int x1, int y1, int t, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0, n = dx < 0 ? -dx : dx, m = dy < 0 ? -dy : dy, s = n > m ? n : m;
    if (s < 1) s = 1;
    for (int i = 0; i <= s; i++) plot(x0 + dx * i / s, y0 + dy * i / s, t, c);
}
enum { G_BACK, G_FWD, G_UP, G_NEWF, G_TRASH };
static void draw_glyph(int g, int cx, int cy, int r, uint32_t c) {
    int t = 2;
    if (g == G_BACK) { vline_(cx + r / 2, cy - r, cx - r / 2, cy, t, c); vline_(cx - r / 2, cy, cx + r / 2, cy + r, t, c); }
    else if (g == G_FWD) { vline_(cx - r / 2, cy - r, cx + r / 2, cy, t, c); vline_(cx + r / 2, cy, cx - r / 2, cy + r, t, c); }
    else if (g == G_UP) { vline_(cx, cy + r, cx, cy - r, t, c); vline_(cx, cy - r, cx - r, cy, t, c); vline_(cx, cy - r, cx + r, cy, t, c); }
    else if (g == G_NEWF) {                                   /* folder + plus */
        ugfx_fill(cx - r, cy - 1, 2 * r, r, c);
        ugfx_fill(cx - r, cy - 4, r, 4, c);
        vline_(cx + r - 1, cy + r - 1, cx + r - 1, cy + r + 5, t, c);   /* + vertical */
        vline_(cx + r - 4, cy + r + 2, cx + r + 2, cy + r + 2, t, c);   /* + horizontal */
    }
    else if (g == G_TRASH) {                                  /* can */
        ugfx_fill(cx - r + 1, cy - r + 3, 2 * r - 2, 2, c);   /* lid  */
        ugfx_fill(cx - 3, cy - r, 6, 2, c);                   /* handle */
        ugfx_frame(cx - r + 2, cy - r + 5, 2 * r - 4, 2 * r - 4, c);    /* body */
        vline_(cx - 1, cy - r + 7, cx - 1, cy + r - 1, 1, c);
        vline_(cx + 2, cy - r + 7, cx + 2, cy + r - 1, 1, c);
    }
}

/* --------------------------------------------------------------- IconButton */
class IconButton : public ui::Widget {
public:
    int   glyph = 0; bool enabled = true;
    void (*on_click)(void *) = nullptr; void *ctx = nullptr;
    IconButton() { focusable = false; }
    void draw() override {
        if (!visible) return;
        int rad = TH_R_SM;
        ugfx_rrect_a(r.x, r.y, r.w, r.h, rad, enabled ? ARGB(22, 255, 255, 255) : ARGB(10, 255, 255, 255));
        if (enabled && hovered) ugfx_state_layer(r.x, r.y, r.w, r.h, rad, TH_HOVER_A);  /* hover lift */
        draw_glyph(glyph, r.x + r.w / 2, r.y + r.h / 2, (r.h - 14) / 2, enabled ? TH_TEXT : TH_MUTED);
    }
    bool on_mouse(int, int, int) override { if (enabled && on_click) on_click(ctx); return true; }
};

/* ------------------------------------------------------------------- Sidebar */
struct SideItem { const char *label; char path[64]; int sep_before; };
class Sidebar : public ui::Widget {
public:
    SideItem items[10];
    int   n = 0, row_h = 26, head_h = 0, hover_row = -1;
    const char *cur = nullptr;
    void *ctx = nullptr; void (*on_pick)(void *, const char *) = nullptr;
    Sidebar() { focusable = false; }
    bool on_hover(int, int y) override {
        int i = (y - (r.y + head_h)) / row_h;
        if (y < r.y + head_h || i < 0 || i >= n) i = -1;
        if (i == hover_row) return false;
        hover_row = i; return true;
    }
    void on_leave() override { hovered = false; hover_row = -1; }
    void add_item(const char *label, const char *path, int sep) {
        SideItem *it = &items[n]; it->label = label; it->sep_before = sep;
        int i = 0; for (; path[i] && i < 63; i++) it->path[i] = path[i]; it->path[i] = 0; n++;
    }
    int row_y(int i) const { return r.y + head_h + i * row_h; }
    void draw() override {
        if (!visible) return;
        int fh = ugfx_font_h();
        ugfx_fill(r.x, r.y, r.w, r.h, C_SIDEBAR);
        ugfx_fill_a(r.x + r.w - 1, r.y, 1, r.h, ARGB(70, 0, 0, 0));
        ugfx_text(r.x + 14, r.y + 9, "Favorites", TH_MUTED, UGFX_TRANSPARENT);
        for (int i = 0; i < n; i++) {
            int y = row_y(i), sel = cur && eqn(cur, items[i].path);
            if (items[i].sep_before) ugfx_fill_a(r.x + 12, y - 3, r.w - 24, 1, ARGB(40, 150, 170, 230));
            if (sel)                 ugfx_rrect_a(r.x + 6, y + 1, r.w - 12, row_h - 2, TH_R_SM, ARGB(150, 96, 152, 252));
            else if (i == hover_row) ugfx_state_layer(r.x + 6, y + 1, r.w - 12, row_h - 2, TH_R_SM, TH_HOVER_A);
            int iy = y + (row_h - 18) / 2;
            blit_scaled(r.x + 12, iy, 18, 18, fileicons_argb[FILEICON_FOLDER], FILEICON_SZ, FILEICON_SZ);
            ugfx_text(r.x + 38, y + (row_h - fh) / 2, items[i].label, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
        }
    }
    bool on_mouse(int x, int y, int) override {
        (void)x; int i = (y - (r.y + head_h)) / row_h;
        if (y >= r.y + head_h && i >= 0 && i < n && on_pick) on_pick(ctx, items[i].path);
        return true;
    }
};

/* -------------------------------------------------------------- DetailsPanel */
class DetailsPanel : public ui::Widget {
public:
    bool has = false, is_file = false;
    char name[64] = {0}, where[256] = {0};
    const char *kind = "";
    unsigned size = 0;
    const uint32_t *icon = nullptr; int iw = 0, ih = 0, file_icon = FILEICON_FILE;
    void field(int x, int y, const char *k, const char *v) {
        int fh = ugfx_font_h();
        ugfx_text(x, y, k, TH_MUTED, UGFX_TRANSPARENT);
        ugfx_text(x, y + fh, v, TH_TEXT, UGFX_TRANSPARENT);
    }
    void draw() override {
        if (!visible) return;
        int fh = ugfx_font_h(), fw = ugfx_font_w(), cx = r.x + r.w / 2;
        ugfx_fill(r.x, r.y, r.w, r.h, C_DETAILS);
        ugfx_fill_a(r.x, r.y, 1, r.h, ARGB(70, 0, 0, 0));
        ugfx_set_clip(r.x, r.y, r.w, r.h);
        if (!has) {
            const char *m = "No selection";
            ugfx_text(cx - ugfx_text_w(m) / 2, r.y + r.h / 2 - fh / 2, m, TH_MUTED, UGFX_TRANSPARENT);
            ugfx_clip_none(); return;
        }
        int IS = 64, iy = r.y + 26;                          /* crisp hi-res preview */
        if (icon) blit_scaled(cx - IS / 2, iy, IS, IS, icon, iw, ih);
        else      blit_scaled(cx - IS / 2, iy, IS, IS, fileicons_argb[file_icon], FILEICON_SZ, FILEICON_SZ);
        int ty = iy + IS + 14;
        ugfx_text(cx - ugfx_text_w(name) / 2, ty, name, TH_TEXT, UGFX_TRANSPARENT); ty += fh + 8;
        ugfx_fill_a(r.x + 16, ty, r.w - 32, 1, ARGB(40, 150, 170, 230)); ty += 12;
        field(r.x + 16, ty, "Kind", kind); ty += fh * 2 + 9;
        if (is_file) { char sz[32]; snprintf(sz, sizeof sz, "%u bytes", size); field(r.x + 16, ty, "Size", sz); ty += fh * 2 + 9; }
        ugfx_text(r.x + 16, ty, "Where", TH_MUTED, UGFX_TRANSPARENT); ty += fh;
        int cols = (r.w - 32) / fw; if (cols < 1) cols = 1; if (cols > 62) cols = 62;
        int wl = (int)strlen(where);
        for (int off = 0; off < wl && ty <= r.y + r.h - fh; off += cols) {
            char line[64]; int c = 0;
            for (; c < cols && off + c < wl; c++) line[c] = where[off + c];
            line[c] = 0;
            ugfx_text(r.x + 16, ty, line, TH_TEXT, UGFX_TRANSPARENT); ty += fh;
        }
        ugfx_clip_none();
    }
};

/* ---------------------------------------------------------------- StatusBar */
/* The bottom bar (Finder/Dolphin both have one): item count on the left, a
 * selection summary on the right. Free-space + zoom slider are future (need a
 * statfs syscall) -- see design/files-app.md §6. */
class StatusBar : public ui::Widget {
public:
    char left[96] = {0};            /* "12 items" / "3 of 12 shown"          */
    char right[96] = {0};           /* "name selected -- 42 bytes" / empty   */
    StatusBar() { focusable = false; }
    void draw() override {
        if (!visible) return;
        int fh = ugfx_font_h();
        ugfx_fill(r.x, r.y, r.w, r.h, C_TOOLBAR);
        ugfx_fill_a(r.x, r.y, r.w, 1, ARGB(70, 0, 0, 0));            /* top hairline */
        ugfx_set_clip(r.x, r.y, r.w, r.h);
        int ty = r.y + (r.h - fh) / 2;
        ugfx_text(r.x + 14, ty, left, TH_MUTED, UGFX_TRANSPARENT);
        if (right[0]) {
            int rw = ugfx_text_w(right);
            int lx = r.x + 14 + ugfx_text_w(left) + 16;             /* don't overlap the left text */
            int rx = r.x + r.w - rw - 14; if (rx < lx) rx = lx;
            ugfx_text(rx, ty, right, TH_TEXT, UGFX_TRANSPARENT);
        }
        ugfx_clip_none();
    }
};

/* ---------------------------------------------------------------- Popup menu */
/* Doubles as the right-click context menu and the "Open With" chooser. When open
 * its rect is the whole window so it captures the next click anywhere (modal):
 * inside an item -> pick (callback with the item's tag); a toggle item flips and
 * stays open; outside -> dismiss. */
struct PopItem { char label[40]; int kind; int tag; const uint32_t *icon; int iw, ih; };
enum { PK_ACTION, PK_TOGGLE, PK_SEP };
class Popup : public ui::Widget {
public:
    bool open = false, toggle = false;
    int  px = 0, py = 0, pw = 0, rowh = 0, hover_item = -1;
    PopItem it[MAXAPPS + 6]; int n = 0;
    const char *toggle_label = "";
    void *ctx = nullptr; void (*on_pick)(void *, int tag) = nullptr;
    Popup() { visible = false; focusable = false; }
    void reset() { n = 0; toggle = false; pw = 0; hover_item = -1; }
    int item_at(int x, int y) {                          /* index of the row under (x,y), or -1 */
        int cy = py + 5;
        for (int i = 0; i < n; i++) {
            int h = (it[i].kind == PK_SEP ? 7 : rowh);
            if (it[i].kind != PK_SEP && x >= px && x < px + pw && y >= cy && y < cy + h) return i;
            cy += h;
        }
        return -1;
    }
    bool on_hover(int x, int y) override {
        if (!open) return false;
        int i = item_at(x, y);
        if (i == hover_item) return false;
        hover_item = i; return true;
    }
    void on_leave() override { hovered = false; hover_item = -1; }
    void add(const char *label, int kind, int tag, const uint32_t *icon, int iw, int ih) {
        if (n >= (int)(sizeof it / sizeof it[0])) return;
        PopItem *p = &it[n]; p->kind = kind; p->tag = tag; p->icon = icon; p->iw = iw; p->ih = ih;
        int i = 0; for (; label[i] && i < 39; i++) p->label[i] = label[i]; p->label[i] = 0;
        n++;
    }
    int item_h() { return ugfx_font_h() + 12; }
    int total_h() { int H = 6; for (int i = 0; i < n; i++) H += (it[i].kind == PK_SEP ? 7 : item_h()); return H + 4; }
    void measure() {
        rowh = item_h();
        int wmax = 90;
        for (int i = 0; i < n; i++) { int w = ugfx_text_w(it[i].label) + (it[i].icon ? 28 : 16) + 24; if (w > wmax) wmax = w; }
        pw = wmax;
    }
    void show(int x, int y) {
        measure();
        int H = total_h();
        if (win) {
            if (x + pw > win->w) x = win->w - pw - 6;
            if (y + H  > win->h) y = win->h - H - 6;
            if (x < 6) x = 6; if (y < 6) y = 6;
        }
        px = x; py = y; open = true; visible = true;
        if (win) r = { 0, 0, win->w, win->h };               /* modal capture */
    }
    void dismiss() { open = false; visible = false; }
    void draw() override {
        if (!open) return;
        int fh = ugfx_font_h(), H = total_h(), rad = TH_R_MD;
        ugfx_elevation(px, py, pw, H, rad, 4);                         /* floating menu shadow */
        ugfx_rrect_aa(px, py, pw, H, rad, TH_SURF_3);
        ugfx_rrect_border(px, py, pw, H, rad, 1, TH_BORDER);           /* crisp edge */
        ugfx_fill_a(px + rad, py, pw - 2 * rad, 1, ARGB(48, 255, 255, 255));  /* top sheen */
        int cy = py + 5;
        for (int i = 0; i < n; i++) {
            if (it[i].kind == PK_SEP) { ugfx_fill_a(px + 10, cy + 3, pw - 20, 1, ARGB(50, 150, 170, 230)); cy += 7; continue; }
            if (i == hover_item) ugfx_state_layer(px + 5, cy, pw - 10, rowh, TH_R_SM, TH_HOVER_A);  /* hover */
            int tx = px + 12;
            if (it[i].icon) { blit_scaled(px + 10, cy + (rowh - 18) / 2, 18, 18, it[i].icon, it[i].iw, it[i].ih); tx = px + 36; }
            if (it[i].kind == PK_TOGGLE) {                  /* checkbox */
                uint32_t bc = toggle ? TH_ACCENT : ARGB(120, 150, 160, 180);
                ugfx_rrect_a(px + 12, cy + (rowh - 14) / 2, 14, 14, 3, bc);
                if (toggle) { int bx = px + 14, by = cy + rowh / 2; vline_(bx, by, bx + 3, by + 3, 2, RGB(255,255,255)); vline_(bx + 3, by + 3, bx + 9, by - 4, 2, RGB(255,255,255)); }
                tx = px + 34;
            }
            ugfx_text(tx, cy + (rowh - fh) / 2, it[i].label, TH_TEXT, UGFX_TRANSPARENT);
            cy += rowh;
        }
    }
    bool on_mouse(int x, int y, int) override {
        if (!open) return true;
        int hit = item_at(x, y);
        if (hit >= 0 && it[hit].kind == PK_TOGGLE) { toggle = !toggle; return true; }  /* stay open */
        int tag = (hit >= 0) ? it[hit].tag : -1;
        dismiss();
        if (on_pick) on_pick(ctx, tag);
        return true;
    }
};

/* an app discovered under /Apps, for the Open With chooser */
struct AppEntry { char label[32], exec[160]; uint32_t *icon; int iw, ih; };

/* ------------------------------------------------------------------ FilesApp */
struct FilesApp : ui::Window {
    ui::Panel    bar;
    IconButton   back, fwd, up, newf, del;
    ui::Button   info;
    ui::Label    pathlbl;
    ui::TextField filterfld;                 /* the "/" live name-filter field */
    ui::ListView list;
    Sidebar      side;
    DetailsPanel details;
    StatusBar    status;
    Popup        menu;

    char          path[256] = HOMEDIR;
    struct dirent ents[NMAX];
    int           nents = 0;
    int           view[NMAX]; int nview = 0;     /* ents indices currently shown (after filter) */
    bool          filter_open = false;           /* the live filter bar is up ("/") */
    bool          loading = false;               /* guard: set_text() fires on_change mid-load */
    uint32_t     *appicon[NMAX] = {};
    int           appiw[NMAX] = {}, appih[NMAX] = {};
    char          appexec[NMAX][160] = {};
    bool          details_open = true;
    int           fw = 0, fh = 0, TBH = 0, SBW = 0, DPW = 0, STH = 0, listy = 0;
    char          hist[HISTN][256] = {};
    int           hist_n = 0, hist_i = -1;

    AppEntry      apps[MAXAPPS]; int napps = 0;
    int           menu_mode = 0;                 /* 0 = context actions, 1 = open-with */
    char          ow_path[256] = {0};            /* file the open-with chooser targets */
    char          ow_ext[16] = {0};
    char          cut_src[256] = {0};            /* full path of a cut file, pending a paste-move */
    bool          have_cut = false;              /* the active clipboard file came from a Cut (Ctrl+X) */

    int at_root()  const { return path[0] == '/' && path[1] == 0; }
    int has_up()   const { return at_root() ? 0 : 1; }
    int dpw_now()  const { return details_open ? DPW : 0; }

    static int cmp(const struct dirent *a, const struct dirent *b) {
        if ((a->type == FT_DIR) != (b->type == FT_DIR)) return a->type == FT_DIR ? -1 : 1;
        const char *x = a->name, *y = b->name;
        while (*x && *y) { if (*x != *y) return (int)(unsigned char)*x - (int)(unsigned char)*y; x++; y++; }
        return *x - *y;
    }

    /* scan /Apps once for the Open With chooser */
    void load_apps() {
        struct dirent e[MAXAPPS * 2];
        int n = readdir("/Apps", e, MAXAPPS * 2);
        for (int i = 0; i < n && napps < MAXAPPS; i++) {
            if (!is_app_dir(e[i].type, e[i].name)) continue;
            char base[160]; join(base, sizeof base, "/Apps", e[i].name);
            char mp[200]; join(mp, sizeof mp, base, "manifest");
            int ml = 0; char *mb = sys_slurp(mp, &ml);
            if (!mb) continue;
            AppEntry *a = &apps[napps];
            char val[120];
            disp_name(e[i].name, a->label, sizeof a->label);
            if (manifest_get(mb, "name", val, sizeof val)) { int j = 0; for (; val[j] && j < 31; j++) a->label[j] = val[j]; a->label[j] = 0; }
            a->exec[0] = 0; a->icon = 0; a->iw = a->ih = 0;
            if (manifest_get(mb, "exec", val, sizeof val)) join(a->exec, sizeof a->exec, base, val);
            char ic[64];
            if (manifest_get(mb, "icon", ic, sizeof ic) && ic[0]) { char ip[200]; join(ip, sizeof ip, base, ic); a->icon = load_icon_argb(ip, &a->iw, &a->ih); }
            if (a->exec[0]) napps++;
            free(mb);
        }
    }

    void free_appcache() { for (int i = 0; i < NMAX; i++) { if (appicon[i]) { free(appicon[i]); appicon[i] = 0; } appexec[i][0] = 0; } }
    void load_dir() {
        free_appcache();
        nents = readdir(path, ents, NMAX);
        if (nents < 0) nents = 0;
        for (int i = 0; i < nents; i++)
            for (int j = i + 1; j < nents; j++)
                if (cmp(&ents[j], &ents[i]) < 0) { struct dirent t = ents[i]; ents[i] = ents[j]; ents[j] = t; }
        for (int i = 0; i < nents; i++) {
            if (!is_app_dir(ents[i].type, ents[i].name)) continue;
            char base[256]; join(base, sizeof base, path, ents[i].name);
            char mpath[300]; join(mpath, sizeof mpath, base, "manifest");
            int ml = 0; char *mbuf = sys_slurp(mpath, &ml);
            if (!mbuf) continue;
            char val[120];
            if (manifest_get(mbuf, "exec", val, sizeof val)) join(appexec[i], sizeof appexec[i], base, val);
            char iconrel[64];
            if (manifest_get(mbuf, "icon", iconrel, sizeof iconrel) && iconrel[0]) {
                char ipath[320]; join(ipath, sizeof ipath, base, iconrel);
                appicon[i] = load_icon_argb(ipath, &appiw[i], &appih[i]);
            }
            free(mbuf);
        }
        apply_filter();                 /* sets list.count, resets sel, refreshes status */
    }
    /* map a visible list row to an ents[] index, or -1 for the ".." row / off-list */
    int ent_at(int row) const {
        int hu = has_up();
        if (row < 0 || (hu && row == 0)) return -1;
        int v = row - hu;
        return (v >= 0 && v < nview) ? view[v] : -1;
    }
    bool filtering() const { return filter_open && filterfld.length() > 0; }
    /* rebuild the visible set from the current filter text (case-insensitive substring
     * over the display name); resets the selection, like Finder/Dolphin's filter. */
    void apply_filter() {
        const char *q = filter_open ? filterfld.text() : "";
        nview = 0;
        for (int i = 0; i < nents; i++) {
            char label[64]; disp_name(ents[i].name, label, sizeof label);
            if (tu_ci_contains(label, q)) view[nview++] = i;
        }
        list.count = nview + has_up();
        list.sel = -1; list.top = 0;
        details.has = false;
        update_status();
        if (q[0]) { print("[files] filter "); printu((unsigned)nview); print("\r\n"); }
        invalidate();
    }
    /* refresh the bottom status bar from the folder count + current selection */
    void update_status() {
        if (filtering()) snprintf(status.left, sizeof status.left, "%d of %d shown", nview, nents);
        else             snprintf(status.left, sizeof status.left, "%d item%s", nents, nents == 1 ? "" : "s");
        int hu = has_up();
        if (details.has && list.sel >= 0 && !(hu && list.sel == 0)) {
            if (details.is_file)
                snprintf(status.right, sizeof status.right, "%s selected  --  %u bytes", details.name, details.size);
            else
                snprintf(status.right, sizeof status.right, "%s selected", details.name);
        } else status.right[0] = 0;
    }
    /* "/" opens the filter bar (or refocuses it if already open) */
    void open_filter() {
        if (!filter_open) {
            filter_open = true; filterfld.visible = true;
            loading = true; filterfld.set_text(""); loading = false;
            layout_widgets();
        }
        focus = &filterfld;
        apply_filter();
    }
    /* Esc (or "/" toggle) closes it, clearing the filter and restoring the full list */
    void close_filter() {
        if (!filter_open) return;
        filter_open = false; filterfld.visible = false;
        loading = true; filterfld.set_text(""); loading = false;
        layout_widgets();
        focus = &list;
        apply_filter();
    }
    void load_path(const char *p) {
        strncpy(path, p, sizeof path - 1); path[sizeof path - 1] = 0;
        if (!path[0]) { path[0] = '/'; path[1] = 0; }
        if (filterfld.length() > 0) { loading = true; filterfld.set_text(""); loading = false; }  /* fresh folder, fresh filter */
        load_dir();
        pathlbl.text = path; side.cur = path;
    }
    void update_nav() { back.enabled = hist_i > 0; fwd.enabled = hist_i < hist_n - 1; }
    void nav_to(const char *p) {
        load_path(p);
        if (hist_i >= 0 && eqn(hist[hist_i], path)) { update_nav(); return; }
        if (hist_i + 1 >= HISTN) { for (int i = 1; i < HISTN; i++) { strncpy(hist[i - 1], hist[i], 255); hist[i - 1][255] = 0; } hist_i = HISTN - 2; }
        hist_i++; strncpy(hist[hist_i], path, 255); hist[hist_i][255] = 0; hist_n = hist_i + 1;
        update_nav();
    }
    void go_back() { if (hist_i > 0)          { hist_i--; load_path(hist[hist_i]); update_nav(); invalidate(); } }
    void go_fwd()  { if (hist_i < hist_n - 1) { hist_i++; load_path(hist[hist_i]); update_nav(); invalidate(); } }
    void go_up() {
        if (at_root()) return;
        char parent[256]; strncpy(parent, path, sizeof parent - 1); parent[sizeof parent - 1] = 0;
        int last = -1; for (int i = 0; parent[i]; i++) if (parent[i] == '/') last = i;
        if (last <= 0) { parent[0] = '/'; parent[1] = 0; } else parent[last] = 0;
        nav_to(parent);
    }

    /* open a non-.app file: use a remembered default app for its extension, else
     * pop the Open With chooser. */
    void open_file(const char *name) {
        char full[256]; join(full, sizeof full, path, name);
        char ext[16]; ext_of(name, ext, sizeof ext);
        char key[40]; snprintf(key, sizeof key, "open.default.%s", ext[0] ? ext : "_");
        const char *def = reg_get(key, "");
        if (def && def[0]) { struct fstat st; if (stat_(def, &st) == 0) { sys_open_with(def, full); return; } }
        open_with_chooser(name);
    }
    void open_with_chooser(const char *name) {
        char full[256]; join(full, sizeof full, path, name);
        strncpy(ow_path, full, sizeof ow_path - 1); ow_path[sizeof ow_path - 1] = 0;
        ext_of(name, ow_ext, sizeof ow_ext);
        menu_mode = 1;
        menu.reset();
        for (int i = 0; i < napps; i++) menu.add(apps[i].label, PK_ACTION, i, apps[i].icon, apps[i].iw, apps[i].ih);
        if (napps == 0) menu.add("No apps found", PK_ACTION, -1, 0, 0, 0);
        menu.add("", PK_SEP, 0, 0, 0, 0);
        char tl[40]; snprintf(tl, sizeof tl, "Always use for .%s", ow_ext[0] ? ow_ext : "file");
        menu.add(tl, PK_TOGGLE, -2, 0, 0, 0);
        menu.show(w / 2 - 90, h / 2 - 60);
    }

    void enter(int row) {
        if (has_up() && row == 0) { go_up(); return; }
        int idx = ent_at(row);
        if (idx < 0) return;
        struct dirent *e = &ents[idx];
        if (is_app_dir(e->type, e->name)) { if (appexec[idx][0]) sys_launch(appexec[idx]); return; }
        if (e->type == FT_DIR) { char t[256]; join(t, sizeof t, path, e->name); nav_to(t); }
        else open_file(e->name);
    }
    /* A row click mutates the details pane + status bar, which lie outside the
     * list's own rect; without a repaint request the damage-tracked frame would
     * only refresh the list. Repaint the whole window (selection is low-frequency). */
    void select_row(int row) {
        int idx = ent_at(row);
        if (idx < 0) { details.has = false; update_status(); invalidate(); return; }
        struct dirent *e = &ents[idx];
        disp_name(e->name, details.name, sizeof details.name);
        details.kind = kind_for(e->type, e->name);
        details.is_file = (e->type == FT_FILE);
        details.size = e->size;
        details.icon = appicon[idx]; details.iw = appiw[idx]; details.ih = appih[idx];
        details.file_icon = file_icon_for(e->type, e->name);
        join(details.where, sizeof details.where, path, e->name);
        details.has = true;
        update_status();
        invalidate();
    }
    static void rmrf(const char *p) {
        struct fstat st; if (stat_(p, &st) < 0) return;
        if (st.type == FT_DIR) {
            for (;;) {
                struct dirent e[64]; int n = readdir(p, e, 64);
                if (n <= 0) break;
                for (int i = 0; i < n; i++) { char c[256]; join(c, sizeof c, p, e[i].name); rmrf(c); }
            }
            rmdir(p);
        } else funlink(p);
    }
    void do_delete() {
        int idx = ent_at(list.sel);
        if (idx < 0) return;
        struct dirent *e = &ents[idx];
        char child[256]; join(child, sizeof child, path, e->name);
        rmrf(child); load_dir();
    }
    /* Ctrl+C / Ctrl+X: stash the selected file on the clipboard ring; Cut also
     * remembers the source so a later Paste moves it. Files only for now -- copying a
     * whole directory needs a recursive walk we don't do yet. */
    void copy_sel(bool cut) {
        int idx = ent_at(list.sel);
        if (idx < 0) return;
        struct dirent *e = &ents[idx];
        if (e->type != FT_FILE) return;
        char full[256]; join(full, sizeof full, path, e->name);
        int n = 0; char *b = sys_slurp(full, &n);
        if (!b) return;
        clip_put(CLIP_FILE, e->name, b, n);
        free(b);
        have_cut = cut;
        if (cut) { strncpy(cut_src, full, sizeof cut_src - 1); cut_src[sizeof cut_src - 1] = 0; }
        else cut_src[0] = 0;
        print(cut ? "[files] cut " : "[files] copy "); print(e->name); print("\r\n");
    }
    /* Ctrl+V: drop the active clipboard file into the current directory. Dedupes the
     * name ("copy of X") rather than clobbering; a pending Cut deletes the source. */
    void paste() {
        if (clip_count() <= 0) return;
        int idx = clip_active(-1);
        struct clipinfo ci;
        if (idx < 0 || clip_info(idx, &ci) != 0 || ci.type != CLIP_FILE || ci.len == 0) return;
        char name[40]; strncpy(name, ci.name, sizeof name - 1); name[sizeof name - 1] = 0;
        char dst[256]; join(dst, sizeof dst, path, name);
        if (have_cut && cut_src[0] && eqn(dst, cut_src)) {     /* cut+paste in the same dir: no-op */
            have_cut = false; cut_src[0] = 0; return;
        }
        if (sys_exists(dst, 0)) {                              /* don't clobber an existing file */
            char alt[80]; snprintf(alt, sizeof alt, "copy of %s", name);
            join(dst, sizeof dst, path, alt);
        }
        char *buf = (char *)malloc(ci.len);
        if (!buf) return;
        int n = clip_get(idx, buf, ci.len);
        if (n > 0) sys_spit(dst, buf, n);
        free(buf);
        if (n > 0 && have_cut && cut_src[0]) { rmrf(cut_src); have_cut = false; cut_src[0] = 0; }
        print("[files] paste "); print(dst); print("\r\n");
        load_dir(); invalidate();
    }
    void make_folder() {
        char name[40], child[256];
        for (int k = 0; k < 1000; k++) {
            if (k == 0) strncpy(name, "newfolder", sizeof name); else snprintf(name, sizeof name, "newfolder%d", k);
            join(child, sizeof child, path, name);
            struct fstat st;
            if (stat_(child, &st) < 0) { mkdir(child); load_dir(); return; }
        }
    }

    static void render_row(void *ctx, int i, ui::Rect cell, bool sel) {
        FilesApp *a = (FilesApp *)ctx;
        if (!sel && (i & 1)) ugfx_fill(cell.x, cell.y, cell.w, cell.h, C_ZEBRA);
        int ix = cell.x + 12, ty = cell.y + (cell.h - a->fh) / 2, iyy = cell.y + (cell.h - 20) / 2;
        const char *name; int type; unsigned size = 0; int idx = -1;
        if (a->has_up() && i == 0) { name = ".."; type = FT_DIR; }
        else { idx = a->ent_at(i); if (idx < 0) return; struct dirent *e = &a->ents[idx]; name = e->name; type = e->type; size = e->size; }
        char label[64]; disp_name(name, label, sizeof label);
        if (idx >= 0 && is_app_dir(type, name) && a->appicon[idx]) blit_scaled(ix, iyy, 20, 20, a->appicon[idx], a->appiw[idx], a->appih[idx]);
        else blit_scaled(ix, iyy, 20, 20, fileicons_argb[file_icon_for(type, name)], FILEICON_SZ, FILEICON_SZ);
        ugfx_text(ix + 30, ty, label, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
        if (type == FT_FILE) {
            char sz[20]; snprintf(sz, sizeof sz, "%u B", size);
            ugfx_text(cell.x + cell.w - ugfx_text_w(sz) - 14, ty, sz, sel ? RGB(230, 238, 250) : TH_MUTED, UGFX_TRANSPARENT);
        }
    }

    void layout_widgets() {
        fw = ugfx_font_w(); fh = ugfx_font_h();
        TBH = fh + 20;
        STH = fh + 12;
        SBW = 11 * fw + 34; if (SBW < 150) SBW = 150;
        DPW = 20 * fw + 28; if (DPW < 200) DPW = 200; if (DPW > 264) DPW = 264;
        int dp = dpw_now(), mainx = SBW, mainw = w - SBW - dp;

        side.r = { 0, 0, SBW, h };
        side.row_h = fh + 10; side.head_h = fh + 18;

        bar.r = { SBW, 0, w - SBW, TBH };
        int sz = TBH - 12, bx = SBW + 8, by = 6;
        IconButton *b[5] = { &back, &fwd, &up, &newf, &del };
        int gl[5] = { G_BACK, G_FWD, G_UP, G_NEWF, G_TRASH };
        for (int i = 0; i < 5; i++) {
            b[i]->glyph = gl[i]; b[i]->r = { bx, by, sz, sz }; bx += sz + 5;
            if (i == 2) bx += 12;                            /* gap after the nav cluster */
        }
        int iw_ = ugfx_text_w("Info") + 18;
        info.text = "Info"; info.r = { w - iw_ - 8, by, iw_, sz };

        pathlbl.r = { mainx + 12, TBH + 6, mainw - 24, fh };
        int loc_bottom = TBH + fh + 14;                  /* where the location bar ends   */
        int FBH = filter_open ? fh + 16 : 0;             /* the live-filter bar band       */
        filterfld.visible = filter_open;
        filterfld.r = { mainx + 10, loc_bottom + 3, mainw - 20, fh + 8 };
        listy = loc_bottom + FBH;
        int lh = h - listy - STH; if (lh < fh) lh = fh;
        list.r = { mainx, listy, mainw, lh }; list.row_h = fh + 12;
        status.r = { mainx, h - STH, mainw, STH };
        details.r = { w - DPW, TBH, DPW, h - TBH };
    }

    bool build() {
        struct sysinfo si; sysinfo(&si);
        int cw = (int)si.fb_w - 150, ch = (int)si.fb_h - 130;
        if (cw < 640) cw = 640; if (cw > 1000) cw = 1000;
        if (ch < 420) ch = 420; if (ch > 720) ch = 720;
        if (cw > (int)si.fb_w - 20) cw = (int)si.fb_w - 20;
        if (ch > (int)si.fb_h - 60) ch = (int)si.fb_h - 60;
        if (!create(cw, ch, "Files")) return false;

        reg_load();
        load_apps();

        bar.color = C_TOOLBAR; bar.sep_bottom = true;
        list.bg = C_LIST; list.sel_bg = C_SELROW;
        pathlbl.fg = TH_MUTED;

        side.ctx = this; side.on_pick = [](void *c, const char *p) { ((FilesApp *)c)->nav_to(p); };
        side.add_item("Home",         HOMEDIR,              0);
        side.add_item("Desktop",      HOMEDIR "/Desktop",   0);
        side.add_item("Documents",    HOMEDIR "/Documents", 0);
        side.add_item("Downloads",    HOMEDIR "/Downloads", 0);
        side.add_item("Pictures",     HOMEDIR "/Pictures",  0);
        side.add_item("Applications", "/Apps",              1);
        side.add_item("System",       "/System",            0);
        side.add_item("Computer",     "/",                  0);
        side.cur = path;

        list.ctx = this; list.render_row = render_row;
        list.on_select   = [](void *c, int i) { ((FilesApp *)c)->select_row(i); };
        list.on_activate = [](void *c, int i) { ((FilesApp *)c)->enter(i); };

        back.ctx = fwd.ctx = up.ctx = newf.ctx = del.ctx = info.ctx = this;
        back.on_click = [](void *c) { ((FilesApp *)c)->go_back(); };
        fwd.on_click  = [](void *c) { ((FilesApp *)c)->go_fwd(); };
        up.on_click   = [](void *c) { ((FilesApp *)c)->go_up(); };
        newf.on_click = [](void *c) { ((FilesApp *)c)->make_folder(); };
        del.on_click  = [](void *c) { ((FilesApp *)c)->do_delete(); };
        info.on_click = [](void *c) {
            auto *a = (FilesApp *)c;
            a->details_open = !a->details_open; a->details.visible = a->details_open;
            a->layout_widgets(); a->invalidate();
        };

        menu.ctx = this;
        menu.on_pick = [](void *c, int tag) { ((FilesApp *)c)->menu_pick(tag); };

        layout_widgets();
        add(&side); add(&bar); add(&back); add(&fwd); add(&up); add(&newf); add(&del); add(&info);
        add(&pathlbl); add(&list); add(&details); add(&status); add(&menu);   /* menu last = on top + modal */
        focus = &list;
        details.visible = details_open;

        menu_begin();                                 /* app menus #6: a real File/Edit/Go bar */
        int mf = menu_add("File"); menu_item(mf, "New Folder", 'N'); menu_item(mf, "Refresh", 0);
        int me = menu_add("Edit"); menu_item(me, "Copy", 'C'); menu_item(me, "Cut", 'X');
                                   menu_item(me, "Paste", 'V'); menu_item(me, "Delete", 0);
        int mg = menu_add("Go");   menu_item(mg, "Up", 0); menu_item(mg, "Back", 0); menu_item(mg, "Forward", 0);
        menu_commit();

        struct fstat st;
        nav_to(sys_exists(HOMEDIR, &st) ? HOMEDIR : "/");
        return true;
    }

    void menu_pick(int tag) {
        if (tag == -1) { invalidate(); return; }              /* dismissed */
        if (menu_mode == 1) {                                 /* Open With */
            if (tag >= 0 && tag < napps) {
                if (menu.toggle && ow_ext[0]) {               /* remember the default for this extension */
                    char key[40]; snprintf(key, sizeof key, "open.default.%s", ow_ext);
                    reg_set(key, apps[tag].exec); reg_save();
                }
                sys_open_with(apps[tag].exec, ow_path);
            }
            invalidate(); return;
        }
        switch (tag) {                                        /* context actions */
        case 1: if (list.sel >= 0) enter(list.sel); break;    /* Open */
        case 2: if (list.sel >= 0) { int hu = has_up(); if (!(hu && list.sel == 0)) open_with_chooser(ents[list.sel - hu].name); } break;
        case 3: do_delete(); break;
        case 4: make_folder(); break;
        case 5: load_dir(); break;
        case 6: copy_sel(false); break;                   /* Copy a file's bytes to the clipboard */
        case 7: copy_sel(true);  break;                   /* Cut: copy + mark the source for a move */
        case 8: paste();         break;                   /* Paste the clipboard file here */
        }
        invalidate();
    }

    /* right-click: select the row under the cursor (if any), then open a menu */
    void on_context(int x, int y) override {
        if (x >= list.r.x && x < list.r.x + list.r.w && y >= list.r.y && y < list.r.y + list.r.h) {
            int row = list.top + (y - list.r.y) / list.row_h;
            if (row >= 0 && row < list.count) { list.sel = row; select_row(row); }
        }
        menu_mode = 0; menu.reset();
        int hu = has_up();
        int real = (list.sel >= 0 && !(hu && list.sel == 0)) ? list.sel - hu : -1;
        if (real >= 0) {
            struct dirent *e = &ents[real];
            if (e->type == FT_DIR || is_app_dir(e->type, e->name)) menu.add("Open", PK_ACTION, 1, 0, 0, 0);
            else { menu.add("Open", PK_ACTION, 1, 0, 0, 0); menu.add("Open With...", PK_ACTION, 2, 0, 0, 0);
                   menu.add("Copy", PK_ACTION, 6, 0, 0, 0); menu.add("Cut", PK_ACTION, 7, 0, 0, 0); }
            menu.add("Delete", PK_ACTION, 3, 0, 0, 0);
            menu.add("", PK_SEP, 0, 0, 0, 0);
        }
        if (clip_count() > 0) { struct clipinfo ci;             /* offer Paste when a file is on the clipboard */
            if (clip_info(clip_active(-1), &ci) == 0 && ci.type == CLIP_FILE)
                menu.add("Paste", PK_ACTION, 8, 0, 0, 0); }
        menu.add("New Folder", PK_ACTION, 4, 0, 0, 0);
        menu.add("Refresh",    PK_ACTION, 5, 0, 0, 0);
        menu.show(x, y);
        invalidate();
    }
    void on_resize(int, int) override { layout_widgets(); if (menu.open && menu.win) menu.r = { 0, 0, w, h }; }
    void on_key(int key) override {
        if (menu.open) { if (key == ui::UK_ESC) { menu.dismiss(); invalidate(); } return; }
        if (key == ui::UK_BACK) go_up();
        else if (key == 0x03) copy_sel(false);   /* Ctrl+C */
        else if (key == 0x18) copy_sel(true);    /* Ctrl+X */
        else if (key == 0x16) paste();           /* Ctrl+V */
        else if (key == 'r') load_dir();
    }
    void on_nav(int dir) override { if (dir == 0) go_back(); else go_fwd(); }
    /* Menu bar (app menus #6): File / Edit / Go. The Edit accelerators ^C/^X/^V and
     * File's ^N are intercepted by the compositor for the focused Files window and
     * arrive here as WEV_MENU picks (the same actions the toolbar + right-click run). */
    void on_menu(int menu, int item) override {
        print("[files] menu "); printu((unsigned)menu); printc(' '); printu((unsigned)item); print("\r\n");
        if (menu == 0) { if (item == 0) make_folder(); else if (item == 1) load_dir(); }      /* File: New Folder / Refresh */
        else if (menu == 1) {                                                                 /* Edit */
            if (item == 0) copy_sel(false); else if (item == 1) copy_sel(true);
            else if (item == 2) paste(); else if (item == 3) do_delete();
        } else if (menu == 2) {                                                               /* Go */
            if (item == 0) go_up(); else if (item == 1) go_back(); else if (item == 2) go_fwd();
        }
        invalidate();
    }
};

int app_main() {
    FilesApp *app = new FilesApp();
    if (!app->build()) { print("[files] needs the desktop\r\n"); proc_exit(); }
    print("[files] file manager up\r\n");
    return app->run();
}
