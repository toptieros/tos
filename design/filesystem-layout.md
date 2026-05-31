# Design guideline — tOS filesystem layout

> Status: **guideline / not yet implemented.** This is the agreed plan for how the
> on-disk tree should be organised once we move past the current flat root. tosfs
> is already hierarchical (v2: parent-indexed slot table, per-task cwd, mkdir/cd/
> mv/cp/rm -r), so this is a convention + a bit of init/loader work, not a new FS.

## Goal

Today everything (programs, text files, the `shortcuts` file) sits in the flat
root next to `home/` and `docs/`. We want a structured tree like Windows
(`Program Files`, `Users`, …) / macOS (`/Applications`, `/Users`, `/System`) so
there's an obvious home for the OS, for installed apps, and for per-user data.

## Proposed tree

```
/
├── System/                 # OS-owned, treated as read-only by convention
│   ├── bin/                # core programs: init, shell, twm, term, fastfetch
│   ├── lib/                # shared resources (system font, default theme, ...)
│   └── etc/                # system config (motd, default shortcuts, hostname)
├── Apps/                   # installed applications, one bundle each (see app-package-format.md)
│   ├── Files.app/
│   ├── Terminal.app/
│   └── Notepad.app/
├── Users/
│   └── root/               # the single user for now (multi-user is future work)
│       ├── Desktop/        # shortcuts the WM shows on the desktop / dock
│       ├── Documents/
│       ├── Downloads/
│       └── .config/        # per-user settings (theme overrides, dock order, ...)
└── tmp/                    # scratch; cleared on boot
```

Notes on the choices:
- **`/System/bin`** replaces today's root-level binaries. The kernel ELF loader
  resolves programs by path, and `fs_find()` currently resolves system binaries
  from root regardless of cwd — point that lookup at `/System/bin` so `exec("shell")`
  still works from anywhere.
- **`/Apps/<Name>.app/`** is where *user* apps live as self-contained bundles.
  twm builds the dock by scanning `/Apps/*.app/manifest` (so no more hand-edited
  flat `shortcuts` file). Core OS apps (term, files) can either live in `/System`
  or ship as `/Apps` bundles — recommend shipping them as bundles so there's one
  app model, with `/System/bin` reserved for the boot chain (init, shell, twm).
- **`/Users/root/Desktop`** holds the desktop/dock shortcuts (a small file or a
  set of `.lnk`-style entries), replacing the global `fs/shortcuts`. The WM reads
  the *current user's* Desktop, which sets up cleanly for multi-user later.
- **`.config`** gives apps a per-user place to persist state (dock order, window
  positions, theme) without touching `/System`.

## What has to change

1. **`tools/mkfs.c`** — pack the seed image into this tree instead of a flat root
   (it already auto-creates parent dirs for `dest=host` entries, so this is mostly
   changing the destination paths in the Makefile's `mkfs` invocation).
2. **`init` (PID 1)** — ensure the skeleton exists on boot (`mkdir -p` the
   `/Users/root/*` and `/tmp` dirs if missing; clear `/tmp`). A fresh disk should
   self-heal into a valid layout.
3. **Program resolution** — `fs_find()` / the loader should look up system
   binaries under `/System/bin` (and app execs under their bundle dir). Keep an
   absolute-path exec path so bundles can name `exec=/Apps/Files.app/files`.
4. **twm** — read shortcuts from `/Users/root/Desktop` (and/or scan `/Apps`)
   rather than `fs/shortcuts`.
5. **Shell `cd` default / `~`** — make the shell start in `/Users/root` and treat
   `~` as the home dir (`fs` already has per-task cwd + path resolution).
6. **Tests** — `t_seed_tree` etc. assert on `home/notes.txt`; update the seeded
   paths (e.g. `/Users/root/Documents/notes.txt`).

## Phasing (keep `make test` green at each step)

1. Introduce the dirs in `mkfs` + have init create the skeleton; move the seed
   text files. (No behaviour change to apps yet.)
2. Move the boot chain to `/System/bin`; fix program resolution.
3. Move the desktop shortcut source to `/Users/root/Desktop`.
4. Convert apps to `/Apps/*.app` bundles (depends on app-package-format.md).

## Out of scope for now

Real permissions / ownership (we run everything as root in ring 3), symlinks, and
mount points for multiple disks. The layout above is forward-compatible with a
future `Users/<name>` per real user and a `/dev` for device nodes.
