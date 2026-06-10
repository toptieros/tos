# Design guideline — tOS system ownership, authentication & elevation

> Status: **design guideline + implementation spec — NOT yet built.** Today **any**
> process can delete `/System/bin/twm` or the clipboard app, because tosfs has **no
> ownership or permission model at all** and the mutating syscalls do **no** checks.
> `/System` is "read-only **by convention**" ([`filesystem-layout.md`](filesystem-layout.md))
> — convention enforced by nobody. This is the plan to make it real.
>
> **The model (corrected).** tOS is **single-user** and is **not** Linux: there is no
> root account you log into and no `su`. But there *is* a hidden **`system`** owner —
> exactly like Android, where `system` owns the platform and first-party services
> (Play services, etc.) while you are just "the user." So `system` owns the OS; the one
> human user owns their data; and the human modifies system-owned things not by *becoming*
> someone else but by **elevating** a single action with a password — **UAC, not sudo**.
>
> Companion: [`app-runtime.md`](app-runtime.md) (the per-app **Android-style runtime
> permissions** — camera/location/internet/…; the *fine* layer, where ownership here is
> the *coarse* layer, and which **shares this doc's trusted-prompt** mechanism),
> [`packaging.md`](packaging.md) (installs that need elevation — and why the App Store
> path doesn't prompt but the terminal does), [`filesystem-layout.md`](filesystem-layout.md)
> (the `/System` vs `/Users/user` tree), [`files-and-desktop.md`](files-and-desktop.md)
> (how the UI surfaces a locked item).

## Why you can delete `/System` today

Tracing the delete path:

- `tosfs` entries (`kernel/fs/tosfs.h`) carry name / type / size / parent / data location —
  **no owner, no mode bits.**
- `fs_unlink()` (`kernel/fs/fs.c`) and the `SYS_UNLINK` / `SYS_OPEN(write)` / rename / rmdir
  syscalls **never check who's calling** — there is no caller identity to check against.
- So `rm /System/bin/twm` in the shell, or Delete in Files, just works. Nothing owns the
  bytes; nothing guards them.

The fix is three small, composable parts: **per-file ownership** on disk, **per-task
identity** in the kernel checked at the syscall boundary, and an **authenticated elevation**
path (UAC) so the human can knowingly override the system guard without a root account.

## Model

### 1. A hidden `system` owner owns the OS (Android-style, not a root login)

Two ownership identities: **`system` (uid 0)** and the **single human user (uid 1)**.
`system` owns `/System` (its `bin`/`lib`/`etc`), the system app bundles under `/Apps` that
ship with the OS, system config, and the trusted system services. The user owns
`/Users/user` and anything they create there.

The crucial framing the user asked for: **`system` is a platform owner, not an account.**
You never log in as `system`, never `su` to it, never get a "system shell." It's the
identity the OS itself runs as — the direct analog of Android's `system` user owning
Google/Play services while the human is just the user. Modifying system-owned bytes is done
by **elevating one action** (§ Authentication & elevation), the UAC way, not by switching
identity the sudo way.

### 2. Per-entry ownership on disk

Add an **owner uid** (1 byte) to each tosfs entry, plus a tiny **mode**: at minimum a
"system-protected" bit (writable only via `system` or an elevated action). World-readable
and world-executable stay the default (the user must read and run `/System/bin/twm`; they
just can't modify or delete it without elevating).

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
launcher (a privileged `SYS_SETUID`-style call available only to a uid-0 caller; drop-only
for others). This dovetails with the capability model: the boot chain is the
"unsandboxed, full-caps" set in [`app-runtime.md`](app-runtime.md); give that same set uid 0.

### 4. Enforcement at the syscall boundary

The mutating fs syscalls check identity before touching a system-owned entry:

```
may_write(task, entry):
    !entry.protected                     # the user owns it (or it's unprotected)
    || task.uid == SYSTEM                # the OS itself
    || task.elevated                     # an action the user just authenticated (UAC; §below)
    # else -> needs elevation (offer the prompt) / -EPERM if refused
```

Guard `SYS_UNLINK`, `SYS_OPEN` with write/create/trunc, `SYS_RENAME` (source *and* dest dir),
`SYS_MKDIR`/`SYS_RMDIR`. **Reads and exec are unguarded** — `/System` stays world-readable and
world-executable, so the user runs the OS normally; they only hit the guard trying to modify
or delete it, and the guard is *overridable by authenticating*, not a dead end.

## Authentication & elevation (UAC, not sudo)

This is the human-facing security the user described: "doing some stuff will ask for user
pass, kinda like UAC." It is the sanctioned way to override the system guard **without a root
account** — the action runs elevated, *you stay you*.

### The credential

The single user has a **password**, stored **hashed** (never plaintext) in a **system-owned**
credential file (e.g. `/System/etc/auth`) so it can't be rewritten without already being
authorized. It's set at install / first boot ([`installation.md`](installation.md)) and
changed only through an authenticated "change password" flow. (There is no login *screen*
yet — the box auto-starts the single user — so today this password gates *elevation*, not
sign-in; it's the same credential a future login would use.)

### What "elevated" means

Elevation is **per-action authorization to modify system-owned state**, not a change of
identity. When an unauthorized task hits the `may_write` guard, the kernel returns a distinct
**"needs elevation"** result (not a flat `-EPERM`); a trusted prompt collects the password;
on success the action proceeds and the task is marked **elevated for a short window**
(sudo-timestamp style — a few minutes), so a burst of related operations doesn't re-prompt.
Cancel ⇒ the action is refused and nothing changes.

### Two ways to be authorized (the App-Store-vs-terminal rule)

The user's key point — *the App Store that wraps installs shouldn't prompt, but the same
thing through the terminal should.* That falls out of having **two routes** to authorization:

1. **Fresh authentication — the password prompt.** Used by **manual / raw** paths that hold
   no standing authority: `tos package add …` typed into the **terminal**, deleting a
   protected file in Files, editing system config by hand.
2. **A trusted system app acting for the user.** The **App Store / Installer** is a
   first-party, **system-owned** app granted the install capability. The user's tap on
   **"Install"** in that trusted UI *is* the consent, so it installs/updates **without a
   per-action password** — exactly like the macOS App Store (no password) vs running a
   `.pkg` or `tos` by hand (password). The terminal is not a blessed component and carries no
   standing authority, so it falls to route 1 and prompts.

So authorization tracks *how trusted the requester is*, not *what is being done*: the blessed
GUI flow is frictionless; the manual flow confirms.

### The threshold — keep it un-intrusive

Per the chosen policy (minimal gating), the password is asked for **only**:

- **installing / removing software** *(via a non-blessed path — the App Store route doesn't
  prompt)*,
- **writing or deleting a `protected` (system-owned) file**,
- **changing the password or other security settings**.

Everything else is **not** password-gated. In particular, the routine **app permissions**
(camera, location, microphone, internet, notifications — [`app-runtime.md`](app-runtime.md))
are a simple **Allow / Deny tap**, never a password. The goal is that a normal session never
sees the password box; it appears only when you reach in and touch the system itself.

### Anti-spoofing: the prompt is system-drawn

The password prompt — and the app-permission prompts in [`app-runtime.md`](app-runtime.md) —
**must be drawn by the compositor / a trusted authenticator process, never by the requesting
app.** An app must not be able to render a fake password box or read the keystrokes (the
whole point of UAC's secure desktop and Android's system-drawn permission dialog). This **one
trusted-prompt mechanism is shared** by elevation and by permission grants — build it once
here, reuse it there.

## UX (how it surfaces)

- **Files / desktop**: system-owned items render with a subtle **lock badge**. A destructive
  action on one (Delete / Rename / Move-to-Trash) opens the **trusted "Authenticate to
  continue" prompt** rather than silently failing; succeed → it proceeds; cancel → it's
  blocked with a clear message. (Paste into a system dir is the same write-to-a-protected-dir
  path.) This is the single rule [`files-and-desktop.md`](files-and-desktop.md)'s FileView
  reads off the entry's `protected` flag.
- **Shell**: `rm /System/bin/twm` says *"that's a system file — authenticate to modify it?"*
  and shows the prompt; on cancel the file survives with a message — instead of either
  silently deleting it (today) or a bare permission error. `tos package add …` in the
  terminal prompts the same way ([`packaging.md`](packaging.md)).
- **App Store / Installer**: installs and updates run through the trusted route — **no
  per-action password** — because tapping Install in the first-party UI is the consent.

## Phasing (keep `make test` green)

1. **Disk + kernel plumbing.** Add `owner` (+ a `protected` bit) to the tosfs entry; bump the
   format; `mkfs` stamps `/System` + shipped `/Apps` = system, `/Users` = user; add a
   per-task uid (default user, boot chain = system). No enforcement yet — pure plumbing.
2. **Enforcement.** Add `may_write()` to the mutating syscalls; return the distinct
   **"needs elevation"** result (and `-EPERM` when refused). Add the `SYS_SETUID`-style drop.
   Tests: `rm /System/bin/<x>` is blocked + the file survives; a user file still deletes; an
   app can't write into `/System/bin`.
3. **Authentication + elevation.** A hashed password (set at install) + a **trusted
   authenticator** that draws the prompt + the short-lived **elevated** task state + the
   "needs elevation → prompt → retry" flow. Grant the App Store/Installer a **standing install
   capability** (route 2). Tests: terminal `tos package add` prompts and a correct password
   lets it through; a wrong/cancelled password blocks it and the system is unchanged; the
   store-route install needs no password.
4. **UX.** FileView lock badge + "authenticate to continue"; shell messages; the
   change-password flow.

## Testing strategy (pyramid)

- **Unit (host):** `may_write(uid, protected, elevated)` and the `mkfs` owner-assignment rule
  are pure logic — unit-test the truth table (user→user file = ok; user→protected = needs
  elevation; elevated user→protected = ok; system→anything = ok). The **two-route
  authorization** rule (trusted requester vs manual) is likewise a small predicate to test.
- **e2e (QEMU):** one smoke that tries `rm` + a Files delete on a `/System` item and asserts
  it's blocked (prompt cancelled) and the file/OS survive; one that authenticates and *does*
  modify it; one that confirms a normal `~/…` file still deletes (so we didn't over-lock).

## Relationship to the capability sandbox

Ownership + elevation (this doc) answer **"who owns these bytes, and how does the human get
authorized to change system state?"** — coarse, kernel-enforced, the floor under the security
model. App permissions ([`app-runtime.md`](app-runtime.md)) answer **"what may *this app*
touch?"** — fine, **Android-style runtime grants** (`fs:home`, `camera`, `net`, …). They
compose: an app needs *both* the permission for the operation *and* (ownership of the target
or an elevated user action) to modify system bytes. They also **share the trusted-prompt
mechanism** defined here. Build ownership + elevation first — it's the higher-value change and
it directly fixes "I can delete `/System`."

## Out of scope (for now)

A real **login screen** and multiple human users (still one auto-started user; the password
gates elevation, not sign-in), **biometrics**, a root/`su` shell (deliberately none — UAC
elevation replaces it), per-app password policies, and full POSIX mode bits / groups / ACLs.
The `system`-owner + `protected`-bit + UAC-elevation model is the minimum that makes
`/System` genuinely OS-owned, keeps a single un-intrusive human user, and unblocks the rest.
