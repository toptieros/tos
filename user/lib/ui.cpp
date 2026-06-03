/* tOS UI toolkit implementation (see ui.h). Styling comes from theme.h, drawing
 * from the C ugfx backend. */
#include "ui.h"
#include "theme.h"
#include "textutil.h"
#include "editlog.h"
#include "perm.h"        /* tos_may_write / TOS_UID_* -- FileDialog greys system-owned folders */

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
Button::Button() { icon_tint = TH_TEXT; value_fg = TH_MUTED; }
void Button::draw() {
    if (!visible) return;
    int rad = TH_R_SM;
    ugfx_rrect_aa(r.x, r.y, r.w, r.h, rad, enabled ? TH_SURF_3 : TH_SURF_2);
    if (enabled && hovered) ugfx_state_layer(r.x, r.y, r.w, r.h, rad, TH_HOVER_A);  /* hover lift */
    ugfx_rrect_border(r.x, r.y, r.w, r.h, rad, 1, TH_BORDER);                       /* crisp edge */
    ugfx_fill_a(r.x + rad, r.y + 1, r.w - 2 * rad, 1, ARGB(48, 255, 255, 255));     /* top sheen  */
    uint32_t fg = enabled ? TH_TEXT : TH_MUTED;
    if (icon || value) {                          /* settings-row: [icon] label .... value */
        int pad = 12, lx = r.x + pad;
        if (icon && icon_sz) {
            ugfx_blit_tint(lx, r.y + (r.h - icon_sz) / 2, icon_sz, icon_sz, icon, enabled ? icon_tint : TH_MUTED);
            lx += icon_sz + 12;
        }
        ugfx_text(lx, text_cy(r), text, fg, UGFX_TRANSPARENT);
        if (value) {
            int vx = r.x + r.w - pad - ugfx_text_w(value);
            ugfx_text(vx, text_cy(r), value, enabled ? value_fg : TH_MUTED, UGFX_TRANSPARENT);
        }
    } else {
        int tx = r.x + (r.w - ugfx_text_w(text)) / 2;
        ugfx_text(tx, text_cy(r), text, fg, UGFX_TRANSPARENT);
    }
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
TextField::~TextField() {
    if (buf) free(buf);
    hist_clear(ust, un); hist_clear(rst, rn);
    if (ust) free(ust); if (rst) free(rst);
}

/* --- edit history (undo/redo) ------------------------------------------------
 * Two bounded stacks of {op, pos, span text, caret-before}. ins()/del_range()
 * record() every mutation; undo/redo pop one stack and apply the inverse, which
 * record() routes onto the opposite stack (so the chain is reversible). Runs of
 * single-char typing or backspacing coalesce into the top record. */
void TextField::hist_init() {
    if (!ust) { ust = (Edit *)malloc(sizeof(Edit) * UNDO_MAX); un = 0; }
    if (!rst) { rst = (Edit *)malloc(sizeof(Edit) * UNDO_MAX); rn = 0; }
}
void TextField::hist_clear(Edit *stk, int &n) {
    if (stk) for (int i = 0; i < n; i++) if (stk[i].txt) free(stk[i].txt);
    n = 0;
}
void TextField::hist_push(Edit *stk, int &n, uint8_t op, int pos, const char *txt, int cnt, int caret0) {
    char *cp = (char *)malloc(cnt > 0 ? cnt : 1);
    memcpy(cp, txt, cnt);
    if (n == UNDO_MAX) { if (stk[0].txt) free(stk[0].txt); memmove(stk, stk + 1, sizeof(Edit) * (UNDO_MAX - 1)); n--; }
    stk[n] = { op, pos, cnt, caret0, cp }; n++;
}
/* Merge a single-char edit into the top undo record per editlog.h's pure rule
 * (APPEND grows the right end, PREPEND the left). caret0 is left at the run's
 * start, so one Ctrl+Z drops the whole run. */
bool TextField::hist_coalesce(uint8_t op, int pos, const char *txt, int cnt) {
    if (cnt != 1 || un == 0) return false;
    Edit &t = ust[un - 1];
    int kind = el_coalesce_kind(t.op, t.pos, t.n, t.txt[0], t.txt[t.n - 1], op, pos, txt[0]);
    if (kind == EL_NONE) return false;
    char *nt = (char *)realloc(t.txt, t.n + 1); if (!nt) return false;
    t.txt = nt;
    if (kind == EL_PREPEND) { for (int i = t.n; i > 0; i--) t.txt[i] = t.txt[i - 1]; t.txt[0] = txt[0]; t.pos = pos; }
    else                    { t.txt[t.n] = txt[0]; }
    t.n++;
    return true;
}
void TextField::record(uint8_t op, int pos, const char *txt, int cnt, int caret0) {
    if (cnt <= 0) return;
    hist_init();
    if (!applying) {
        if (!brk && hist_coalesce(op, pos, txt, cnt)) { hist_clear(rst, rn); return; }
        hist_push(ust, un, op, pos, txt, cnt, caret0);
        hist_clear(rst, rn);                              /* a fresh edit invalidates redo */
        brk = false;
        return;
    }
    hist_push(cur_stk, *cur_n, op, pos, txt, cnt, caret0);
}
/* Pop one record off `from`, apply its inverse (which record() pushes onto `to`),
 * and restore the caret. Shared by undo() (ust->rst) and redo() (rst->ust). */
bool TextField::pop_apply(Edit *from, int &fn, Edit *to, int &tn) {
    if (fn == 0) return false;
    Edit e = from[--fn];                                  /* take ownership of e.txt */
    anchor = -1;
    applying = true; cur_stk = to; cur_n = &tn;
    if (e.op == 0) { caret = e.pos + e.n; del_range(e.pos, e.pos + e.n); }  /* undo an insert */
    else           { caret = e.pos;       ins(e.txt, e.n); }                /* undo a delete  */
    applying = false;
    caret = e.caret0;
    if (e.txt) free(e.txt);
    brk = true;
    if (win) win->invalidate();
    return true;
}
bool TextField::undo() { return pop_apply(ust, un, rst, rn); }
bool TextField::redo() { return pop_apply(rst, rn, ust, un); }

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
    hist_clear(ust, un); hist_clear(rst, rn); brk = true;  /* loading content resets the history */
    changed();
}
void TextField::ins(const char *s, int n) {
    if (has_sel()) { int a, b; sel_bounds(a, b); del_range(a, b); }
    int c0 = caret;                                       /* caret before the insert (undo target) */
    ensure(len + n + 1);
    for (int i = len; i >= caret; i--) buf[i + n] = buf[i];   /* shift tail (incl NUL) right */
    for (int i = 0; i < n; i++) buf[caret + i] = s[i];
    len += n; caret += n; buf[len] = 0; anchor = -1;
    record(0, c0, s, n, c0);
    changed();
}
void TextField::del_range(int a, int b) {
    if (a < 0) a = 0; if (b > len) b = len; if (a >= b) return;
    record(1, a, buf + a, b - a, caret);                 /* capture the span before it's gone */
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
    if (multiline && sb.hit(x)) { sb.dragging = true; sb_set_top_from_y(y); return true; }  /* press on the scroll track */
    brk = true;                            /* repositioning the caret ends the typing-coalesce run */
    int idx = index_at(x, y);
    unsigned now = win ? win->ticks : 0;
    if (btn & WEV_MOUSE_SHIFT) {           /* Shift+click: extend the selection to the click */
        if (anchor < 0) anchor = caret;    /* seed the anchor from the existing caret */
        caret = idx;
        printf("[ui] shsel %d %d\r\n", anchor < caret ? anchor : caret, anchor < caret ? caret : anchor);
    } else if (last_click_i >= 0 && (now - last_click_t) < TF_DBLCLICK &&
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
    if (key == 0x1a) { undo(); return true; }                     /* Ctrl+Z */
    if (key == 0x19) { redo(); return true; }                     /* Ctrl+Y */
    if (key == 0x01) { anchor = 0; caret = len; brk = true; if (win) win->invalidate(); return true; }  /* Ctrl+A */
    /* a caret jump (arrows / home / end / word-jump) ends the typing-coalesce run */
    if (key == UK_LEFT || key == UK_RIGHT || key == UK_UP || key == UK_DOWN ||
        key == UK_HOME || key == UK_END || key == UK_WORD_LEFT || key == UK_WORD_RIGHT) brk = true;
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
    bool foc = force_focus || (win && win->focus == this);

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
/* --- app menu bar builder (design/ui.md #6) ------------------------------- */
static void menu_cpy(char *dst, const char *src, int cap) {
    int i = 0; for (; i < cap - 1 && src && src[i]; i++) dst[i] = src[i]; dst[i] = 0;
}
void Window::menu_begin() { menu_spec.nmenus = 0; }
int Window::menu_add(const char *title) {
    if (menu_spec.nmenus >= WINMENU_MAX) return -1;
    int i = (int)menu_spec.nmenus++;
    menu_cpy(menu_spec.m[i].title, title, WINMENU_LBL);
    menu_spec.m[i].nitems = 0;
    return i;
}
void Window::menu_item(int menu, const char *label, char accel, unsigned flags) {
    if (menu < 0 || menu >= (int)menu_spec.nmenus) return;
    unsigned n = menu_spec.m[menu].nitems;
    if (n >= WINMENU_ITEMS) return;
    menu_cpy(menu_spec.m[menu].items[n], label, WINMENU_LBL);
    menu_spec.m[menu].flags[n] = (uint8_t)flags;
    menu_spec.m[menu].accel[n] = accel;
    menu_spec.m[menu].nitems = n + 1;
}
/* Flip a per-item flag bit and re-publish so the compositor's next read reflects it. */
static void menu_flag(struct winmenu &s, int menu, int item, unsigned bit, bool on, int id) {
    if (menu < 0 || menu >= (int)s.nmenus) return;
    if (item < 0 || item >= (int)s.m[menu].nitems) return;
    if (on) s.m[menu].flags[item] |= (uint8_t)bit;
    else    s.m[menu].flags[item] &= (uint8_t)~bit;
    if (id >= 0) win_setmenu(id, &s);
}
void Window::menu_set_checked(int menu, int item, bool on) { menu_flag(menu_spec, menu, item, WMI_CHECKED, on, id); }
void Window::menu_set_enabled(int menu, int item, bool on) { menu_flag(menu_spec, menu, item, WMI_DISABLED, !on, id); }
bool Window::menu_is_checked(int menu, int item) const {
    if (menu < 0 || menu >= (int)menu_spec.nmenus) return false;
    if (item < 0 || item >= (int)menu_spec.m[menu].nitems) return false;
    return (menu_spec.m[menu].flags[item] & WMI_CHECKED) != 0;
}
void Window::menu_commit() { if (id >= 0) win_setmenu(id, &menu_spec); }

/* --- ConfirmDialog (modal sheet) ----------------------------------------- */
void ConfirmDialog::show(const char *t, const char *m, const char *b0, const char *b1, const char *b2) {
    menu_cpy(title, t, sizeof title);
    menu_cpy(msg, m, sizeof msg);
    const char *bs[3] = { b0, b1, b2 };
    nbtn = 0; esc_btn = -1;
    for (int i = 0; i < 3; i++) if (bs[i]) {
        menu_cpy(btn[nbtn], bs[i], sizeof btn[nbtn]);
        if (btn[nbtn][0] == 'C' && btn[nbtn][1] == 'a') esc_btn = nbtn;   /* "Cancel" -> the Esc target */
        nbtn++;
    }
    open = true; visible = true; hover = -1;
    if (win) { r = { 0, 0, win->w, win->h }; prev_focus = win->focus; win->focus = this; win->invalidate(); }
    layout();
    /* report each button's window-local centre so a test can click it (client-relative,
     * pairs with the compositor's "[twm] win" client rect). */
    for (int i = 0; i < nbtn; i++)
        printf("[ui] dlgbtn %d %d %d\r\n", i, brect[i].x + brect[i].w / 2, brect[i].y + brect[i].h / 2);
}
void ConfirmDialog::dismiss() {
    open = false; visible = false;
    if (win) { if (win->focus == this) win->focus = prev_focus; win->invalidate(); }
}
void ConfirmDialog::layout() {
    if (!win) return;
    int fh = ugfx_font_h(), pad = 20, gap = 10, bh = fh + 16;
    int bw[3], btot = 0;
    for (int i = 0; i < nbtn; i++) { bw[i] = ugfx_text_w(btn[i]) + 30; if (bw[i] < 78) bw[i] = 78; btot += bw[i] + (i ? gap : 0); }
    int cw = ugfx_text_w(title); int mw = ugfx_text_w(msg); if (mw > cw) cw = mw; if (btot > cw) cw = btot;
    cw += pad * 2; if (cw < 300) cw = 300; if (cw > win->w - 40) cw = win->w - 40;
    int ch = pad + fh + 12 + fh + 18 + bh + pad;
    int cx = (win->w - cw) / 2, cy = (win->h - ch) / 2;
    card = { cx, cy, cw, ch };
    int bx = cx + cw - pad, by = cy + ch - pad - bh;     /* primary (i=0) at the far right */
    for (int i = 0; i < nbtn; i++) { bx -= bw[i]; brect[i] = { bx, by, bw[i], bh }; bx -= gap; }
}
int ConfirmDialog::btn_at(int x, int y) const {
    for (int i = 0; i < nbtn; i++) if (brect[i].has(x, y)) return i;
    return -1;
}
void ConfirmDialog::choose(int idx) {
    dismiss();                                            /* restore focus first, so the callback may re-show */
    printf("[ui] confirm %d\r\n", idx);
    if (on_choice) on_choice(ctx, idx);
}
void ConfirmDialog::draw() {
    if (!open || !win) return;
    layout();
    int fh = ugfx_font_h(), pad = 20, rad = TH_R_MD;
    ugfx_fill_a(0, 0, win->w, win->h, ARGB(150, 8, 10, 16));        /* dim the window behind the sheet */
    ugfx_elevation(card.x, card.y, card.w, card.h, rad, 6);
    ugfx_rrect_aa(card.x, card.y, card.w, card.h, rad, TH_SURF_3);
    ugfx_rrect_border(card.x, card.y, card.w, card.h, rad, 1, TH_BORDER);
    ugfx_text(card.x + pad, card.y + pad, title, TH_TEXT, UGFX_TRANSPARENT);
    ugfx_text(card.x + pad, card.y + pad + fh + 12, msg, TH_MUTED, UGFX_TRANSPARENT);
    for (int i = 0; i < nbtn; i++) {
        Rect b = brect[i]; bool primary = (i == 0);
        ugfx_rrect_aa(b.x, b.y, b.w, b.h, TH_R_SM, primary ? TH_ACCENT : TH_SURF_4);
        if (!primary) ugfx_rrect_border(b.x, b.y, b.w, b.h, TH_R_SM, 1, TH_BORDER);
        if (i == hover) ugfx_state_layer(b.x, b.y, b.w, b.h, TH_R_SM, TH_HOVER_A);
        uint32_t tc = primary ? RGB(255, 255, 255) : TH_TEXT;
        ugfx_text(b.x + (b.w - ugfx_text_w(btn[i])) / 2, b.y + (b.h - fh) / 2, btn[i], tc, UGFX_TRANSPARENT);
    }
}
bool ConfirmDialog::on_mouse(int x, int y, int) {
    if (!open) return true;
    int b = btn_at(x, y);
    if (b >= 0) choose(b);                                /* clicks off the buttons are swallowed (stay modal) */
    return true;
}
bool ConfirmDialog::on_hover(int x, int y) {
    if (!open) return false;
    int b = btn_at(x, y);
    if (b == hover) return false;
    hover = b; return true;
}
bool ConfirmDialog::on_key(int key, bool) {
    if (!open) return false;
    if (key == UK_ESC)   { choose(esc_btn); return true; }
    if (key == UK_ENTER || key == '\n') { choose(0); return true; }   /* Enter = the primary button */
    return true;                                          /* swallow everything else while modal */
}

/* --- FileDialog (modal Open/Save browser) -------------------------------- */
static void fd_join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (int i = 0; dir[i] && n < cap - 1; i++) out[n++] = dir[i];
    if (n == 0 || out[n - 1] != '/') { if (n < cap - 1) out[n++] = '/'; }
    for (int i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = 0;
}
/* Build a non-colliding "<stem> (N)<ext>" sibling of `full` -- the "Keep Both"
 * choice on an overwrite: picked.txt -> "picked (2).txt" -> "picked (3).txt" ...
 * The extension is only honoured in the basename (a dot in a parent dir is not a
 * suffix); a name with no extension just gets " (N)". */
static void fd_dedup(char *out, int cap, const char *full) {
    int n = (int)strlen(full), slash = -1, dot = -1;
    for (int i = 0; i < n; i++) if (full[i] == '/') slash = i;
    for (int i = slash + 1; i < n; i++) if (full[i] == '.') dot = i;
    char dir[256] = {0}, stem[128] = {0}, ext[40] = {0};
    int di = 0; for (int i = 0; i <= slash && di < (int)sizeof dir - 1; i++) dir[di++] = full[i]; dir[di] = 0;
    int stem_end = (dot > slash) ? dot : n;
    int si = 0; for (int i = slash + 1; i < stem_end && si < (int)sizeof stem - 1; i++) stem[si++] = full[i]; stem[si] = 0;
    int ei = 0; if (dot > slash) for (int i = dot; i < n && ei < (int)sizeof ext - 1; i++) ext[ei++] = full[i]; ext[ei] = 0;
    for (int k = 2; k < 1000; k++) {
        snprintf(out, cap, "%s%s (%d)%s", dir, stem, k, ext);
        if (!sys_exists(out, 0)) return;
    }
    snprintf(out, cap, "%s%s (copy)%s", dir, stem, ext);   /* last-ditch */
}
/* a small Finder-ish folder glyph (tabbed rectangle) in an 18px box at (x,y) */
static void fd_folder(int x, int y, uint32_t c) {
    ugfx_fill_a(x + 1, y + 3, 7, 3, c);                 /* tab   */
    ugfx_rrect_a(x, y + 5, 18, 11, 2, c);               /* body  */
}
/* a generic file glyph (sheet with a folded corner) */
static void fd_file(int x, int y, uint32_t c) {
    ugfx_rrect_a(x + 2, y + 1, 13, 16, 2, c);
    ugfx_fill_a(x + 11, y + 1, 4, 4, ARGB(120, 12, 14, 20));   /* dog-ear */
}
/* an up-arrow chevron centred in a box of side s at (x,y) */
static void fd_up(int x, int y, int s, uint32_t c) {
    int cx = x + s / 2, cy = y + s / 2;
    for (int i = 0; i <= 5; i++) { ugfx_fill(cx - i, cy - 3 + i, 2, 2, c); ugfx_fill(cx + i, cy - 3 + i, 2, 2, c); }
    ugfx_fill(cx - 1, cy - 3, 2, 8, c);                 /* stem */
}

FileDialog::FileDialog() { visible = false; focusable = true; }

static int fd_cmp(const struct dirent *a, const struct dirent *b) {
    if ((a->type == FT_DIR) != (b->type == FT_DIR)) return a->type == FT_DIR ? -1 : 1;
    const char *x = a->name, *y = b->name;
    while (*x && *y) { if (*x != *y) return (int)(unsigned char)*x - (int)(unsigned char)*y; x++; y++; }
    return *x - *y;
}
void FileDialog::load_dir() {
    nents = readdir(path, ents, FD_NMAX);
    if (nents < 0) nents = 0;
    for (int i = 0; i < nents; i++)
        for (int j = i + 1; j < nents; j++)
            if (fd_cmp(&ents[j], &ents[i]) < 0) { struct dirent t = ents[i]; ents[i] = ents[j]; ents[j] = t; }
    list.count = nents + (has_up() ? 1 : 0);
    list.sel = -1; list.top = 0;
}
void FileDialog::navigate(const char *p) {
    int i = 0; for (; p[i] && i < (int)sizeof path - 1; i++) path[i] = p[i]; path[i] = 0;
    if (!path[0]) { path[0] = '/'; path[1] = 0; }
    load_dir();
    printf("[filedialog] cd %s\r\n", path);
    if (win) win->invalidate();
}
void FileDialog::go_up() {
    if (!has_up()) return;
    char parent[256]; int i = 0; for (; path[i] && i < 255; i++) parent[i] = path[i]; parent[i] = 0;
    int last = -1; for (int k = 0; parent[k]; k++) if (parent[k] == '/') last = k;
    if (last <= 0) { parent[0] = '/'; parent[1] = 0; } else parent[last] = 0;
    navigate(parent);
}
bool FileDialog::dir_writable() const {
    struct fstat st;
    if (stat_(path, &st) != 0) return true;             /* unknown -> let the kernel decide on write */
    return tos_may_write(getuid(), (int)st.owner) != 0;
}
void FileDialog::row_selected(int row) {
    int hu = has_up() ? 1 : 0;
    if (hu && row == 0) return;
    int idx = row - hu; if (idx < 0 || idx >= nents) return;
    if (mode == FD_SAVE && ents[idx].type == FT_FILE) {  /* clicking a file fills the name field */
        name.set_text(ents[idx].name); name.caret = name.length(); name.force_focus = true;
    }
    if (win) win->invalidate();
}
void FileDialog::row_activated(int row) {
    int hu = has_up() ? 1 : 0;
    if (hu && row == 0) { go_up(); return; }
    int idx = row - hu; if (idx < 0 || idx >= nents) return;
    if (ents[idx].type == FT_DIR) { char c[256]; fd_join(c, sizeof c, path, ents[idx].name); navigate(c); return; }
    if (mode == FD_OPEN) { char f[256]; fd_join(f, sizeof f, path, ents[idx].name); finish(f); }
    else { name.set_text(ents[idx].name); name.caret = name.length(); do_ok(); }   /* double-click a file = save over it */
}
void FileDialog::do_ok() {
    if (mode == FD_OPEN) {
        int sel = list.sel; if (sel < 0) return;
        int hu = has_up() ? 1 : 0;
        if (hu && sel == 0) { go_up(); return; }
        int idx = sel - hu; if (idx < 0 || idx >= nents) return;
        if (ents[idx].type == FT_DIR) { char c[256]; fd_join(c, sizeof c, path, ents[idx].name); navigate(c); return; }
        char f[256]; fd_join(f, sizeof f, path, ents[idx].name); finish(f);
        return;
    }
    /* SAVE */
    const char *nm = name.text();
    if (!nm[0] || !dir_writable()) return;               /* nothing typed / can't write here */
    char target[256]; fd_join(target, sizeof target, path, nm);
    struct fstat st;
    if (stat_(target, &st) == 0 && st.type == FT_FILE) {  /* overwrite warning (#4) */
        int i = 0; for (; target[i] && i < (int)sizeof pending - 1; i++) pending[i] = target[i]; pending[i] = 0;
        char msg[160]; snprintf(msg, sizeof msg, "\"%s\" already exists in this folder.", nm);
        overwrite.show("Replace file?", msg, "Replace", "Keep Both", "Cancel");   /* Keep Both = save as "name (N)" */
        if (win) win->invalidate();
        return;
    }
    finish(target);
}
void FileDialog::finish(const char *p) {
    open = false; visible = false; overwrite.dismiss();
    if (win && win->focus == this) win->focus = prev_focus;
    if (p) printf("[filedialog] pick %s\r\n", p);
    else   printf("[filedialog] cancel\r\n");
    if (on_pick) on_pick(ctx, p);
    if (win) win->invalidate();
}
void FileDialog::dismiss() { finish(nullptr); }

void FileDialog::open_dialog(int m, const char *start_dir, const char *suggest) {
    mode = m; open = true; visible = true; hover_btn = -1; hover_fav = -1;
    if (win) {
        r = { 0, 0, win->w, win->h };
        prev_focus = win->focus; win->focus = this;
        list.win = win; name.win = win; overwrite.win = win;
    }
    /* favourites (same quick-access set as Files) */
    nfav = 0;
    static const Fav defs[] = {
        { "Home",         "/Users/user" },          { "Desktop",   "/Users/user/Desktop" },
        { "Documents",    "/Users/user/Documents" }, { "Downloads", "/Users/user/Downloads" },
        { "Pictures",     "/Users/user/Pictures" },  { "Applications", "/Apps" },
        { "Computer",     "/" },
    };
    for (unsigned i = 0; i < sizeof defs / sizeof defs[0] && nfav < 8; i++) {
        fav[nfav].label = defs[i].label;
        int j = 0; for (; defs[i].path[j] && j < 63; j++) fav[nfav].path[j] = defs[i].path[j]; fav[nfav].path[j] = 0;
        nfav++;
    }
    list.ctx = this; list.render_row = render_row;
    list.on_select   = [](void *c, int i) { ((FileDialog *)c)->row_selected(i); };
    list.on_activate = [](void *c, int i) { ((FileDialog *)c)->row_activated(i); };
    list.bg = TH_SURF_0; list.sel_bg = ARGB(235, 96, 152, 252);
    overwrite.ctx = this;
    overwrite.on_choice = [](void *c, int idx) {
        FileDialog *d = (FileDialog *)c;
        if (idx == 0) d->finish(d->pending);                                       /* Replace      */
        else if (idx == 1) { char dup[256]; fd_dedup(dup, sizeof dup, d->pending); d->finish(dup); }  /* Keep Both */
        else if (d->win) { d->win->focus = d; d->win->invalidate(); }              /* Cancel: stay */
    };
    name.multiline = false; name.bg = TH_SURF_0; name.fg = TH_TEXT;
    name.set_text(mode == FD_SAVE ? (suggest ? suggest : "untitled.txt") : "");
    name.caret = name.length(); name.force_focus = true;
    if (mode == FD_SAVE) name.on_key(0x01);              /* select-all so the first keystroke replaces it */
    navigate(start_dir ? start_dir : "/Users/user");
    printf("[filedialog] open %s %s\r\n", mode == FD_SAVE ? "save" : "open", path);
    layout();
    printf("[filedialog] okbtn %d %d\r\n", okbtn.x + okbtn.w / 2, okbtn.y + okbtn.h / 2);
    printf("[filedialog] cancelbtn %d %d\r\n", cancelbtn.x + cancelbtn.w / 2, cancelbtn.y + cancelbtn.h / 2);
    if (mode == FD_SAVE) printf("[filedialog] name %d %d %d %d\r\n", namer.x, namer.y, namer.w, namer.h);
    if (win) win->invalidate();
}

void FileDialog::layout() {
    if (!win) return;
    int fh = ugfx_font_h(), pad = 16, bh = fh + 14, gap = 10;
    int cw = win->w - 40; if (cw > 560) cw = 560; if (cw < 320) cw = 320;
    int ch = win->h - 60; if (ch > 460) ch = 460; if (ch < 300) ch = 300;
    int cx = (win->w - cw) / 2, cy = (win->h - ch) / 2;
    card = { cx, cy, cw, ch };
    upbtn = { cx + pad, cy + pad + fh + 8, fh + 8, fh + 8 };          /* path-bar Up button */
    int by = cy + ch - pad - bh;                                     /* buttons row        */
    int okw = ugfx_text_w(mode == FD_SAVE ? "Save" : "Open") + 34; if (okw < 92) okw = 92;
    int cw_ = ugfx_text_w("Cancel") + 30; if (cw_ < 84) cw_ = 84;
    okbtn     = { cx + cw - pad - okw, by, okw, bh };
    cancelbtn = { okbtn.x - gap - cw_, by, cw_, bh };
    int content_top = upbtn.y + upbtn.h + 10;
    int content_bot = (mode == FD_SAVE) ? (by - 12 - (fh + 10) - 8) : (by - 12);
    int sbw = 116;
    side = { cx + pad, content_top, sbw, content_bot - content_top };
    fav_rowh = fh + 9;
    list.r = { side.x + sbw + 10, content_top, cw - pad * 2 - sbw - 10, content_bot - content_top };
    list.row_h = fh + 12;
    if (mode == FD_SAVE) namer = { side.x + sbw + 10 + 54, content_bot + 8, list.r.w - 54, fh + 10 };
}

int FileDialog::fav_at(int x, int y) const {
    if (x < side.x || x >= side.x + side.w) return -1;
    int i = (y - side.y) / fav_rowh;
    return (i >= 0 && i < nfav) ? i : -1;
}

void FileDialog::render_row(void *ctx, int index, Rect cell, bool sel) {
    FileDialog *d = (FileDialog *)ctx;
    int fh = ugfx_font_h();
    int ix = cell.x + 12, iy = cell.y + (cell.h - 18) / 2, ty = cell.y + (cell.h - fh) / 2;
    int hu = d->has_up() ? 1 : 0;
    const char *nm; int type; unsigned size = 0;
    if (hu && index == 0) { nm = ".."; type = FT_DIR; }
    else { int idx = index - hu; nm = d->ents[idx].name; type = (int)d->ents[idx].type; size = d->ents[idx].size; }
    uint32_t gc = sel ? RGB(255, 255, 255) : (type == FT_DIR ? RGB(150, 188, 255) : TH_MUTED);
    if (type == FT_DIR) fd_folder(ix, iy, gc); else fd_file(ix, iy, gc);
    ugfx_text(ix + 28, ty, nm, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
    if (type == FT_FILE) {
        char sz[20]; snprintf(sz, sizeof sz, "%u B", size);
        ugfx_text(cell.x + cell.w - ugfx_text_w(sz) - 14, ty, sz, sel ? RGB(230, 238, 250) : TH_MUTED, UGFX_TRANSPARENT);
    }
}

void FileDialog::draw() {
    if (!open || !win) return;
    layout();
    int fh = ugfx_font_h(), pad = 16, rad = TH_R_MD;
    ugfx_fill_a(0, 0, win->w, win->h, ARGB(150, 8, 10, 16));          /* dim the window behind */
    ugfx_elevation(card.x, card.y, card.w, card.h, rad, 6);
    ugfx_rrect_aa(card.x, card.y, card.w, card.h, rad, TH_SURF_2);
    ugfx_rrect_border(card.x, card.y, card.w, card.h, rad, 1, TH_BORDER);
    /* title */
    const char *ttl = mode == FD_SAVE ? "Save As" : "Open";
    ugfx_text(card.x + pad, card.y + pad, ttl, TH_TEXT, UGFX_TRANSPARENT);
    /* path bar: an Up button + the current path (right-truncated to fit) */
    ugfx_rrect_aa(upbtn.x, upbtn.y, upbtn.w, upbtn.h, TH_R_SM, has_up() ? TH_SURF_3 : TH_SURF_2);
    if (hover_btn == 2 && has_up()) ugfx_state_layer(upbtn.x, upbtn.y, upbtn.w, upbtn.h, TH_R_SM, TH_HOVER_A);
    ugfx_rrect_border(upbtn.x, upbtn.y, upbtn.w, upbtn.h, TH_R_SM, 1, TH_BORDER);
    fd_up(upbtn.x, upbtn.y, upbtn.w, has_up() ? TH_TEXT : TH_MUTED);
    int px = upbtn.x + upbtn.w + 10, pw = card.x + card.w - pad - px, fwc = ugfx_font_w();
    int maxc = pw / fwc; if (maxc < 1) maxc = 1;
    int pl = (int)strlen(path);
    const char *ps = path; char trunc[80];
    if (pl > maxc && maxc > 4) {                          /* right-truncate: show "...tail" */
        int off = pl - (maxc - 3);
        snprintf(trunc, sizeof trunc, "...%s", path + off);
        ps = trunc;
    }
    ugfx_text(px, upbtn.y + (upbtn.h - fh) / 2, ps, TH_MUTED, UGFX_TRANSPARENT);
    /* sidebar */
    ugfx_rrect_aa(side.x, side.y, side.w, side.h, TH_R_SM, TH_SURF_1);
    for (int i = 0; i < nfav; i++) {
        int ry = side.y + i * fav_rowh; if (ry + fav_rowh > side.y + side.h) break;
        bool cur = strcmp(path, fav[i].path) == 0;
        if (cur)               ugfx_rrect_a(side.x + 5, ry + 1, side.w - 10, fav_rowh - 2, TH_R_SM, ARGB(150, 96, 152, 252));
        else if (i == hover_fav) ugfx_state_layer(side.x + 5, ry + 1, side.w - 10, fav_rowh - 2, TH_R_SM, TH_HOVER_A);
        fd_folder(side.x + 12, ry + (fav_rowh - 18) / 2, cur ? RGB(255, 255, 255) : RGB(150, 188, 255));
        ugfx_text(side.x + 38, ry + (fav_rowh - fh) / 2, fav[i].label, cur ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
    }
    /* directory list (the embedded ListView draws its bg + rows + scroll thumb) */
    ugfx_rrect_border(list.r.x, list.r.y, list.r.w, list.r.h, TH_R_SM, 1, TH_BORDER);
    list.draw();
    /* name field (SAVE) */
    if (mode == FD_SAVE) {
        ugfx_text(side.x + side.w + 10, namer.y + (namer.h - fh) / 2, "Name:", TH_MUTED, UGFX_TRANSPARENT);
        name.r = namer; name.draw();
    }
    /* buttons */
    bool ok_en = (mode == FD_OPEN) ? (list.sel >= 0) : (name.length() > 0 && dir_writable());
    ugfx_rrect_aa(okbtn.x, okbtn.y, okbtn.w, okbtn.h, TH_R_SM, ok_en ? TH_ACCENT : TH_SURF_3);
    if (ok_en && hover_btn == 0) ugfx_state_layer(okbtn.x, okbtn.y, okbtn.w, okbtn.h, TH_R_SM, TH_HOVER_A);
    ugfx_text(okbtn.x + (okbtn.w - ugfx_text_w(ttl)) / 2, okbtn.y + (okbtn.h - fh) / 2, ttl, ok_en ? RGB(255, 255, 255) : TH_MUTED, UGFX_TRANSPARENT);
    ugfx_rrect_aa(cancelbtn.x, cancelbtn.y, cancelbtn.w, cancelbtn.h, TH_R_SM, TH_SURF_3);
    ugfx_rrect_border(cancelbtn.x, cancelbtn.y, cancelbtn.w, cancelbtn.h, TH_R_SM, 1, TH_BORDER);
    if (hover_btn == 1) ugfx_state_layer(cancelbtn.x, cancelbtn.y, cancelbtn.w, cancelbtn.h, TH_R_SM, TH_HOVER_A);
    ugfx_text(cancelbtn.x + (cancelbtn.w - ugfx_text_w("Cancel")) / 2, cancelbtn.y + (cancelbtn.h - fh) / 2, "Cancel", TH_TEXT, UGFX_TRANSPARENT);
    /* the overwrite confirm draws on top of everything */
    if (overwrite.open) overwrite.draw();
}

bool FileDialog::on_mouse(int x, int y, int btn) {
    if (!open) return true;
    if (overwrite.open) return overwrite.on_mouse(x, y, btn);
    if (okbtn.has(x, y))      { do_ok(); return true; }
    if (cancelbtn.has(x, y))  { finish(nullptr); return true; }
    if (upbtn.has(x, y))      { go_up(); return true; }
    int f = fav_at(x, y);
    if (f >= 0)               { navigate(fav[f].path); return true; }
    if (list.r.has(x, y))     { list.on_mouse(x, y, btn); if (win) win->invalidate(); return true; }
    if (mode == FD_SAVE && namer.has(x, y)) { name.on_mouse(x, y, btn); if (win) win->invalidate(); return true; }
    return true;                                          /* swallow stray clicks (modal) */
}
void FileDialog::on_drag(int x, int y) {
    if (!open || overwrite.open) return;
    list.on_drag(x, y);                                  /* no-ops unless its scroll thumb is held */
    if (mode == FD_SAVE) name.on_drag(x, y);             /* drag-select in the name field          */
    if (win) win->invalidate();
}
void FileDialog::on_button_up() { list.on_button_up(); name.on_button_up(); }
bool FileDialog::on_scroll(int delta) {
    if (!open || overwrite.open) return false;
    bool r2 = list.on_scroll(delta);
    if (r2 && win) win->invalidate();
    return r2;
}
bool FileDialog::on_key(int key, bool shift) {
    if (!open) return false;
    if (overwrite.open) return overwrite.on_key(key, shift);
    if (key == UK_ESC) { finish(nullptr); return true; }
    if (key == UK_ENTER || key == '\n') { do_ok(); return true; }
    if (mode == FD_OPEN) { if (key == UK_UP || key == UK_DOWN) { list.on_key(key, shift); if (win) win->invalidate(); } return true; }
    name.on_key(key, shift);                              /* SAVE: typing goes to the name field */
    if (win) win->invalidate();
    return true;
}
bool FileDialog::on_hover(int x, int y) {
    if (!open) return false;
    if (overwrite.open) return overwrite.on_hover(x, y);
    int hb = okbtn.has(x, y) ? 0 : cancelbtn.has(x, y) ? 1 : upbtn.has(x, y) ? 2 : -1;
    int hf = fav_at(x, y);
    bool changed = (hb != hover_btn) || (hf != hover_fav);
    hover_btn = hb; hover_fav = hf;
    if (list.r.has(x, y)) { if (list.on_hover(x, y)) changed = true; }
    else if (list.hover_row != -1) { list.hover_row = -1; changed = true; }
    return changed;
}
void FileDialog::on_leave() { hovered = false; hover_btn = -1; hover_fav = -1; list.hover_row = -1; }

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
            case WEV_MENU:  on_menu((int)WEV_MENU_M(ev.a), (int)WEV_MENU_I(ev.a)); dirty = true; break;
            case WEV_CLOSE: if (on_close()) running = false; break;
            }
        }
        ticks++;
        on_tick(ticks);                       /* periodic app work (e.g. Notepad session autosave) */
        if (focus && focus->shows_caret() && (ticks % TF_BLINK) == 0)
            dirty = true;                 /* pulse a repaint so the caret keeps blinking while idle */
        if (dirty) { redraw(); dirty = false; }
        sleep_ms(15);
    }
    win_close(id);
    return 0;
}

} // namespace ui
