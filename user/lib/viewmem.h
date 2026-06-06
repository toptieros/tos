/* Per-folder view memory for the Files app (design/files-app.md §2). Finder/Dolphin
 * remember each folder's view mode, sort and zoom; we persist that in the registry as
 * ONE value per folder (so a folder costs a single entry, not five) plus a global
 * "view.default" used for folders never visited. Both the value codec and the key
 * derivation are pure -- no OS deps, like filesort.h / pathbar.h -- so they compile on
 * the host and are unit-tested directly (tests/unit/t_viewmem). Files calls these from
 * load_path() (restore) and set_view/set_sort/set_zoom (persist). */
#pragma once

#define VIEWMEM_KEYMAX 64   /* must match REG_KEYMAX in registry.h */

/* The remembered state for one folder. Defaults match the app's startup view:
 * list mode, sort by name ascending, folders first, "actual size" zoom. */
struct view_prefs { int mode, sort_key, sort_desc, sort_ff, zoom; };

static inline struct view_prefs viewmem_defaults(void) {
    struct view_prefs v = { 0 /*list*/, 0 /*name*/, 0 /*asc*/, 1 /*folders first*/, 1 /*actual size*/ };
    return v;
}

/* FNV-1a 32-bit, rendered below as 8 lowercase hex -- the fallback key for paths too
 * long to embed literally. */
static inline unsigned viewmem_hash(const char *s) {
    unsigned h = 2166136261u;
    for (int i = 0; s[i]; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

/* Build the registry key for a folder's view state. Uses the human-readable
 * "view.<path>" when it fits in REG_KEYMAX; otherwise a stable "view~<hash8>" so any
 * folder -- however deep -- is still addressable (REG_KEYMAX can't hold a 255-char
 * path). out must be at least VIEWMEM_KEYMAX bytes. */
static inline void viewmem_key(const char *path, char *out, int outsz) {
    int pl = 5;                                       /* strlen("view.") */
    int n = 0; while (path[n]) n++;
    if (pl + n < VIEWMEM_KEYMAX && pl + n < outsz) {
        const char *pfx = "view.";
        int j = 0; for (; pfx[j]; j++) out[j] = pfx[j];
        for (int i = 0; path[i]; i++) out[j++] = path[i];
        out[j] = 0;
        return;
    }
    static const char hx[] = "0123456789abcdef";
    unsigned h = viewmem_hash(path);
    const char *pfx = "view~";
    int j = 0; for (; pfx[j] && j < outsz - 1; j++) out[j] = pfx[j];
    for (int s = 28; s >= 0 && j < outsz - 1; s -= 4) out[j++] = hx[(h >> s) & 0xf];
    out[j] = 0;
}

/* Encode the prefs as "m;sk;sd;ff;z" -- always all five fields, comfortably under
 * REG_VALMAX. */
static inline void viewmem_encode(const struct view_prefs *v, char *out, int outsz) {
    int vals[5] = { v->mode, v->sort_key, v->sort_desc, v->sort_ff, v->zoom };
    int o = 0;
    for (int f = 0; f < 5; f++) {
        if (f && o < outsz - 1) out[o++] = ';';
        int x = vals[f];
        if (x < 0 && o < outsz - 1) { out[o++] = '-'; x = -x; }
        char tmp[12]; int t = 0;
        if (x == 0) tmp[t++] = '0';
        while (x) { tmp[t++] = (char)('0' + x % 10); x /= 10; }
        while (t && o < outsz - 1) out[o++] = tmp[--t];
    }
    out[o < outsz ? o : outsz - 1] = 0;
}

/* Decode "m;sk;sd;ff;z" back into prefs; an empty/missing string yields the defaults
 * (and any field the string omits keeps its default). */
static inline struct view_prefs viewmem_decode(const char *s) {
    struct view_prefs v = viewmem_defaults();
    if (!s || !s[0]) return v;
    int *fields[5] = { &v.mode, &v.sort_key, &v.sort_desc, &v.sort_ff, &v.zoom };
    const char *p = s;
    for (int f = 0; f < 5; f++) {
        while (*p == ' ') p++;
        if (*p != '-' && (*p < '0' || *p > '9')) break;   /* nothing left to parse */
        int neg = 0; if (*p == '-') { neg = 1; p++; }
        int val = 0;
        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
        *fields[f] = neg ? -val : val;
        if (*p == ';') p++; else break;
    }
    return v;
}
