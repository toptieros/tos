/* Pure codec for the file-picker request channel (design/file-picker.md). The
 * system Open/Save dialog is the Files app launched in a *picker mode*: the caller
 * writes a key=value request to /tmp/.picker-req, Files reads it, and the chosen
 * path comes back via /tmp/.picker-res. This header owns only the *pure* part --
 * the request struct, its key=value encode/parse, and the extension-filter
 * predicate -- with NO OS dependencies (no syscalls, no string.h), like
 * textutil.h, so it compiles on the host and is unit-tested directly in
 * tests/unit. The OS-side wrappers (sys_pick_begin/poll/req) live in sys.{h,c}. */
#pragma once

enum { PICK_OPEN = 0, PICK_SAVE = 1 };

/* A picker request. Field sizes match the temp-file blob; values never contain
 * newlines (paths/names don't), so a line-based key=value form is lossless. */
struct pick_req {
    int  mode;          /* PICK_OPEN | PICK_SAVE                                  */
    char dir[192];      /* start directory (empty => the app picks ~)             */
    char name[64];      /* suggested filename (save mode; ignored for open)       */
    char ext[64];       /* "txt,md" allowed-extension list; empty = all files     */
    char title[32];     /* window title (optional; empty => default Open/Save As) */
};

/* ---- tiny self-contained string helpers (no libc, host + tOS) ---------------- */
static inline int  pr_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline char pr_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
/* bounded copy of src into dst[cap], NUL-terminated; returns chars written */
static inline int pr_cpy(char *dst, int cap, const char *src) {
    int i = 0; for (; src[i] && i < cap - 1; i++) dst[i] = src[i]; dst[i] = 0; return i;
}
/* append "key=value\n" to out[cap] starting at *pos (bumped); silently truncates */
static inline void pr_kv(char *out, int cap, int *pos, const char *key, const char *val) {
    int p = *pos;
    for (int i = 0; key[i] && p < cap - 1; i++) out[p++] = key[i];
    if (p < cap - 1) out[p++] = '=';
    for (int i = 0; val[i] && p < cap - 1; i++) out[p++] = val[i];
    if (p < cap - 1) out[p++] = '\n';
    out[p] = 0; *pos = p;
}

/* Encode a request into the key=value blob. Returns the byte count written. The
 * mode is emitted as the word "open"/"save"; all five keys are always present so
 * the form is deterministic (a missing value is just an empty line, forgiven on
 * parse). */
static inline int pickreq_encode(const struct pick_req *r, char *out, int cap) {
    int p = 0;
    pr_kv(out, cap, &p, "mode",  r->mode == PICK_SAVE ? "save" : "open");
    pr_kv(out, cap, &p, "dir",   r->dir);
    pr_kv(out, cap, &p, "name",  r->name);
    pr_kv(out, cap, &p, "ext",   r->ext);
    pr_kv(out, cap, &p, "title", r->title);
    return p;
}

/* Parse a key=value blob into *out (zeroed first). Unknown keys are ignored
 * (forward-compat). Returns 1 if at least one "key=" line was seen, else 0. */
static inline int pickreq_parse(const char *blob, struct pick_req *out) {
    out->mode = PICK_OPEN; out->dir[0] = out->name[0] = out->ext[0] = out->title[0] = 0;
    int any = 0, i = 0;
    while (blob[i]) {
        /* one line: key up to '=', value up to '\n' */
        char key[16]; int k = 0;
        while (blob[i] && blob[i] != '=' && blob[i] != '\n' && k < 15) key[k++] = blob[i++];
        key[k] = 0;
        if (blob[i] != '=') {                          /* no '=' on this line: skip it */
            while (blob[i] && blob[i] != '\n') i++;
            if (blob[i] == '\n') i++;
            continue;
        }
        i++;                                           /* consume '=' */
        const char *val = blob + i;
        int vl = 0; while (val[vl] && val[vl] != '\n') vl++;
        any = 1;
        char *dst = 0; int dcap = 0;
        if      (key[0]=='m'&&key[1]=='o') { out->mode = (val[0]=='s') ? PICK_SAVE : PICK_OPEN; }
        else if (key[0]=='d'&&key[1]=='i') { dst = out->dir;   dcap = (int)sizeof out->dir;   }
        else if (key[0]=='n'&&key[1]=='a') { dst = out->name;  dcap = (int)sizeof out->name;  }
        else if (key[0]=='e'&&key[1]=='x') { dst = out->ext;   dcap = (int)sizeof out->ext;   }
        else if (key[0]=='t'&&key[1]=='i') { dst = out->title; dcap = (int)sizeof out->title; }
        if (dst) { int j = 0; for (; j < vl && j < dcap - 1; j++) dst[j] = val[j]; dst[j] = 0; }
        i += vl; if (blob[i] == '\n') i++;
    }
    return any;
}

/* The extension-filter predicate: does `name` pass the comma-separated allowed
 * `ext` list? An empty list matches everything. Matching is case-insensitive on
 * the text after the final '.'; an extensionless name never matches a non-empty
 * list. Directories are shown regardless and are the caller's concern, not this. */
static inline int pickreq_ext_match(const char *name, const char *ext_csv) {
    if (!ext_csv[0]) return 1;
    int dot = -1; for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    if (dot < 0) return 0;                              /* no extension */
    const char *ne = name + dot + 1;                   /* the name's extension */
    int t = 0;                                          /* walk csv tokens */
    while (ext_csv[t]) {
        while (ext_csv[t] == ',' || ext_csv[t] == ' ' || ext_csv[t] == '.') t++;
        if (!ext_csv[t]) break;
        int j = 0, match = 1;                           /* compare token vs ne */
        while (ext_csv[t + j] && ext_csv[t + j] != ',') {
            char a = pr_lc(ext_csv[t + j]), b = pr_lc(ne[j]);
            if (a != b) match = 0;
            j++;
        }
        if (match && ne[j] == 0) return 1;              /* token fully equals ne */
        t += j;
    }
    return 0;
}
