#!/usr/bin/env python3
"""Generate the tOS icon set: macOS-style app tiles + file-type icons.

App icons are squarish rounded-corner tiles (a superellipse-ish rounded rect with
a soft gradient, a top sheen, and a simple white glyph) -- one per app. File icons
are small document/folder glyphs for the Files app. All rendered at 4x and
downsampled, emitted as 0xAARRGGBB for ugfx_blit_argb().

Usage:  python3 tools/genicons.py [out.h] [--preview out.png] [--app-icons DIR]
Run once and commit the header; the build does not depend on PIL.

--app-icons DIR also writes each app tile as a standalone binary icon.argb
(the .app bundle icon format: u32 width, u32 height, then w*h little-endian
0xAARRGGBB pixels) named <Label>.argb, for packing into /Apps/<Label>.app.
"""
import sys, os, struct
from PIL import Image, ImageDraw

APP = 48        # app tile array/display size (APPICON_SZ); the dock blits it 1:1
FILE = 64       # file-type icon array res -- downsampled crisply in the Files lists
                # and shown 1:1 in the 64px details preview
BUNDLE = 128    # .app bundle icon.argb master res; dock/launchpad/switcher scale it
                # DOWN with the smooth resampler, so the visible app tiles stay crisp
SS = 4          # supersample factor for anti-aliased rendering

def rrect_mask(S, rad):
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, S - 1, S - 1], radius=rad, fill=255)
    return m

def tile(c_top, c_bot, glyph, out=APP):
    """A rounded-square app tile with a vertical gradient, sheen, and a glyph,
    rendered supersampled and downsized to `out` px (the glyphs are S/48 scaled,
    so any output size stays proportional)."""
    S = out * SS
    rad = int(out * 0.235) * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    grad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        t = y / (S - 1)
        col = tuple(int(c_top[k] * (1 - t) + c_bot[k] * t) for k in range(3))
        gd.line([(0, y), (S, y)], fill=col + (255,))
    img.paste(grad, (0, 0), rrect_mask(S, rad))
    d = ImageDraw.Draw(img)
    # top sheen
    sheen = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sheen).rounded_rectangle([2 * SS, 2 * SS, (out - 3) * SS, (out * 0.45) * SS],
                                            radius=rad, fill=(255, 255, 255, 38))
    img.alpha_composite(sheen)
    glyph(d, S)
    return img.resize((out, out), Image.LANCZOS)

WHITE = (245, 248, 252, 255)

def g_terminal(d, S):
    u = S / 48.0
    # inset dark screen
    d.rounded_rectangle([12 * u, 14 * u, 36 * u, 35 * u], radius=4 * u, fill=(18, 22, 30, 255))
    lw = int(2.4 * u)
    # ">" chevron
    d.line([(17 * u, 21 * u), (22 * u, 25 * u), (17 * u, 29 * u)], fill=WHITE, width=lw, joint="curve")
    # underscore cursor
    d.line([(24 * u, 30 * u), (31 * u, 30 * u)], fill=WHITE, width=lw)

def g_folder(d, S):
    u = S / 48.0
    d.rounded_rectangle([12 * u, 18 * u, 36 * u, 34 * u], radius=3 * u, fill=WHITE)
    d.polygon([(12 * u, 19 * u), (12 * u, 16 * u), (20 * u, 16 * u), (23 * u, 19 * u)], fill=WHITE)  # tab
    d.rounded_rectangle([14 * u, 21 * u, 34 * u, 32 * u], radius=2 * u, fill=(255, 255, 255, 70))

def g_app(d, S):
    u = S / 48.0
    # a little window with a title bar (toolkit/app)
    d.rounded_rectangle([13 * u, 14 * u, 35 * u, 34 * u], radius=3 * u, fill=WHITE)
    d.rectangle([13 * u, 14 * u, 35 * u, 20 * u], fill=(255, 255, 255, 120))
    for i in range(3):
        d.ellipse([(16 + i * 3) * u, 16 * u, (17.6 + i * 3) * u, 17.6 * u], fill=(60, 70, 90, 255))

def g_gear(d, S):
    u = S / 48.0
    d.ellipse([17 * u, 17 * u, 31 * u, 31 * u], outline=WHITE, width=int(3 * u))
    d.ellipse([21 * u, 21 * u, 27 * u, 27 * u], fill=WHITE)

def g_note(d, S):
    u = S / 48.0                                    # a sheet of ruled paper (Notepad)
    d.rounded_rectangle([14 * u, 12 * u, 34 * u, 36 * u], radius=3 * u, fill=WHITE)
    for i in range(4):
        d.line([(18 * u, (18 + i * 4.4) * u), (30 * u, (18 + i * 4.4) * u)],
               fill=(150, 120, 60, 255), width=max(1, int(1.6 * u)))

# ---- file-type icons (small documents / folder) ----------------------------
def doc(base, fold=(230, 234, 240), accent=None, mark=None, out=FILE):
    S = out * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = S / 22.0
    if base == "folder":
        back  = (74, 134, 222, 255)        # darker back panel
        front = (116, 176, 244, 255)       # lighter front flap
        sheen = (150, 198, 248, 255)
        # back panel with a rounded tab
        d.polygon([(3 * u, 7.5 * u), (3 * u, 5.5 * u), (8.5 * u, 5.5 * u), (10.8 * u, 7.5 * u)], fill=back)
        d.rounded_rectangle([3 * u, 6.5 * u, 19 * u, 17.5 * u], radius=2.2 * u, fill=back)
        # front flap, slightly lower, with a thin top sheen line
        d.rounded_rectangle([3 * u, 9 * u, 19 * u, 17.8 * u], radius=2.2 * u, fill=front)
        d.line([(5 * u, 9.6 * u), (17 * u, 9.6 * u)], fill=sheen, width=max(1, int(0.6 * u)))
        return img.resize((out, out), Image.LANCZOS)
    # a page with a folded corner
    body = fold
    d.polygon([(5 * u, 3 * u), (13 * u, 3 * u), (17 * u, 7 * u), (17 * u, 19 * u), (5 * u, 19 * u)], fill=body + (255,))
    d.polygon([(13 * u, 3 * u), (13 * u, 7 * u), (17 * u, 7 * u)], fill=(190, 196, 206, 255))   # fold
    if accent:
        d.rectangle([5 * u, 3 * u, 17 * u, 6 * u], fill=accent + (255,))   # coloured header band
    if mark == "lines":
        for i in range(3):
            d.line([(7 * u, (9 + i * 2.5) * u), (15 * u, (9 + i * 2.5) * u)], fill=(120, 128, 140, 255), width=int(1.0 * u))
    elif mark == "exec":
        d.line([(7.5 * u, 10 * u), (9.5 * u, 12 * u), (7.5 * u, 14 * u)], fill=(70, 170, 90, 255), width=int(1.2 * u), joint="curve")
        d.line([(10.5 * u, 14 * u), (14 * u, 14 * u)], fill=(70, 170, 90, 255), width=int(1.2 * u))
    elif mark == "image":
        d.ellipse([7 * u, 9 * u, 9.5 * u, 11.5 * u], fill=(245, 200, 90, 255))
        d.polygon([(6 * u, 17 * u), (10 * u, 12 * u), (13 * u, 15 * u), (15 * u, 13 * u), (16 * u, 17 * u)], fill=(90, 170, 120, 255))
    return img.resize((out, out), Image.LANCZOS)

def write_argb_bin(path, img):
    """Bundle icon format: u32 width, u32 height, then w*h LE 0xAARRGGBB pixels."""
    w, h = img.size
    px = img.load()
    with open(path, "wb") as f:
        f.write(struct.pack("<II", w, h))
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                f.write(struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b))


# app tile name -> the .app bundle label its icon.argb is written under
APP_LABEL = {"TERMINAL": "Terminal", "FILES": "Files",
             "NOTEPAD": "Notepad", "SETTINGS": "Settings", "APP": "app"}


def main():
    out = "user/lib/icons.h"; preview = None; app_icons_dir = None
    args = sys.argv[1:]; i = 0
    while i < len(args):
        if args[i] == "--preview": preview = args[i + 1]; i += 2
        elif args[i] == "--app-icons": app_icons_dir = args[i + 1]; i += 2
        else: out = args[i]; i += 1

    # (name, gradient-top, gradient-bottom, glyph) -- rendered to the small in-binary
    # array (APP px, the dock 1:1 size) and, for the bundles, to the BUNDLE master.
    app_specs = [("TERMINAL", (58, 64, 82),    (32, 36, 50),  g_terminal),
                 ("FILES",    (86, 150, 240),  (44, 96, 200), g_folder),
                 ("NOTEPAD",  (247, 207, 112), (224, 162, 58), g_note),
                 ("SETTINGS", (132, 142, 162), (74, 82, 104), g_gear),
                 ("APP",      (96, 104, 124),  (60, 66, 82),  g_gear)]
    apps = [(nm, tile(ct, cb, gl, APP)) for nm, ct, cb, gl in app_specs]
    files = [("FOLDER",  doc("folder")),
             ("TEXT",    doc("doc", accent=(96, 152, 232), mark="lines")),
             ("EXEC",    doc("doc", accent=(80, 180, 100), mark="exec")),
             ("IMAGE",   doc("doc", accent=(232, 150, 90), mark="image")),
             ("FILE",    doc("doc", mark="lines"))]

    def emit_arr(f, name, seq, sz):
        f.write("static const uint32_t %s[%d][%d * %d] = {\n" % (name, len(seq), sz, sz))
        for nm, img in seq:
            px = img.load()
            f.write("  { /* %s */\n" % nm)
            for y in range(sz):
                f.write("    ")
                for x in range(sz):
                    r, g, b, a = px[x, y]
                    f.write("0x%02X%02X%02X%02X," % (a, r, g, b))
                f.write("\n")
            f.write("  },\n")
        f.write("};\n\n")

    with open(out, "w") as f:
        f.write("/* tOS icon set (app tiles + file-type icons), baked by tools/genicons.py.\n")
        f.write(" * ARGB (0xAARRGGBB); blit with ugfx_blit_argb(). Regenerate: python3 tools/genicons.py */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define APPICON_SZ %d\n#define FILEICON_SZ %d\n\n" % (APP, FILE))
        for idx, (nm, _) in enumerate(apps):
            f.write("#define ICON_%s %d\n" % (nm, idx))
        f.write("\n")
        for idx, (nm, _) in enumerate(files):
            f.write("#define FILEICON_%s %d\n" % (nm, idx))
        f.write("\n")
        emit_arr(f, "appicons_argb", apps, APP)
        emit_arr(f, "fileicons_argb", files, FILE)
    print("wrote %s (%d app icons %dpx, %d file icons %dpx)" % (out, len(apps), APP, len(files), FILE))

    if app_icons_dir:
        os.makedirs(app_icons_dir, exist_ok=True)
        for nm, ct, cb, gl in app_specs:          # bundles get the hi-res BUNDLE master
            p = os.path.join(app_icons_dir, "%s.argb" % APP_LABEL[nm])
            write_argb_bin(p, tile(ct, cb, gl, BUNDLE))
            print("wrote %s (%dpx master)" % (p, BUNDLE))

    if preview:
        bg = Image.new("RGB", (APP * len(apps) + 20, APP + FILE + 40), (28, 32, 44))
        x = 8
        for nm, img in apps:
            bg.paste(img, (x, 8), img); x += APP + 2
        x = 8
        for nm, img in files:
            bg.paste(img.resize((FILE * 2, FILE * 2), Image.LANCZOS), (x, APP + 16)); x += FILE * 2 + 6
        bg.save(preview); print("wrote preview %s" % preview)

if __name__ == "__main__":
    main()
