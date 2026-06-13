# Design guideline — tOS settings / system registry

> Status: **guideline + implementation spec.** This is the plan for a single place
> to keep system and per-user state (theme, dock, window positions, preferences),
> and the spec the registry implementation follows.

## Goal

Right now there is no place to persist "state and settings" — the theme lives in
`user/lib/theme.h` (compiled in), the dock comes from scanning `/Apps`, and nothing
a user changes survives a reboot. We want a **registry**: a small, typed key/value
store that any program (the compositor, the shell, apps, a future Settings app)
reads and writes, layered so the system ships defaults and the user overrides them.

## Why not SQLite

SQLite would be a poor fit for tOS: it expects a real libc, a VFS with file
locking, `mmap`, and a few hundred KB of code — none of which our freestanding
ring-3 environment provides. The data we need is a flat namespace of small typed
values, not relational tables. A **custom key/value store** is the right size:
a few hundred lines, debuggable with `cat`, and trivially backed by tosfs.

## Model

A flat namespace of dotted keys → string values, parsed on demand into the type
the reader wants:

```
theme.accent      = #5AA0FC
theme.mode        = dark
desktop.wallpaper = gradient
ui.bar.autohide   = false
ui.dock.autohide  = false
ui.dock.size      = 48
clock.format      = 24h
dock.pinned       = Terminal,Files
```

Conventions: keys are `area.name` (or `area.sub.name`), lowercase, dotted. Values
are plain text; the reader interprets them (`reg_int`, `reg_bool`, a CSV list, a
`#RRGGBB` colour). No nesting, no schema enforcement — keep it dead simple, like
the `.app` manifest parser.

### Dock pinning is policy + user choice, never the app's call

`dock.pinned` (a CSV of app names) is the **OS-shipped default-pinned set**. Because
it lives in the system layer (`/System/etc/registry`, which an installed app can't
write — `design/system-ownership.md`), no downloaded app can pin itself; the manifest
has no `pinned` field at all. On top of the default set the user pins/unpins
individual apps by **right-clicking a dock tile**, which writes a per-app override
`dock.pin.<name> = true|false` to *their* registry. twm computes each app's pin state
as: user override if present, else membership in `dock.pinned`. So the factory layout
stays pristine and the user's customisation is a resettable delta on top (see
`design/ui.md`).

### Two layers

| File | Role | Writable |
|---|---|---|
| `/System/etc/registry` | **defaults** shipped with the OS (read-only by convention) | no |
| `/Users/user/.config/registry` | **per-user overrides** | yes |

`reg_get(key)` returns the user value if present, else the system default, else the
caller's fallback. `reg_set(key, val)` writes to the **user** layer only; `reg_save()`
flushes the user layer to disk. So the system file is pristine and resettable
("reset to defaults" = delete the user file), and each future user gets their own
overrides — forward-compatible with multi-user.

On-disk format is the same `key = value` line format as the manifest (blank lines
and `#` comments ignored), so it round-trips through `cat`/`write` in the shell and
reuses the existing parser style.

## API (`user/lib/registry.{c,h}`, in the SDK)

```c
void        reg_load(void);                       /* load system + user layers     */
const char *reg_get(const char *key, const char *fallback);
int         reg_int(const char *key, int fallback);
int         reg_bool(const char *key, int fallback);   /* true/1/yes/on            */
void        reg_set(const char *key, const char *val); /* user layer, in memory    */
void        reg_set_int(const char *key, int val);
int         reg_save(void);                       /* flush user layer to disk      */
int         reg_keys(const char *prefix, char out[][REG_KEYMAX], int max); /* enumerate */
```

In-memory store: a fixed-capacity array of `{char key[64]; char val[96]; uint8_t
user;}` entries (a few hundred is plenty), loaded by `reg_load()`. `reg_set` updates
or appends a `user=1` entry; `reg_save` writes every `user=1` entry to the user
file. No kernel changes — it is pure userspace over the existing file syscalls
(`fopen`/`fread_`/`fwrite_`), like the manifest reader.

## Consumers (initial)

- **twm** — reads `theme.accent` / `ui.bar.autohide` / `ui.dock.autohide` /
  `clock.format` at startup instead of hard-coding them; re-reads on a registry
  change signal later.
- **shell** — a `reg` builtin: `reg get <key>`, `reg set <key> <val>`, `reg list
  [prefix]` — so settings are inspectable/editable today without a GUI.
- **future Settings app** — a `ui::` app that lists keys by area and edits them; it
  just calls the same API.

## What has to change

1. `user/lib/registry.{c,h}` — the store + parser + the API above; add to `ULIBOBJ`
   so every app links it (like `libc`/`sys`).
2. Seed `/System/etc/registry` in `mkfs` (a committed `fs/etc/registry` with the
   defaults); ensure `/Users/user/.config` exists (already created by `init`).
3. twm: replace the hard-coded accent/clock with `reg_*` lookups.
4. shell: the `reg` builtin.
5. Tests: a `t_registry` that sets a key, reads it back, and (with a reused disk)
   confirms it persisted across a reboot.

## Out of scope (for now)

A change-notification bus (apps re-reading live), per-key types/validation, access
control on who can write which keys (ties into [app-runtime.md](app-runtime.md)
capabilities), and a binary format. The text store is enough to make the desktop
stateful; these are later refinements.
