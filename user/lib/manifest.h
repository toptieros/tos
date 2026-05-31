/* Minimal key=value manifest reader for .app bundles (see
 * design/app-package-format.md). One `key = value` per line; blank lines and
 * lines beginning with '#' are ignored; whitespace around the key and the value
 * is trimmed. Header-only (static inline) so twm and any future launcher can
 * share it without a separate translation unit. */
#pragma once
#include "ulib.h"

/* Copy the value for `key` out of the manifest text `buf` into out[outsz].
 * Returns 1 if the key was found, 0 otherwise. */
static inline int manifest_get(const char *buf, const char *key, char *out, int outsz) {
    int klen = 0; while (key[klen]) klen++;
    const char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;            /* leading space */
        if (*p == '#' || *p == '\n' || *p == '\r') {    /* comment / blank line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        int i = 0;
        while (i < klen && p[i] == key[i]) i++;
        if (i == klen) {                                /* key prefix matched */
            const char *q = p + i;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {                            /* ...and it's really "key =" */
                q++;
                while (*q == ' ' || *q == '\t') q++;
                int j = 0;
                while (*q && *q != '\n' && *q != '\r' && j < outsz - 1) out[j++] = *q++;
                while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\t')) j--;  /* trim trailing */
                out[j] = 0;
                return 1;
            }
        }
        while (*p && *p != '\n') p++;                   /* not our key: next line */
        if (*p == '\n') p++;
    }
    return 0;
}
