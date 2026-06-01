/* tOS userspace graphics: a thin layer over the framebuffer mapped by
 * SYS_FBINFO. Drawing goes straight to the mapped framebuffer (32-bit XRGB).
 * Coordinates are in pixels; all primitives clip to the screen. Text uses the
 * shared anti-aliased system font (kernel/sysfont.h). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
/* ARGB: alpha in the high byte (0=transparent, 255=opaque). Used by the
 * alpha-aware primitives (translucent panels, shadows, blends). */
#define ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define UGFX_ALPHA(c)    ((c) >> 24)
#define UGFX_TRANSPARENT 0x01000000u        /* sentinel bg for ugfx_char/text */

int  ugfx_init(void);                       /* map the FB as the target; 0 ok, -1 if text-mode boot */
int  ugfx_width(void);                      /* current target width  */
int  ugfx_height(void);                     /* current target height */

/* ugfx draws into a "target": the framebuffer (compositor) or a window surface
 * (apps). Apps point ugfx at their surface; the compositor blits surfaces to the
 * framebuffer with ugfx_blit. */
void ugfx_set_target(uint32_t *buf, int w, int h, int pitch);
void ugfx_use_fb(void);                     /* switch the target back to the framebuffer */
void ugfx_blit(int x, int y, const uint32_t *src, int sw, int sh, int spitch); /* opaque copy */
int  ugfx_font_w(void);                     /* system font cell width  (px) */
int  ugfx_font_h(void);                     /* system font cell height (px) */
int  ugfx_text_w(const char *s);            /* pixel width a string will occupy */

void ugfx_pixel(int x, int y, uint32_t color);
void ugfx_fill(int x, int y, int w, int h, uint32_t color);
void ugfx_frame(int x, int y, int w, int h, uint32_t color);   /* 1px outline */
void ugfx_rrect(int x, int y, int w, int h, int rad, uint32_t color); /* rounded filled rect */
void ugfx_clear(uint32_t color);
void ugfx_char(int x, int y, char c, uint32_t fg, uint32_t bg); /* bg may be UGFX_TRANSPARENT */
void ugfx_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* Blit a w*h ARGB image (alpha in the high byte) over the framebuffer at (x,y),
 * alpha-blended against what is already there. Used for the desktop logo. */
void ugfx_blit_argb(int x, int y, int w, int h, const uint32_t *argb);
/* Nearest-neighbour scaled ARGB blit: draw a sw*sh source into a dw*dh dest box. */
void ugfx_blit_scaled(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh);

uint32_t ugfx_get(int x, int y);            /* read a pixel back from the target */
void ugfx_cursor(int x, int y);             /* move the arrow cursor (saves/restores under it) */
void ugfx_cursor_hide(void);                /* restore pixels under the cursor (call before redrawing under it) */

/* --- modern primitives: clip, alpha, gradients, AA shapes, soft shadows ----- *
 * These power the compositor's back-buffered, sleek look. All honour the clip
 * rectangle and the current target, and use integer math only (no FPU -- user
 * programs build with -mno-sse and the kernel doesn't save FP state). */
void ugfx_set_clip(int x, int y, int w, int h);   /* confine drawing to a rect (intersect target) */
void ugfx_clip_none(void);                         /* reset the clip to the whole target           */
uint32_t ugfx_blend(uint32_t bg, uint32_t fg, uint32_t a);          /* a=0..255 coverage of fg over bg */
void ugfx_fill_a(int x, int y, int w, int h, uint32_t argb);        /* alpha-blended fill              */
void ugfx_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bot); /* vertical gradient (opaque) */
void ugfx_rrect_aa(int x, int y, int w, int h, int rad, uint32_t color); /* opaque rounded rect, AA    */
void ugfx_rrect_a(int x, int y, int w, int h, int rad, uint32_t argb);   /* translucent rounded rect   */
/* AA stroke following a rounded-rect outline, inset `t` px (the crisp hairline that
 * makes a modern surface read as a distinct card). */
void ugfx_rrect_border(int x, int y, int w, int h, int rad, int t, uint32_t argb);
/* A translucent-white "state layer" over a rounded rect: hover/press feedback. */
void ugfx_state_layer(int x, int y, int w, int h, int rad, int alpha);
/* The one scrollbar-thumb renderer shared by every scrollable surface in the OS
 * (the toolkit ScrollBar and the terminal). Draws a rounded thumb inside the
 * vertical strip [x,x+w) x [y,y+h), sized/placed to show `vis` of `total` units
 * scrolled to `top`; `active` brightens it (e.g. while dragging). No-op if it fits. */
void ugfx_scroll_thumb(int x, int y, int w, int h, int top, int total, int vis, int active);
/* Frosted glass: blur the backdrop already painted in the target inside this
 * (optionally rounded) rect, then overlay `tint` (ARGB) on top -- the translucent
 * panel look of macOS/Quickshell, but with a real backdrop blur instead of a flat
 * alpha wash. Separable box blur, integer only. Call during compose, after the
 * desktop+windows are painted and before the panel's own contents. Falls back to a
 * plain tinted rrect if the region is too large to buffer. */
void ugfx_frost(int x, int y, int w, int h, int rad, uint32_t tint);
/* Soft elevation shadow; `level` 0..5 raises feather + downward offset together. */
void ugfx_elevation(int x, int y, int w, int h, int rad, int level);
/* The feather + downward offset a given elevation level extends, so a compositor can
 * size the shadow's dirty/cull halo from the same numbers it's drawn with. */
void ugfx_elevation_extent(int level, int *spread, int *dy);
/* Soft drop shadow for a rounded rect of (w,h,rad): a `spread`-px feathered ring
 * of `color` at peak alpha `max_a`, drawn OUTSIDE the shape (the window covers
 * the inside). Draw it before the window. */
void ugfx_shadow(int x, int y, int w, int h, int rad, int spread, uint32_t color, int max_a);
/* Blit a surface with rounded BOTTOM corners (top stays square -- it meets the
 * title bar). Corners are AA-blended against whatever is already in the target. */
void ugfx_blit_round_bottom(int x, int y, const uint32_t *src, int sw, int sh, int spitch, int rad);
/* Blit a surface with all four corners rounded (a borderless popup/overlay). */
void ugfx_blit_round(int x, int y, const uint32_t *src, int sw, int sh, int spitch, int rad);
/* Like ugfx_blit_round, but source pixels whose RGB equals `key` are skipped
 * (left transparent) -- so an app can paint its panel background with a sentinel
 * colour and the compositor lets a frosted backdrop show through there, while the
 * app's real content (tiles, text) stays opaque. */
void ugfx_blit_round_key(int x, int y, const uint32_t *src, int sw, int sh, int spitch, int rad, uint32_t key);
void ugfx_cursor_draw(int x, int y);        /* draw the arrow into the current target (no save/restore) */
/* Copy a rectangle from a back buffer (tightly packed, pitch = screen width)
 * straight to the real framebuffer -- the compositor's "present". */
void ugfx_present_rect(const uint32_t *back, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif
