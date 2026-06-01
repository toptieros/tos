#!/usr/bin/env python3
"""Fetch Lucide (MIT) line icons for the menu-bar status cluster, render them
anti-aliased, and bake a tintable alpha-mask header (user/lib/statusicons.h).

The old status glyphs were hand-stacked 1px rectangles (no AA) -- blocky at any
size. Lucide is a clean, minimal line set (the Feather successor; NOT Material),
so it matches the macOS-ish menu bar. Each glyph is rendered big via librsvg,
downsampled with Lanczos, and emitted as white ARGB (RGB=0xFFFFFF, A = stroke
coverage) so twm recolours it to the theme ink or the accent via ugfx_blit_tint().

Run once and commit the header; the OS build depends on neither PIL, librsvg, nor
the network. Re-fetch + regenerate:  python3 tools/genstatus.py [out.h] [--preview p.png]
Requires: rsvg-convert (librsvg) + PIL, and network access for the fetch.
"""
import sys, io, subprocess, urllib.request
from PIL import Image

SZ  = 18         # status-glyph box (square), px -- matches the menu-bar cluster slots
SUP = 8          # supersample factor rendered by librsvg, then Lanczos-downsampled
RAW = "https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/%s.svg"

# C identifier suffix -> Lucide icon slug. volume-2 keeps the two sound waves the
# old speaker glyph had; battery / wifi / bell map straight across.
GLYPHS = [("WIFI", "wifi"), ("VOL", "volume-2"), ("BATT", "battery"), ("BELL", "bell")]


def render(slug):
    """Fetch + rasterise one Lucide glyph to an SZ*SZ white alpha mask."""
    svg = urllib.request.urlopen(RAW % slug, timeout=20).read()
    n = SZ * SUP
    png = subprocess.run(["rsvg-convert", "-w", str(n), "-h", str(n)],
                         input=svg, stdout=subprocess.PIPE, check=True).stdout
    big = Image.open(io.BytesIO(png)).convert("RGBA")
    alpha = big.resize((SZ, SZ), Image.LANCZOS).split()[3]   # keep coverage only
    if not alpha.getbbox():
        raise SystemExit("genstatus: %s rendered empty -- check librsvg" % slug)
    mask = Image.new("RGBA", (SZ, SZ), (255, 255, 255, 0))   # white, to be tinted
    mask.putalpha(alpha)
    return mask


def main():
    out = "user/lib/statusicons.h"; preview = None
    args = sys.argv[1:]; i = 0
    while i < len(args):
        if args[i] == "--preview": preview = args[i + 1]; i += 2
        else: out = args[i]; i += 1

    imgs = [(nm, render(slug)) for nm, slug in GLYPHS]
    with open(out, "w") as f:
        f.write("/* Menu-bar status glyphs baked from Lucide (MIT, lucide.dev) by\n")
        f.write(" * tools/genstatus.py. White ARGB (0xAARRGGBB) alpha masks -- only the\n")
        f.write(" * alpha matters; recolour with ugfx_blit_tint(). Regenerate: python3 tools/genstatus.py */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define STATUSICON_SZ %d\n\n" % SZ)
        for idx, (nm, _) in enumerate(imgs):
            f.write("#define STATUSICON_%s %d\n" % (nm, idx))
        f.write("\nstatic const uint32_t statusicons_argb[%d][%d * %d] = {\n" % (len(imgs), SZ, SZ))
        for nm, img in imgs:
            px = img.load()
            f.write("  { /* %s */\n" % nm)
            for y in range(SZ):
                f.write("    ")
                for x in range(SZ):
                    r, g, b, a = px[x, y]
                    f.write("0x%02X%02X%02X%02X," % (a, r, g, b))
                f.write("\n")
            f.write("  },\n")
        f.write("};\n")
    print("wrote %s (%d glyphs %dpx)" % (out, len(imgs), SZ))

    if preview:
        bg = Image.new("RGB", (SZ * len(imgs) + 20, SZ + 16), (30, 34, 46))
        x = 8
        for nm, img in imgs:
            tin = Image.new("RGBA", (SZ, SZ), (230, 234, 242, 255))
            tin.putalpha(img.split()[3])
            bg.paste(tin, (x, 8), tin); x += SZ + 4
        bg.save(preview); print("wrote preview %s" % preview)


if __name__ == "__main__":
    main()
