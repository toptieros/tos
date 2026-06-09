/* tabtitle.h -- the folder-pill label for a path in the Files tab strip (§4): the last
 * path component, with the volume root shown as "Computer". Pure, no libc; shared by the
 * Files app and the host unit test (t_tabtitle). */
#pragma once

static inline void tab_title(const char *path, char *out, int cap) {
    if (cap <= 0) return;
    int n = 0; while (path[n]) n++;
    int end = n; while (end > 0 && path[end - 1] == '/') end--;   /* drop trailing slashes */
    int start = end; while (start > 0 && path[start - 1] != '/') start--;
    int p = 0;
    for (int i = start; i < end && p < cap - 1; i++) out[p++] = path[i];
    out[p] = 0;
    if (p == 0) {                                                 /* "/" or all-slashes */
        const char *r = "Computer";
        for (p = 0; r[p] && p < cap - 1; p++) out[p] = r[p];
        out[p] = 0;
    }
}
