# Design guideline — tOS system ownership & file permissions

> Status: **design guideline + implementation spec — NOT yet built.** This corrects a
> common misconception: "system ownership (#1)" is **open**, not done (NEXT_STEPS line ~95
> is `[ ]`). Today **any** process can delete `/System/bin/twm` or the clipboard app,
> because tosfs has **no ownership or permission model at all** and the mutating syscalls
> do **no** checks. `/System` is "read-only **by convention**" ([`filesystem-layout.md`](filesystem-layout.md))
> — convention enforced by nobody. This is the plan to make it real.
>
> Companion: [`app-runtime.md`](app-runtime.md) (the per-app capability sandbox — the *fine*
> layer; ownership here is the *coarse* layer), [`filesystem-layout.md`](filesystem-layout.md)
> (the `/System` vs `/Users` tree), [`files-and-desktop.md`](files-and-desktop.md) (how the
> UI surfaces a locked item).

## Why you can delete `/System` today

Tracing the delete path:

- `tosfs` entries (`kernel/fs/tosfs.h`) carry name / type / size / parent / data location —
  **no owner, no uid, no mode bits.**
- `fs_unlink()` (`kernel/fs/fs.c`) and the `SYS_UNLINK` / `SYS_OPEN(write)` / rename / rmdir
  syscalls **never check who's calling** — there is no caller identity to check against.
- So `rm /System/bin/twm` in the shell, or Delete in Files, just works. Nothing owns the
  bytes; nothing guards them.

The fix is a small, two-part model: **per-file ownership** on disk, and **per-task identity**
in the kernel, checked at the syscall boundary.

## Model

### 1. A hidden `system` user owns the OS

Two identities to start (multi-user is future): **`system` (uid 0)** and the **single human
user (uid 1)**. `system` owns `/System` (its `bin`/`lib`/`etc`), the system app bundles under
`/Apps` that ship with the OS, and system config. The user owns `/Users/user` and anything
they create there. `system` is *hidden* — not a login, just the owner the OS runs as.

### 2. Per-entry ownership on disk

Add an **owner uid** (1 byte is plenty) to each tosfs entry, and a tiny **mode**: at minimum
a "system-protected" bit (writable only by `system`). World-readable/executable stays the
default (the user must read and run `/System/bin/twm`; they just can't modify or delete it).

`mkfs` (`tools/mkfs.c`) stamps the owner as it packs the seed tree: everything under
`/System` and the shipped `/Apps/*.app` bundles → `owner = system`; everything under
`/Users/user` → `owner = user`; new files inherit the **caller's** uid.

> **On-disk format change — sync gotcha.** Adding a field changes the entry size and the
> tosfs layout, so bump the tosfs **version/magic** and keep the three size knobs in sync
> exactly as the icon work did: `TOSFS_DISK_SECTORS` (kernel/fs/tosfs.h) ↔ `FS_PART_CNT`
> (boot/stage1.asm) ↔ `UFS_SECTORS` (Makefile UEFI). A clean reformat (`mkfs`) is fine since
> the image is rebuilt each `make`.

### 3. Per-task identity in the kernel

Each task carries a **uid**. The **boot chain / OS services** — `init`, `twm`, the `desktop`
app, `term`, `shell` when it's the system shell, system daemons — run as **`system` (uid 0)**:
they are the OS. **User-launched apps** (everything started from the desktop session — Files,
Notepad, Settings, Spotlight launches) run as **the user (uid 1)**.

How a task gets its uid: inherited across `fork`, and (re)set at `exec`/launch by a trusted
launcher. Simplest rule that matches intent: a program **loaded from `/System`** that the
*boot chain* starts runs as `system`; anything the **desktop session launches on the user's
behalf** runs as the user. The launcher (init for the boot chain; the WM/Spotlight/Files for
user apps) sets the child's uid — a privileged `SYS_SETUID`-style call available only to a
uid-0 caller (drop-only for others). This dovetails with the capability model: the boot chain
is already the "unsandboxed, full-caps" set in [`app-runtime.md`](app-runtime.md); give that
same set uid 0.

### 4. Enforcement at the syscall boundary

The mutating fs syscalls check identity before touching a system-owned entry:

```
may_write(task, entry):
    entry.owner == task.uid            # you own it
    || task.uid == SYSTEM              # system can write anything
    # else -> -EPERM
```

Guard `SYS_UNLINK`, `SYS_OPEN` with write/create/trunc, `SYS_RENAME` (source *and* dest dir),
`SYS_MKDIR`/`SYS_RMDIR`. **Reads and exec are unguarded** — `/System` stays world-readable and
world-executable, so the user runs the OS normally; they just get `-EPERM` trying to modify or
delete it. (Creating a file *inside* a dir is a write to that dir, so a user can't drop files
into `/System/bin` either.)

## UX (how it surfaces)

- **Files / desktop**: system-owned items render with a subtle **lock badge** and their
  Rename / Cut / Move-to-Trash / Delete menu items are **disabled** (greyed), so the user is
  never offered an action that will `-EPERM`. Paste into a system dir is likewise disabled.
  This is the single UI rule the [`files-and-desktop.md`](files-and-desktop.md) FileView reads
  off the entry's owner.
- **Shell**: `rm /System/bin/twm` prints `rm: permission denied (system file)` and the file
  survives — instead of silently deleting it.
- **Elevation (future)**: a `sudo`-like path (or the installer running as `system`) is the
  sanctioned way to modify `/System`; out of scope for the first cut.

## Phasing (keep `make test` green)

1. **Disk + kernel plumbing.** Add `owner` (+ a protected bit) to the tosfs entry; bump the
   format; `mkfs` stamps `/System`=system, `/Users`=user; add a per-task uid (default user,
   boot chain = system). No enforcement yet — pure plumbing, suite stays green.
2. **Enforcement.** Add `may_write()` to the mutating syscalls returning `-EPERM`. Add the
   `SYS_SETUID`-style drop. Tests: `rm /System/bin/<x>` fails + the file survives; a user
   file still deletes; an app can't write into `/System/bin`.
3. **UX.** FileView lock badge + disabled actions; shell permission message.

## Testing strategy (pyramid)

- **Unit (host):** the `may_write(uid, owner)` predicate and the `mkfs` owner-assignment rule
  are pure logic — unit-test the truth table (user→user file = ok, user→system file = EPERM,
  system→anything = ok).
- **e2e (QEMU):** one smoke that tries `rm` + a Files delete on a `/System` item and asserts
  the file is still listable and the OS is unharmed; one that confirms a normal `~/…` file
  still deletes (so we didn't over-lock).

## Relationship to the capability sandbox

Ownership (this doc) answers **"who owns these bytes?"** — coarse, kernel-enforced, the floor
under multi-user. Capabilities ([`app-runtime.md`](app-runtime.md)) answer **"what may *this
app* touch?"** — fine, manifest-declared (`fs:home` jails an app to `~`, etc.). They compose:
an app needs *both* the capability for the operation *and* ownership (or `system`) of the
target. Build ownership first — it's the smaller, higher-value change and it directly fixes
"I can delete `/System`."

## Out of scope (for now)

Full POSIX mode bits / groups, ACLs, multiple human users + login, `sudo`/elevation UI, and
setuid programs. The 2-uid + protected-bit model is the minimum that makes `/System`
genuinely OS-owned and unblocks the rest.
