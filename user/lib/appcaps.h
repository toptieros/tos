/* appcaps.h -- map a .app bundle's manifest `caps = a, b, c` list onto the kernel's
 * CAP_* bitmask (cap.h via syscall.h), so the trusted launcher (twm) can drop a
 * freshly-forked child to exactly its declared capabilities before exec. A bundle
 * that declares no `caps` line gets CAP_NORMAL -- every low-risk capability, none of
 * the dangerous ones (net/notify). See design/app-runtime.md. Header-only so the
 * launcher links it without a new translation unit. */
#pragma once
#include "ulib.h"       /* CAP_* (via syscall.h), fopen/fread_/fclose_, O_RDONLY */
#include "manifest.h"   /* manifest_get */

/* one cap token ("window", "fs:home", "notify", ...) of length n -> its bit, or 0 */
static inline unsigned appcap_bit(const char *t, int n) {
    static const struct { const char *name; unsigned bit; } TBL[] = {
        { "window", CAP_WINDOW }, { "fs:bundle", CAP_FS_BUNDLE }, { "fs:home", CAP_FS_HOME },
        { "fs:system", CAP_FS_SYSTEM }, { "time", CAP_TIME }, { "spawn", CAP_SPAWN },
        { "net", CAP_NET }, { "notify", CAP_NOTIFY },
    };
    for (unsigned i = 0; i < sizeof TBL / sizeof TBL[0]; i++) {
        const char *nm = TBL[i].name; int j = 0;
        while (j < n && nm[j] && nm[j] == t[j]) j++;
        if (j == n && nm[j] == 0) return TBL[i].bit;
    }
    return 0;                                       /* unknown token: ignored (forward-compatible) */
}

/* "window, notify, fs:home" -> the OR of the named bits */
static inline unsigned appcaps_from_csv(const char *s) {
    unsigned m = 0;
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        int n = 0; while (s[n] && s[n] != ',' && s[n] != ' ' && s[n] != '\t') n++;
        if (!n) break;
        m |= appcap_bit(s, n); s += n;
    }
    return m;
}

/* The caps for the bundle that owns `exec` (an absolute path like
 * /Apps/Foo.app/bin/foo): strip the trailing "/bin/<name>" to the bundle root and
 * read its manifest's `caps`. Defaults to CAP_NORMAL when there's no bundle, no
 * manifest, or no `caps` line -- so today's manifests keep working unchanged. */
static inline unsigned appcaps_for_exec(const char *exec) {
    char p[256]; int n = 0;
    while (exec[n] && n < (int)sizeof(p) - 1) { p[n] = exec[n]; n++; }
    p[n] = 0;
    int cut = 0;                                    /* drop the last two path components */
    for (int i = n - 1; i >= 0; i--)
        if (p[i] == '/') { if (++cut == 2) { p[i] = 0; break; } }
    if (cut < 2) return CAP_NORMAL;                 /* not a /Apps/<X>.app/bin/<y> path */
    char mp[288]; int k = 0;
    for (int i = 0; p[i] && k < (int)sizeof(mp) - 10; i++) mp[k++] = p[i];
    const char *suf = "/manifest";
    for (int i = 0; suf[i] && k < (int)sizeof(mp) - 1; i++) mp[k++] = suf[i];
    mp[k] = 0;
    int fd = fopen(mp, O_RDONLY);
    if (fd < 0) return CAP_NORMAL;
    char buf[1024]; int mn = fread_(fd, buf, sizeof buf - 1); fclose_(fd);
    if (mn <= 0) return CAP_NORMAL;
    buf[mn] = 0;
    char val[160];
    if (!manifest_get(buf, "caps", val, sizeof val)) return CAP_NORMAL;
    return appcaps_from_csv(val);
}
