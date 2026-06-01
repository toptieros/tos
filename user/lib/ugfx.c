#include "ugfx.h"
#include "ulib.h"
#include "sysfont.h"     /* shared system font, from kernel/ (the user build adds -Ikernel) */

/* The current drawing target (framebuffer or a window surface) and a saved copy
 * of the framebuffer target so the compositor can switch back with ugfx_use_fb. */
static volatile uint32_t *FB;
static int W, H, PITCH;                      /* target geometry */
static volatile uint32_t *fb_buf;
static int fb_w, fb_h, fb_pitch;

/* Clip rectangle (target-space, exclusive on the high edge). Every primitive
 * stays inside it; the compositor sets it to a damage rect so a repaint touches
 * no pixels outside the region being refreshed. Reset whenever the target moves. */
static int clx0, cly0, clx1, cly1;
static void clip_reset(void) { clx0 = 0; cly0 = 0; clx1 = W; cly1 = H; }

int ugfx_init(void) {
    struct fbinfo fb;
    if (fbinfo(&fb) < 0 || !fb.present) return -1;
    fb_buf = (volatile uint32_t *)fb.vaddr;
    fb_w = (int)fb.width; fb_h = (int)fb.height; fb_pitch = (int)fb.pitch;
    FB = fb_buf; W = fb_w; H = fb_h; PITCH = fb_pitch;
    clip_reset();
    return 0;
}

void ugfx_set_target(uint32_t *buf, int w, int h, int pitch) {
    FB = (volatile uint32_t *)buf; W = w; H = h; PITCH = pitch;
    clip_reset();
}
void ugfx_use_fb(void) { FB = fb_buf; W = fb_w; H = fb_h; PITCH = fb_pitch; clip_reset(); }

void ugfx_set_clip(int x, int y, int w, int h) {
    clx0 = x < 0 ? 0 : x;  cly0 = y < 0 ? 0 : y;
    clx1 = x + w > W ? W : x + w;  cly1 = y + h > H ? H : y + h;
    if (clx1 < clx0) clx1 = clx0;
    if (cly1 < cly0) cly1 = cly0;
}
void ugfx_clip_none(void) { clip_reset(); }
static int in_clip(int x, int y) { return x >= clx0 && x < clx1 && y >= cly0 && y < cly1; }

int ugfx_width(void)  { return W; }
int ugfx_height(void) { return H; }

/* Copy an opaque sw*sh image (pitch spitch) onto the target at (x,y), clipped. */
void ugfx_blit(int x, int y, const uint32_t *src, int sw, int sh, int spitch) {
    for (int j = 0; j < sh; j++) {
        int dy = y + j;
        if (dy < cly0 || dy >= cly1) continue;
        const uint32_t *srow = src + (long)j * spitch;
        for (int i = 0; i < sw; i++) {
            int dx = x + i;
            if (dx >= clx0 && dx < clx1) FB[(long)dy * PITCH + dx] = srow[i];
        }
    }
}
int ugfx_font_w(void) { return SYSFONT_W; }
int ugfx_font_h(void) { return SYSFONT_H; }
int ugfx_text_w(const char *s) { int n = 0; while (*s++) n++; return n * SYSFONT_W; }

/* Blend fg over bg by 8-bit coverage (anti-aliasing). */
static uint32_t blend(uint32_t bg, uint32_t fg, uint32_t a) {
    uint32_t ia = 255 - a;
    uint32_t r = (((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * ia) / 255;
    uint32_t g = (((fg >> 8)  & 0xff) * a + ((bg >> 8)  & 0xff) * ia) / 255;
    uint32_t b = (( fg        & 0xff) * a + ( bg        & 0xff) * ia) / 255;
    return (r << 16) | (g << 8) | b;
}

void ugfx_pixel(int x, int y, uint32_t c) {
    if (in_clip(x, y)) FB[(long)y * PITCH + x] = c;
}

void ugfx_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < clx0 ? clx0 : x, y0 = y < cly0 ? cly0 : y;
    int x1 = x + w > clx1 ? clx1 : x + w, y1 = y + h > cly1 ? cly1 : y + h;
    for (int j = y0; j < y1; j++) {
        volatile uint32_t *row = FB + (long)j * PITCH;
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}

void ugfx_frame(int x, int y, int w, int h, uint32_t c) {
    ugfx_fill(x, y, w, 1, c);
    ugfx_fill(x, y + h - 1, w, 1, c);
    ugfx_fill(x, y, 1, h, c);
    ugfx_fill(x + w - 1, y, 1, h, c);
}

/* Filled rectangle with rounded corners. Fills only the rounded shape itself
 * (each row inset by the corner arc), so it must be drawn over an already
 * painted background -- the corners simply let that background show through. */
void ugfx_rrect(int x, int y, int w, int h, int rad, uint32_t c) {
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    for (int j = 0; j < h; j++) {
        int dy = -1;
        if (j < rad)            dy = rad - 1 - j;       /* top corners    */
        else if (j >= h - rad)  dy = j - (h - rad);     /* bottom corners */
        int inset = 0;
        if (dy >= 0) {
            int cut = rad, d2 = rad * rad - dy * dy;
            while (cut * cut > d2) cut--;               /* cut = floor(sqrt(d2)) */
            inset = rad - cut;
        }
        ugfx_fill(x + inset, y + j, w - 2 * inset, 1, c);
    }
}

void ugfx_clear(uint32_t c) { ugfx_fill(0, 0, W, H, c); }

void ugfx_char(int x, int y, char ch, uint32_t fg, uint32_t bg) {
    if ((uint8_t)ch < SYSFONT_FIRST || (uint8_t)ch >= SYSFONT_FIRST + SYSFONT_COUNT) ch = ' ';
    const uint8_t *g = sysfont[(uint8_t)ch - SYSFONT_FIRST];
    for (int r = 0; r < SYSFONT_H; r++)
        for (int col = 0; col < SYSFONT_W; col++) {
            uint8_t cov = g[r * SYSFONT_W + col];
            int px = x + col, py = y + r;
            if (bg == UGFX_TRANSPARENT) {
                if (cov == 0) continue;
                ugfx_pixel(px, py, cov == 255 ? fg : blend(ugfx_get(px, py), fg, cov));
            } else {
                ugfx_pixel(px, py, cov == 0 ? bg : cov == 255 ? fg : blend(bg, fg, cov));
            }
        }
}

void ugfx_text(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    for (; *s; s++) { ugfx_char(x, y, *s, fg, bg); x += SYSFONT_W; }
}

void ugfx_blit_argb(int x, int y, int w, int h, const uint32_t *argb) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t s = argb[j * w + i];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            int px = x + i, py = y + j;
            ugfx_pixel(px, py, a == 255 ? (s & 0xffffff)
                                        : blend(ugfx_get(px, py), s & 0xffffff, a));
        }
}

/* Smooth scaled ARGB blit: draw a sw*sh source into a dw*dh dest box. Down-scales
 * with an area (box) average, up-scales (and 1:1) with bilinear sampling -- both in
 * PREMULTIPLIED alpha so transparent icon edges don't bleed a dark halo. Replaces
 * the old nearest-neighbour path, which left visible jaggies on the launchpad and
 * the file-list icons. Sizes are tiny (icons), so the per-pixel cost is negligible. */
void ugfx_blit_scaled(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || !src) return;
    if (dw >= sw && dh >= sh) {                  /* up-scale or 1:1 -> bilinear */
        for (int j = 0; j < dh; j++) {
            long fy = ((long)(2 * j + 1) * sh * 32768) / dh - 32768;   /* (j+.5)*sh/dh-.5 in 16.16 */
            if (fy < 0) fy = 0;
            int y0 = (int)(fy >> 16), y1 = y0 + 1, wy = (int)(fy & 0xffff) >> 8;
            if (y0 >= sh) y0 = sh - 1;
            if (y1 >= sh) y1 = sh - 1;
            int py = dy + j;
            for (int i = 0; i < dw; i++) {
                long fx = ((long)(2 * i + 1) * sw * 32768) / dw - 32768;
                if (fx < 0) fx = 0;
                int x0 = (int)(fx >> 16), x1 = x0 + 1, wx = (int)(fx & 0xffff) >> 8;
                if (x0 >= sw) x0 = sw - 1;
                if (x1 >= sw) x1 = sw - 1;
                uint32_t s00 = src[(long)y0 * sw + x0], s10 = src[(long)y0 * sw + x1];
                uint32_t s01 = src[(long)y1 * sw + x0], s11 = src[(long)y1 * sw + x1];
                int wx1 = wx, wx0 = 256 - wx, wy1 = wy, wy0 = 256 - wy;       /* 8-bit weights */
                long w00 = (long)wx0 * wy0 * (s00 >> 24), w10 = (long)wx1 * wy0 * (s10 >> 24);
                long w01 = (long)wx0 * wy1 * (s01 >> 24), w11 = (long)wx1 * wy1 * (s11 >> 24);
                long aw = w00 + w10 + w01 + w11;                /* sum of weight*alpha */
                if (!aw) continue;
                long r = (w00 * ((s00 >> 16) & 0xff) + w10 * ((s10 >> 16) & 0xff)
                        + w01 * ((s01 >> 16) & 0xff) + w11 * ((s11 >> 16) & 0xff)) / aw;
                long g = (w00 * ((s00 >> 8) & 0xff) + w10 * ((s10 >> 8) & 0xff)
                        + w01 * ((s01 >> 8) & 0xff) + w11 * ((s11 >> 8) & 0xff)) / aw;
                long b = (w00 * (s00 & 0xff) + w10 * (s10 & 0xff)
                        + w01 * (s01 & 0xff) + w11 * (s11 & 0xff)) / aw;
                unsigned a = (unsigned)(aw / 65536);           /* weights sum to 65536 */
                if (a > 255) a = 255;
                uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                ugfx_pixel(dx + i, py, a >= 255 ? rgb : blend(ugfx_get(dx + i, py), rgb, a));
            }
        }
        return;
    }
    for (int j = 0; j < dh; j++) {               /* down-scale -> area (box) average */
        int sy0 = j * sh / dh, sy1 = (j + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sh) sy1 = sh;
        int py = dy + j;
        for (int i = 0; i < dw; i++) {
            int sx0 = i * sw / dw, sx1 = (i + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > sw) sx1 = sw;
            unsigned long sa = 0, sr = 0, sg = 0, sb = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    uint32_t s = src[(long)yy * sw + xx];
                    unsigned a = s >> 24;
                    sa += a;
                    sr += ((s >> 16) & 0xff) * a;
                    sg += ((s >> 8) & 0xff) * a;
                    sb += (s & 0xff) * a;
                }
            int n = (sx1 - sx0) * (sy1 - sy0);
            unsigned a = (unsigned)(sa / n);              /* coverage = mean alpha */
            if (!a) continue;
            uint32_t rgb = (uint32_t)((sr / sa) << 16 | (sg / sa) << 8 | (sb / sa));  /* premul */
            ugfx_pixel(dx + i, py, a >= 255 ? rgb : blend(ugfx_get(dx + i, py), rgb, a));
        }
    }
}

/* Blit a baked alpha mask (white ARGB; only the high alpha byte matters) recoloured
 * to `tint` -- used for the monochrome menu-bar status glyphs so one set of icons
 * tints to the theme ink or the accent. `tint`'s own alpha scales the coverage. */
void ugfx_blit_tint(int x, int y, int w, int h, const uint32_t *mask, uint32_t tint) {
    uint32_t rgb = tint & 0xffffff;
    unsigned ta = tint >> 24; if (!ta) ta = 255;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            unsigned ma = mask[j * w + i] >> 24;
            if (!ma) continue;
            unsigned a = ma * ta / 255;
            if (!a) continue;
            int px = x + i, py = y + j;
            ugfx_pixel(px, py, a >= 255 ? rgb : blend(ugfx_get(px, py), rgb, a));
        }
}

uint32_t ugfx_get(int x, int y) {
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H)
        return FB[(long)y * PITCH + x];
    return 0;
}

uint32_t ugfx_blend(uint32_t bg, uint32_t fg, uint32_t a) { return blend(bg, fg, a); }

/* Blend an ARGB colour (alpha in the high byte) over the target pixel at (x,y). */
static void blend_at(int x, int y, uint32_t argb) {
    uint32_t a = argb >> 24;
    if (!a || !in_clip(x, y)) return;
    long o = (long)y * PITCH + x;
    FB[o] = (a >= 255) ? (argb & 0xffffff) : blend(FB[o], argb & 0xffffff, a);
}

void ugfx_fill_a(int x, int y, int w, int h, uint32_t argb) {
    uint32_t a = argb >> 24;
    if (!a) return;
    if (a >= 255) { ugfx_fill(x, y, w, h, argb & 0xffffff); return; }
    uint32_t rgb = argb & 0xffffff;
    int x0 = x < clx0 ? clx0 : x, y0 = y < cly0 ? cly0 : y;
    int x1 = x + w > clx1 ? clx1 : x + w, y1 = y + h > cly1 ? cly1 : y + h;
    for (int j = y0; j < y1; j++) {
        volatile uint32_t *row = FB + (long)j * PITCH;
        for (int i = x0; i < x1; i++) row[i] = blend(row[i], rgb, a);
    }
}

void ugfx_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    if (h <= 0) return;
    int tr = (top >> 16) & 0xff, tg = (top >> 8) & 0xff, tb = top & 0xff;
    int br = (bot >> 16) & 0xff, bg = (bot >> 8) & 0xff, bb = bot & 0xff;
    int den = h > 1 ? h - 1 : 1;
    for (int j = 0; j < h; j++) {
        int r = tr + (br - tr) * j / den;
        int g = tg + (bg - tg) * j / den;
        int b = tb + (bb - tb) * j / den;
        ugfx_fill(x, y + j, w, 1, RGB(r, g, b));
    }
}

static uint32_t isqrt32(uint32_t v) {                 /* floor(sqrt(v)), no FPU */
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* Coverage (0..255) of pixel (i,j) inside a w*h rounded rect (corner radius rad),
 * by 4x4 supersampling the standard rounded-corner inside test (eighth-pixel
 * grid, integer only). Used for the corner bands; straight runs fill solidly. */
static int rrect_cov(int i, int j, int w, int h, int rad) {
    int r8 = rad * 8, w8 = w * 8, h8 = h * 8, rr = r8 * r8, inside = 0;
    for (int sy = 0; sy < 4; sy++) {
        int py = j * 8 + (2 * sy + 1);
        int qy = py < r8 ? r8 - py : (py > h8 - r8 ? py - (h8 - r8) : 0);
        for (int sx = 0; sx < 4; sx++) {
            int px = i * 8 + (2 * sx + 1);
            int qx = px < r8 ? r8 - px : (px > w8 - r8 ? px - (w8 - r8) : 0);
            if (qx * qx + qy * qy <= rr) inside++;
        }
    }
    return inside * 255 / 16;
}

/* Filled rounded rect at the given alpha, AA at the corners. Straight bands are
 * solid fills; only the four rad*rad corner boxes are supersampled + blended. */
static void rrect_core(int x, int y, int w, int h, int rad, uint32_t rgb, int alpha) {
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    if (rad < 0) rad = 0;
    uint32_t base = ((uint32_t)alpha << 24) | (rgb & 0xffffff);
    ugfx_fill_a(x + rad, y, w - 2 * rad, rad, base);             /* top straight band   */
    ugfx_fill_a(x, y + rad, w, h - 2 * rad, base);              /* middle              */
    ugfx_fill_a(x + rad, y + h - rad, w - 2 * rad, rad, base);  /* bottom straight band */
    for (int cy = 0; cy < 2; cy++)
        for (int cx = 0; cx < 2; cx++) {
            int bx = cx ? (w - rad) : 0, by = cy ? (h - rad) : 0;
            for (int j = 0; j < rad; j++)
                for (int i = 0; i < rad; i++) {
                    int cov = rrect_cov(bx + i, by + j, w, h, rad);
                    if (cov) blend_at(x + bx + i, y + by + j,
                                      ((uint32_t)(cov * alpha / 255) << 24) | (rgb & 0xffffff));
                }
        }
}
void ugfx_rrect_aa(int x, int y, int w, int h, int rad, uint32_t color) {
    rrect_core(x, y, w, h, rad, color & 0xffffff, 255);
}
void ugfx_rrect_a(int x, int y, int w, int h, int rad, uint32_t argb) {
    rrect_core(x, y, w, h, rad, argb & 0xffffff, (int)(argb >> 24));
}

void ugfx_shadow(int x, int y, int w, int h, int rad, int spread, uint32_t color, int max_a) {
    if (spread <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    uint32_t rgb = color & 0xffffff;
    int ix0 = x + rad, ix1 = x + w - rad, iy0 = y + rad, iy1 = y + h - rad;
    int X0 = x - spread, Y0 = y - spread, X1 = x + w + spread, Y1 = y + h + spread;
    if (X0 < clx0) X0 = clx0;
    if (Y0 < cly0) Y0 = cly0;
    if (X1 > clx1) X1 = clx1;
    if (Y1 > cly1) Y1 = cly1;
    int sp2 = spread * spread;
    for (int py = Y0; py < Y1; py++) {
        int ey = py < iy0 ? iy0 - py : (py > iy1 ? py - iy1 : 0);
        volatile uint32_t *row = FB + (long)py * PITCH;
        for (int px = X0; px < X1; px++) {
            int ex = px < ix0 ? ix0 - px : (px > ix1 ? px - ix1 : 0);
            int d = (int)isqrt32((uint32_t)(ex * ex + ey * ey)) - rad;
            if (d <= 0 || d >= spread) continue;       /* inside (covered) or past the feather */
            int t = spread - d;
            int a = max_a * t * t / sp2;               /* quadratic falloff -> soft edge */
            row[px] = blend(row[px], rgb, (uint32_t)a);
        }
    }
}

/* AA stroke that follows a rounded-rect outline, inset `t` px into the shape (the
 * "inner border" / hairline that gives modern surfaces a crisp edge). Straight runs
 * are solid t-px bands; the four corners are supersampled as the difference between
 * the outer and the inset rounded-corner coverage, so the ring itself is smooth. */
void ugfx_rrect_border(int x, int y, int w, int h, int rad, int t, uint32_t argb) {
    int alpha = (int)(argb >> 24);
    if (t <= 0 || !alpha || w <= 0 || h <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    if (rad < 0) rad = 0;
    if (t * 2 > w) t = w / 2;
    if (t * 2 > h) t = h / 2;
    uint32_t rgb = argb & 0xffffff;
    uint32_t band = ((uint32_t)alpha << 24) | rgb;
    ugfx_fill_a(x + rad, y, w - 2 * rad, t, band);                 /* top edge    */
    ugfx_fill_a(x + rad, y + h - t, w - 2 * rad, t, band);         /* bottom edge */
    ugfx_fill_a(x, y + rad, t, h - 2 * rad, band);                 /* left edge   */
    ugfx_fill_a(x + w - t, y + rad, t, h - 2 * rad, band);         /* right edge  */
    int iw = w - 2 * t, ih = h - 2 * t, ir = rad - t;
    if (ir < 0) ir = 0;
    for (int cy = 0; cy < 2; cy++)
        for (int cx = 0; cx < 2; cx++) {
            int bx = cx ? (w - rad) : 0, by = cy ? (h - rad) : 0;
            for (int j = 0; j < rad; j++)
                for (int i = 0; i < rad; i++) {
                    int oc = rrect_cov(bx + i, by + j, w, h, rad);   /* outer edge */
                    if (!oc) continue;
                    int ic = (iw > 0 && ih > 0)                      /* inset edge */
                             ? rrect_cov(bx + i - t, by + j - t, iw, ih, ir) : 0;
                    int cov = oc - ic;
                    if (cov <= 0) continue;
                    blend_at(x + bx + i, y + by + j,
                             ((uint32_t)(cov * alpha / 255) << 24) | rgb);
                }
        }
}

/* A Material-style "state layer": a faint translucent-white overlay clipped to a
 * rounded rect, painted over a control to signal hover/press. `alpha` is the 0..255
 * coverage (see TH_HOVER_A / TH_PRESS_A). */
void ugfx_state_layer(int x, int y, int w, int h, int rad, int alpha) {
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    ugfx_rrect_a(x, y, w, h, rad, ((uint32_t)alpha << 24) | 0xffffff);
}

void ugfx_scroll_thumb(int x, int y, int w, int h, int top, int total, int vis, int active) {
    if (total <= vis || vis <= 0 || h <= 0) return;          /* it all fits: no bar */
    int maxtop = total - vis;
    int th = (long)h * vis / total; if (th < 16) th = 16; if (th > h) th = h;
    int travel = h - th;
    if (top < 0) top = 0; if (top > maxtop) top = maxtop;
    int ty = y + (maxtop > 0 && travel > 0 ? (long)travel * top / maxtop : 0);
    ugfx_rrect_a(x, ty, w, th, w / 2,
                 active ? ARGB(235, 150, 180, 230) : ARGB(150, 200, 210, 230));
}

/* --- frosted glass (backdrop blur) --------------------------------------- *
 * A separable box blur over the backdrop, then a tint on top. The big visual
 * difference between "a flat translucent rectangle" and "frosted glass". Integer
 * only; bounded static scratch (no per-frame malloc churn). */
#define FROST_MAX  160000     /* max region in pixels we will buffer/blur          */
#define FROST_LINE 4096       /* max panel edge length we support (>= any W or H)  */
#define FROST_R    9          /* box-blur radius (px); washes the backdrop softly   */

/* Scratch is heap-allocated once on first use (a ~700 KB static array would bloat
 * the program's BSS past what the ELF loader maps). */
static uint32_t *frost_scratch, *frost_lin, *frost_lout;
static int frost_ready(void) {
    if (frost_scratch) return 1;
    frost_scratch = (uint32_t *)malloc((size_t)FROST_MAX * 4);
    frost_lin     = (uint32_t *)malloc((size_t)FROST_LINE * 4);
    frost_lout    = (uint32_t *)malloc((size_t)FROST_LINE * 4);
    return frost_scratch && frost_lin && frost_lout;
}

/* Box-blur a 1-D RGB run of n pixels (radius br) from `in` into `out` (in != out),
 * clamping at the ends so brightness stays constant across the whole run. */
static void frost_box1d(const uint32_t *in, uint32_t *out, int n, int br) {
    int win = 2 * br + 1;
    long sr = 0, sg = 0, sb = 0;
    for (int k = -br; k <= br; k++) {
        int idx = k < 0 ? 0 : (k >= n ? n - 1 : k);
        uint32_t c = in[idx];
        sr += (c >> 16) & 0xff; sg += (c >> 8) & 0xff; sb += c & 0xff;
    }
    for (int i = 0; i < n; i++) {
        out[i] = ((uint32_t)(sr / win) << 16) | ((uint32_t)(sg / win) << 8) | (uint32_t)(sb / win);
        int rem = i - br;     rem = rem < 0 ? 0 : (rem >= n ? n - 1 : rem);
        int add = i + br + 1;  add = add < 0 ? 0 : (add >= n ? n - 1 : add);
        uint32_t cr = in[rem], ca = in[add];
        sr += (long)((ca >> 16) & 0xff) - ((cr >> 16) & 0xff);
        sg += (long)((ca >> 8)  & 0xff) - ((cr >> 8)  & 0xff);
        sb += (long)( ca        & 0xff) - ( cr        & 0xff);
    }
}

void ugfx_frost(int x, int y, int w, int h, int rad, uint32_t tint) {
    if (w <= 0 || h <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    if (rad < 0) rad = 0;
    /* the scratch region is the panel clamped to the target */
    int px0 = x < 0 ? 0 : x, py0 = y < 0 ? 0 : y;
    int px1 = x + w > W ? W : x + w, py1 = y + h > H ? H : y + h;
    int pw = px1 - px0, ph = py1 - py0;
    if (pw <= 0 || ph <= 0) return;
    if ((long)pw * ph > FROST_MAX || pw > FROST_LINE || ph > FROST_LINE || !frost_ready()) {
        ugfx_rrect_a(x, y, w, h, rad, tint);            /* too big / no heap: plain tint fallback */
        return;
    }
    uint32_t *scratch = frost_scratch, *l_in = frost_lin, *l_out = frost_lout;
    for (int j = 0; j < ph; j++) {                      /* copy backdrop in */
        volatile uint32_t *row = FB + (long)(py0 + j) * PITCH + px0;
        uint32_t *srow = scratch + (long)j * pw;
        for (int i = 0; i < pw; i++) srow[i] = row[i];
    }
    for (int j = 0; j < ph; j++) {                      /* horizontal blur pass */
        uint32_t *srow = scratch + (long)j * pw;
        frost_box1d(srow, l_out, pw, FROST_R);
        for (int i = 0; i < pw; i++) srow[i] = l_out[i];
    }
    for (int i = 0; i < pw; i++) {                      /* vertical blur pass */
        for (int j = 0; j < ph; j++) l_in[j] = scratch[(long)j * pw + i];
        frost_box1d(l_in, l_out, ph, FROST_R);
        for (int j = 0; j < ph; j++) scratch[(long)j * pw + i] = l_out[j];
    }
    uint32_t ta = tint >> 24, trgb = tint & 0xffffff;
    int wx0 = px0 < clx0 ? clx0 : px0, wy0 = py0 < cly0 ? cly0 : py0;
    int wx1 = px1 > clx1 ? clx1 : px1, wy1 = py1 > cly1 ? cly1 : py1;
    for (int Y = wy0; Y < wy1; Y++) {
        volatile uint32_t *frow = FB + (long)Y * PITCH;
        const uint32_t *srow = scratch + (long)(Y - py0) * pw;
        for (int X = wx0; X < wx1; X++) {
            int i = X - x, j = Y - y;                    /* panel-relative (for corners) */
            int inCorner = (i < rad || i >= w - rad) && (j < rad || j >= h - rad);
            int cov = (rad == 0 || !inCorner) ? 255 : rrect_cov(i, j, w, h, rad);
            if (!cov) continue;
            uint32_t bg = srow[X - px0];
            uint32_t col = ta >= 255 ? trgb : blend(bg, trgb, ta);   /* tint over blurred backdrop */
            frow[X] = cov >= 255 ? col : blend(frow[X], col, (uint32_t)cov);
        }
    }
}

/* Material-style elevation: a soft drop shadow whose feather AND downward offset
 * both grow with `level` (0 = flush, 5 = highest), so surfaces read as a consistent
 * stack of layers rather than a flat collage. Mirrors caelestia's Elevation.qml
 * (blur + offset.y rise together with the elevation step). */
void ugfx_elevation_extent(int level, int *spread, int *dy) {
    static const int sp[6]  = { 0, 8, 14, 20, 27, 34 };    /* feather (px)  */
    static const int off[6] = { 0, 2,  4,  6,  9, 12 };    /* offset  (px)  */
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    if (spread) *spread = sp[level];
    if (dy)     *dy     = off[level];
}
void ugfx_elevation(int x, int y, int w, int h, int rad, int level) {
    static const int al[6] = { 0, 64, 84, 104, 122, 140 }; /* peak alpha    */
    int sp, dy; ugfx_elevation_extent(level, &sp, &dy);    /* single source for feather/offset */
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    if (!sp) return;
    ugfx_shadow(x, y + dy, w, h, rad, sp, 0x000000, al[level]);
}

void ugfx_blit_round_bottom(int x, int y, const uint32_t *src, int sw, int sh, int spitch, int rad) {
    if (rad * 2 > sw) rad = sw / 2;
    if (rad * 2 > sh) rad = sh / 2;
    ugfx_blit(x, y, src, sw, sh - rad, spitch);        /* everything above the corner band */
    for (int j = sh - rad; j < sh; j++) {
        int dy = y + j;
        if (dy < cly0 || dy >= cly1) continue;
        const uint32_t *srow = src + (long)j * spitch;
        for (int i = 0; i < sw; i++) {
            int dx = x + i;
            if (dx < clx0 || dx >= clx1) continue;
            int cov = (i >= rad && i < sw - rad) ? 255 : rrect_cov(i, j, sw, sh, rad);
            if (!cov) continue;
            uint32_t s = srow[i] & 0xffffff;
            long o = (long)dy * PITCH + dx;
            FB[o] = (cov >= 255) ? s : blend(FB[o], s, (uint32_t)cov);
        }
    }
}

/* Blit a surface with ALL FOUR corners rounded (a borderless popup/overlay). The
 * straight middle band is an opaque copy; the top/bottom `rad`-tall bands are
 * per-pixel, AA-blended against the target at the corners. */
void ugfx_blit_round(int x, int y, const uint32_t *src, int sw, int sh, int spitch, int rad) {
    if (rad * 2 > sw) rad = sw / 2;
    if (rad * 2 > sh) rad = sh / 2;
    if (rad < 0) rad = 0;
    if (sh - 2 * rad > 0) ugfx_blit(x, y + rad, src + (long)rad * spitch, sw, sh - 2 * rad, spitch);
    for (int j = 0; j < sh; j++) {
        if (j >= rad && j < sh - rad) continue;        /* straight middle already copied */
        int dy = y + j;
        if (dy < cly0 || dy >= cly1) continue;
        const uint32_t *srow = src + (long)j * spitch;
        for (int i = 0; i < sw; i++) {
            int dx = x + i;
            if (dx < clx0 || dx >= clx1) continue;
            int cov = (i >= rad && i < sw - rad) ? 255 : rrect_cov(i, j, sw, sh, rad);
            if (!cov) continue;
            uint32_t s = srow[i] & 0xffffff;
            long o = (long)dy * PITCH + dx;
            FB[o] = (cov >= 255) ? s : blend(FB[o], s, (uint32_t)cov);
        }
    }
}

void ugfx_blit_round_key(int x, int y, const uint32_t *src, int sw, int sh, int spitch, int rad, uint32_t key) {
    if (rad * 2 > sw) rad = sw / 2;
    if (rad * 2 > sh) rad = sh / 2;
    if (rad < 0) rad = 0;
    key &= 0xffffff;
    for (int j = 0; j < sh; j++) {
        int dy = y + j;
        if (dy < cly0 || dy >= cly1) continue;
        const uint32_t *srow = src + (long)j * spitch;
        for (int i = 0; i < sw; i++) {
            int dx = x + i;
            if (dx < clx0 || dx >= clx1) continue;
            uint32_t s = srow[i] & 0xffffff;
            if (s == key) continue;                        /* sentinel: leave the frost showing */
            int cov = (i >= rad && i < sw - rad) ? 255 : rrect_cov(i, j, sw, sh, rad);
            if (!cov) continue;
            long o = (long)dy * PITCH + dx;
            FB[o] = (cov >= 255) ? s : blend(FB[o], s, (uint32_t)cov);
        }
    }
}

void ugfx_present_rect(const uint32_t *back, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    for (int j = 0; j < h; j++) {
        const uint32_t *srow = back + (long)(y + j) * fb_w + x;
        volatile uint32_t *drow = fb_buf + (long)(y + j) * fb_pitch + x;
        for (int i = 0; i < w; i++) drow[i] = srow[i];
    }
}

/* --- software arrow cursor: save the pixels under it, draw, restore on move -- */
#define CUR_W 12
#define CUR_H 19
static const char cur_bmp[CUR_H][CUR_W + 1] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      X..X  ",
    "       XX   ",
};
static uint32_t cur_save[CUR_W * CUR_H];
static int cur_x = -1, cur_y = -1, cur_shown = 0;

void ugfx_cursor_hide(void) {
    if (!cur_shown) return;
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            ugfx_pixel(cur_x + i, cur_y + j, cur_save[j * CUR_W + i]);
    cur_shown = 0;
}

void ugfx_cursor(int x, int y) {
    ugfx_cursor_hide();                               /* restore what was under the old cursor */
    cur_x = x; cur_y = y; cur_shown = 1;
    for (int j = 0; j < CUR_H; j++)                   /* snapshot the new spot, then draw */
        for (int i = 0; i < CUR_W; i++)
            cur_save[j * CUR_W + i] = ugfx_get(x + i, y + j);
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            char c = cur_bmp[j][i];
            if (c == 'X')      ugfx_pixel(x + i, y + j, RGB(0, 0, 0));
            else if (c == '.') ugfx_pixel(x + i, y + j, RGB(255, 255, 255));
        }
}

/* Draw the arrow into the current target with no save/restore -- the compositor
 * regenerates the background under it each frame, so there is nothing to undo
 * (this is what kills the old stationary-cursor smear). 'X' = soft black outline,
 * '.' = near-white fill, both slightly translucent so the pointer reads cleanly
 * over any background. */
void ugfx_cursor_draw(int x, int y) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            char c = cur_bmp[j][i];
            if (c == 'X')      blend_at(x + i, y + j, ARGB(210, 18, 20, 26));
            else if (c == '.') blend_at(x + i, y + j, ARGB(245, 245, 247, 250));
        }
}
