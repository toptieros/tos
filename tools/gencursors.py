#!/usr/bin/env python3
"""Bake the tOS cursor set from a real macOS-style Xcursor theme.

Source: the "apple_cursor" project (github.com/ful1e5/apple_cursor, GPL-3.0) -- a
license-clean macOS pointer family. We parse its Xcursor binary files, pick the
nominal size nearest our target, downsample to CURSOR_W x CURSOR_H with smooth
edges, and emit 0xAARRGGBB pixels that ugfx_blit_argb() blends. The output header
layout is exactly what twm already consumes (CUR_* indices + cursor_hotspot[] +
cursors_argb[]), so no compositor change is needed.

Shapes: arrow, I-beam, pointing hand, the four resize double-arrows, and a
12-frame busy spinner sampled from the theme's animated 'wait' cursor.

Usage:
  python3 tools/gencursors.py [out.h] [--variant macOS-White|macOS]
                              [--theme DIR] [--preview out.png]

The theme is fetched + cached under --theme (default /tmp/curtheme) on first run.
Run once and commit the header; the build does not depend on PIL or the theme.
"""
import sys, os, struct, io, lzma, tarfile, urllib.request
from PIL import Image

SIZE = 24                       # output cursor is SIZE x SIZE (matches twm's CURSOR_W/H)
TARGET = 24                     # prefer the source nominal size nearest this (downsample, never up)
NBUSY = 12                      # spinner frames twm expects (CUR_BUSY_FRAMES)
REL = "v2.0.1"
URL = "https://github.com/ful1e5/apple_cursor/releases/download/%s/{v}.tar.xz" % REL
IMG_TYPE = 0xfffd0002

# slot name -> ordered X11 cursor-name candidates (first that exists wins).
SHAPES = [
    ("ARROW",       ["left_ptr", "arrow", "default"]),
    ("IBEAM",       ["xterm", "text", "ibeam"]),
    ("HAND",        ["hand2", "pointer", "pointing_hand", "hand1"]),
    ("RESIZE_NWSE", ["bd_double_arrow", "size_fdiag", "bottom_right_corner", "nwse-resize"]),
    ("RESIZE_NESW", ["fd_double_arrow", "size_bdiag", "bottom_left_corner", "nesw-resize"]),
    ("RESIZE_WE",   ["sb_h_double_arrow", "size_hor", "h_double_arrow", "ew-resize"]),
    ("RESIZE_NS",   ["sb_v_double_arrow", "size_ver", "v_double_arrow", "ns-resize"]),
]
SPINNER = ["wait", "watch", "left_ptr_watch", "progress"]
# slots that read better with a centred hotspot (the source corner/tip hotspots
# are wrong for our use as affordances): I-beam, every resize arrow, the spinner.
CENTRE_HOTSPOT = {"IBEAM", "RESIZE_NWSE", "RESIZE_NESW", "RESIZE_WE", "RESIZE_NS"}


def fetch_theme(variant, root):
    cdir = os.path.join(root, variant, "cursors")
    if os.path.isdir(cdir):
        return cdir
    os.makedirs(root, exist_ok=True)
    url = URL.format(v=variant)
    sys.stderr.write("fetching %s ...\n" % url)
    data = urllib.request.urlopen(url).read()
    with tarfile.open(fileobj=io.BytesIO(lzma.decompress(data))) as tf:
        tf.extractall(root)
    return cdir


def find_cursor(cdir, names):
    for n in names:
        p = os.path.join(cdir, n)
        if os.path.exists(p):
            return os.path.realpath(p)
    return None


def parse_xcursor(path):
    """Return [(size, w, h, xhot, yhot, delay, premultiplied-RGBA Image), ...] for
    each image chunk, in file (animation) order."""
    with open(path, "rb") as f:
        data = f.read()
    magic, hsz, _ver, ntoc = struct.unpack_from("<4sIII", data, 0)
    assert magic == b"Xcur", "not an Xcursor file: %s" % path
    out = []
    for i in range(ntoc):
        typ, _sub, pos = struct.unpack_from("<III", data, hsz + i * 12)
        if typ != IMG_TYPE:
            continue
        chsz, _ct, csz, _cv, w, h, xhot, yhot, delay = struct.unpack_from("<IIIIIIIII", data, pos)
        px = pos + chsz
        # Xcursor pixels are premultiplied-alpha ARGB stored little-endian == BGRA
        # bytes; PIL's "BGRA" raw decoder maps them straight to an RGBA image.
        img = Image.frombytes("RGBA", (w, h), data[px:px + w * h * 4], "raw", "BGRA", 0, 1)
        out.append((csz, w, h, xhot, yhot, delay, img))
    return out


def pick_size(frames):
    sizes = sorted({fr[0] for fr in frames})
    ge = [s for s in sizes if s >= TARGET]
    return min(ge) if ge else max(sizes)


def finalize(img):
    """premultiplied native RGBA -> straight-alpha SIZE x SIZE RGBA (resize is done
    in premultiplied space, which is correct for alpha; un-premultiply after)."""
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    px = img.load()
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    o = out.load()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            if a:
                r = min(255, r * 255 // a); g = min(255, g * 255 // a); b = min(255, b * 255 // a)
            else:
                r = g = b = 0
            o[x, y] = (r, g, b, a)
    return out


def shape_image(path, name):
    frames = parse_xcursor(path)
    sz = pick_size(frames)
    fr = next(f for f in frames if f[0] == sz)
    _s, w, h, xhot, yhot, _d, img = fr
    if name in CENTRE_HOTSPOT:
        hs = (SIZE // 2, SIZE // 2)
    else:
        hs = (round(xhot * SIZE / w), round(yhot * SIZE / h))
    return finalize(img), hs


def spinner_frames(path):
    frames = parse_xcursor(path)
    sz = pick_size(frames)
    grp = [f for f in frames if f[0] == sz]           # animation frames at the chosen size
    imgs = [finalize(f[6]) for f in grp]
    if not imgs:
        raise SystemExit("spinner has no frames")
    # resample to exactly NBUSY frames (evenly across the loop)
    return [imgs[(i * len(imgs)) // NBUSY] for i in range(NBUSY)]


def emit(out, seq, busy0):
    with open(out, "w") as f:
        f.write("/* tOS cursor set, baked by tools/gencursors.py from the apple_cursor\n")
        f.write(" * macOS-style Xcursor theme (github.com/ful1e5/apple_cursor, GPL-3.0).\n")
        f.write(" * %dx%d ARGB (0xAARRGGBB) per cursor; blit with ugfx_blit_argb() at\n" % (SIZE, SIZE))
        f.write(" * (x - hotspot_x, y - hotspot_y). Regenerate: python3 tools/gencursors.py */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define CURSOR_W %d\n#define CURSOR_H %d\n" % (SIZE, SIZE))
        f.write("#define NCURSORS %d\n" % len(seq))
        f.write("#define CUR_BUSY0 %d\n#define CUR_BUSY_FRAMES %d\n\n" % (busy0, NBUSY))
        for idx, (name, _img, _hs) in enumerate(seq):
            if not name.startswith("BUSY_"):
                f.write("#define CUR_%s %d\n" % (name, idx))
        f.write("\nstatic const int cursor_hotspot[NCURSORS][2] = {\n")
        for name, _img, hs in seq:
            f.write("  { %d, %d },  /* %s */\n" % (hs[0], hs[1], name))
        f.write("};\n\n")
        f.write("static const uint32_t cursors_argb[NCURSORS][CURSOR_W * CURSOR_H] = {\n")
        for name, img, _hs in seq:
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


def preview(path, seq):
    cell, pad = SIZE, 8
    cols = len(seq)
    strips = [(28, 31, 40), (90, 96, 110), (220, 224, 232)]   # dark / mid / light
    W = cols * (cell + pad) + pad
    H = len(strips) * (cell + pad) + pad
    bg = Image.new("RGB", (W, H))
    for si, col in enumerate(strips):
        band = Image.new("RGB", (W, cell + pad), col)
        bg.paste(band, (0, si * (cell + pad) + pad))
        x = pad
        for _n, img, _hs in seq:
            bg.paste(img, (x, si * (cell + pad) + pad), img)
            x += cell + pad
    bg.save(path)


def main():
    out = "user/lib/cursors.h"; variant = "macOS-White"; theme_root = "/tmp/curtheme"; prev = None
    a = sys.argv[1:]; i = 0
    while i < len(a):
        if a[i] == "--variant": variant = a[i + 1]; i += 2
        elif a[i] == "--theme": theme_root = a[i + 1]; i += 2
        elif a[i] == "--preview": prev = a[i + 1]; i += 2
        else: out = a[i]; i += 1

    cdir = fetch_theme(variant, theme_root)
    seq = []
    for name, names in SHAPES:
        p = find_cursor(cdir, names)
        if not p:
            raise SystemExit("missing cursor for %s (tried %s)" % (name, names))
        img, hs = shape_image(p, name)
        seq.append((name, img, hs))
    busy0 = len(seq)
    sp = find_cursor(cdir, SPINNER)
    if not sp:
        raise SystemExit("missing spinner cursor (tried %s)" % SPINNER)
    for fi, img in enumerate(spinner_frames(sp)):
        seq.append(("BUSY_%d" % fi, img, (SIZE // 2, SIZE // 2)))

    emit(out, seq, busy0)
    print("wrote %s (%s, %d cursors, %dx%d)" % (out, variant, len(seq), SIZE, SIZE))
    if prev:
        preview(prev, seq)
        print("wrote preview %s" % prev)


if __name__ == "__main__":
    main()
