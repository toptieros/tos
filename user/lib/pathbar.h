/* Pure path -> breadcrumb logic for the Files location bar (design/files-app.md §3).
 * Splitting an absolute path into clickable crumbs (each a display label + the
 * absolute path to navigate to) is plain string arithmetic with NO OS dependencies
 * -- like textutil.h / pickreq.h -- so it compiles on the host and is unit-tested
 * directly in tests/unit. The Breadcrumb widget (user/files/files.cpp) draws +
 * hit-tests over the crumbs this produces. */
#pragma once

struct crumb { char label[48]; char path[256]; };

/* Split an absolute `path` into breadcrumb crumbs, root first. The root is always
 * crumb 0 (label "/", path "/"); each following crumb is a path segment with the
 * absolute path up to and including it. Repeated and trailing slashes collapse.
 * Returns the crumb count (>=1), capped at `max`. */
static inline int pathbar_split(const char *path, struct crumb *out, int max) {
    if (max <= 0) return 0;
    out[0].label[0] = '/'; out[0].label[1] = 0;
    out[0].path[0]  = '/'; out[0].path[1]  = 0;
    int n = 1;
    char acc[256]; int al = 1; acc[0] = '/'; acc[1] = 0;   /* path accumulated so far */
    int i = 0;
    while (path[i]) {
        while (path[i] == '/') i++;                        /* skip the separator run */
        if (!path[i]) break;
        char seg[48]; int sl = 0;
        while (path[i] && path[i] != '/' && sl < 47) seg[sl++] = path[i++];
        seg[sl] = 0;
        while (path[i] && path[i] != '/') i++;             /* drop an over-long segment's tail */
        if (al > 1 && al < 255) acc[al++] = '/';           /* separator (none right after root) */
        for (int k = 0; seg[k] && al < 255; k++) acc[al++] = seg[k];
        acc[al] = 0;
        if (n < max) {
            int j = 0; for (; seg[j] && j < 47;  j++) out[n].label[j] = seg[j]; out[n].label[j] = 0;
            int p = 0; for (; acc[p] && p < 255; p++) out[n].path[p]  = acc[p]; out[n].path[p]  = 0;
            n++;
        }
    }
    return n;
}
