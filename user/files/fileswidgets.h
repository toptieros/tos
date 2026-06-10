/* files -- the file manager's custom widgets on the C++ toolkit (ui::Widget).
 *
 * IconButton (toolbar glyph), Sidebar (pinned places), DetailsPanel (inspector),
 * StatusBar (item count + selection summary), Breadcrumb (clickable location bar),
 * IconGrid (the icon/grid view), and Popup (context menu + Open With chooser). They
 * are self-contained (callbacks via fn-pointer + ctx); FilesApp (files.cpp) wires
 * them together. The colour palette + MAXAPPS used by both live here too. */
#pragma once
#include "ui.h"
#include "icons.h"
#include "pathbar.h"       /* pathbar_split + struct crumb: the breadcrumb model */
#include "filesutil.h"     /* eqn / blit_scaled / vline_ / draw_glyph + G_* glyph ids */
#include "fstime.h"        /* fstime_unpack: render an entry's packed "Modified" time */
#include "humansize.h"     /* human_bytes: recursive folder size on the Get Info pane  */
#include "fileinfo.h"      /* info_owner_label / info_count_label: Get Info fields (§8) */
#include "colfit.h"        /* colfit: details-view column layout (§1)                  */
#include "tagstore.h"      /* TAG_NCOLORS + tag_names_: the color-tag palette (§10)    */

#define MAXAPPS 16          /* Open With chooser app cap (also FilesApp's apps[])      */

static const uint32_t C_ZEBRA   = RGB(33, 37, 49);
static const uint32_t C_SIDEBAR = RGB(27, 31, 43);
static const uint32_t C_DETAILS = RGB(30, 34, 46);
static const uint32_t C_LIST    = RGB(24, 27, 37);
static const uint32_t C_TOOLBAR = RGB(34, 39, 52);
static const uint32_t C_SELROW  = ARGB(255, 70, 116, 200);

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
/* The Places sidebar (files-app §7): a sectioned list -- Favorites (the user's
 * registry-backed pins, edited via "Add to / Remove from Places") and Locations
 * (system roots; the volume row carries an inline used-space bar) -- each section
 * collapsible by clicking its header, plus Trash pinned at the bottom. Rows live
 * in one growable array tagged with a section id; vrow() maps a visible row to a
 * header/item so draw + hit-test share one layout. FilesApp rebuilds the items
 * when the places change, and routes right-clicks here via fav_at(). */
#define SIDE_FAV  0
#define SIDE_LOC  1
#define SIDE_TAGS 2                /* §10: the seven color tags (rows are "tag:<i>" pseudo-paths) */
#define SIDE_NSECT 3
/* §10: the fixed tag palette (indexes match tag_names_ / the ~/.tags bitmask). Content
 * colors, not chrome -- the slate-blue theme stays untouched. */
static const uint32_t tag_colors_[TAG_NCOLORS] = {
    ARGB(255, 235,  95,  95),   /* Red    */
    ARGB(255, 240, 160,  70),   /* Orange */
    ARGB(255, 235, 205,  80),   /* Yellow */
    ARGB(255, 120, 200, 120),   /* Green  */
    ARGB(255,  96, 152, 252),   /* Blue (the accent family) */
    ARGB(255, 175, 130, 235),   /* Purple */
    ARGB(255, 150, 160, 175),   /* Gray   */
};
struct SideItem { char label[32]; char path[64]; int sect; };
class Sidebar : public ui::Widget {
public:
    SideItem *items = nullptr; int n = 0, cap = 0;       /* grows on demand */
    int collapsed[SIDE_NSECT] = { 0, 0, 1 };             /* Tags starts collapsed (§10) */
    int row_h = 26, top_pad = 6, hover_row = -1;
    int active_tag = -1;            /* §10: the tag row lit as the active filter (-1 none) */
    int vol_permille = -1;          /* used/total on the "/" row (0..1000); <0 hides the bar */
    char trash_path[64] = {0};
    const char *cur = nullptr;
    void *ctx = nullptr;
    void (*on_pick)(void *, const char *) = nullptr;
    void (*on_changed)(void *) = nullptr;                /* a header was collapsed/expanded */
    Sidebar() { focusable = false; }
    void clear() { n = 0; }
    void add_item(const char *label, const char *path, int sect) {
        if (n >= cap) {
            int nc = cap ? cap * 2 : 12;
            SideItem *ni = (SideItem *)malloc(sizeof(SideItem) * nc); if (!ni) return;
            for (int i = 0; i < n; i++) ni[i] = items[i];
            if (items) free(items);
            items = ni; cap = nc;
        }
        SideItem *it = &items[n];
        int i = 0; for (; label[i] && i < 31; i++) it->label[i] = label[i]; it->label[i] = 0;
        int j = 0; for (; path[j] && j < 63; j++) it->path[j] = path[j]; it->path[j] = 0;
        it->sect = sect; n++;
    }
    void set_trash(const char *path) {
        int i = 0; for (; path[i] && i < 63; i++) trash_path[i] = path[i]; trash_path[i] = 0;
    }
    /* ---- the shared visible-row model ---- */
    int vrows() const {
        int v = 0;
        for (int s = 0; s < SIDE_NSECT; s++) {
            v++;
            if (!collapsed[s]) for (int i = 0; i < n; i++) if (items[i].sect == s) v++;
        }
        return v;
    }
    /* visible row v -> its section + item (a header has *item == -1); 0 = out of range */
    int vrow(int v, int *sect, int *item) const {
        int r_ = 0;
        for (int s = 0; s < SIDE_NSECT; s++) {
            if (r_++ == v) { *sect = s; *item = -1; return 1; }
            if (!collapsed[s]) for (int i = 0; i < n; i++) if (items[i].sect == s)
                if (r_++ == v) { *sect = s; *item = i; return 1; }
        }
        return 0;
    }
    /* With Tags expanded the rows can outgrow the panel (no fixed cap on favourites
     * either), so the section area scrolls by whole rows on the wheel, clipped above
     * the pinned Trash divider. */
    int scroll = 0;                                      /* first visible row index */
    int trash_y() const { return r.y + r.h - row_h - 8; }
    int row_limit() const { return trash_path[0] ? trash_y() - 6 : r.y + r.h; }
    int rows_fit() const { int a = row_limit() - (r.y + top_pad); return a > 0 ? a / row_h : 0; }
    int max_scroll() const { int m = vrows() - rows_fit(); return m > 0 ? m : 0; }
    int row_y(int v) const { return r.y + top_pad + (v - scroll) * row_h; }
    int row_at(int y) const {                            /* visible row under y, or -1 */
        if (y < r.y + top_pad || y >= row_limit()) return -1;
        int v = (y - r.y - top_pad) / row_h + scroll;
        return v < vrows() ? v : -1;
    }
    bool on_scroll(int delta) override {
        int ns = scroll - delta;                         /* wheel down (delta<0) reveals later rows */
        if (ns < 0) ns = 0; if (ns > max_scroll()) ns = max_scroll();
        if (ns == scroll) return false;
        scroll = ns;
        if (on_changed) on_changed(ctx);                 /* fresh siderow dump + repaint */
        return true;
    }
    /* The Favorites item under (x, y) as an items[] index, or -1 -- favorites are
     * added first, so this doubles as the places-list index. The window uses it
     * to route a right-click to the "Remove from Places" menu. */
    int fav_at(int x, int y) const {
        if (x < r.x || x >= r.x + r.w) return -1;
        int s, i, v = row_at(y);
        if (v < 0 || !vrow(v, &s, &i) || i < 0 || s != SIDE_FAV) return -1;
        return i;
    }
    bool on_hover(int, int y) override {
        int v = row_at(y);
        if (trash_path[0] && y >= trash_y() && y < trash_y() + row_h) v = 1000;  /* the pinned Trash row */
        if (v == hover_row) return false;
        hover_row = v; return true;
    }
    void on_leave() override { hovered = false; hover_row = -1; }
    /* one item row (glyph >= 0 swaps the folder icon for a toolbar glyph: Trash) */
    void draw_item(int y, const char *label, const char *path, int hot, int glyph) {
        int fh = ugfx_font_h();
        int sel = cur && eqn(cur, path);
        if (sel)      ugfx_rrect_a(r.x + 6, y + 1, r.w - 12, row_h - 2, TH_R_SM, ARGB(150, 96, 152, 252));
        else if (hot) ugfx_state_layer(r.x + 6, y + 1, r.w - 12, row_h - 2, TH_R_SM, TH_HOVER_A);
        if (glyph >= 0) draw_glyph(glyph, r.x + 21, y + row_h / 2, 7, sel ? RGB(255, 255, 255) : TH_TEXT);
        else blit_scaled(r.x + 12, y + (row_h - 18) / 2, 18, 18, fileicons_argb[FILEICON_FOLDER], FILEICON_SZ, FILEICON_SZ);
        ugfx_text(r.x + 38, y + (row_h - fh) / 2, label, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
        if (vol_permille >= 0 && path[0] == '/' && !path[1]) {   /* the volume's used-space bar (§7) */
            int bx = r.x + 38, bw = r.w - 38 - 14, by = y + row_h - 5;
            int used = bw * vol_permille / 1000;
            ugfx_fill_a(bx, by, bw, 3, ARGB(40, 255, 255, 255));
            ugfx_fill_a(bx, by, used > bw ? bw : used, 3, ARGB(190, 96, 152, 252));
        }
    }
    /* §10: a Tags row -- colour dot + name; lit while it is the active filter */
    void draw_tag(int y, const SideItem &it, int hot) {
        int fh = ugfx_font_h();
        int t = (it.path[4] >= '0' && it.path[4] <= '6') ? it.path[4] - '0' : -1;   /* "tag:N" */
        int sel = (t >= 0 && t == active_tag);
        if (sel)      ugfx_rrect_a(r.x + 6, y + 1, r.w - 12, row_h - 2, TH_R_SM, ARGB(150, 96, 152, 252));
        else if (hot) ugfx_state_layer(r.x + 6, y + 1, r.w - 12, row_h - 2, TH_R_SM, TH_HOVER_A);
        uint32_t c = (t >= 0) ? tag_colors_[t] : TH_MUTED;
        ugfx_rrect_aa(r.x + 16, y + (row_h - 10) / 2, 10, 10, 5, c);
        ugfx_text(r.x + 38, y + (row_h - fh) / 2, it.label, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
    }
    void draw() override {
        if (!visible) return;
        int fh = ugfx_font_h();
        ugfx_fill(r.x, r.y, r.w, r.h, C_SIDEBAR);
        ugfx_fill_a(r.x + r.w - 1, r.y, 1, r.h, ARGB(70, 0, 0, 0));
        static const char *sect_name[SIDE_NSECT] = { "Favorites", "Locations", "Tags" };
        if (scroll > max_scroll()) scroll = max_scroll();    /* rows shrank (a collapse) */
        int nv = vrows(), limit = row_limit();
        for (int v = scroll; v < nv; v++) {
            int s, i; if (!vrow(v, &s, &i)) break;
            int y = row_y(v);
            if (y + row_h > limit) break;                /* clipped at the Trash divider */
            if (i < 0) {                                 /* section header + disclosure caret */
                int cy = y + row_h / 2, cx = r.x + 16;   /* (opaque fill: RGB() carries no alpha) */
                if (collapsed[s]) for (int k = 0; k < 4; k++) ugfx_fill(cx - 2 + k, cy - 3 + k, 1, 7 - 2 * k, TH_MUTED);  /* > */
                else              for (int k = 0; k < 4; k++) ugfx_fill(cx - 3 + k, cy - 2 + k, 7 - 2 * k, 1, TH_MUTED);  /* v */
                ugfx_text(r.x + 26, y + (row_h - fh) / 2, sect_name[s], TH_MUTED, UGFX_TRANSPARENT);
            } else if (items[i].sect == SIDE_TAGS) draw_tag(y, items[i], v == hover_row);
            else draw_item(y, items[i].label, items[i].path, v == hover_row, -1);
        }
        if (trash_path[0]) {                             /* pinned at the bottom, set apart (§9) */
            int y = trash_y();
            ugfx_fill_a(r.x + 12, y - 4, r.w - 24, 1, ARGB(40, 150, 170, 230));
            draw_item(y, "Trash", trash_path, hover_row == 1000, G_TRASH);
        }
    }
    bool on_mouse(int x, int y, int) override {
        (void)x;
        if (trash_path[0] && y >= trash_y() && y < trash_y() + row_h) {
            if (on_pick) on_pick(ctx, trash_path);
            return true;
        }
        int s, i, v = row_at(y);
        if (v < 0 || !vrow(v, &s, &i)) return true;
        if (i < 0) {                                     /* a header click toggles its section */
            collapsed[s] = !collapsed[s];
            print("[files] sidesect "); printu((unsigned)s); printc(' ');
            printu((unsigned)collapsed[s]); print("\r\n");
            if (on_changed) on_changed(ctx);
        } else if (on_pick) on_pick(ctx, items[i].path);
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
    uint32_t mtime = 0;                               /* packed mtime (fstime.h); 0 = hide */
    uint32_t owner = INFO_UID_USER;                   /* §8: owning uid (fstat.owner)      */
    bool     locked = false;                          /* §8: system-owned & not ours (RO)  */
    unsigned dir_items = 0;                           /* §8: recursive item count (folders) */
    char     opens_with[40] = {0};                    /* §8: default app label (files; "")  */
    char freeline[28] = {0};                          /* "<n> free": volume free space footer (§6) */
    const uint32_t *icon = nullptr; int iw = 0, ih = 0, file_icon = FILEICON_FILE;
    void field(int x, int y, const char *k, const char *v) {
        int fh = ugfx_font_h();
        ugfx_text(x, y, k, TH_MUTED, UGFX_TRANSPARENT);
        ugfx_text(x, y + fh, v, TH_TEXT, UGFX_TRANSPARENT);
    }
    /* a small padlock glyph (no line primitive, so build it from fills): a filled
     * body with a keyhole + an open-topped shackle above it. Drawn at the badge's
     * baseline so it sits next to the "Read only" label. */
    static void lock_glyph(int x, int y, uint32_t c) {
        ugfx_fill_a(x + 1, y + 4, 8, 6, c);              /* body */
        ugfx_fill_a(x + 1, y,     1, 4, c);              /* shackle left  */
        ugfx_fill_a(x + 8, y,     1, 4, c);              /* shackle right */
        ugfx_fill_a(x + 2, y - 1, 6, 1, c);              /* shackle top   */
        ugfx_fill_a(x + 4, y + 6, 2, 2, ARGB(180, 0, 0, 0));  /* keyhole */
    }
    void draw() override {
        if (!visible) return;
        int fh = ugfx_font_h(), fw = ugfx_font_w(), cx = r.x + r.w / 2;
        ugfx_fill(r.x, r.y, r.w, r.h, C_DETAILS);
        ugfx_fill_a(r.x, r.y, 1, r.h, ARGB(70, 0, 0, 0));
        ugfx_set_clip(r.x, r.y, r.w, r.h);
        if (freeline[0]) {                               /* §6: volume free space, always-visible footer */
            ugfx_fill_a(r.x + 16, r.y + r.h - fh - 13, r.w - 32, 1, ARGB(40, 150, 170, 230));
            ugfx_text(r.x + 16, r.y + r.h - fh - 6, freeline, TH_MUTED, UGFX_TRANSPARENT);
        }
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
        if (locked) {                                 /* §8: read-only / system-owned badge */
            const char *rl = "Read only"; int tw = ugfx_text_w(rl);
            int bx = cx - (tw + 16) / 2, by = ty;
            lock_glyph(bx, by + 2, ARGB(230, 210, 170, 90));
            ugfx_text(bx + 14, by, rl, ARGB(230, 210, 170, 90), UGFX_TRANSPARENT);
            ty += fh + 8;
        }
        ugfx_fill_a(r.x + 16, ty, r.w - 32, 1, ARGB(40, 150, 170, 230)); ty += 12;
        field(r.x + 16, ty, "Kind", kind); ty += fh * 2 + 9;
        {                                             /* Size: bytes for files, du-walk for folders (§8) */
            char sz[48];
            if (is_file) snprintf(sz, sizeof sz, "%u bytes", size);
            else { char hb[24], cl[20]; human_bytes(size, hb, sizeof hb);
                   info_count_label(cl, sizeof cl, dir_items);
                   snprintf(sz, sizeof sz, "%s, %s", hb, cl); }
            field(r.x + 16, ty, "Size", sz); ty += fh * 2 + 9;
        }
        if (mtime) {                                  /* §8: the file's last-modified time */
            int yy, mo, dd, hh, mi; fstime_unpack(mtime, &yy, &mo, &dd, &hh, &mi);
            char mt[32]; snprintf(mt, sizeof mt, "%04d-%02d-%02d %02d:%02d", yy, mo, dd, hh, mi);
            field(r.x + 16, ty, "Modified", mt); ty += fh * 2 + 9;
        }
        field(r.x + 16, ty, "Owner", info_owner_label(owner)); ty += fh * 2 + 9;   /* §8 */
        if (is_file && opens_with[0]) {               /* §8: the default app for this type */
            field(r.x + 16, ty, "Opens with", opens_with); ty += fh * 2 + 9;
        }
        ugfx_text(r.x + 16, ty, "Where", TH_MUTED, UGFX_TRANSPARENT); ty += fh;
        int cols = (r.w - 32) / fw; if (cols < 1) cols = 1; if (cols > 62) cols = 62;
        int wl = (int)strlen(where);
        int wmax = r.y + r.h - fh - (freeline[0] ? fh + 14 : 0);   /* leave room for the free footer */
        for (int off = 0; off < wl && ty <= wmax; off += cols) {
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

/* ----------------------------------------------------------------- TabStrip */
/* Tabs (design/files-app.md §4): a strip of folder pills under the location bar, shown
 * only when there's more than one tab. Click a pill to switch; click its × to close it;
 * the trailing "+" opens a new tab. The app owns the tab *store* (unbounded, heap); the
 * strip only renders the titles handed to it each frame and reports each pill's geometry
 * for hit-testing. Pills shrink to share the width when they'd overflow. */
class TabStrip : public ui::Widget {
public:
    static const int VIS = 48;                 /* pills laid out at once (store is unbounded) */
    static const int XW  = 16;                 /* close-× hit zone on a pill's right edge      */
    const char *titles[VIS] = {};
    int  n = 0, cur = 0;
    int  px[VIS] = {}, pw[VIS] = {};           /* each pill's local x + width                  */
    int  plusx = 0, plusw = 22;
    void *ctx = nullptr;
    void (*on_select)(void *, int) = nullptr;
    void (*on_close)(void *, int)  = nullptr;
    void (*on_new)(void *)         = nullptr;
    TabStrip() { focusable = false; }
    void set(const char **t, int count, int current) {
        n = count > VIS ? VIS : count; cur = current;
        for (int i = 0; i < n; i++) titles[i] = t[i];
    }
    void relayout() {
        int gap = 4, avail = r.w - 8 - (plusw + 6);
        int natural[VIS], tot = 0;
        for (int i = 0; i < n; i++) { natural[i] = ugfx_text_w(titles[i]) + 16 + XW; tot += natural[i] + gap; }
        int cap = (n > 0 && tot > avail) ? (avail / n - gap) : 0;
        int x = 4;
        for (int i = 0; i < n; i++) {
            int pwi = natural[i];
            if (cap > 0 && pwi > cap) pwi = cap;
            if (pwi < 40) pwi = 40;
            px[i] = x; pw[i] = pwi; x += pwi + gap;
        }
        plusx = x;
    }
    void draw() override {
        if (!visible) return;
        int fh = ugfx_font_h();
        ugfx_fill(r.x, r.y, r.w, r.h, C_TOOLBAR);
        ugfx_fill_a(r.x, r.y + r.h - 1, r.w, 1, ARGB(70, 0, 0, 0));     /* bottom hairline */
        ugfx_set_clip(r.x, r.y, r.w, r.h);
        relayout();
        for (int i = 0; i < n; i++) {
            int x = r.x + px[i], wpx = pw[i], y = r.y + 3, hh = r.h - 6;
            ugfx_fill_a(x, y, wpx, hh, (i == cur) ? C_LIST : ARGB(36, 150, 170, 230));
            if (i == cur) ugfx_fill_a(x, y, wpx, 2, ARGB(220, 120, 160, 240));   /* active accent */
            ugfx_set_clip(x + 7, r.y, wpx - 7 - XW, r.h);
            ugfx_text(x + 9, r.y + (r.h - fh) / 2, titles[i], (i == cur) ? TH_TEXT : TH_MUTED, UGFX_TRANSPARENT);
            ugfx_set_clip(r.x, r.y, r.w, r.h);
            int cxx = x + wpx - XW + 5, cyy = r.y + r.h / 2;                /* the × close glyph */
            uint32_t xc = (i == cur) ? ARGB(220, 220, 228, 240) : ARGB(150, 200, 210, 230);
            vline_(cxx, cyy - 3, cxx + 5, cyy + 2, 1, xc);
            vline_(cxx + 5, cyy - 3, cxx, cyy + 2, 1, xc);
        }
        int pxx = r.x + plusx, pcy = r.y + r.h / 2, pcx = pxx + plusw / 2;   /* the + new-tab button */
        uint32_t pc = ARGB(200, 200, 210, 230);
        vline_(pcx - 4, pcy, pcx + 4, pcy, 1, pc);
        vline_(pcx, pcy - 4, pcx, pcy + 4, 1, pc);
        ugfx_clip_none();
    }
    bool on_mouse(int x, int y, int) override {
        (void)y; relayout();
        int lx = x - r.x;
        for (int i = 0; i < n; i++) {
            if (lx >= px[i] && lx < px[i] + pw[i]) {
                if (lx >= px[i] + pw[i] - XW) { if (on_close) on_close(ctx, i); }
                else if (on_select) on_select(ctx, i);
                return true;
            }
        }
        if (lx >= plusx && lx < plusx + plusw && on_new) on_new(ctx);
        return true;
    }
};

/* ---------------------------------------------------------------- SplitDecor */
/* Dual-pane chrome (design/files-app.md §4): a thin vertical splitter between the two
 * panes plus a 2px accent frame around whichever pane is active. Purely decorative -- it
 * sits on top of the two list views and never eats clicks (the app routes those). */
class SplitDecor : public ui::Widget {
public:
    ui::Rect pane[2];        /* the two pane content rects (window coords) */
    int  active = 0;
    bool on = false;
    SplitDecor() { focusable = false; }
    static void frame(ui::Rect q, uint32_t c) {
        ugfx_fill_a(q.x, q.y, q.w, 2, c); ugfx_fill_a(q.x, q.y + q.h - 2, q.w, 2, c);
        ugfx_fill_a(q.x, q.y, 2, q.h, c); ugfx_fill_a(q.x + q.w - 2, q.y, 2, q.h, c);
    }
    void draw() override {
        if (!visible || !on) return;
        int sx = (pane[0].x + pane[0].w + pane[1].x) / 2;        /* mid-gap splitter */
        ugfx_fill_a(sx, pane[0].y, 1, pane[0].h, ARGB(80, 0, 0, 0));
        frame(pane[active], ARGB(200, 120, 160, 240));
    }
    bool on_mouse(int, int, int) override { return false; }      /* never consume clicks */
};

/* ---------------------------------------------------------------- ColumnHeader */
/* The details-view header row (design/files-app.md §1): Name / Kind / Size / Date Modified.
 * Clicking a column's label sorts by it (toggling asc/desc on the active one, shown by a
 * caret); Name/Kind/Size are resized by dragging the divider on their right edge, and Date
 * fills the rest. colfit() (host-tested) owns the width math; the app reads colx[]/colw[] so
 * the rows it draws line up under the header. The app is told of a label click (on_sort) and
 * of a finished divider drag (on_resize, to persist the new widths). Focusable only so a
 * divider drag's WEV_MOUSE_DRAG packets reach on_drag; the app restores list focus after. */
class ColumnHeader : public ui::Widget {
public:
    static const int GRAB = 6;                       /* divider grab half-width in px */
    int  cw[3] = { 230, 96, 96 };                    /* user widths: Name, Kind, Size (Date fills) */
    int  colx[COLFIT_NCOL] = {0}, colw[COLFIT_NCOL] = {0};
    int  sort_key = 0, sort_desc = 0;                /* drives the active label + caret */
    int  drag_div = -1, drag_x0 = 0, drag_w0 = 0;    /* active divider drag, -1 = none */
    void *ctx = nullptr;
    void (*on_sort)(void *, int colkey) = nullptr;
    void (*on_resize)(void *) = nullptr;
    ColumnHeader() { focusable = true; }

    static int col_key(int c) { return c; }          /* col index == FSORT_* (Name0 Kind1 Size2 Date3) */
    void relayout() { colfit(r.w, cw, colx, colw); }
    void set_widths(const int w[3]) { cw[0] = w[0]; cw[1] = w[1]; cw[2] = w[2]; relayout(); }

    /* a 7px-wide triangle from stacked horizontal lines; up = ascending (apex on top) */
    static void caret(int cx, int ytop, int up, uint32_t c) {
        for (int i = 0; i < 4; i++) {
            int row = up ? i : (3 - i);
            ugfx_fill(cx - row, ytop + i, row * 2 + 1, 1, c);
        }
    }
    void draw() override {
        if (!visible) return;
        relayout();
        int fh = ugfx_font_h(), ty = r.y + (r.h - fh) / 2;
        ugfx_fill(r.x, r.y, r.w, r.h, C_TOOLBAR);
        ugfx_fill_a(r.x, r.y + r.h - 1, r.w, 1, ARGB(90, 0, 0, 0));     /* bottom hairline */
        static const char *lbl[COLFIT_NCOL] = { "Name", "Kind", "Size", "Date Modified" };
        for (int c = 0; c < COLFIT_NCOL; c++) {
            int cx = r.x + colx[c] + 10, active = (sort_key == col_key(c));
            ugfx_set_clip(r.x + colx[c], r.y, colw[c] - 2 > 0 ? colw[c] - 2 : 1, r.h);
            ugfx_text(cx, ty, lbl[c], active ? TH_TEXT : TH_MUTED, UGFX_TRANSPARENT);
            if (active) caret(cx + ugfx_text_w(lbl[c]) + 7, r.y + r.h / 2 - 2, !sort_desc, TH_ACCENT);
            ugfx_clip_none();
            if (c < 3)                               /* divider; the drag-to-resize affordance is
                                                      * the ⇔ cursor (cursor_at), not a highlight */
                ugfx_fill_a(r.x + colx[c] + colw[c], r.y + 5, 1, r.h - 10, ARGB(70, 0, 0, 0));
        }
    }
    /* the divider whose grab zone covers x, or -1 */
    int div_at(int x) {
        relayout();
        for (int c = 0; c < 3; c++) {
            int dvx = r.x + colx[c] + colw[c];
            if (x >= dvx - GRAB && x <= dvx + GRAB) return c;
        }
        return -1;
    }
    /* the resize affordance: a ⇔ cursor whenever the pointer is in a divider's grab zone */
    int cursor_at(int x, int) override { return div_at(x) >= 0 ? CUR_RESIZE_WE : CUR_ARROW; }
    /* press: a divider grab starts a resize; a label body requests a sort */
    bool on_mouse(int x, int y, int) override {
        (void)y;
        int c = div_at(x);
        if (c >= 0) { drag_div = c; drag_x0 = x; drag_w0 = cw[c]; return true; }
        int lx = x - r.x;
        for (c = 0; c < COLFIT_NCOL; c++)
            if (lx >= colx[c] && lx < colx[c] + colw[c]) { if (on_sort) on_sort(ctx, col_key(c)); break; }
        return true;
    }
    void on_drag(int x, int y) override {
        (void)y;
        if (drag_div < 0) return;
        int nw = drag_w0 + (x - drag_x0); if (nw < COLFIT_MINW) nw = COLFIT_MINW;
        cw[drag_div] = nw; relayout();
        if (win) win->invalidate();
    }
    void on_button_up() override { if (drag_div >= 0) { drag_div = -1; if (on_resize) on_resize(ctx); } }
};

/* ---------------------------------------------------------------- Breadcrumb */
/* The location bar (design/files-app.md §3): the path as clickable segment chips
 * ( / › Users › user › Documents ), each navigating to that ancestor, with a chevron
 * between. When the whole path is too wide it keeps the root + the trailing folders
 * that fit and shows a "..." chip for the elided middle. Clicking empty space (or the
 * ellipsis) switches the bar to an editable path field (handled by the app). The pure
 * path->crumbs split is pathbar_split() (host-unit-tested in t_pathbar). */
class Breadcrumb : public ui::Widget {
public:
    char path[256] = "/";
    char printed[256] = {0};                               /* last path whose crumb geometry was logged */
    struct crumb crumbs[24]; int ncr = 0;
    int slot_idx[28], slot_x[28], slot_w[28], nslot = 0;   /* drawn slots; idx -1 = ellipsis */
    int hover = -1;
    void *ctx = nullptr;
    void (*on_nav)(void *, const char *) = nullptr;        /* clicked a crumb -> navigate there  */
    void (*on_edit)(void *) = nullptr;                     /* clicked empty / ellipsis -> edit    */
    Breadcrumb() { focusable = false; }
    static const int CPAD = 7, SEPW = 14;
    void set_path(const char *p) {
        int i = 0; for (; p[i] && i < 255; i++) path[i] = p[i]; path[i] = 0;
        if (!path[0]) { path[0] = '/'; path[1] = 0; }
        ncr = pathbar_split(path, crumbs, 24);
    }
    /* lay the crumbs into drawn slots, eliding the middle when they overflow r.w */
    void rebuild() {
        int cw[24], total = 0;
        for (int i = 0; i < ncr; i++) { cw[i] = ugfx_text_w(crumbs[i].label) + 2 * CPAD; total += cw[i]; }
        total += (ncr - 1) * SEPW;
        nslot = 0;
        int x = r.x + 2;
        if (total <= r.w || ncr <= 2) {
            for (int i = 0; i < ncr; i++) { slot_idx[nslot] = i; slot_x[nslot] = x; slot_w[nslot] = cw[i]; x += cw[i] + SEPW; nslot++; }
            return;
        }
        int ellw = ugfx_text_w("...") + 2 * CPAD;
        int avail = r.w - cw[0] - SEPW - ellw - SEPW - 4;     /* room left for trailing crumbs */
        int start = ncr, used = 0;
        for (int i = ncr - 1; i >= 1; i--) { int need = cw[i] + (start < ncr ? SEPW : 0); if (used + need > avail) break; used += need; start = i; }
        if (start < 2) start = 2;                             /* always elide at least one crumb */
        slot_idx[nslot] = 0;  slot_x[nslot] = x; slot_w[nslot] = cw[0];  x += cw[0]  + SEPW; nslot++;   /* root */
        slot_idx[nslot] = -1; slot_x[nslot] = x; slot_w[nslot] = ellw;   x += ellw   + SEPW; nslot++;   /* ...  */
        for (int i = start; i < ncr; i++) { slot_idx[nslot] = i; slot_x[nslot] = x; slot_w[nslot] = cw[i]; x += cw[i] + SEPW; nslot++; }
    }
    void draw() override {
        if (!visible) return;
        rebuild();
        if (strcmp(printed, path) != 0) {                  /* log crumb click-centres once per path (e2e) */
            strncpy(printed, path, sizeof printed - 1); printed[sizeof printed - 1] = 0;
            int cy = r.y + r.h / 2;
            for (int s = 0; s < nslot; s++) if (slot_idx[s] >= 0) {
                print("[files] crumb "); printu((unsigned)(slot_x[s] + slot_w[s] / 2)); printc(' ');
                printu((unsigned)cy); printc(' '); print(crumbs[slot_idx[s]].path); print("\r\n");
            }
            int endx = nslot ? slot_x[nslot - 1] + slot_w[nslot - 1] + 16 : r.x + 8;   /* empty area past the crumbs */
            print("[files] crumbend "); printu((unsigned)endx); printc(' '); printu((unsigned)cy); print("\r\n");
        }
        ugfx_set_clip(r.x, r.y, r.w, r.h);
        int fh = ugfx_font_h(), ty = r.y + (r.h - fh) / 2;
        for (int s = 0; s < nslot; s++) {
            int idx = slot_idx[s], x = slot_x[s], w = slot_w[s];
            const char *lbl = (idx < 0) ? "..." : crumbs[idx].label;
            bool last = (idx == ncr - 1);
            if (s == hover && idx >= 0) ugfx_state_layer(x, r.y + 2, w, r.h - 4, TH_R_SM, TH_HOVER_A);
            uint32_t col = last ? TH_TEXT : (idx < 0 ? TH_MUTED : RGB(190, 205, 235));
            ugfx_text(x + CPAD, ty, lbl, col, UGFX_TRANSPARENT);
            if (s < nslot - 1) {                              /* chevron separator between chips */
                int cx = x + w + SEPW / 2, cy = r.y + r.h / 2;
                vline_(cx - 2, cy - 4, cx + 2, cy, 1, TH_MUTED);
                vline_(cx + 2, cy, cx - 2, cy + 4, 1, TH_MUTED);
            }
        }
        ugfx_clip_none();
    }
    int slot_at(int px, int py) const {
        if (py < r.y || py >= r.y + r.h) return -1;
        for (int s = 0; s < nslot; s++) if (px >= slot_x[s] && px < slot_x[s] + slot_w[s]) return s;
        return -1;
    }
    bool on_hover(int x, int y) override {
        int s = slot_at(x, y);
        if (s >= 0 && slot_idx[s] < 0) s = -1;                /* the ellipsis chip isn't a nav target */
        if (s == hover) return false;
        hover = s; return true;
    }
    void on_leave() override { hovered = false; hover = -1; }
    bool on_mouse(int x, int y, int) override {
        int s = slot_at(x, y);
        if (s >= 0 && slot_idx[s] >= 0) { if (on_nav) on_nav(ctx, crumbs[slot_idx[s]].path); return true; }
        if (on_edit) on_edit(ctx);                            /* empty space / ellipsis -> edit the path */
        return true;
    }
};

/* ----------------------------------------------------------------- IconGrid */
/* The icon (grid) view (design/files-app.md §1): the same directory model as the
 * list, rendered as a wrapping grid of large icons with a centred name below. Items
 * are addressed by the same index as the list (0 = the ".." row when present), so
 * selection + activation reuse the app's select_row/enter. The app draws each tile
 * via render_tile; the grid owns layout, scrolling, hit-testing and the zoom box. */
class IconGrid : public ui::Widget {
public:
    int count = 0, sel = -1, top = 0;                /* top = pixel scroll offset      */
    int tile_w = 100, tile_h = 90, icon_box = 56;    /* set from the zoom level         */
    int hover_i = -1;
    uint32_t bg = C_LIST, sel_bg = C_SELROW;
    void *ctx = nullptr;
    void (*render_tile)(void *, int idx, ui::Rect cell, bool sel, int icon_box) = nullptr;
    void (*on_select)(void *, int) = nullptr;
    void (*on_activate)(void *, int) = nullptr;
    ui::ScrollBar sb;
    unsigned last_tick = 0; int last_i = -1;
    IconGrid() { focusable = true; }
    int cols()    const { int c = tile_w > 0 ? r.w / tile_w : 1; return c < 1 ? 1 : c; }
    int nrows()   const { int c = cols(); return (count + c - 1) / c; }
    int content_h() const { return nrows() * tile_h; }
    int maxtop()  const { int m = content_h() - r.h; return m < 0 ? 0 : m; }
    void clamp_top() { if (top < 0) top = 0; if (top > maxtop()) top = maxtop(); }
    void ensure_visible(int i) {
        int c = cols(), row = i / c, y = row * tile_h;
        if (y < top) top = y; else if (y + tile_h > top + r.h) top = y + tile_h - r.h;
        clamp_top();
    }
    int index_at(int x, int y) const {
        if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return -1;
        int c = cols(), col = (x - r.x) / tile_w; if (col >= c) col = c - 1;
        int row = (y - r.y + top) / tile_h, i = row * c + col;
        return (i >= 0 && i < count) ? i : -1;
    }
    void draw() override {
        if (!visible) return;
        ugfx_fill(r.x, r.y, r.w, r.h, bg);
        ugfx_set_clip(r.x, r.y, r.w, r.h);
        int c = cols();
        for (int i = 0; i < count; i++) {
            int col = i % c, row = i / c;
            int x = r.x + col * tile_w, y = r.y + row * tile_h - top;
            if (y + tile_h <= r.y || y >= r.y + r.h) continue;           /* offscreen row */
            ui::Rect cell = { x, y, tile_w, tile_h };
            if (i == sel)          ugfx_rrect_a(cell.x + 4, cell.y + 3, cell.w - 8, cell.h - 6, TH_R_SM, sel_bg);
            else if (i == hover_i) ugfx_state_layer(cell.x + 4, cell.y + 3, cell.w - 8, cell.h - 6, TH_R_SM, TH_HOVER_A);
            if (render_tile) render_tile(ctx, i, cell, i == sel, icon_box);
        }
        ugfx_clip_none();
        sb.set(r, top, content_h(), r.h); sb.draw();                    /* pixel-unit scroll thumb */
    }
    bool on_mouse(int x, int y, int) override {
        if (sb.hit(x)) { sb.dragging = true; top = sb.top_from_y(y); if (win) win->invalidate(); return true; }
        int i = index_at(x, y);
        if (i < 0) return true;
        sel = i;
        if (on_select) on_select(ctx, i);
        unsigned now = win ? win->ticks : 0;
        if (i == last_i && now - last_tick < 26) { if (on_activate) on_activate(ctx, i); last_i = -1; }
        else { last_i = i; last_tick = now; }
        return true;
    }
    bool on_hover(int x, int y) override {
        int i = sb.hit(x) ? -1 : index_at(x, y);
        if (i == hover_i) return false;
        hover_i = i; return true;
    }
    void on_leave() override { hovered = false; hover_i = -1; }
    bool on_scroll(int delta) override {
        int nt = top - delta * 36; if (nt < 0) nt = 0; if (nt > maxtop()) nt = maxtop();
        if (nt == top) return false;
        top = nt; return true;
    }
    void on_drag(int x, int y) override { (void)x; if (sb.dragging) { top = sb.top_from_y(y); clamp_top(); if (win) win->invalidate(); } }
    void on_button_up() override { sb.dragging = false; }
    bool on_key(int key, bool shift) override {
        (void)shift; int c = cols();
        if (key == ui::UK_LEFT  && sel > 0)            { sel--;     ensure_visible(sel); if (on_select) on_select(ctx, sel); return true; }
        if (key == ui::UK_RIGHT && sel + 1 < count)    { sel++;     ensure_visible(sel); if (on_select) on_select(ctx, sel); return true; }
        if (key == ui::UK_UP    && sel - c >= 0)       { sel -= c;  ensure_visible(sel); if (on_select) on_select(ctx, sel); return true; }
        if (key == ui::UK_DOWN  && sel + c < count)    { sel += c;  ensure_visible(sel); if (on_select) on_select(ctx, sel); return true; }
        if (key == ui::UK_ENTER || key == '\n')        { if (sel >= 0 && on_activate) on_activate(ctx, sel); return true; }
        return false;
    }
};

/* ---------------------------------------------------------------- Popup menu */
/* Doubles as the right-click context menu and the "Open With" chooser. When open
 * its rect is the whole window so it captures the next click anywhere (modal):
 * inside an item -> pick (callback with the item's tag); a toggle item flips and
 * stays open; outside -> dismiss. */
struct PopItem { char label[40]; int kind; int tag; const uint32_t *icon; int iw, ih;
                 int checked; uint32_t dot; };   /* per-item toggle state + colour dot (§10 tags) */
enum { PK_ACTION, PK_TOGGLE, PK_SEP };
class Popup : public ui::Widget {
public:
    bool open = false, toggle = false;
    int  px = 0, py = 0, pw = 0, rowh = 0, hover_item = -1;
    PopItem it[MAXAPPS + 6]; int n = 0;
    const char *toggle_label = "";
    void *ctx = nullptr; void (*on_pick)(void *, int tag) = nullptr;
    void (*on_toggle)(void *, int tag, int on) = nullptr;   /* a PK_TOGGLE flipped (menu stays open) */
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
        p->checked = 0; p->dot = 0;
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
        /* canary so e2e can click a context-menu row deterministically: first item's
         * centre is (px+12, py+5+rowh/2), each later row +rowh below. */
        print("[files] ctxmenu "); printu((unsigned)px); printc(' '); printu((unsigned)py);
        printc(' '); printu((unsigned)rowh); printc(' '); printu((unsigned)n); print("\r\n");
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
                int on = it[i].checked;
                uint32_t bc = on ? TH_ACCENT : ARGB(120, 150, 160, 180);
                ugfx_rrect_a(px + 12, cy + (rowh - 14) / 2, 14, 14, 3, bc);
                if (on) { int bx = px + 14, by = cy + rowh / 2; vline_(bx, by, bx + 3, by + 3, 2, RGB(255,255,255)); vline_(bx + 3, by + 3, bx + 9, by - 4, 2, RGB(255,255,255)); }
                tx = px + 34;
            }
            if (it[i].dot) {                                /* §10: the tag's colour swatch */
                ugfx_rrect_aa(tx, cy + (rowh - 10) / 2, 10, 10, 5, it[i].dot);
                tx += 16;
            }
            ugfx_text(tx, cy + (rowh - fh) / 2, it[i].label, TH_TEXT, UGFX_TRANSPARENT);
            cy += rowh;
        }
    }
    bool on_mouse(int x, int y, int) override {
        if (!open) return true;
        int hit = item_at(x, y);
        if (hit >= 0 && it[hit].kind == PK_TOGGLE) {                          /* flips, stays open */
            it[hit].checked = !it[hit].checked;
            toggle = it[hit].checked;             /* legacy mirror: Open With's single toggle */
            if (on_toggle) on_toggle(ctx, it[hit].tag, it[hit].checked);
            return true;
        }
        int tag = (hit >= 0) ? it[hit].tag : -1;
        dismiss();
        if (on_pick) on_pick(ctx, tag);
        return true;
    }
};
