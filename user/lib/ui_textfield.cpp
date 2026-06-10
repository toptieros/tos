/* tOS UI toolkit -- the TextField widget (split out of ui.cpp; see ui.h).
 *
 * The single-line + multi-line editable text widget: caret, selection, word jumps,
 * double/triple-click, clipboard, undo/redo (editlog), and scrolling. Big enough to
 * earn its own translation unit. */
#include "ui.h"
#include "theme.h"
#include "textutil.h"
#include "editlog.h"

namespace ui {

/* ---------------------------------------------------------------- TextField */
#define TF_PAD 6
#define TF_SEL RGB(54, 88, 144)
/* TF_BLINK (the caret period) is shared with Window::redraw -- it lives in ui.h. */
#define TF_DBLCLICK 24           /* max ticks between presses to count as a double-click */

/* a "word" character for double-click select + Ctrl+arrow word jumps */
static inline bool tf_wordch(char c) { return tu_wordch(c); }  /* pure; unit-tested in tests/unit */

TextField::TextField() { bg = TH_SURF_0; fg = TH_TEXT; focusable = true; cursor = CUR_IBEAM; }
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
/* repaint just this field (typing is the hottest path); on_change may invalidate
 * more (e.g. notepad's tab dirty-dot), which a full invalidate() there promotes. */
void TextField::changed() { if (on_change) on_change(ctx); if (win) win->invalidate(r.x, r.y, r.w, r.h); }
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

}  // namespace ui
