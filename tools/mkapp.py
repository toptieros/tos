#!/usr/bin/env python3
"""Assemble a tOS .app bundle (see design/app-package-format.md).

A bundle is a directory <Name>.app/ holding everything the OS needs to show and
launch an app -- a key=value `manifest`, the executable under `bin/`, and an
`icon.argb` (the binary icon format tools/genicons.py --app-icons emits):

    <Name>.app/
      manifest
      bin/<exec>
      icon.argb

twm scans /Apps/*.app at boot, reads each manifest, loads its icon, and builds the
dock from the bundles marked `pinned`. "Installing" an app is dropping its bundle
in /Apps; "uninstalling" is deleting it.

Usage:
  tools/mkapp.py --name Terminal --exec bin/term --icon Terminal.argb \\
                 [--elf build/term.elf] [--pinned true] [--category utility] \\
                 [--version 1.0] [--min-width 360] [--min-height 220] \\
                 --out fs/apps

Without --elf only the manifest + icon are written (the build packs the ELF into
bin/ separately); with --elf the ELF is copied to bin/<exec> for a complete bundle.
"""
import argparse, os, shutil


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--exec", dest="exe", required=True, help="exec path relative to the bundle, e.g. bin/term")
    ap.add_argument("--icon", help="path to an icon.argb to copy in")
    ap.add_argument("--elf", help="optional ELF to copy to bin/<exec>")
    ap.add_argument("--version", default="1.0")
    ap.add_argument("--category", default="utility")
    ap.add_argument("--pinned", default="true")
    ap.add_argument("--min-width", type=int)
    ap.add_argument("--min-height", type=int)
    ap.add_argument("--out", required=True, help="directory to create <Name>.app under")
    a = ap.parse_args()

    bundle = os.path.join(a.out, a.name + ".app")
    os.makedirs(os.path.join(bundle, os.path.dirname(a.exe) or "."), exist_ok=True)

    lines = [
        "name      = %s" % a.name,
        "exec      = %s" % a.exe,
        "icon      = icon.argb",
        "version   = %s" % a.version,
        "category  = %s" % a.category,
        "pinned    = %s" % a.pinned,
    ]
    if a.min_width:  lines.append("min_width = %d" % a.min_width)
    if a.min_height: lines.append("min_height= %d" % a.min_height)
    with open(os.path.join(bundle, "manifest"), "w") as f:
        f.write("\n".join(lines) + "\n")

    if a.icon:
        shutil.copyfile(a.icon, os.path.join(bundle, "icon.argb"))
    if a.elf:
        shutil.copyfile(a.elf, os.path.join(bundle, a.exe))

    print("wrote bundle %s" % bundle)


if __name__ == "__main__":
    main()
