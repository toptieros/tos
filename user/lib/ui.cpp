/* tOS UI toolkit implementation (see ui.h). Styling comes from theme.h, drawing
 * from the C ugfx backend. */
#include "ui.h"
#include "theme.h"
#include "textutil.h"

namespace ui {

static int text_cy(Rect r) { return r.y + (r.h - ugfx_font_h()) / 2; }   /* vertically centre text in a row */

/* -------------------------------------------------------------------- Label */
Label::Label() { fg = TH_TEXT; }
void Label::draw() {
    if (!visible || !text) return;
    int tx = r.x;
    if (align == 1) tx = r.x + (r.w - ugfx_text_w(text)) / 2;
    else if (align == 2) tx = r.x + r.w - ugfx_text_w(text);
    ugfx_text(tx, text_cy(r), text, fg, UGFX_TRANSPARENT);
}

/* ------------------------------------------------------------------- Button */
Button::Button() {}
void Button::draw() {
    if (!visible) return;
    int rad = TH_R_SM;
    ugfx_rrect_aa(r.x, r.y, r.w, r.h, rad, enabled ? TH_SURF_3 : TH_SURF_2);
    if (enabled && hovered) ugfx_state_layer(r.x, r.y, r.w, r.h, rad, TH_HOVER_A);  /* hover lift */
    ugfx_rrect_border(r.x, r.y, r.w, r.h, rad, 1, TH_BORDER);                       /* crisp edge */
    ugfx_fill_a(r.x + rad, r.y + 1, r.w - 2 * rad, 1, ARGB(48, 255, 255, 255));     /* top sheen  */
    int tx = r.x + (r.w - ugfx_text_w(text)) / 2;
    ugfx_text(tx, text_cy(r), text, enabled ? TH_TEXT : TH_MUTED, UGFX_TRANSPARENT);
}
bool Button::on_mouse(int x, int y, int btn) {
    (void)x; (void)y; (void)btn;
    if (enabled && on_click) on_click(ctx);
    return true;
}

/* -------------------------------------------------------------------- Panel */
Panel::Panel() { color = TH_CHROME; sep = ARGB(60, 150, 170, 230); }
void Panel::draw() {
    if (!visible) return;
    ugfx_fill(r.x, r.y, r.w, r.h, color);
    if (sep_bottom) ugfx_fill_a(r.x, r.y + r.h - 1, r.w, 1, sep);
}

/* ----------------------------------------------------------------- TextView */
TextView::TextView() { bg = RGB(26, 30, 40); fg = TH_TEXT; }
void TextView::draw() {
    if (!visible) return;
    ugfx_fill(r.x, r.y, r.w, r.h, bg);
    if (!text) return;
    int fw = ugfx_font_w(), fh = ugfx_font_h();
    int cols = fw ? (r.w - 12) / fw : 0;
    int x0 = r.x + 6, y = r.y + 4, col = 0;
    ugfx_set_clip(r.x, r.y, r.w, r.h);
    for (int i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n' || (cols > 0 && col >= cols)) { y += fh; col = 0; if (c == '\n') continue; }
        if (y > r.y + r.h - fh) break;
        if (c == '\r') continue;
        if (c == '\t') { col = (col + 4) & ~3; continue; }
        ugfx_char(x0 + col * fw, y, c, fg, bg);
        col++;
    }
    ugfx_clip_none();
}

/* ----------------------------------------------------------------- ListView */
ListView::ListView() { bg = TH_SURF_0; sel_bg = ARGB(235, 96, 152, 252); focusable = true; }
void ListView::ensure_visible(int i) {
    if (i < top) top = i;
    int vis = rows_visible();
    if (vis > 0 && i >= top + vis) top = i - vis + 1;
    if (top < 0) top = 0;
}
void ListView::draw() {
    if (!visible) return;
    ugfx_fill(r.x, r.y, r.w, r.h, bg);
    int vis = rows_visible();
    for (int k = 0; k < vis; k++) {
        int i = top + k;
        if (i >= count) break;
        Rect cell = { r.x, r.y + k * row_h, r.w, row_h };
        /* selection = an inset accent pill (Finder-style), under the row content so
         * its text reads on top; hover = a faint state layer painted OVER the content
         * (incl. any zebra fill the app draws), as a state layer should sit on top. */
        if (i == sel)
            ugfx_rrect_a(cell.x + TH_SP_XS, cell.y + 1, cell.w - 2 * TH_SP_XS, cell.h - 2, TH_R_SM, sel_bg);
        if (render_row) render_row(ctx, i, cell, i == sel);
        if (i != sel && i == hover_row)
            ugfx_state_layer(cell.x + TH_SP_XS, cell.y + 1, cell.w - 2 * TH_SP_XS, cell.h - 2, TH_R_SM, TH_HOVER_A);
    }
    sb.set(r, top, count, rows_visible()); sb.draw();        /* the shared scroll thumb (Files / Spotlight) */
}
bool ListView::on_mouse(int x, int y, int btn) {
    (void)btn;
    if (sb.hit(x)) { sb.dragging = true; top = sb.top_from_y(y); if (win) win->invalidate(); return true; }  /* scroll track */
    int row = top + (y - r.y) / row_h;
    if (row < 0 || row >= count) return true;
    sel = row;
    if (on_select) on_select(ctx, row);
    unsigned now = win ? win->ticks : 0;
    if (row == last_row && now - last_tick < 26) {       /* double-click -> activate */
        if (on_activate) on_activate(ctx, row);
        last_row = -1;
    } else {
        last_row = row; last_tick = now;
    }
    return true;
}
bool ListView::on_hover(int x, int y) {
    (void)x;
    int row = top + (y - r.y) / row_h;
    if (row < top || row >= count || row >= top + rows_visible()) row = -1;
    if (row == hover_row) return false;
    hover_row = row;
    return true;
}
void ListView::on_leave() { hovered = false; hover_row = -1; }
bool ListView::on_scroll(int delta) {
    int vis = rows_visible(), maxtop = count - vis; if (maxtop < 0) maxtop = 0;
    int nt = top - delta * 3; if (nt < 0) nt = 0; if (nt > maxtop) nt = maxtop;   /* wheel up -> earlier rows */
    if (nt == top) return false;
    top = nt; return true;
}
void ListView::on_drag(int x, int y) {
    (void)x;
    if (sb.dragging) { int nt = sb.top_from_y(y); if (nt != top) { top = nt; if (win) win->invalidate(); } }
}
bool ListView::on_key(int key, bool shift) {
    (void)shift;
    if (key == UK_UP)    { if (sel > 0) { sel--; ensure_visible(sel); if (on_select) on_select(ctx, sel); } return true; }
    if (key == UK_DOWN)  { if (sel + 1 < count) { sel++; ensure_visible(sel); if (on_select) on_select(ctx, sel); } return true; }
    if (key == UK_ENTER || key == '\n') { if (sel >= 0 && on_activate) on_activate(ctx, sel); return true; }
    return false;
}

/* ---------------------------------------------------------------- TextField */
#define TF_PAD 6
#define TF_SEL RGB(54, 88, 144)
#define TF_BLINK 32              /* caret on/off period, in event-loop ticks (~0.5s) */
#define TF_DBLCLICK 24           /* max ticks between presses to count as a double-click */

/* a "word" character for double-click select + Ctrl+arrow word jumps */
static inline bool tf_wordch(char c) { return tu_wordch(c); }  /* pure; unit-tested in tests/unit */

TextField::TextField() { bg = TH_SURF_0; fg = TH_TEXT; focusable = true; }
TextField::~TextField() { if (buf) free(buf); }

void TextField::ensure(int need) {
    if (need <= cap) return;
    int nc = cap ? cap : 32;
    while (nc < need) nc *= 2;
    buf = (char *)realloc(buf, (size_t)nc);
    cap = nc;
}
void TextField::changed() { if (on_change) on_change(ctx); if (win) win->invalidate(); }
void TextField::set_text(const char *s) {
    len = 0; for (const char *p = s; p && *p; p++) len++;
    ensure(len + 1);
    for (int i = 0; i < len; i++) buf[i] = s[i];
    if (buf) buf[len] = 0;
    caret = len; anchor = -1; top = hoff = 0;
    changed();
}
void TextField::ins(const char *s, int n) {
    if (has_sel()) { int a, b; sel_bounds(a, b); del_range(a, b); }
    ensure(len + n + 1);
    for (int i = len; i >= caret; i--) buf[i + n] = buf[i];   /* shift tail (incl NUL) right */
    for (int i = 0; i < n; i++) buf[caret + i] = s[i];
    len += n; caret += n; buf[len] = 0; anchor = -1;
    changed();
}
void TextField::del_range(int a, int b) {
    if (a < 0) a = 0; if (b > len) b = len; if (a >= b) return;
    int n = b - a;
    for (int i = b; i <= len; i++) buf[i - n] = buf[i];
    len -= n; if (caret > b) caret -= n; else if (caret > a) caret = a;
    buf[len] = 0; anchor = -1;
    changed();
}
void TextField::sel_bounds(int &a, int &b) const {
    a = anchor < caret ? anchor : caret;
    b = anchor < caret ? caret : anchor;
}
void TextField::drop_sel_if(bool shift) {
    if (shift) { if (anchor < 0) anchor = caret; }
    else anchor = -1;
}
void TextField::copy_sel(bool cut) {
    if (!has_sel()) return;
    int a, b; sel_bounds(a, b);
    clip_put(CLIP_TEXT, "text", buf + a, b - a);
    if (cut) { caret = a; del_range(a, b); }
}
void TextField::paste() {
    int idx = clip_active(-1);
    if (idx < 0) return;
    char tmp[1024];
    int n = clip_get(idx, tmp, sizeof tmp);
    if (n > 0) {
        if (!multiline) for (int i = 0; i < n; i++) if (tmp[i] == '\n' || tmp[i] == '\r') tmp[i] = ' ';
        ins(tmp, n);
    }
}
/* visual (row,col) of a buffer index, honouring char-wrap (multiline only). */
static void tf_posof(const char *buf, int len, int idx, int cols, bool ml, int &vr, int &vc) {
    int r = 0, c = 0;
    for (int i = 0; i < idx && i < len; i++) {
        char ch = buf[i];
        if (ml && ch == '\n') { r++; c = 0; }
        else { c++; if (ml && c >= cols) { r++; c = 0; } }
    }
    vr = r; vc = c;
}
int TextField::index_at(int px, int py) {
    int fw = ugfx_font_w(), fh = ugfx_font_h(), cols = cols_cache > 0 ? cols_cache : 1, rowh = fh + 2;
    int tcol = (px - r.x - TF_PAD + fw / 2) / fw; if (tcol < 0) tcol = 0;
    if (!multiline) { int idx = hoff + tcol; if (idx < 0) idx = 0; if (idx > len) idx = len; return idx; }
    int trow = top + (py - r.y - TF_PAD) / rowh; if (trow < 0) trow = 0;
    int vr = 0, vc = 0;
    for (int i = 0; i <= len; i++) {
        if (vr == trow && vc == tcol) return i;
        if (vr > trow) return i > 0 ? i - 1 : 0;
        if (i < len) {
            char ch = buf[i];
            if (ch == '\n') { if (vr == trow) return i; vr++; vc = 0; }
            else { vc++; if (vc >= cols) { if (vr == trow) return i + 1; vr++; vc = 0; } }
        }
    }
    return len;
}
bool TextField::on_mouse(int x, int y, int btn) {
    (void)btn;
    if (multiline && sb.hit(x)) { sb.dragging = true; sb_set_top_from_y(y); return true; }  /* press on the scroll track */
    int idx = index_at(x, y);
    unsigned now = win ? win->ticks : 0;
    if (last_click_i >= 0 && (now - last_click_t) < TF_DBLCLICK &&
        idx >= last_click_i - 1 && idx <= last_click_i + 1) {
        select_word(idx);                 /* a quick second press near the first: select the word */
    } else {
        caret = idx; anchor = idx;        /* start a possible drag-selection */
    }
    last_click_t = now; last_click_i = idx;
    if (win) win->invalidate();
    return true;
}
void TextField::select_word(int idx) {
    if (!buf || len == 0) { anchor = -1; caret = 0; return; }
    if (idx > len) idx = len;
    int a, b;
    if (idx < len && tf_wordch(buf[idx])) {           /* on a word char: take the whole run        */
        a = b = idx;
        while (a > 0   && tf_wordch(buf[a - 1])) a--;
        while (b < len && tf_wordch(buf[b]))     b++;
    } else if (idx > 0 && tf_wordch(buf[idx - 1])) {  /* just past a word: take the word to the left */
        a = b = idx;
        while (a > 0 && tf_wordch(buf[a - 1])) a--;
    } else { anchor = idx; caret = idx; return; }     /* not on a word: just place the caret         */
    anchor = a; caret = b;
    printf("[ui] word %d %d\r\n", a, b);              /* double-click selection (also drives the test) */
}
int TextField::word_prev(int i) const { return buf ? tu_word_prev(buf, i) : 0; }
int TextField::word_next(int i) const { return buf ? tu_word_next(buf, len, i) : 0; }
void TextField::drag_to(int x, int y) {
    int ni = index_at(x, y);
    if (ni != caret) { caret = ni; if (win) win->invalidate(); }
}
/* Map a pointer y on the track to a scroll position (via the shared ScrollBar),
 * clamp, and emit telemetry the test reads. */
void TextField::sb_set_top_from_y(int py) {
    int nt = sb.top_from_y(py);
    if (nt != top) { top = nt; if (win) win->invalidate(); }
    printf("[ui] sbtop=%d\r\n", top);
}
bool TextField::on_key(int key, bool shift) {  /* shift => extend the selection (anchor kept) */
    if (key == 0x03) { copy_sel(false); return true; }            /* Ctrl+C */
    if (key == 0x18) { copy_sel(true);  return true; }            /* Ctrl+X */
    if (key == 0x16) { paste();         return true; }            /* Ctrl+V */
    if (key == 0x01) { anchor = 0; caret = len; if (win) win->invalidate(); return true; }  /* Ctrl+A */
    if (key >= 0x20 && key < 0x7f) { char c = (char)key; ins(&c, 1); return true; }
    if (key == UK_ENTER || key == '\n') {     /* the keyboard sends '\n' (10) for Return */
        if (multiline) { char c = '\n'; ins(&c, 1); }
        else if (on_submit) on_submit(ctx);
        return true;
    }
    if (key == UK_BACK) {
        if (has_sel()) { int a, b; sel_bounds(a, b); caret = a; del_range(a, b); }
        else if (caret > 0) { del_range(caret - 1, caret); }
        return true;
    }
    if (key == UK_DEL) {
        if (has_sel()) { int a, b; sel_bounds(a, b); caret = a; del_range(a, b); }
        else if (caret < len) del_range(caret, caret + 1);
        return true;
    }
    /* report a shift-extended selection (drives the test + is handy telemetry) */
    #define SHSEL() do { if (shift && has_sel()) { int _a, _b; sel_bounds(_a, _b); printf("[ui] shsel %d %d\r\n", _a, _b); } } while (0)
    if (key == UK_LEFT)  { drop_sel_if(shift); if (caret > 0) caret--;   SHSEL(); if (win) win->invalidate(); return true; }
    if (key == UK_RIGHT) { drop_sel_if(shift); if (caret < len) caret++; SHSEL(); if (win) win->invalidate(); return true; }
    if (key == UK_WORD_LEFT)  { drop_sel_if(shift); caret = word_prev(caret); SHSEL(); printf("[ui] wjump %d\r\n", caret); if (win) win->invalidate(); return true; }
    if (key == UK_WORD_RIGHT) { drop_sel_if(shift); caret = word_next(caret); SHSEL(); printf("[ui] wjump %d\r\n", caret); if (win) win->invalidate(); return true; }
    if (key == 0x17)          { del_range(word_prev(caret), caret); printf("[ui] wdel %d\r\n", caret); return true; }  /* Ctrl+Backspace */
    if (key == UK_WORD_DEL)   { del_range(caret, word_next(caret)); printf("[ui] wdel %d\r\n", caret); return true; }  /* Ctrl+Delete    */
    if (key == UK_HOME)  { drop_sel_if(shift); while (caret > 0 && buf[caret - 1] != '\n') caret--; SHSEL(); if (win) win->invalidate(); return true; }
    if (key == UK_END)   { drop_sel_if(shift); while (caret < len && buf[caret] != '\n') caret++; SHSEL(); if (win) win->invalidate(); return true; }
    if ((key == UK_UP || key == UK_DOWN) && multiline) {
        int vr, vc; tf_posof(buf, len, caret, cols_cache, true, vr, vc);
        int tr = key == UK_UP ? vr - 1 : vr + 1; if (tr < 0) return true;
        /* find the index at (tr, vc) */
        int r0 = 0, c0 = 0, found = len;
        for (int i = 0; i <= len; i++) {
            if (r0 == tr && c0 == vc) { found = i; break; }
            if (r0 > tr) { found = i > 0 ? i - 1 : 0; break; }
            if (i < len) { char ch = buf[i];
                if (ch == '\n') { if (r0 == tr) { found = i; break; } r0++; c0 = 0; }
                else { c0++; if (c0 >= cols_cache) { if (r0 == tr) { found = i + 1; break; } r0++; c0 = 0; } } }
        }
        drop_sel_if(shift); caret = found; SHSEL(); if (win) win->invalidate(); return true;
    }
    #undef SHSEL
    return false;
}
void TextField::draw() {
    if (!visible) return;
    int fw = ugfx_font_w(), fh = ugfx_font_h();
    int cols = (r.w - 2 * TF_PAD) / fw; if (cols < 1) cols = 1;
    cols_cache = cols;
    int rowh = fh + 2;
    int visrows = (r.h - 2 * TF_PAD) / rowh; if (visrows < 1) visrows = 1;
    bool foc = win && win->focus == this;

    ugfx_rrect_aa(r.x, r.y, r.w, r.h, radius, bg);       /* rounded well (square corners read as "boxy") */
    ugfx_rrect_border(r.x, r.y, r.w, r.h, radius, 1, foc ? TH_ACCENT : TH_BORDER);
    /* single-line: vertically centre the text so a taller (pill) field looks right */
    int sl_y = r.y + (r.h - fh) / 2;

    /* Follow the caret only when it MOVED (typing/arrows) so a wheel scroll is free;
     * always clamp the viewport to the content. */
    int cvr = 0, cvc = 0; tf_posof(buf, len, caret, cols, multiline, cvr, cvc);
    int total_rows = 1;
    if (multiline) {
        int er = 0, ec = 0; tf_posof(buf, len, len, cols, true, er, ec); total_rows = er + 1;
        if (caret != last_caret) {
            if (cvr < top) top = cvr;
            if (cvr >= top + visrows) top = cvr - visrows + 1;
        }
        int maxtop = total_rows - visrows; if (maxtop < 0) maxtop = 0;
        if (top > maxtop) top = maxtop; if (top < 0) top = 0;
        last_caret = caret;
    }
    else { if (cvc < hoff) hoff = cvc; if (cvc > hoff + cols - 1) hoff = cvc - (cols - 1); if (hoff < 0) hoff = 0; }

    int sa = -1, selb = -1; if (has_sel()) sel_bounds(sa, selb);   /* selection bounds (sb is the member) */
    ugfx_set_clip(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    int vr = 0, vc = 0, caret_px = -1, caret_py = -1;
    for (int i = 0; i <= len; i++) {
        int dx, dy, drow;
        if (multiline) { drow = vr - top; dx = r.x + TF_PAD + vc * fw; dy = r.y + TF_PAD + drow * rowh; }
        else           { drow = 0;        dx = r.x + TF_PAD + (vc - hoff) * fw; dy = sl_y; }
        bool onscreen = multiline ? (drow >= 0 && drow < visrows) : (vc >= hoff && vc < hoff + cols);
        if (i == caret && onscreen) { caret_px = dx; caret_py = dy; }
        if (i == len) break;
        char ch = buf[i];
        if (multiline && ch == '\n') { vr++; vc = 0; continue; }
        char gc = (ch == '\n' || ch == '\t') ? ' ' : ch;
        if (onscreen) {
            uint32_t cbg = (i >= sa && i < selb) ? TF_SEL : bg;
            if (cbg != bg) ugfx_fill(dx, dy, fw, fh, cbg);
            ugfx_char(dx, dy, gc, fg, cbg);
        }
        vc++;
        if (multiline && vc >= cols) { vr++; vc = 0; }
    }
    if (foc && caret_px >= 0 && (((win ? win->ticks : 0) / TF_BLINK) & 1) == 0)
        ugfx_fill(caret_px, caret_py, 2, fh, TH_ACCENT);     /* blinking caret (Window pulses the repaint) */
    if (multiline) { sb.set(r, top, total_rows, visrows); sb.draw(); }   /* the shared scroll thumb */
    ugfx_clip_none();
}
bool TextField::on_scroll(int delta) {
    if (!multiline) return false;
    int nt = top - delta * 3;                                /* wheel up -> earlier lines */
    if (nt < 0) nt = 0;
    if (nt == top) return false;                             /* draw() clamps the lower bound to content */
    top = nt; if (win) win->invalidate(); return true;
}

/* ------------------------------------------------------------------- Window */
Window::Window() { bg = TH_CHROME; }
bool Window::create(int cw, int ch, const char *t) {
    struct wininfo wi;
    wi.w = (uint32_t)cw; wi.h = (uint32_t)ch;
    wi.flags = ((popup || overlay) ? WIN_POPUP : 0) | (overlay ? WIN_OVERLAY : 0);
    int i = 0; for (; t && t[i] && i < 31; i++) { title[i] = t[i]; wi.title[i] = t[i]; }
    title[i] = 0; wi.title[i] = 0;
    id = win_create(&wi);
    if (id < 0) return false;
    surf = (uint32_t *)wi.vaddr; w = (int)wi.w; h = (int)wi.h;
    return true;
}
void Window::add(Widget *c) {
    c->win = this;
    kids.push(c);
    if (!focus) focus = c;
}
void Window::redraw() {
    ugfx_set_target(surf, w, h, w);
    ugfx_clip_none();
    ugfx_clear(bg);
    for (int i = 0; i < kids.size(); i++) if (kids[i]->visible) kids[i]->draw();
    win_present(id);
}
void Window::dispatch_mouse(int x, int y, int btn) {
    if (btn & 2) { on_context(x, y); dirty = true; return; }   /* right-click -> context menu */
    for (int i = kids.size() - 1; i >= 0; i--) {          /* topmost (last-added) first */
        Widget *c = kids[i];
        if (c->visible && c->r.has(x, y)) {
            if (c->focusable) focus = c;
            c->on_mouse(x, y, btn);
            dirty = true;
            return;
        }
    }
}
/* A pointer move (no button) from the compositor: track which widget is under the
 * cursor so it can draw a hover state layer. The compositor sends an out-of-range
 * coordinate (>= 0xfff) when the pointer leaves the window, which clears the hover. */
void Window::dispatch_hover(int x, int y) {
    Widget *t = nullptr;
    if (x < 0xfff && y < 0xfff)
        for (int i = kids.size() - 1; i >= 0; i--) {          /* topmost first */
            Widget *c = kids[i];
            if (c->visible && c->r.has(x, y)) { t = c; break; }
        }
    if (t != hot) {
        if (hot) hot->on_leave();
        hot = t;
        if (hot) hot->hovered = true;
        dirty = true;
    }
    if (hot && hot->on_hover(x, y)) dirty = true;             /* sub-element hover (rows) */
}
void Window::dispatch_scroll(int x, int y, int delta) {
    for (int i = kids.size() - 1; i >= 0; i--) {              /* topmost widget under the cursor */
        Widget *c = kids[i];
        if (c->visible && c->r.has(x, y)) { if (c->on_scroll(delta)) dirty = true; return; }
    }
    on_scroll(delta);                                         /* nothing scrollable -> app-level */
}
void Window::feed_key(int b) {
    int key = -1;
    bool shift = false;                                /* set from the CSI modifier param (Shift => bit 0) */
    if (esc == 0) {
        if (b == 27) { esc = 1; return; }
        key = b;
    } else if (esc == 1) {
        if (b == '[') { esc = 2; csi_n = 0; return; }
        esc = 0; key = b;
    } else { /* esc == 2: collect CSI parameters (digits + ';'), then a final byte */
        if ((b >= '0' && b <= '9') || b == ';') {
            if (csi_n < (int)sizeof(csi) - 1) csi[csi_n++] = (char)b;
            return;
        }
        esc = 0; csi[csi_n] = 0;
        int mod = 0;                                   /* xterm modifier param after ';' (Ctrl => 5) */
        for (int i = 0; i < csi_n; i++) if (csi[i] == ';') {
            for (int j = i + 1; j < csi_n && csi[j] >= '0' && csi[j] <= '9'; j++) mod = mod * 10 + (csi[j] - '0');
            break;
        }
        bool ctrl = mod >= 2 && ((mod - 1) & 4);       /* (param-1) is a bitmask: Ctrl = bit 2 (4) */
        shift     = mod >= 2 && ((mod - 1) & 1);        /* Shift = bit 0 (xterm param 2 = Shift)     */
        switch (b) {
        case 'A': key = UK_UP;    break;
        case 'B': key = UK_DOWN;  break;
        case 'C': key = ctrl ? UK_WORD_RIGHT : UK_RIGHT; break;
        case 'D': key = ctrl ? UK_WORD_LEFT  : UK_LEFT;  break;
        case 'H': key = UK_HOME;  break;
        case 'F': key = UK_END;   break;
        case '~': if (csi_n >= 1 && csi[0] == '3') key = ctrl ? UK_WORD_DEL : UK_DEL;  /* ESC[3~ Del; ESC[3;5~ Ctrl+Del */
                  else return;                                    /* Insert/PgUp/PgDn: ignored for now */
                  break;
        default:  return;
        }
    }
    if (key < 0) return;
    bool handled = focus ? focus->on_key(key, shift) : false;
    if (!handled) on_key(key);                       /* unconsumed keys -> app-level */
    dirty = true;
}
int Window::run() {
    redraw(); dirty = false;
    while (running) {
        struct winevent ev;
        while (win_poll(id, &ev)) {
            switch (ev.type) {
            case WEV_KEY:   feed_key((int)(ev.a & 0xff)); break;
            case WEV_MOUSE: {
                int mx = (int)WEV_MOUSE_X(ev.a), my = (int)WEV_MOUSE_Y(ev.a), mb = (int)WEV_MOUSE_BTN(ev.a);
                if (mb == 0)                  { if (focus) focus->on_button_up(); dispatch_hover(mx, my); }  /* move/release -> end drag, hover */
                else if (mb & WEV_MOUSE_DRAG)   {                                /* drag-select */
                    if (focus) focus->on_drag(mx, my);       /* widget-level (TextField selection)   */
                    on_drag(mx, my, mb & ~WEV_MOUSE_DRAG);   /* window-level (Files rubber-band)      */
                    dirty = true;
                }
                else                            dispatch_mouse(mx, my, mb);
                break;
            }
            case WEV_RESIZE: {
                int nw = (int)(ev.a >> 16), nh = (int)(ev.a & 0xffff);
                if (nw > 0 && nh > 0 && win_resize(id, nw, nh) == 0) {
                    w = nw; h = nh;                       /* win_resize keeps the same vaddr */
                    on_resize(w, h);
                    dirty = true;
                }
                break;
            }
            case WEV_SCROLL: {
                int mx = (int)WEV_MOUSE_X(ev.a), my = (int)WEV_MOUSE_Y(ev.a);
                int d = (int)WEV_MOUSE_BTN(ev.a); if (d > 127) d -= 256;   /* signed 8-bit delta */
                dispatch_scroll(mx, my, d);
                break;
            }
            case WEV_NAV:   on_nav((int)ev.a); dirty = true; break;
            case WEV_CLOSE: running = false; break;
            }
        }
        ticks++;
        if (focus && focus->shows_caret() && (ticks % TF_BLINK) == 0)
            dirty = true;                 /* pulse a repaint so the caret keeps blinking while idle */
        if (dirty) { redraw(); dirty = false; }
        sleep_ms(15);
    }
    win_close(id);
    return 0;
}

} // namespace ui
