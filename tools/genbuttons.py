#!/usr/bin/env python3
"""Generate the window title-bar control buttons (macOS "traffic light" style).

Three flat colour dots -- minimize (yellow), maximize (green), close (red) --
with a subtle top highlight, a thin darker rim, and a perfectly centred glyph
in a dark tint of the dot's hue; plus one grey dot for an unfocused window.
Rendered at 4x and downsampled, emitted as 0xAARRGGBB for ugfx_blit_argb().

Usage:  python3 tools/genbuttons.py [out.h] [--preview out.png]
Run once and commit the header; the build does not depend on PIL.
"""
import sys
from PIL import Image, ImageDraw

SIZE = 16
SS = 4

# iconic macOS hues
RED    = (255, 95, 87)
YELLOW = (254, 188, 46)
GREEN  = (40, 200, 64)
GREY   = (88, 96, 112)

def _dot(base, draw_glyph):
    S = SIZE * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = SIZE / 2.0 - 0.5
    r = 5.7                                   # dot radius (px, final scale)
    bb = [(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS]
    # vertical highlight->base gradient inside a circular mask
    top = tuple(min(255, int(c * 1.18 + 24)) for c in base)
    grad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        t = y / (S - 1)
        col = tuple(int(top[k] * (1 - t) + base[k] * t) for k in range(3))
        gd.line([(0, y), (S, y)], fill=col + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse(bb, fill=255)
    img.paste(grad, (0, 0), mask)
    # thin darker rim
    rim = tuple(int(c * 0.72) for c in base)
    d.ellipse(bb, outline=rim + (255,), width=max(1, int(0.8 * SS)))
    # centred glyph in a dark tint of the hue
    if draw_glyph:
        ink = tuple(int(c * 0.34) for c in base) + (255,)
        g = 3.0                               # glyph half-extent
        lw = int(1.5 * SS)
        if draw_glyph == "close":
            d.line([((cx - g) * SS, (cy - g) * SS), ((cx + g) * SS, (cy + g) * SS)], fill=ink, width=lw)
            d.line([((cx - g) * SS, (cy + g) * SS), ((cx + g) * SS, (cy - g) * SS)], fill=ink, width=lw)
        elif draw_glyph == "min":
            d.line([((cx - g) * SS, cy * SS), ((cx + g) * SS, cy * SS)], fill=ink, width=lw)
        elif draw_glyph == "max":
            d.line([((cx - g) * SS, cy * SS), ((cx + g) * SS, cy * SS)], fill=ink, width=lw)
            d.line([(cx * SS, (cy - g) * SS), (cx * SS, (cy + g) * SS)], fill=ink, width=lw)
    return img.resize((SIZE, SIZE), Image.LANCZOS)

def main():
    out = "user/lib/winbtns.h"; preview = None
    args = sys.argv[1:]; i = 0
    while i < len(args):
        if args[i] == "--preview": preview = args[i + 1]; i += 2
        else: out = args[i]; i += 1

    seq = [("MIN", _dot(YELLOW, "min")), ("MAX", _dot(GREEN, "max")),
           ("CLOSE", _dot(RED, "close")), ("INACTIVE", _dot(GREY, None))]
    with open(out, "w") as f:
        f.write("/* Window control buttons (macOS traffic-light style), baked by\n")
        f.write(" * tools/genbuttons.py. %dx%d ARGB; blit centred with ugfx_blit_argb()\n" % (SIZE, SIZE))
        f.write(" * at (cx - WINBTN_W/2, cy - WINBTN_H/2). Regenerate: python3 tools/genbuttons.py */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define WINBTN_W %d\n#define WINBTN_H %d\n" % (SIZE, SIZE))
        for idx, (name, _) in enumerate(seq):
            f.write("#define WB_%s %d\n" % (name, idx))
        f.write("\nstatic const uint32_t winbtns_argb[%d][WINBTN_W * WINBTN_H] = {\n" % len(seq))
        for name, img in seq:
            px = img.load()
            f.write("  { /* %s */\n" % name)
            for y in range(SIZE):
                f.write("    ")
                for x in range(SIZE):
                    r, g, b, a = px[x, y]
                    f.write("0x%02X%02X%02X%02X," % (a, r, g, b))
                f.write("\n")
            f.write("  },\n")
        f.write("};\n")
    print("wrote %s (%d buttons, %dx%d)" % (out, len(seq), SIZE, SIZE))

    if preview:
        Z = 10
        bg = Image.new("RGB", ((SIZE * Z + 12) * len(seq) + 12, SIZE * Z + 24), (40, 45, 60))
        x = 12
        for name, img in seq:
            big = img.resize((SIZE * Z, SIZE * Z), Image.NEAREST)
            bg.paste(big, (x, 12), big)
            x += SIZE * Z + 12
        bg.save(preview); print("wrote preview %s" % preview)

if __name__ == "__main__":
    main()
