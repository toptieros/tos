#!/usr/bin/env python3
"""Fetch Lucide (MIT) line icons for the toolkit's APP glyph set, render them
anti-aliased, and bake a tintable alpha-mask header (user/lib/glyphs.h).

Sibling of tools/genstatus.py (the menu-bar status cluster). This set is the
general-purpose Lucide icons any toolkit app can draw -- e.g. the settings rows'
leading icons -- blitted recoloured to the theme via ugfx_blit_tint(). Each glyph
is rendered big via librsvg, downsampled with Lanczos, and emitted as white ARGB
(RGB=0xFFFFFF, A = stroke coverage).

Run once and commit the header; the OS build depends on neither PIL, librsvg, nor
the network. Re-fetch + regenerate:  python3 tools/genglyphs.py [out.h] [--preview p.png]
Requires: rsvg-convert (librsvg) + PIL, and network access for the fetch.
"""
import sys, io, subprocess, urllib.request
from PIL import Image

SZ  = 18         # glyph box (square), px -- matches the menu-bar status glyphs
SUP = 8          # supersample factor rendered by librsvg, then Lanczos-downsampled
RAW = "https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/%s.svg"

# C identifier suffix -> Lucide icon slug. Chosen to read as the settings rows:
# layers=drop shadows, image=wallpaper, palette=accent, panel-bottom/top=auto-hide
# dock/menu-bar, clock/timer/calendar=the clock rows, settings=the window title,
# toggle-right/left=an On/Off pill the value can show.
GLYPHS = [("SETTINGS", "settings"), ("SHADOWS", "layers"), ("WALLPAPER", "image"),
          ("ACCENT", "palette"), ("DOCK", "panel-bottom"), ("MENUBAR", "panel-top"),
          ("CLOCK", "clock"), ("SECONDS", "timer"), ("WEEKDAY", "calendar-days"),
          ("TOGGLE_ON", "toggle-right"), ("TOGGLE_OFF", "toggle-left")]


def render(slug):
    """Fetch + rasterise one Lucide glyph to an SZ*SZ white alpha mask."""
    svg = urllib.request.urlopen(RAW % slug, timeout=20).read()
    n = SZ * SUP
    png = subprocess.run(["rsvg-convert", "-w", str(n), "-h", str(n)],
                         input=svg, stdout=subprocess.PIPE, check=True).stdout
    big = Image.open(io.BytesIO(png)).convert("RGBA")
    alpha = big.resize((SZ, SZ), Image.LANCZOS).split()[3]   # keep coverage only
    if not alpha.getbbox():
        raise SystemExit("genglyphs: %s rendered empty -- check librsvg" % slug)
    mask = Image.new("RGBA", (SZ, SZ), (255, 255, 255, 0))   # white, to be tinted
    mask.putalpha(alpha)
    return mask


def main():
    out = "user/lib/glyphs.h"; preview = None
    args = sys.argv[1:]; i = 0
    while i < len(args):
        if args[i] == "--preview": preview = args[i + 1]; i += 2
        else: out = args[i]; i += 1

    imgs = [(nm, render(slug)) for nm, slug in GLYPHS]
    with open(out, "w") as f:
        f.write("/* Toolkit app glyphs baked from Lucide (MIT, lucide.dev) by\n")
        f.write(" * tools/genglyphs.py. White ARGB (0xAARRGGBB) alpha masks -- only the\n")
        f.write(" * alpha matters; recolour with ugfx_blit_tint(). Regenerate: python3 tools/genglyphs.py */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define GLYPH_SZ %d\n\n" % SZ)
        for idx, (nm, _) in enumerate(imgs):
            f.write("#define GLYPH_%s %d\n" % (nm, idx))
        f.write("\nstatic const uint32_t glyphs_argb[%d][%d * %d] = {\n" % (len(imgs), SZ, SZ))
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
