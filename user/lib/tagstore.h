/* Pure codec for the Files app's tag index (~/.tags): one "<mask>\t<path>\n" line
 * per tagged item, where mask is a decimal bitmask of the seven fixed Finder-style
 * tag colors (files-app §10). tOS has no fs xattrs, so tags live in this sidecar;
 * an item with no tags has no line.
 *
 * Every function works on in-memory buffers -- the FilesApp wrappers do the
 * slurp/spit around them -- so this header unit-tests on the host with no FS. */
#pragma once

#define TAG_NCOLORS 7
static const char *tag_names_[TAG_NCOLORS] = {
    "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Gray"
};

/* internal: NUL-terminated length */
static inline int tagstore_len_(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* internal: does the line's path span [ps, pe) equal path? */
static inline int tagstore_eq_(const char *idx, int ps, int pe, const char *path) {
    int pl = tagstore_len_(path);
    if (pe - ps != pl) return 0;
    for (int k = 0; k < pl; k++) if (idx[ps + k] != path[k]) return 0;
    return 1;
}

/* The tag mask recorded for `path` in the index text [idx, idx+len), or 0. */
static inline unsigned tagstore_get(const char *idx, int len, const char *path) {
    for (int i = 0; i < len; ) {
        int ls = i; while (i < len && idx[i] != '\n') i++;         /* line [ls, i)  */
        int tab = ls; while (tab < i && idx[tab] != '\t') tab++;   /* mask | path   */
        if (tab < i && tagstore_eq_(idx, tab + 1, i, path)) {
            unsigned m = 0;
            for (int k = ls; k < tab; k++)
                if (idx[k] >= '0' && idx[k] <= '9') m = m * 10 + (unsigned)(idx[k] - '0');
            return m;
        }
        if (i < len) i++;                                          /* step over '\n' */
    }
    return 0;
}

/* Rewrite the index with `path`'s mask set to m (m == 0 drops its line), writing
 * the result into dst[cap]. Returns the new length (excluding the NUL); the caller
 * sizes dst to hold len + the path + ~16. */
static inline int tagstore_set(char *dst, int cap, const char *idx, int len,
                               const char *path, unsigned m) {
    int n = 0;
    for (int i = 0; i < len; ) {                       /* copy every other line through */
        int ls = i; while (i < len && idx[i] != '\n') i++;
        int le = i; if (i < len) i++;                  /* span [ls, i) incl. '\n' */
        int tab = ls; while (tab < le && idx[tab] != '\t') tab++;
        if (tab < le && tagstore_eq_(idx, tab + 1, le, path)) continue;
        for (int k = ls; k < i && n < cap - 1; k++) dst[n++] = idx[k];
    }
    if (m) {                                           /* append the fresh record */
        char num[12]; int t = 0;
        unsigned v = m;
        while (v) { num[t++] = (char)('0' + v % 10); v /= 10; }
        while (t > 0 && n < cap - 1) dst[n++] = num[--t];
        if (n < cap - 1) dst[n++] = '\t';
        for (int i = 0; path[i] && n < cap - 1; i++) dst[n++] = path[i];
        if (n < cap - 1) dst[n++] = '\n';
    }
    dst[n] = 0;
    return n;
}

/* A path was renamed/moved: carry its tags from `from` to `to` (no-op when
 * untagged). Returns the new length. */
static inline int tagstore_move(char *dst, int cap, const char *idx, int len,
                                const char *from, const char *to) {
    unsigned m = tagstore_get(idx, len, from);
    int n = tagstore_set(dst, cap, idx, len, from, 0);
    if (m) n = tagstore_set(dst, cap, dst, n, to, m);  /* dst aliasing: set() re-reads then appends */
    return n;
}
