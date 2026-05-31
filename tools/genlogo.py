#!/usr/bin/env python3
"""Generate the tOS desktop logo as an ARGB C array for the taskbar.

A small rounded-square badge with a terminal prompt mark (a chevron and an
underscore cursor). Rendered at 4x and downsampled so the edges are smooth, then
emitted as 0xAARRGGBB pixels that ugfx_blit_argb() alpha-blends onto the bar.

Usage:  python3 tools/genlogo.py [out.h] [--preview out.png]
Run once and commit the header; the build does not depend on PIL.
"""
import sys
from PIL import Image, ImageDraw

SIZE = 24            # final logo is SIZE x SIZE
SS = 4               # supersample factor for anti-aliasing
ACCENT = (54, 122, 196)    # badge background (blue)
ACCENT2 = (84, 162, 226)   # lighter, top edge
INK = (240, 246, 252)      # the prompt mark

def render():
    S = SIZE * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rad = 6 * SS
    # badge with a subtle vertical light->dark gradient
    for y in range(S):
        t = y / (S - 1)
        col = tuple(int(ACCENT2[k] * (1 - t) + ACCENT[k] * t) for k in range(3))
        d.line([(0, y), (S, y)], fill=col + (255,))
    # mask to rounded rect
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=rad, fill=255)
    img.putalpha(mask)
    # prompt chevron ">" on the left
    dd = ImageDraw.Draw(img)
    lw = 2 * SS
    cx, cy = 7 * SS, 12 * SS
    arm = 4 * SS
    dd.line([(cx - arm, cy - arm), (cx, cy), (cx - arm, cy + arm)],
            fill=INK + (255,), width=lw, joint="curve")
    # underscore cursor to the right
    dd.line([(11 * SS, 16 * SS), (18 * SS, 16 * SS)], fill=INK + (255,), width=lw)
    return img.resize((SIZE, SIZE), Image.LANCZOS)

def main():
    out = "user/lib/logo.h"
    preview = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--preview":
            preview = args[i + 1]; i += 2
        else:
            out = args[i]; i += 1

    img = render()
    px = img.load()
    with open(out, "w") as f:
        f.write("/* tOS desktop logo, baked by tools/genlogo.py.\n")
        f.write(" * %dx%d ARGB (0xAARRGGBB); blit with ugfx_blit_argb().\n" % (SIZE, SIZE))
        f.write(" * Regenerate with: python3 tools/genlogo.py */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define LOGO_W %d\n#define LOGO_H %d\n\n" % (SIZE, SIZE))
        f.write("static const uint32_t logo_argb[LOGO_W * LOGO_H] = {\n")
        for y in range(SIZE):
            f.write("  ")
            for x in range(SIZE):
                r, g, b, a = px[x, y]
                f.write("0x%02X%02X%02X%02X," % (a, r, g, b))
            f.write("\n")
        f.write("};\n")
    print("wrote %s (%dx%d)" % (out, SIZE, SIZE))

    if preview:
        bg = Image.new("RGB", (SIZE * 6, SIZE * 6), (18, 22, 32))
        big = img.resize((SIZE * 6, SIZE * 6), Image.NEAREST)
        bg.paste(big, (0, 0), big)
        bg.save(preview)
        print("wrote preview %s" % preview)

if __name__ == "__main__":
    main()
