# Design guideline — tOS packaging: the `tos` command, apps & packages

> Status: **design / not yet built.** The plan for `tos`, the single command-line
> entry point for getting software onto a tOS system — installing/removing **apps**
> (GUI bundles) and **packages** (headless components), tracking what's installed,
> and (with [`repository.md`](repository.md)) fetching and upgrading from a remote
> repo. The on-disk **`.app` bundle** format already exists
> ([`app-package-format.md`](app-package-format.md)) and twm already scans `/Apps`;
> this doc adds the package format, the install **engine**, the **receipts
> database**, and the `tos` command surface that ties them together.
>
> Companions: [`app-package-format.md`](app-package-format.md) (the `.app` bundle
> we install), [`installation.md`](installation.md) (the OS installer — same
> copy/format primitives, system-scale), [`system-ownership.md`](system-ownership.md)
> (who may write `/System`), [`app-runtime.md`](app-runtime.md) (an installer is a
> trusted, elevated task), [`repository.md`](repository.md) (the remote half:
> `tos get` / `tos sync`).

## The mental model: two kinds of installable unit

The user's framing is exactly right, and it maps onto a clean split that mirrors
macOS (`/Applications` vs Homebrew/system frameworks) and Android (APKs vs system
components):

| | **App** | **Package** |
|---|---|---|
| Example | `google-chrome`, Files, Notepad | `git`, the toolchain, a font, lwIP, libposix |
| What it is | a **GUI application**, user-facing | a **headless component**: CLI tool, library, runtime, resource |
| Format | a **`.app` bundle** ([`app-package-format.md`](app-package-format.md)) | a **`.tpkg`** (defined here) |
| Lives in | `/Apps/<Name>.app/` | under `/System` (e.g. `/System/bin`, `/System/lib`, `/System/share`) |
| Surfaces in | the **dock / Launchpad / Spotlight** (twm scans `/Apps`) | the **shell** (a new command on `PATH`), not the dock |
| Owned by | the **user** if they installed it; **system** if it shipped with the OS | **system** (it's part of the OS surface) — install needs elevation |
| Command | `tos app install / uninstall` | `tos package add / remove` |

Both are *installable units* with a manifest, a version, a payload, and a
dependency list. The difference is **where the payload lands** and **how the user
encounters it** — so the install/remove machinery is **one engine** with two
front-ends, not two systems.

## The `tos` command surface

One binary (`/System/bin/tos`), subcommand-dispatched — the umbrella the user
typed: `tos package add git`, `tos app install Foo.app`, `tos get install …`,
`tos sync`.

```
tos app install <file.app>      install a local .app bundle into /Apps
tos app uninstall <name>        remove an installed app + its receipt
tos app list                    list installed apps (name, version)
tos app info <name>             manifest + receipt (files, size, source)

tos package add <name>          install a package (alias: tos pkg add)
tos package remove <name>       remove a package (refuses if depended-on)
tos package list                installed packages
tos package info <name>         manifest + receipt + reverse-deps
tos package upgrade <name>      replace with a newer local/cataloged version

tos get install <name>          fetch (app OR package) from the repo, then install   ─┐
tos get-app install <name>      friendly alias when you know it's an app              ├─ network half:
tos search <query>              search the cached catalog                             │  repository.md
tos sync                        refresh catalog + upgrade everything (incl. system)  ─┘
```

`tos app` / `tos package` are **local** (install from a file you already have) and
work without networking. `tos get` / `tos search` / `tos sync` are the **online**
front-ends defined in [`repository.md`](repository.md); they resolve a name to an
artifact, download + verify it, then call the very same local engine. So the
dependency arrow is one-way: *get/sync → local install engine*, and the local path
is always usable on its own (and testable offline).

> **Why one `tos` umbrella** rather than separate `tos-app`/`tos-pkg` binaries:
> shared engine, one help surface, one tab-completion target
> ([`shell.md`](shell.md): `tos <Tab>` lists the subcommands with descriptions),
> and it reads well — `tos package add git` is the user's own phrasing.

## The package format — `.tpkg`

A package is the headless sibling of a `.app` bundle, and we keep the format
**deliberately parallel** so the manifest parser, the icon/resource conventions,
and the copy primitive are shared rather than duplicated. Like `.app`
([`app-package-format.md`](app-package-format.md)), ship the **directory bundle**
first (works with tosfs's real directories and the existing loader) and keep the
layout identical to what a single-file archive would contain, so "make it one
file" is later purely a decompressor change.

```
git.tpkg/
├── manifest                 # key=value, same parser as .app manifests
└── root/                    # payload tree, laid down relative to the install prefix
    ├── bin/git              # -> /System/bin/git
    └── share/git/...        # -> /System/share/git/...
```

### `manifest` (same `key = value` grammar as `.app`)

```
name      = git
version   = 2.44
kind      = package           # app | package  (app bundles already imply 'app')
summary   = Distributed version control
prefix    = /System           # where root/ is laid down (default /System for packages)
deps      = libz, libcurl     # other package names this needs (no version constraints yet)
provides  = git               # the command(s)/capability it adds (for conflict + search)
# post-install / pre-remove hooks are intentionally omitted for now (see Out of scope)
```

`kind` is the one field that distinguishes the two unit types; a `.app` manifest
is implicitly `kind = app`. Everything else (`name`, `version`, `deps`, `summary`)
is common, so `tos list`/`info`/`search` treat apps and packages uniformly and
only the **install destination** differs (`/Apps/<Name>.app` for an app; the
package's `prefix` + `root/` for a package). Reuse `user/lib/manifest.h`'s parser
verbatim — it already trims, splits on the first `=`, and ignores `#`/blank lines.

## The install engine (shared core)

One routine, used by `tos app install`, `tos package add`, `tos get`, and `tos
sync` alike. It is essentially the installer's `copytree`
([`installation.md`](installation.md)) plus bookkeeping:

```
install(unit):
  1. locate    — a local path (tos app/package) or a downloaded artifact (tos get)
  2. read      — parse the manifest; reject if kind/name/version missing
  3. verify    — checksum the payload against the catalog hash (online path)
  4. authorize — packages write /System ⇒ require uid 0 / elevation
                 (system-ownership.md: may_write); apps to /Apps are user-owned
  5. resolve   — for each dep not already installed, install it first (transitively)
  6. lay down  — copytree:  app  -> /Apps/<Name>.app/
                            pkg  -> <prefix> from root/   (record every created path)
  7. record    — write a RECEIPT (below) listing exactly what was created
  8. activate  — app: twm rescans /Apps (dock/Launchpad pick it up, no restart)
                 pkg: the new bin is on PATH; refresh the shell's command cache
```

`copytree(src, dst)` — `readdir` + create + stream — is the same primitive the OS
installer needs, so build it once in a shared lib and let both callers use it.

### The receipts database (what makes uninstall exact)

Without a record of what an install created, removal is guesswork. So every
install writes a **receipt** — one file per installed unit — under a small package
DB:

```
/System/var/tos/
├── db/
│   ├── git           # receipt: kind, version, source, sha256, and the EXACT file list
│   └── Notepad.app   # apps get receipts too (esp. user-installed ones)
└── catalog           # cached copy of the repo index (repository.md)
```

A receipt records `kind`, `version`, `source` (local file / repo URL),
`installed` (timestamp), `deps`, and the **explicit list of files + dirs created**.
That gives us, with no heuristics:

- **Exact uninstall** — delete precisely the recorded paths (and only-now-empty
  dirs), never orphaning or over-deleting.
- **Dependency safety** — `tos package remove` refuses if another installed unit's
  `deps` names this one (reverse-dep check over the receipts); `--force` overrides.
- **Upgrade as diff** — replace a unit by installing the new version's files and
  pruning paths the old receipt had but the new one doesn't.
- **The basis for `tos sync`** — comparing receipt versions to the catalog is how
  [`repository.md`](repository.md) knows what's out of date.

(The DB lives under `/System` because it describes system state and is
system-owned; reads are world-readable so `tos list` works unprivileged, writes go
through the elevated install path.)

## Dependencies (kept minimal first)

`deps` is a flat list of **package names, no version constraints**. The resolver
is a depth-first install of missing deps from the catalog/receipts, with cycle
detection — deliberately *not* a SAT solver. Conflicts (`provides` collision) are
reported, not auto-resolved. Version constraints, optional deps, and alternatives
are explicitly out of scope until there's a real ecosystem that needs them.

## Permissions & ownership

This is where packaging meets [`system-ownership.md`](system-ownership.md) — and tOS
is single-user with UAC-style elevation, **not** a root/`sudo` world:

- **Apps → `/Apps`.** A user-installed app and its receipt are **user-owned**; the
  user can later uninstall it without authenticating. Apps that *shipped* with the OS
  are system-owned (the mkfs seed stamps them) and are protected from deletion.
- **Packages → `/System`.** Writing `/System/bin`, `/System/lib`, … is a **protected**
  write, so it needs **authorization**. There is no "become root" — instead the action
  must be authorized one of the two ways in
  [`system-ownership.md`](system-ownership.md):
  - **Through the terminal** (`tos package add …` typed by hand): no standing
    authority, so it triggers the **UAC password prompt** to elevate that one action.
  - **Through the App Store / Installer**: that's a first-party, system-owned app
    holding the install capability, so tapping **Install** is the consent and it
    runs **without a per-action password**. Same operation, different trust on the
    requester — exactly the user's "the store won't prompt, the terminal will."
  Until enforcement lands this is advisory; once it lands, an unauthorized
  `tos package add` cleanly reports "authenticate to continue" rather than silently
  succeeding — the same gate the OS installer relies on.

So the **App Store app** carries the install capability (write protected paths +
register apps with twm) as a trusted system component; the **`tos` CLI** earns the
same authority per-invocation via the password prompt; ordinary apps have neither.

## Relationship to the OS installer

[`installation.md`](installation.md) is "install the *whole system* onto a disk";
`tos` is "install *one unit* into the running system." They are the same operation
at different scales and should **share the primitives**: `copytree`, the manifest
parser, raw/streamed file writes, and (for `tos sync`'s system updates) the
boot-partition writer. Build those once; the installer lays the initial
`/System` + `/Apps`, and `tos` mutates them afterward.

## What has to change (to build it)

1. **argv passing** so `tos package add git` reaches the program with its
   arguments — the shared blocker called out in [`shell.md`](shell.md) (the loader
   currently uses the whole exec string as the path and seeds the data page with
   it; split path from argv). Until then, `tos` can live as a **shell built-in**
   that parses the line directly — but the engine should be a **library** so the
   built-in and the eventual `/System/bin/tos` binary share one implementation.
2. **`.tpkg` format + `tools/mkpkg.py`** — the host tool that assembles
   `manifest` + `root/` into a bundle, the sibling of the existing
   `tools/mkapp.py`.
3. **`copytree(src,dst)`** in a shared lib (with the installer).
4. **The receipts DB** — write/read/list/remove receipts under `/System/var/tos/`;
   the reverse-dep and diff logic.
5. **The `tos` engine + subcommand dispatch** (`app`, `package`, and stubs for
   `get`/`sync` until networking lands).
6. **twm rescan trigger** — a way for `tos app install` to ask the running twm to
   re-read `/Apps` so the new app appears without a restart (a WM message, or twm
   already rescans on focus — wire whichever is cheapest).
7. **(Later) single-file archives** — a decompressor so `.app`/`.tpkg` can be one
   file, shared with [`app-package-format.md`](app-package-format.md)'s option B.

## Phasing (keep `make test` green)

1. **`tos app install/uninstall <local .app>`** — copytree a local bundle into
   `/Apps`, trigger a twm rescan, remove it again. Almost entirely on top of
   what exists (bundles + the `/Apps` scan); proves the engine end-to-end with no
   network and no new format.
2. **`.tpkg` + receipts** — `mkpkg.py`; `tos package add/remove` from a **local**
   `.tpkg`; write/read receipts; exact uninstall. Ship one real package (e.g. a
   trivial CLI tool) as a `.tpkg` in the build and install it in a test.
3. **Deps + `list`/`info`** — transitive local resolution, reverse-dep refusal,
   `tos {app,package} list/info` off the receipts DB.
4. **Elevation gate** — once [`system-ownership.md`](system-ownership.md) enforces,
   make `tos package add` require uid 0 and report `-EPERM` cleanly otherwise.
5. **Wire the network front-ends** — `tos get` / `tos sync` call this engine once
   [`repository.md`](repository.md)'s client + networking exist.

## Out of scope (for now)

Version-constrained dependency solving, install/remove **hook scripts** (needs the
scripting language from [`shell.md`](shell.md) first), code signing / PKI (deferred
with [`repository.md`](repository.md)), partial/delta upgrades, multi-arch, paid
apps, and a GUI "App Store" front-end (a future `Store.app` would sit on top of
this same `tos get` client). One human user for now, so "per-user vs system-wide
install" is just "apps to `/Apps` (user), packages to `/System` (system)."

## Ties

- The bundle we install: [`app-package-format.md`](app-package-format.md).
- Same primitives, system scale: [`installation.md`](installation.md).
- Who may write `/System`: [`system-ownership.md`](system-ownership.md).
- The installer as a trusted/elevated task + the `net` cap `tos get` needs:
  [`app-runtime.md`](app-runtime.md).
- The remote repo, `tos get`/`tos sync`, and system updates:
  [`repository.md`](repository.md).
- The command line that drives `tos` + the argv dependency: [`shell.md`](shell.md).
- Where apps/packages/the DB live: [`filesystem-layout.md`](filesystem-layout.md).
