/* Pure codec for the Files app's Trash sidecar (~/.Trash/.trashinfo): one
 * "<trashedname>\t<originalpath>\n" line per trashed item, recording where each
 * trashed thing came from so "Put Back" can restore it (files-app §9).
 *
 * Every function works on in-memory buffers -- the FilesApp wrappers do the
 * slurp/spit around them -- so this header unit-tests on the host with no FS. */
#pragma once

/* internal: NUL-terminated length */
static inline int trashinfo_len_(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Find the recorded origin path for the trashed item `tname` in the sidecar text
 * [info, info+len). On a hit, copies the path (NUL-terminated, truncated to cap)
 * into out and returns 1; otherwise clears out and returns 0. */
static inline int trashinfo_find(const char *info, int len, const char *tname,
                                 char *out, int cap) {
    int tl = trashinfo_len_(tname);
    for (int i = 0; i < len; ) {
        int ls = i; while (i < len && info[i] != '\n') i++;        /* line [ls, i) */
        int tab = ls; while (tab < i && info[tab] != '\t') tab++;  /* name | path  */
        if (tab < i && tab - ls == tl) {
            int eq = 1; for (int k = 0; k < tl; k++) if (info[ls + k] != tname[k]) { eq = 0; break; }
            if (eq) {
                int pl = i - (tab + 1); if (pl > cap - 1) pl = cap - 1;
                for (int k = 0; k < pl; k++) out[k] = info[tab + 1 + k];
                out[pl] = 0; return 1;
            }
        }
        if (i < len) i++;                                          /* step over '\n' */
    }
    if (cap > 0) out[0] = 0;
    return 0;
}

/* Append a "<tname>\t<origpath>\n" record to the sidecar text [info, info+len),
 * writing the concatenation into dst[cap]. Returns the new length (excluding the
 * NUL); the caller sizes dst to hold len + the two strings + 3. */
static inline int trashinfo_add(char *dst, int cap, const char *info, int len,
                                const char *tname, const char *origpath) {
    int n = 0;
    for (int i = 0; i < len        && n < cap - 1; i++) dst[n++] = info[i];
    for (int i = 0; tname[i]       && n < cap - 1; i++) dst[n++] = tname[i];
    if (n < cap - 1) dst[n++] = '\t';
    for (int i = 0; origpath[i]    && n < cap - 1; i++) dst[n++] = origpath[i];
    if (n < cap - 1) dst[n++] = '\n';
    dst[n] = 0;
    return n;
}

/* Drop the record for `tname` from the sidecar text [info, info+len), writing the
 * remaining lines into dst[cap]. Returns the new length (0 == sidecar now empty). */
static inline int trashinfo_drop(char *dst, int cap, const char *info, int len,
                                 const char *tname) {
    int tl = trashinfo_len_(tname), n = 0;
    for (int i = 0; i < len; ) {
        int ls = i; while (i < len && info[i] != '\n') i++;
        int le = i; if (i < len) i++;                             /* span [ls, i) incl. '\n' */
        int tab = ls; while (tab < le && info[tab] != '\t') tab++;
        int match = (tab < le && tab - ls == tl);
        if (match) for (int k = 0; k < tl; k++) if (info[ls + k] != tname[k]) { match = 0; break; }
        if (!match) for (int k = ls; k < i && n < cap - 1; k++) dst[n++] = info[k];
    }
    dst[n] = 0;
    return n;
}
