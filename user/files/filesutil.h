/* files -- small path / name / icon helpers + the toolbar glyph strokes, shared by
 * the file manager's widgets (fileswidgets.h) and its app logic (files.cpp). */
#pragma once
#include <stdint.h>

int  eqn(const char *a, const char *b);
int  endsw(const char *s, const char *suf);
int  hasdot(const char *s);
int  is_app_dir(int type, const char *n);
int  file_icon_for(int type, const char *n);
const char *kind_for(int type, const char *n);
void disp_name(const char *name, char *out, int cap);   /* strip a trailing ".app"        */
void ext_of(const char *name, char *out, int cap);      /* lowercased extension, or ""     */
void join(char *out, int cap, const char *dir, const char *name);
/* smooth (area/bilinear) alpha-blended scale of a sw*sh ARGB image into a dst box;
 * forwards to the shared ugfx resampler so file-list icons get crisp scaling */
void blit_scaled(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh);
uint32_t *load_icon_argb(const char *path, int *w, int *h);

/* thin vector strokes for the toolbar glyphs (ugfx has no line primitive) */
enum { G_BACK, G_FWD, G_UP, G_NEWF, G_TRASH };
void vline_(int x0, int y0, int x1, int y1, int t, uint32_t c);
void draw_glyph(int g, int cx, int cy, int r, uint32_t c);
