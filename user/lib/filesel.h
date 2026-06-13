/* filesel.h -- the FileView selection-set algebra (design/files-and-desktop.md).
 *
 * A selection is a SET of visible-row indices [0,n) with an ANCHOR (the pivot a
 * Shift-range extends from) and a CURSOR (the keyboard-focused row). Pointer +
 * keyboard map onto it the Finder way (with tOS's Ctrl standing in for macOS's Cmd):
 * plain click replaces, Ctrl-click toggles, Shift-click range-extends from the anchor,
 * a marquee band selects a row range, Ctrl+A selects all, arrows move the cursor (and
 * Shift+arrow extends). This is the file-side twin of the text selection contract in
 * ui.md -- selecting icons reads like selecting characters.
 *
 * Pure logic, no libc: the Files app, the future desktop, and the host unit tests all
 * share this one definition of "what does each gesture do", so the behaviour is
 * unit-tested once (tests/unit/t_filesel.c) rather than re-derived per surface. */
#pragma once

#define FILESEL_MAX 256   /* visible rows the set can track (matches Files' NMAX) */

struct filesel {
    unsigned char mark[FILESEL_MAX];  /* mark[i] != 0  <=>  row i is selected   */
    int anchor;                       /* Shift-range pivot; -1 = none            */
    int cursor;                       /* keyboard-focused row; -1 = none         */
    int n;                            /* live row count -- rows are [0,n)        */
};

/* (re)initialise an empty selection over `n` rows (clamped to the capacity). */
static inline void fsel_init(struct filesel *s, int n) {
    for (int i = 0; i < FILESEL_MAX; i++) s->mark[i] = 0;
    s->anchor = s->cursor = -1;
    s->n = n < 0 ? 0 : (n > FILESEL_MAX ? FILESEL_MAX : n);
}
static inline int fsel_valid(const struct filesel *s, int i) { return i >= 0 && i < s->n; }
static inline int fsel_has(const struct filesel *s, int i) { return fsel_valid(s, i) && s->mark[i]; }
static inline int fsel_count(const struct filesel *s) {
    int c = 0; for (int i = 0; i < s->n; i++) if (s->mark[i]) c++; return c;
}
/* the single selected row, or -1 if the selection isn't exactly one row. */
static inline int fsel_single(const struct filesel *s) {
    int found = -1;
    for (int i = 0; i < s->n; i++) if (s->mark[i]) { if (found >= 0) return -1; found = i; }
    return found;
}

/* empty the selection (a click on empty space / Esc). */
static inline void fsel_clear(struct filesel *s) {
    for (int i = 0; i < s->n; i++) s->mark[i] = 0;
    s->anchor = s->cursor = -1;
}
/* set every row in the inclusive span [a,b] (either order) on/off; clamped to [0,n). */
static inline void fsel_band(struct filesel *s, int a, int b, int on) {
    if (a > b) { int t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b > s->n - 1) b = s->n - 1;
    for (int k = a; k <= b; k++) s->mark[k] = on ? 1 : 0;
}

/* plain click: the selection becomes exactly {i}; anchor = cursor = i. An invalid
 * index (e.g. a click below the last row = empty space) clears instead. */
static inline void fsel_click(struct filesel *s, int i) {
    if (!fsel_valid(s, i)) { fsel_clear(s); return; }
    for (int k = 0; k < s->n; k++) s->mark[k] = (k == i);
    s->anchor = s->cursor = i;
}
/* Ctrl/Super-click: toggle i in/out of the set; the anchor + cursor move to i so a
 * following Shift-click pivots from here. */
static inline void fsel_toggle(struct filesel *s, int i) {
    if (!fsel_valid(s, i)) return;
    s->mark[i] = !s->mark[i];
    s->anchor = s->cursor = i;
}
/* Shift-click: select the range anchor..i. Plain replaces the whole selection with
 * that range; `additive` (Ctrl held too) adds it to what's already selected. The
 * cursor moves to i but the anchor stays put, so repeated Shift-clicks re-pivot from
 * the same origin (Finder behaviour). With no prior anchor, i becomes the anchor. */
static inline void fsel_range(struct filesel *s, int i, int additive) {
    if (!fsel_valid(s, i)) return;
    if (!fsel_valid(s, s->anchor)) s->anchor = i;
    if (!additive) for (int k = 0; k < s->n; k++) s->mark[k] = 0;
    fsel_band(s, s->anchor, i, 1);
    s->cursor = i;
}
/* Ctrl/Super + A: select all rows; anchor at the top, cursor at the bottom. */
static inline void fsel_all(struct filesel *s) {
    for (int k = 0; k < s->n; k++) s->mark[k] = 1;
    s->anchor = s->n > 0 ? 0 : -1;
    s->cursor = s->n - 1;
}
/* arrow-key move to row i: Shift extends the range from the anchor (replacing);
 * plain replaces the selection with {i} and re-anchors there. */
static inline void fsel_move(struct filesel *s, int i, int shift) {
    if (!fsel_valid(s, i)) return;
    if (shift) fsel_range(s, i, 0);
    else fsel_click(s, i);
}
/* rubber-band marquee covering visible rows [a,b] (pass a<0 || b<0 for "the band
 * touched no rows"). Plain replaces the selection with the band; `additive`
 * (Ctrl/Shift held during the drag) unions it with the existing selection. */
static inline void fsel_marquee(struct filesel *s, int a, int b, int additive) {
    if (!additive) for (int k = 0; k < s->n; k++) s->mark[k] = 0;
    if (a < 0 || b < 0) { if (!additive) { s->anchor = s->cursor = -1; } return; }
    fsel_band(s, a, b, 1);
    s->anchor = a < b ? a : b;
    s->cursor = a < b ? b : a;
}
