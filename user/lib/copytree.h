/* copytree.h -- the recursive copy + remove primitives behind the install engine
 * (design/packaging.md) and, later, the OS installer. Userspace, syscall-backed
 * (readdir / mkdir / fopen / fread_ / fwrite_ / funlink / rmdir), header-only so the
 * `tos` CLI and any future installer share one implementation. */
#pragma once
#include "ulib.h"

/* Join "<a>/<b>" into out[cap] (a "" / trailing-slash handled), NUL-terminated. */
static inline void ct_join(char *out, int cap, const char *a, const char *b) {
    int n = 0;
    for (int i = 0; a[i] && n < cap - 1; i++) out[n++] = a[i];
    if (n == 0 || out[n - 1] != '/') { if (n < cap - 1) out[n++] = '/'; }
    for (int i = 0; b[i] && n < cap - 1; i++) out[n++] = b[i];
    out[n] = 0;
}

/* The last path component of `path` (the basename), copied into out[cap]. */
static inline void ct_basename(char *out, int cap, const char *path) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    int n = 0;
    for (int i = last + 1; path[i] && n < cap - 1; i++) out[n++] = path[i];
    out[n] = 0;
}

/* Copy a single file src -> dst (dst created/truncated). 0 ok, -1 on any error. */
static inline int ct_copy_file(const char *src, const char *dst) {
    int in = fopen(src, O_RDONLY);
    if (in < 0) return -1;
    int out = fopen(dst, O_CREATE | O_TRUNC);
    if (out < 0) { fclose_(in); return -1; }
    char buf[512]; int n, rc = 0;
    while ((n = fread_(in, buf, sizeof buf)) > 0)
        if (fwrite_(out, buf, n) != n) { rc = -1; break; }
    fclose_(in); fclose_(out);
    return rc;
}

/* Recursively copy src -> dst (a file, or a directory tree). Creates dst dirs as it
 * goes (an already-existing dst dir is reused). Returns 0, or -1 on the first error.
 * Bundles are shallow, so a single 64-entry readdir per directory is enough. */
static inline int copytree(const char *src, const char *dst) {
    struct fstat st;
    if (stat_(src, &st) < 0) return -1;
    if (st.type == FT_FILE) return ct_copy_file(src, dst);
    if (mkdir(dst) < 0) {                                  /* may already exist */
        struct fstat d;
        if (stat_(dst, &d) < 0 || d.type != FT_DIR) return -1;
    }
    struct dirent ents[64];
    int n = readdir(src, ents, 64);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        char s[256], d[256];
        ct_join(s, sizeof s, src, ents[i].name);
        ct_join(d, sizeof d, dst, ents[i].name);
        if (copytree(s, d) < 0) return -1;
    }
    return 0;
}

/* Recursively delete `path` (best-effort on children; the final rmdir fails if any
 * child could not be removed -- e.g. a system-owned file -> the caller sees -1).
 * Returns 0 on a clean removal, -1 otherwise. */
static inline int rmtree(const char *path) {
    struct fstat st;
    if (stat_(path, &st) < 0) return -1;
    if (st.type != FT_DIR) return funlink(path);
    struct dirent ents[64];
    int n = readdir(path, ents, 64);
    for (int i = 0; i < n; i++) {
        char c[256];
        ct_join(c, sizeof c, path, ents[i].name);
        rmtree(c);                                        /* best-effort */
    }
    return rmdir(path);
}
