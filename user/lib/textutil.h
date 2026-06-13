/* Pure text/index helpers shared by the toolkit + launchers. NO OS dependencies
 * (no syscalls, no globals) -- plain C arithmetic only -- so they compile on the
 * host and are unit-tested directly in tests/unit. The toolkit (ui.cpp) and the
 * launchers (applist.h) delegate to these. See design/testing.md. */
#pragma once

/* Case-insensitive "does name contain query"? An empty query matches everything. */
static inline int tu_ci_contains(const char *name, const char *q) {
    if (!q[0]) return 1;
    for (int i = 0; name[i]; i++) {
        int k = 0;
        while (q[k]) {
            char a = name[i + k], b = q[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            k++;
        }
        if (!q[k]) return 1;
    }
    return 0;
}

/* A "word" character for word-wise caret motion (alphanumeric + underscore). */
static inline int tu_wordch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Previous word boundary from `caret`, over a buffer (caret is a valid index,
 * 0..len). Skips separators left, then the word -- the start of the word the
 * caret is in or just past. */
static inline int tu_word_prev(const char *buf, int caret) {
    int i = caret;
    while (i > 0 && !tu_wordch(buf[i - 1])) i--;
    while (i > 0 &&  tu_wordch(buf[i - 1])) i--;
    return i;
}

/* Next word boundary from `caret` over a buffer of length `len`: skips
 * separators right, then the word -- the position just past the next word. */
static inline int tu_word_next(const char *buf, int len, int caret) {
    int i = caret;
    while (i < len && !tu_wordch(buf[i])) i++;
    while (i < len &&  tu_wordch(buf[i])) i++;
    return i;
}

/* Destination index for a same-field text-drag MOVE: the source span [a,b) is
 * deleted, then re-inserted at drop point `p` (an index into the ORIGINAL buffer).
 * A drop inside the moved span lands at its start; a drop at or past the span end
 * shifts left by the removed length. Returns the post-deletion insert index. Pure
 * index arithmetic -- unit-tested in tests/unit. */
static inline int tu_textmove_dest(int a, int b, int p) {
    if (p > a && p < b) p = a;       /* inside the selection -> no real move */
    if (p >= b) p -= (b - a);        /* removal shifts a right-of-span drop left */
    return p;
}

/* Greedy word-wrap of `src` into at most `maxlines` lines no wider than `maxw`,
 * using measure(s) for the pixel width of a NUL-terminated string. Each completed
 * line is passed (NUL-terminated) to emit(ctx, line, index); pass emit=0 to only
 * count. A word that overflows the current line moves to the next, keeping its
 * trailing spaces with it (so a continuation never glues two words together and
 * never starts with a stray space). Returns the line count (>=1). Pure: no
 * globals, no I/O -- unit-tested in tests/unit. */
static inline int tu_wrap(const char *src, int maxw, int maxlines,
                          int (*measure)(const char *),
                          void (*emit)(void *, const char *, int), void *ctx) {
    char line[256]; int ll = 0, lines = 0, i = 0;
    while (src[i] && lines < maxlines) {
        int we = i; while (src[we] && src[we] != ' ') we++;   /* end of the word   */
        int ns = we; while (src[ns] == ' ') ns++;             /* end of its spaces */
        char cand[256]; int cl = 0;
        for (int k = 0; k < ll; k++) cand[cl++] = line[k];
        for (int k = i; k < we && cl < (int)sizeof cand - 1; k++) cand[cl++] = src[k];
        cand[cl] = 0;
        if (ll > 0 && measure(cand) > maxw) {                 /* word overflows -> flush line */
            line[ll] = 0;
            if (emit) emit(ctx, line, lines);
            lines++; ll = 0;
        }
        for (int k = i; k < ns && ll < (int)sizeof line - 1; k++) line[ll++] = src[k];  /* word + spaces */
        i = ns;
    }
    if (ll > 0 && lines < maxlines) {
        line[ll] = 0;
        if (emit) emit(ctx, line, lines);
        lines++;
    }
    return lines ? lines : 1;
}
