/* Pure directory-sort comparator for the Files app (design/files-app.md §2). The
 * sort key (Name/Kind/Size) + direction + folders-first rule is plain arithmetic
 * with NO OS dependencies -- like textutil.h / pathbar.h -- so it compiles on the
 * host and is unit-tested directly in tests/unit. Files calls filesort_cmp() from
 * its readdir sort. (Date sort waits on fs timestamps -- see the supporting-stack
 * section of the design doc.) */
#pragma once

enum { FSORT_NAME = 0, FSORT_KIND = 1, FSORT_SIZE = 2 };

static inline char fs_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Natural, case-insensitive name compare so "file2" < "file10" (a digit run is
 * compared by numeric value, not character-by-character). Returns <0/0/>0. */
static inline int fs_natcmp(const char *a, const char *b) {
    int i = 0, j = 0;
    while (a[i] && b[j]) {
        char ca = a[i], cb = b[j];
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            while (a[i] == '0') i++;                    /* skip leading zeros */
            while (b[j] == '0') j++;
            int da = i, db = j;
            while (a[i] >= '0' && a[i] <= '9') i++;
            while (b[j] >= '0' && b[j] <= '9') j++;
            int la = i - da, lb = j - db;
            if (la != lb) return la < lb ? -1 : 1;     /* more digits => larger number */
            for (int k = 0; k < la; k++) if (a[da + k] != b[db + k]) return a[da + k] < b[db + k] ? -1 : 1;
            continue;                                  /* equal numeric run; keep going */
        }
        char x = fs_lc(ca), y = fs_lc(cb);
        if (x != y) return x < y ? -1 : 1;
        i++; j++;
    }
    if (a[i]) return 1;
    if (b[j]) return -1;
    return 0;
}

/* lowercased extension (text after the final '.'), or "" if none */
static inline const char *fs_ext(const char *n) {
    const char *e = "";
    for (int i = 0; n[i]; i++) if (n[i] == '.') e = n + i + 1;
    return e;
}
static inline int fs_extcmp(const char *a, const char *b) {
    const char *ea = fs_ext(a), *eb = fs_ext(b);
    int i = 0;
    for (; ea[i] && eb[i]; i++) { char x = fs_lc(ea[i]), y = fs_lc(eb[i]); if (x != y) return x < y ? -1 : 1; }
    if (ea[i]) return 1;
    if (eb[i]) return -1;
    return 0;
}

/* The directory comparator: returns <0 if entry a sorts before b. `key` is FSORT_*,
 * `desc` reverses the order, `folders_first` keeps directories grouped at the top
 * regardless of direction (Finder/Dolphin default). Name is the tiebreak for Kind
 * and Size so the order is stable. */
static inline int filesort_cmp(const char *an, int a_isdir, unsigned asz,
                               const char *bn, int b_isdir, unsigned bsz,
                               int key, int desc, int folders_first) {
    if (folders_first && (a_isdir != b_isdir)) return a_isdir ? -1 : 1;  /* dirs first, before the direction flip */
    int c;
    if (key == FSORT_SIZE)      { c = (asz < bsz) ? -1 : (asz > bsz) ? 1 : 0; if (c == 0) c = fs_natcmp(an, bn); }
    else if (key == FSORT_KIND) { c = fs_extcmp(an, bn); if (c == 0) c = fs_natcmp(an, bn); }
    else                        { c = fs_natcmp(an, bn); }
    return desc ? -c : c;
}
