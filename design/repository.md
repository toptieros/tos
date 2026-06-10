# Design guideline — tOS software repository & remote updates (`tos get` / `tos sync`)

> Status: **design / roadmap, hard-gated on networking.** This is the "server"
> side of [`packaging.md`](packaging.md): the remote repository the `tos get` and
> `tos sync` commands pull from — the tOS analog of an APT mirror, a Homebrew tap,
> or an app-store backend — **plus** the payoff the user is really after: updating
> the system **over the network** instead of recompiling the image and rebooting.
> tOS has **no networking at all today** ([`virtio-net.md`](virtio-net.md):
> [`roadmap.md`](roadmap.md) Phase 4), so the online path can't run yet — but the
> **repository format** and the **client design** can be fixed now, and the whole
> client is exercisable against a **local mirror** (a mounted disk/path) before a
> single packet is sent.
>
> Companions: [`packaging.md`](packaging.md) (the local install engine `tos get`
> feeds; the receipts DB `tos sync` diffs), [`virtio-net.md`](virtio-net.md) (the
> NIC + stack + sockets this needs), [`installation.md`](installation.md) (the
> boot-partition writer a kernel update reuses), [`app-runtime.md`](app-runtime.md)
> (the `net` capability), [`roadmap.md`](roadmap.md) (Phase 4 + why "remote update"
> is high leverage).

## Goal & the leverage

Two things at once:

1. **Bring in outside software** — `tos get install <name>` fetches an app or
   package from a repository and installs it. This is what turns tOS from a
   "closed world that runs only its own apps" ([`roadmap.md`](roadmap.md)) into a
   system you can add to.
2. **Update the system without the build-and-reboot cycle** — `tos sync` brings the
   whole machine up to date over the wire: catalog → newer apps/packages → and the
   **base system itself**. Today every change means rebuild `tOS.img` on the host
   and reboot; the user's insight is that most of the system is *just files and
   processes* and can be replaced live. That's the real prize here, and it directly
   serves the roadmap ("stop recompiling everything and booting").

## Design choice: a *dumb* static server

The cheapest, most robust, most "small-OS" server is **no server logic at all** —
a plain static file host. All the intelligence lives in the `tos` client.

| | **Static file host** (recommended) | **Smart API server** |
|---|---|---|
| Server is | a directory of files behind HTTP (or even a mounted disk) | a service with a DB + query API |
| Cost to build/host | ~zero (any web server, or a local path) | a backend to write, deploy, secure |
| Client work | fetch an index + named files, resolve locally | thin client, server does resolution |
| Matches | Homebrew taps, static APT/Arch mirrors | the App Store, npm registry |
| tOS fit | ✅ no backend, mirrorable, offline-testable | ❌ overkill for a hobby OS |

**Recommendation: static.** The client downloads one **index** and then the
**named artifacts** it resolves against that index. This makes a mirror trivially
a copy of a directory, makes the whole thing testable from a local path before
networking exists, and keeps the only code we maintain on the *client* (where it's
shared with the local installer anyway).

## Repository layout (on the server)

```
repo/
├── index                      # the catalog: every available unit + the system channel
├── apps/
│   ├── Chrome-120.app.tar      # artifact: a .app bundle (archived; dir bundle until then)
│   └── ...
├── pkg/
│   ├── git-2.44.tpkg.tar       # artifact: a .tpkg package
│   └── ...
└── system/
    └── stable/                 # base-system images per channel (the OS itself)
        ├── 0.4.1/
        │   ├── kernel.bin       # + BOOTX64.EFI  (the boot artifacts — kernel update)
        │   └── userland.tar     # /System/bin/*, /System/lib, twm/term/shell, …
        └── ...
```

### `index` — the catalog (one file, our manifest grammar)

Reuse the **same `key = value` manifest grammar** as `.app`/`.tpkg`
([`packaging.md`](packaging.md)) rather than dragging in a JSON parser — one
record per unit, blank-line separated, parsed by `user/lib/manifest.h`. (If a real
JSON need shows up later we can switch; the static-file contract doesn't care about
the encoding.)

```
repo-format = 1
channel     = stable
system      = 0.4.1                # current base-system version on this channel

[unit]
name    = git
kind    = package
version = 2.44
size    = 184320
sha256  = 9f86d0818...             # integrity: client verifies before applying
deps    = libz, libcurl
url     = pkg/git-2.44.tpkg.tar    # relative to the repo root

[unit]
name    = Chrome
kind    = app
version = 120
sha256  = ...
url     = apps/Chrome-120.app.tar
```

The catalog carries **everything the resolver needs without a second request**:
name, kind, version, size, **sha256**, deps, and the artifact URL. `tos` caches it
locally as `/System/var/tos/catalog` ([`packaging.md`](packaging.md)) so `search`
and dependency resolution are offline once synced.

## The client (the network half of `tos`)

These are the `tos` subcommands that talk to the repo; each ends by calling the
**local install engine** in [`packaging.md`](packaging.md):

- **`tos sync`** — the umbrella the user named ("upgrade full system"). It:
  1. fetches the latest `index` → updates the local catalog cache (like
     `apt update`);
  2. diffs **installed receipts vs catalog** to find outdated apps/packages, then
     downloads + applies the newer versions (like `apt upgrade`);
  3. checks the catalog's `system =` version against the running base system and,
     if newer, performs the **system upgrade** (next section).
  (`tos update` = step 1 only; `tos upgrade [name]` = step 2 for everything or one
  unit — handy aliases, but `tos sync` is "make this machine current".)
- **`tos get install <name>`** — resolve `<name>` in the cached catalog → download
  the artifact (+ any missing deps' artifacts) → **verify sha256** → hand each to
  the local engine (`/Apps` for an app, `/System` for a package). `tos get-app
  install <name>` is the same thing with the friendly "I know it's an app" name.
- **`tos search <query>`** — substring match over the cached catalog's names +
  summaries; prints name, kind, version, one-line summary.

Integrity is non-negotiable even before signing: **every artifact is checked
against the catalog's `sha256`** before it's laid down, so a truncated or tampered
download is refused rather than installed. (Authenticity — proving the *catalog
itself* is genuine — is signing, deferred below.)

## The remote system upgrade (the "stop recompiling + rebooting" part)

This is the heart of the user's idea. Split the base system by *what can change
while running*:

| Part | Can hot-update? | How `tos sync` applies it |
|---|---|---|
| **Userland** — `twm`, `term`, `shell`, `/System/bin/*`, `/System/lib`, `/Apps` | **Yes** | replace the ELF/bundle on disk, then **relaunch** the affected process (swap `twm` → restart the desktop session; `shell`/apps pick up the new binary on next launch). Exactly a package upgrade — **no reboot.** |
| **Kernel** — `kernel.bin` / `BOOTX64.EFI` | **No** (it's the running code) | **stage** the new boot artifacts onto the boot partition / ESP, then prompt "**reboot to finish**." Applied atomically on next boot. |

So a `tos sync` that only touches userland and apps **never reboots** — the
running desktop reloads its pieces in place. A `tos sync` that includes a kernel
bump downloads + verifies + **stages** the kernel and tells the user a reboot will
complete it. That's the win: the day-to-day update loop drops the recompile and
(usually) the reboot.

**Doing it safely on today's FS.** tosfs has no journaling yet
([`roadmap.md`](roadmap.md) Phase 1), so apply updates **write-new-then-rename**:
download to a temp path, verify the hash, then atomically swap it into place
(rename over the old), and only delete the old after the new is in. Staging the
kernel reuses the installer's **raw boot-partition write**
([`installation.md`](installation.md)) — the same primitive that lays a bootable
layout in the first place. An A/B "keep the previous kernel to roll back to" scheme
is the natural hardening once that path exists.

**Authorization.** `tos get`/`tos sync`/`tos upgrade` all write protected, system-owned
paths, so they cross the elevation threshold in
[`system-ownership.md`](system-ownership.md): run from the **terminal** they prompt for
the password once and elevate for the run; run from a future first-party **"Software
Update" / App Store** surface they go through the trusted route with no per-action
password. Same two-route rule as installing a single package
([`packaging.md`](packaging.md)).

## Offline / local-mirror path (test before networking)

Because the server is just files, a **source can be a local path or a mounted
disk**, not only a URL. So `tos get`/`tos sync` can run against a `repo/` directory
on a second disk or a build-output folder **before any network stack exists** —
which is both the test harness for the whole client *and* a genuine feature
(install from a USB/mounted image). This mirrors [`installation.md`](installation.md)'s
"the live medium is a source." The `url` in the catalog is resolved relative to
whatever the configured source is (a path or an `http://` base).

## What has to change (to build it)

- **Networking** (the gate): virtio-net driver + a TCP/IP stack + a sockets
  syscall layer, gated by a `net` capability — all per [`virtio-net.md`](virtio-net.md)
  and [`app-runtime.md`](app-runtime.md). Then a small **HTTP client** in userspace
  (HTTP first; **TLS** is a later userspace library).
- **A catalog parser + cache** — reuse `manifest.h`'s grammar; store at
  `/System/var/tos/catalog`.
- **A sha256 implementation** in userspace (small, self-contained) for artifact
  verification.
- **The `tos` client subcommands** (`sync`/`get`/`search`/`upgrade`) on top of the
  [`packaging.md`](packaging.md) engine, with a **source abstraction** so a local
  path and an HTTP base are interchangeable.
- **Staged boot-partition writes** for kernel updates — the installer's raw block
  writer ([`installation.md`](installation.md)).
- **A live-relaunch hook** for userland updates — ask the running twm to restart a
  component (shares the rescan/relaunch mechanism with `tos app install`).
- **Host-side repo tooling** — a `make repo` target that builds the artifacts and
  emits a servable `repo/` tree (index + `apps/` + `pkg/` + `system/<channel>/`)
  straight from the existing build, so there's a real repository to point at.

## Phasing (keep `make test` green)

1. **The format + `make repo`.** Define the `index` grammar; add a host target that
   lays out a servable `repo/` from the build. No client yet — just a real
   repository to test against.
2. **Local-source client.** `tos search` / `tos get install` / `tos upgrade`
   against a `repo/` on a **mounted path** (no network): parse the catalog, resolve,
   verify sha256, install via the [`packaging.md`](packaging.md) engine. Exercises
   the entire client minus sockets — a test installs an app + a package from a
   local mirror.
3. **Networking lands** ([`virtio-net.md`](virtio-net.md)): NIC → stack → sockets →
   HTTP client. Point the source abstraction at an `http://` base; the rest is
   unchanged.
4. **System upgrade.** Userland in-place replace + live relaunch (no reboot); kernel
   **staged** to the boot partition + "reboot to finish." A test boots, `tos sync`s
   a newer userland from a local mirror, and asserts the new binary runs without a
   reboot.
5. **Hardening.** Repo **signing** (a public key baked in; a detached signature over
   `index`), channels (`stable`/`edge`), A/B kernel rollback, real mirrors.

## Out of scope (for now)

TLS and repository **signing/PKI** at first (start HTTP + sha256 integrity; add
authenticity once a crypto lib exists), **delta/incremental** downloads, a
smart-server query API, dependency *version-constraint* solving (see
[`packaging.md`](packaging.md)), ratings/reviews and a graphical store front (a
future `Store.app` rides on this same client), multi-architecture artifacts, and
bandwidth/CDN concerns. The aim here is the **contract** — a static catalog of
verified artifacts, a client that resolves + installs + upgrades against it, and a
live/staged path for updating the system itself.

## Ties

- The engine `tos get`/`tos sync` install through, and the receipts they diff:
  [`packaging.md`](packaging.md).
- The networking this is gated on, and the `net` cap:
  [`virtio-net.md`](virtio-net.md), [`app-runtime.md`](app-runtime.md).
- The raw boot-partition write a kernel update reuses, and "the medium is a
  source": [`installation.md`](installation.md).
- Why remote update is high-leverage, and where networking sits:
  [`roadmap.md`](roadmap.md) (Phase 4 + the closed-world problem).
- The command line that runs `tos sync`: [`shell.md`](shell.md).
