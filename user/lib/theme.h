/* tOS desktop design system: one palette + spacing scale shared by the
 * compositor and the apps, so the whole UI reads as one product. The look is
 * "floating": a deep desktop gradient, elevated rounded surfaces with soft drop
 * shadows, a translucent top bar, and a centred dock. Colours are XRGB; the
 * *_A tokens carry alpha in the high byte (see ugfx ARGB()). */
#pragma once
#include "ugfx.h"

/* --- palette ------------------------------------------------------------- */
#define TH_DESK_TOP   RGB(42, 47, 66)        /* desktop gradient, top         */
#define TH_DESK_BOT   RGB(20, 23, 33)        /* desktop gradient, bottom      */
#define TH_BAR_A      ARGB(196, 16, 18, 26)  /* translucent top bar           */
#define TH_BARLINE_A  ARGB(46, 150, 170, 230)/* hairline under the bar        */
#define TH_CHROME     RGB(38, 43, 57)        /* window frame / title bar      */
#define TH_CHROME_HI  RGB(48, 54, 71)        /* title bar highlight (gradient top) */
#define TH_TEXT       RGB(231, 235, 244)     /* primary text                  */
#define TH_MUTED      RGB(150, 158, 175)     /* secondary text                */
#define TH_ACCENT     RGB(96, 152, 252)      /* focus / interactive accent    */
#define TH_DOCK_A     ARGB(170, 32, 36, 50)  /* translucent dock panel        */
#define TH_DOCK_HI_A  ARGB(36, 255, 255, 255)/* dock top sheen                */
#define TH_TL_RED     RGB(237, 106, 94)      /* window control: close         */
#define TH_TL_AMBER   RGB(245, 191, 79)      /* window control: minimize      */
#define TH_TL_GREEN   RGB(98, 197, 84)       /* window control: maximize      */
#define TH_BTN_GLYPH  ARGB(170, 22, 24, 32)  /* dark glyph drawn on a control */
#define TH_PILL_A     ARGB(160, 52, 58, 78)  /* minimized-window pill in the bar */
#define TH_SHADOW     RGB(0, 0, 0)

/* Frosted-glass tints: painted by ugfx_frost OVER a blurred backdrop, so the alpha
 * is lower than the flat *_A panels above -- the blur supplies the body, the tint
 * just gives it our slate cast. Same hue family; do not Material-ize. */
#define TH_BAR_FROST  ARGB(140, 18, 21, 30)   /* top bar glass        */
#define TH_DOCK_FROST ARGB(120, 30, 34, 48)   /* dock glass           */
#define TH_CC_FROST   ARGB(150, 28, 32, 45)   /* control-center glass */
#define TH_OV_FROST   ARGB(150, 24, 28, 40)   /* overlay (Launchpad) glass */
/* Sentinel an app paints as its panel background so the compositor lets a frosted
 * backdrop show through there (ugfx_blit_round_key). An improbable exact XRGB. */
#define TH_FROST_KEY  RGB(2, 3, 5)

/* A small palette for letter-avatar dock tiles (indexed by shortcut order), so
 * launchers look distinct without per-app art (per-app icons are a later step). */
#define TH_TILE_0     RGB(64, 124, 240)
#define TH_TILE_1     RGB(38, 176, 162)
#define TH_TILE_2     RGB(150, 104, 232)
#define TH_TILE_3     RGB(232, 132, 64)

/* --- surface elevation tiers --------------------------------------------- *
 * One tonal ladder (slate-blue, progressively lighter) shared by every panel so
 * the UI reads as a stack of layers, not a flat collage. Pick a tier by how
 * "raised" a surface is: wells/lists sit low, toolbars mid, buttons/cards high. */
#define TH_SURF_0     RGB(20, 23, 32)        /* lowest: content wells, list bodies   */
#define TH_SURF_1     RGB(27, 31, 43)        /* low:    sidebars, sunken areas        */
#define TH_SURF_2     RGB(34, 39, 52)        /* base:   window frame, toolbars        */
#define TH_SURF_3     RGB(44, 50, 66)        /* raised: buttons, menus, cards         */
#define TH_SURF_4     RGB(55, 62, 82)        /* highest: hovered/active raised tiles   */

/* --- borders, outlines, state layers ------------------------------------- *
 * The crisp top-lit hairline (InnerBorder) + faint hover/press overlays
 * (StateLayer) are what make the widgets feel modern rather than "C with rects". */
#define TH_BORDER     ARGB(42, 255, 255, 255)   /* lit inner border on a raised card   */
#define TH_BORDER_DIM ARGB(20, 255, 255, 255)   /* whisper-thin edge                   */
#define TH_OUTLINE    ARGB(64, 0, 0, 0)         /* dark divider between panels         */
#define TH_HAIRLINE   ARGB(46, 150, 170, 230)   /* cool accent hairline / separator    */
#define TH_HOVER_A    22                        /* state-layer alpha: hover  (~0.085)  */
#define TH_PRESS_A    36                        /* state-layer alpha: pressed (~0.14)  */

/* --- rounding + spacing scales ------------------------------------------- *
 * Consistent radii/gaps across the whole UI (mirrors caelestia's token scale). */
#define TH_R_SM       8
#define TH_R_MD       12
#define TH_R_LG       16
#define TH_R_XL       22
#define TH_R_PILL     999     /* clamps to half-height -> a pill                       */
#define TH_SP_XS      4
#define TH_SP_SM      7
#define TH_SP         10
#define TH_SP_LG      14
#define TH_SP_XL      20

/* --- metrics ------------------------------------------------------------- */
#define TH_RADIUS     13      /* window corner radius                  */
#define TH_SHADOW_SP  26      /* shadow feather (px)                   */
#define TH_SHADOW_A   130     /* shadow peak alpha (focused)           */
#define TH_SHADOW_A2  70      /* shadow peak alpha (unfocused)         */
#define TH_DOCK_RAD   22      /* dock panel corner radius              */
#define TH_TILE       48      /* dock tile size                        */
#define TH_TILE_RAD   14      /* dock tile corner radius               */
