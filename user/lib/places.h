/* Pure model for the Files app's editable Places sidebar (files-app §7): the
 * Favorites list the user pins folders to. Each place is a {label, path} pair,
 * stored in the registry one entry per key ("places.<i>" = "Label|/path", count in
 * "places.n") -- this header is just the codec + list ops, no registry/FS calls.
 *
 * The label is clamped so an encoded entry always fits a registry value
 * (REG_VALMAX 96): 23 label + '|' + 63 path + NUL = 88. The list itself has no
 * fixed ceiling here -- the caller sizes the array (the registry's entry budget is
 * the practical bound).
 *
 * No FS, no libc -- unit-tests on the host (tests/unit/t_places.c) and compiles
 * into the freestanding app. */
#pragma once

#define PLACE_LABELMAX 24            /* incl NUL; keeps "label|path" < REG_VALMAX */
#define PLACE_PATHMAX  64            /* incl NUL; matches the sidebar's path[64]  */

struct place { char label[PLACE_LABELMAX]; char path[PLACE_PATHMAX]; };

/* "Label|/path" -> p; returns 0, or -1 on a malformed value (no '|', empty side). */
static inline int place_decode(const char *s, struct place *p) {
    int bar = -1;
    for (int i = 0; s[i]; i++) if (s[i] == '|') { bar = i; break; }
    if (bar <= 0 || !s[bar + 1]) return -1;
    int i = 0;
    for (; i < bar && i < PLACE_LABELMAX - 1; i++) p->label[i] = s[i];
    p->label[i] = 0;
    int j = 0;
    for (; s[bar + 1 + j] && j < PLACE_PATHMAX - 1; j++) p->path[j] = s[bar + 1 + j];
    p->path[j] = 0;
    return 0;
}

/* p -> "Label|/path"; always NUL-terminates within cap; returns the length. */
static inline int place_encode(const struct place *p, char *out, int cap) {
    int pos = 0;
    for (int i = 0; p->label[i] && pos < cap - 1; i++) out[pos++] = p->label[i];
    if (pos < cap - 1) out[pos++] = '|';
    for (int i = 0; p->path[i] && pos < cap - 1; i++) out[pos++] = p->path[i];
    out[pos] = 0;
    return pos;
}

/* A default label for a path: its last segment ("/Users/user/dev" -> "dev",
 * "/" -> "/"), clamped to the label cap. */
static inline void place_label_from(const char *path, char *out, int cap) {
    int n = 0, slash = -1;
    for (; path[n]; n++) if (path[n] == '/' && path[n + 1]) slash = n;
    int pos = 0;
    if (n == 1 && path[0] == '/') { if (cap > 1) out[pos++] = '/'; }
    else for (int i = slash + 1; i < n && path[i] && pos < cap - 1; i++) {
        if (path[i] == '/') break;
        out[pos++] = path[i];
    }
    out[pos] = 0;
}

/* Index of path in a[0..n), or -1. */
static inline int places_find(const struct place *a, int n, const char *path) {
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (a[i].path[j] && a[i].path[j] == path[j]) j++;
        if (!a[i].path[j] && !path[j]) return i;
    }
    return -1;
}

/* Append {label, path} unless the path is already pinned or the array is full.
 * Returns the new count. */
static inline int places_add(struct place *a, int n, int max,
                             const char *label, const char *path) {
    if (n >= max || places_find(a, n, path) >= 0) return n;
    int i = 0;
    for (; label[i] && i < PLACE_LABELMAX - 1; i++) a[n].label[i] = label[i];
    a[n].label[i] = 0;
    int j = 0;
    for (; path[j] && j < PLACE_PATHMAX - 1; j++) a[n].path[j] = path[j];
    a[n].path[j] = 0;
    return n + 1;
}

/* Remove a[idx], shifting the tail down. Returns the new count. */
static inline int places_remove(struct place *a, int n, int idx) {
    if (idx < 0 || idx >= n) return n;
    for (int i = idx; i < n - 1; i++) a[i] = a[i + 1];
    return n - 1;
}

/* Move a[from] so it lands at insertion gap `to` (0..n, in the *pre-move* index
 * space, as a drag-reorder reports it). Dropping a row back on itself (to == from
 * or from+1) is a no-op. Returns n unchanged (a reorder, never a resize). */
static inline int places_move(struct place *a, int n, int from, int to) {
    if (from < 0 || from >= n || to < 0 || to > n) return n;
    struct place tmp = a[from];
    if (to > from) to--;                              /* removing `from` shifts later gaps down one */
    for (int i = from; i < n - 1; i++) a[i] = a[i + 1];   /* close the gap */
    for (int i = n - 1; i > to; i--) a[i] = a[i - 1];     /* open a slot at `to` */
    a[to] = tmp;
    return n;
}
