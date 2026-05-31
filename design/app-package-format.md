# Design guideline — tOS `.app` package format

> Status: **guideline / not yet implemented.** The plan for how an application is
> packaged so the OS knows its name, icon, entry point, and resources — the
> tOS analog of an `.app` bundle / `.apk` / `.exe`-with-resources.

## Goal

Right now an "app" is just an ELF in the flat root plus a hand-written line in
`fs/shortcuts` (`Label|program`), and its icon is baked into the compositor
(`user/lib/icons.h`). That doesn't scale: adding an app means editing twm, the
shortcuts file, and the icon header. We want a **self-contained bundle** that
carries everything the OS needs, so installing an app is "drop the bundle in
`/Apps`" and removing it is "delete the bundle".

## Two container options

| | **A. Directory bundle** (`MyApp.app/` is a folder) | **B. Zip archive** (`MyApp.app` is one file) |
|---|---|---|
| Reads on tOS today | ✅ tosfs has real directories; the loader execs by path | ❌ needs a zip/inflate reader in the loader |
| Single-file copy/distribute | ❌ it's a tree | ✅ one file, easy to move |
| Inspect/patch in the shell | ✅ `cd`, `cat`, `ls` just work | ❌ opaque until unpacked |
| Implementation cost | low (a manifest parser) | higher (decompressor + a temp extract or in-place zip FS) |

**Recommendation:** ship **A (directory bundle)** first — it works with what we
have and matches macOS `.app` (which is also "just a folder"). Keep the manifest
and internal layout identical to what a zip would contain, so moving to **B**
later is purely "teach the loader to read the bundle out of a zip" — the format
above the container doesn't change. (The user's preference for a single zip file
is the right *end state*; A is the cheap first step that doesn't block it.)

## Bundle layout

```
Files.app/
├── manifest            # key=value metadata the OS reads (see below)
├── bin/
│   └── files           # the ELF executable
├── icon.argb           # the app icon, baked ARGB (same format as user/lib/icons.h)
└── res/                # optional: extra resources the app opens at runtime
    └── ...
```

### `manifest` (simple `key = value`, one per line)

```
name      = Files
exec      = bin/files          # path relative to the bundle root
icon      = icon.argb
version   = 1.0
category  = utility            # used to group in a launcher later
pinned    = true               # show on the dock by default
min_width = 360                # optional window hints
min_height= 220
# permissions (future, advisory for now):
caps      = fs, window
```

Keep the parser dead-simple (the existing `fs/shortcuts` parser in twm is a good
model): trim whitespace, split on the first `=`, ignore blank/`#` lines. No need
for full TOML/JSON.

### `icon.argb`

Reuse the exact format `tools/genicons.py` emits (a small header: width, height,
then `0xAARRGGBB` pixels) so `ugfx_blit_argb` can draw it directly. A bundle that
omits an icon falls back to the generic `ICON_APP` tile. Provide a
`tools/mkicon.py` that turns a PNG into this format so app authors don't hand-roll
pixels.

## How the OS consumes a bundle

- **Install:** copy `MyApp.app/` into `/Apps` (a future Files-app action, or just
  `cp -r`). **Uninstall:** `rm -r /Apps/MyApp.app`.
- **twm / launcher:** on startup, scan `/Apps/*.app`, read each `manifest`, load
  `icon.argb`, and build the dock from the bundles whose `pinned = true`
  (replacing the baked icon table + the flat `shortcuts` file). Double-click
  `exec`s `<bundle>/exec`.
- **Loader:** `exec` takes an absolute path; for a dir bundle that's just
  `/Apps/Files.app/bin/files`. The kernel ELF loader already loads by path, so no
  kernel change is needed for option A. Option B would add: open the `.app` zip,
  locate the `exec` member, and either extract it to `/tmp` and exec that, or
  teach the loader to read an ELF straight out of the archive.
- **Resources:** an app resolves `res/...` relative to its own bundle. Give it its
  bundle path via an env-like mechanism or a `SYS_APP_DIR` query (small addition),
  so apps don't hard-code paths.

## What has to change (when we build it)

1. A manifest parser (shared lib, reused by twm and any launcher).
2. `tools/mkapp.py` — assemble `bin/`, `manifest`, `icon.argb` into a bundle dir
   (and later, zip it for option B).
3. twm: scan `/Apps`, build the dock from manifests + bundle icons; drop the baked
   `icons.h` dock path and the flat `shortcuts` file.
4. `mkfs` / the seed image: ship `term`, `files`, `notepad` as `/Apps/*.app` bundles
   (ties into [filesystem-layout.md](filesystem-layout.md)).
5. Optional `SYS_APP_DIR` so an app can find its own resources.
6. **Later (option B):** an inflate/zip reader so a `.app` can be a single file.

## Phasing

1. Define the manifest + bundle layout; write `mkapp.py`; ship ONE app (e.g.
   Hello) as `/Apps/Hello.app` and have twm read it alongside the existing dock.
2. Move all apps to bundles; twm builds the whole dock from `/Apps`; retire
   `shortcuts` + the baked dock icons.
3. Bundle icons replace `user/lib/icons.h` for apps (keep baked cursors/buttons —
   those are OS chrome, not app assets).
4. Single-file zip `.app` (the end state) once a decompressor exists.
