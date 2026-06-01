/* Shared installed-app scanner for launchers (Spotlight, Launchpad). Walks /Apps
 * for *.app bundles, reads each manifest, and returns the display name + absolute
 * exec path. Header-only so any launcher can use it without a separate TU; twm
 * keeps its own richer scan (it also loads icons). See app-package-format.md. */
#pragma once
#include "ulib.h"
#include "manifest.h"
#include "textutil.h"

struct AppEntry { char name[24]; char exec[160]; uint32_t *icon; int iw, ih; };

/* Load a bundle's icon.argb (u32 w, u32 h, then w*h LE ARGB) into a malloc'd
 * buffer; 0 on any error (the launcher then draws a stand-in). */
static inline uint32_t *applist_load_icon(const char *path, int *w, int *h) {
    int fd = fopen(path, O_RDONLY);
    if (fd < 0) return 0;
    uint32_t hdr[2] = { 0, 0 };
    if (fread_(fd, (char *)hdr, 8) != 8 || !hdr[0] || !hdr[1] || hdr[0] > 256 || hdr[1] > 256) { fclose_(fd); return 0; }
    int need = (int)(hdr[0] * hdr[1] * 4), got = 0;
    uint32_t *px = (uint32_t *)malloc((unsigned)need);
    if (!px) { fclose_(fd); return 0; }
    while (got < need) { int r = fread_(fd, (char *)px + got, need - got); if (r <= 0) break; got += r; }
    fclose_(fd);
    if (got != need) { free(px); return 0; }
    *w = (int)hdr[0]; *h = (int)hdr[1];
    return px;
}

static inline int applist_ends_app(const char *s) {
    int n = 0; while (s[n]) n++;
    return n >= 5 && s[n-4] == '.' && s[n-3] == 'a' && s[n-2] == 'p' && s[n-1] == 'p';
}
static inline void applist_join(char *dst, const char *a, const char *b) {
    int i = 0; while (a[i]) { dst[i] = a[i]; i++; }
    if (i && dst[i-1] != '/') dst[i++] = '/';
    for (int j = 0; b[j]; j++) dst[i++] = b[j];
    dst[i] = 0;
}

/* Fill out[0..max) with the installed apps; returns the count. */
static inline int app_scan(struct AppEntry *out, int max) {
    struct dirent ents[2 * 16];
    int n = readdir("/Apps", ents, 2 * 16);
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        if (ents[i].type != FT_DIR || !applist_ends_app(ents[i].name)) continue;
        char base[96]; applist_join(base, "/Apps", ents[i].name);
        char mpath[128]; applist_join(mpath, base, "manifest");
        char buf[1024]; int fd = fopen(mpath, O_RDONLY);
        if (fd < 0) continue;
        int mn = fread_(fd, buf, sizeof buf - 1); fclose_(fd);
        if (mn <= 0) continue;
        buf[mn] = 0;
        char val[96];
        if (!manifest_get(buf, "name", val, sizeof val)) continue;
        int j = 0; for (; val[j] && j < 23; j++) out[cnt].name[j] = val[j]; out[cnt].name[j] = 0;
        if (!manifest_get(buf, "exec", val, sizeof val)) continue;
        applist_join(out[cnt].exec, base, val);
        out[cnt].icon = 0; out[cnt].iw = out[cnt].ih = 0;
        char iconrel[64];
        if (manifest_get(buf, "icon", iconrel, sizeof iconrel) && iconrel[0]) {
            char ipath[176]; applist_join(ipath, base, iconrel);
            out[cnt].icon = applist_load_icon(ipath, &out[cnt].iw, &out[cnt].ih);
        }
        cnt++;
    }
    return cnt;
}

/* Case-insensitive "does name contain query" (empty query matches everything).
 * The matcher itself is the pure tu_ci_contains (unit-tested in tests/unit). */
static inline int app_match(const char *name, const char *q) { return tu_ci_contains(name, q); }
