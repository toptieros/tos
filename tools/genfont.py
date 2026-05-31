#!/usr/bin/env python3
"""Bake a monospace TTF into a grayscale C bitmap atlas for tOS.

The kernel console and the userspace ugfx library both render text from the same
table (the "system font"). Each glyph is stored as SYSFONT_W*SYSFONT_H bytes of
8-bit coverage (0 = background, 255 = full ink) so renderers can anti-alias by
blending foreground over background by coverage.

JetBrains Mono at 15px has an exact 9.0px advance, which gives a clean integer
cell. We render ASCII 0x20..0x7F (96 glyphs); 0x7F is left blank.

Usage:  python3 tools/genfont.py [out.h] [--preview out.png]
Run once and commit the generated header; the build does not depend on PIL.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf"
SIZE = 15            # 9.0px advance at this size
CW, CH = 9, 19       # cell width/height in pixels
BASE = 15            # baseline row within the cell
FIRST, COUNT = 0x20, 96

def render_glyph(font, ch):
    """Return CW*CH bytes of coverage for a single character."""
    # Render the glyph onto an oversized canvas, then copy into the cell with the
    # baseline aligned, so ascenders/descenders land consistently.
    pad = 8
    img = Image.new("L", (CW + 2 * pad, CH + 2 * pad), 0)
    d = ImageDraw.Draw(img)
    # PIL draws text from the top of the line box; offset so the baseline lands
    # on row BASE of the cell. ascent = pixels from top-of-linebox to baseline.
    asc, _ = font.getmetrics()
    d.text((pad, pad + BASE - asc), ch, fill=255, font=font)
    cell = bytearray(CW * CH)
    px = img.load()
    for y in range(CH):
        for x in range(CW):
            cell[y * CW + x] = px[pad + x, pad + y]
    return cell

def main():
    out = "kernel/sysfont.h"
    preview = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--preview":
            preview = args[i + 1]; i += 2
        else:
            out = args[i]; i += 1

    font = ImageFont.truetype(FONT, SIZE)
    glyphs = []
    for code in range(FIRST, FIRST + COUNT):
        ch = chr(code)
        glyphs.append(bytearray(CW * CH) if code == 0x7F else render_glyph(font, ch))

    with open(out, "w") as f:
        f.write("/* tOS system font: JetBrains Mono, baked by tools/genfont.py.\n")
        f.write(" * %dx%d cell, 8-bit coverage per pixel, ASCII 0x%02X..0x%02X.\n"
                % (CW, CH, FIRST, FIRST + COUNT - 1))
        f.write(" * Regenerate with: python3 tools/genfont.py */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define SYSFONT_W %d\n#define SYSFONT_H %d\n" % (CW, CH))
        f.write("#define SYSFONT_FIRST 0x%02X\n#define SYSFONT_COUNT %d\n\n" % (FIRST, COUNT))
        f.write("static const uint8_t sysfont[SYSFONT_COUNT][SYSFONT_W * SYSFONT_H] = {\n")
        for code, g in zip(range(FIRST, FIRST + COUNT), glyphs):
            disp = chr(code) if 0x20 < code < 0x7F else " "
            f.write("  { /* 0x%02X '%s' */\n    " % (code, disp))
            for j, v in enumerate(g):
                f.write("%3d," % v)
                if (j + 1) % CW == 0 and j + 1 < len(g):
                    f.write("\n    ")
            f.write("\n  },\n")
        f.write("};\n")
    print("wrote %s  (%dx%d, %d glyphs)" % (out, CW, CH, COUNT))

    if preview:
        cols = 16
        rows = (COUNT + cols - 1) // cols
        scale = 4
        sample = "tOS  twm  ls cat write {}[]()  0123 g_y_j_p_q"
        sw = (len(sample) + 1) * CW
        img = Image.new("RGB", (max(cols * CW, sw) * scale, (rows * CH + CH + 4) * scale), (20, 26, 38))
        px = img.load()
        def blit(cell, ox, oy, fg=(235, 238, 242)):
            for y in range(CH):
                for x in range(CW):
                    c = cell[y * CW + x] / 255.0
                    for sy in range(scale):
                        for sx in range(scale):
                            X, Y = (ox + x) * scale + sx, (oy + y) * scale + sy
                            if 0 <= X < img.width and 0 <= Y < img.height:
                                b = px[X, Y]
                                px[X, Y] = tuple(int(b[k] * (1 - c) + fg[k] * c) for k in range(3))
        for n, g in enumerate(glyphs):
            blit(g, (n % cols) * CW, (n // cols) * CH)
        for n, ch in enumerate(sample):
            code = ord(ch)
            if FIRST <= code < FIRST + COUNT:
                blit(glyphs[code - FIRST], n * CW, rows * CH + 4)
        img.save(preview)
        print("wrote preview %s" % preview)

if __name__ == "__main__":
    main()
