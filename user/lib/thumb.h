/* Pure thumbnail math for the Files app (files-app.md §11): aspect-preserving
 * fit into a square box (never upscaling) and an integer box-average ARGB
 * downscaler. No fs/gfx dependencies, so the host unit test (t_thumb) covers
 * it directly; the app wraps it around load_icon_argb's pixels. */
#pragma once

/* Fit (sw x sh) inside box x box, preserving aspect; never upscales and never
 * returns a dimension below 1. */
static inline void thumb_fit(int sw, int sh, int box, int *tw, int *th) {
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    int w = sw, h = sh;
    if (w > box || h > box) {
        if (sw >= sh) { w = box; h = (sh * box + sw / 2) / sw; }
        else          { h = box; w = (sw * box + sh / 2) / sh; }
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *tw = w; *th = h;
}

/* Downscale src (sw x sh) into dst (tw x th, tw<=sw th<=sh), each destination
 * pixel the per-channel average of its source rectangle (box filter -- a
 * nearest-neighbour shrink of a 128px icon shimmers; the average doesn't). */
static inline void thumb_scale(unsigned int *dst, int tw, int th,
                               const unsigned int *src, int sw, int sh) {
    for (int y = 0; y < th; y++) {
        int sy0 = y * sh / th, sy1 = (y + 1) * sh / th;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < tw; x++) {
            int sx0 = x * sw / tw, sx1 = (x + 1) * sw / tw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned int a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    unsigned int p = src[sy * sw + sx];
                    a += (p >> 24) & 255u; r += (p >> 16) & 255u;
                    g += (p >> 8) & 255u;  b += p & 255u; n++;
                }
            dst[y * tw + x] = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
}
