/* files -- shared helper implementations (see filesutil.h). */
#include "ui.h"        /* ugfx rasterizer + libc str/mem + the file API (extern "C") */
#include "icons.h"     /* FILEICON_* + fileicons_argb                                */
#include "filesutil.h"

int eqn(const char *a, const char *b) { return strcmp(a, b) == 0; }
int endsw(const char *s, const char *suf) {
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}
int hasdot(const char *s) { for (; *s; s++) if (*s == '.') return 1; return 0; }
int is_app_dir(int type, const char *n) { return type == FT_DIR && endsw(n, ".app"); }

int file_icon_for(int type, const char *n) {
    if (type == FT_DIR) return FILEICON_FOLDER;
    if (endsw(n, ".txt") || eqn(n, "readme") || eqn(n, "motd") ||
        eqn(n, "guide") || eqn(n, "notes") || eqn(n, "shortcuts")) return FILEICON_TEXT;
    if (endsw(n, ".argb") || endsw(n, ".img") || endsw(n, ".png") || endsw(n, ".bmp")) return FILEICON_IMAGE;
    if (endsw(n, ".md") || endsw(n, ".csv") || endsw(n, ".cfg")) return FILEICON_TEXT;
    if (!hasdot(n)) return FILEICON_EXEC;
    return FILEICON_FILE;
}
const char *kind_for(int type, const char *n) {
    if (is_app_dir(type, n)) return "Application";
    if (type == FT_DIR) return "Folder";
    if (endsw(n, ".txt") || endsw(n, ".md") || eqn(n, "readme") || eqn(n, "motd") ||
        eqn(n, "guide") || eqn(n, "notes")) return "Text Document";
    if (endsw(n, ".argb") || endsw(n, ".img") || endsw(n, ".png") || endsw(n, ".bmp")) return "Image";
    if (!hasdot(n)) return "Executable";
    return "Document";
}
void disp_name(const char *name, char *out, int cap) {       /* strip a trailing ".app" */
    int n = (int)strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".app") == 0) n -= 4;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = name[i];
    out[n] = 0;
}
void ext_of(const char *name, char *out, int cap) {          /* lowercased extension, or "" */
    int dot = -1; for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    int j = 0;
    if (dot >= 0) for (int i = dot + 1; name[i] && j < cap - 1; i++) {
        char c = name[i]; if (c >= 'A' && c <= 'Z') c += 32; out[j++] = c;
    }
    out[j] = 0;
}
void join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (int i = 0; dir[i] && n < cap - 1; i++) out[n++] = dir[i];
    if (n == 0 || out[n - 1] != '/') { if (n < cap - 1) out[n++] = '/'; }
    for (int i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = 0;
}
void blit_scaled(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh) {
    ugfx_blit_scaled(dx, dy, dw, dh, src, sw, sh);
}
uint32_t *load_icon_argb(const char *path, int *w, int *h) {
    int fd = fopen(path, O_RDONLY);
    if (fd < 0) return 0;
    uint32_t hdr[2] = { 0, 0 };
    /* cap 1024: covers app icons and Quick-Look-sized pictures (4 MB decoded) */
    if (fread_(fd, (char *)hdr, 8) != 8 || !hdr[0] || !hdr[1] || hdr[0] > 1024 || hdr[1] > 1024) { fclose_(fd); return 0; }
    int need = (int)(hdr[0] * hdr[1] * 4), got = 0;
    uint32_t *px = (uint32_t *)malloc((unsigned)need);
    if (!px) { fclose_(fd); return 0; }
    while (got < need) { int r = fread_(fd, (char *)px + got, need - got); if (r <= 0) break; got += r; }
    fclose_(fd);
    if (got != need) { free(px); return 0; }
    *w = (int)hdr[0]; *h = (int)hdr[1];
    return px;
}

/* thin vector strokes for the toolbar glyphs (ugfx has no line primitive) */
static void plot(int x, int y, int t, uint32_t c) { ugfx_fill(x - t / 2, y - t / 2, t, t, c); }
void vline_(int x0, int y0, int x1, int y1, int t, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0, n = dx < 0 ? -dx : dx, m = dy < 0 ? -dy : dy, s = n > m ? n : m;
    if (s < 1) s = 1;
    for (int i = 0; i <= s; i++) plot(x0 + dx * i / s, y0 + dy * i / s, t, c);
}
void draw_glyph(int g, int cx, int cy, int r, uint32_t c) {
    int t = 2;
    if (g == G_BACK) { vline_(cx + r / 2, cy - r, cx - r / 2, cy, t, c); vline_(cx - r / 2, cy, cx + r / 2, cy + r, t, c); }
    else if (g == G_FWD) { vline_(cx - r / 2, cy - r, cx + r / 2, cy, t, c); vline_(cx + r / 2, cy, cx - r / 2, cy + r, t, c); }
    else if (g == G_UP) { vline_(cx, cy + r, cx, cy - r, t, c); vline_(cx, cy - r, cx - r, cy, t, c); vline_(cx, cy - r, cx + r, cy, t, c); }
    else if (g == G_NEWF) {                                   /* folder + plus */
        ugfx_fill(cx - r, cy - 1, 2 * r, r, c);
        ugfx_fill(cx - r, cy - 4, r, 4, c);
        vline_(cx + r - 1, cy + r - 1, cx + r - 1, cy + r + 5, t, c);   /* + vertical */
        vline_(cx + r - 4, cy + r + 2, cx + r + 2, cy + r + 2, t, c);   /* + horizontal */
    }
    else if (g == G_TRASH) {                                  /* can */
        ugfx_fill(cx - r + 1, cy - r + 3, 2 * r - 2, 2, c);   /* lid  */
        ugfx_fill(cx - 3, cy - r, 6, 2, c);                   /* handle */
        ugfx_frame(cx - r + 2, cy - r + 5, 2 * r - 4, 2 * r - 4, c);    /* body */
        vline_(cx - 1, cy - r + 7, cx - 1, cy + r - 1, 1, c);
        vline_(cx + 2, cy - r + 7, cx + 2, cy + r - 1, 1, c);
    }
    else if (g == G_LOCK) {                                   /* padlock: shackle + solid body */
        int sh = r - 1; if (sh < 2) sh = 2;                  /* shackle height */
        int by  = cy - (2 * r + 1) / 2 + sh;                 /* body top (centres the whole lock on cy) */
        int top = by - sh;                                   /* shackle top */
        int sx0 = cx - r + 1, sx1 = cx + r - 1;              /* shackle posts */
        vline_(sx0, by, sx0, top, 1, c);
        vline_(sx1, by, sx1, top, 1, c);
        vline_(sx0, top, sx1, top, 1, c);
        ugfx_fill(cx - r, by, 2 * r + 1, r + 2, c);          /* lock body */
    }
}
