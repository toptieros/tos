# Design guideline — tOS app runtime & sandbox model

> Status: **design / roadmap.** How tOS apps execute and how far we isolate them.
> Today an app is a trusted static ELF in ring 3; this is the plan to grow toward a
> real per-app sandbox (Android-style) with manifest-declared capabilities
> (Apple-style entitlements), bounded by what a small OS can actually enforce.

## Where we are today

- **Ring 3 + per-process address space.** Every app runs at CPL 3 in its own
  page tables (`vmm_create_user`); it cannot read another app's memory or the
  kernel's. A wild access faults and the kernel kills *only that task*
  (`sched_kill`) — the OS stays up.
- **The syscall is the only interface.** `int 0x80` is the single door between an
  app and everything else (files, windows, pty, fork/exec, time, mmap). That makes
  the **syscall surface the entire sandbox boundary** — to sandbox an app, gate its
  syscalls. There is no other ambient authority (no shared memory except the
  window surface the compositor maps for it, no devices).
- **Mediated I/O already.** The compositor owns the framebuffer; an app only gets
  its *own* window surface (`win_create`) and its *own* event queue. stdio is a
  pty, not the console. So display and input are already brokered, not direct.

So the foundation (isolation, a single chokepoint, brokered I/O) is in place. What
is missing is **authorization**: today any app may call any syscall and open any
path. Everything is implicitly trusted.

## The spectrum

| | **Android-like** | **Apple-like** |
|---|---|---|
| Identity | per-app UID, private data dir | code signature + team id |
| Permission | runtime prompts ("allow X?") | static **entitlements** in the bundle |
| Enforcement | kernel UID checks + SELinux | kernel sandbox profile + entitlement checks |
| FS view | per-app private storage, shared via providers | container + declared exceptions |

tOS is closest to **Apple's static-entitlement** model: declare what an app may do
in its `manifest` (`caps = ...`), have a trusted launcher install those caps on the
process, and have the kernel enforce them at the syscall boundary. Runtime prompts
(Android) can come later as a UI layer that flips a cap on. We are NOT doing code
signing (no PKI); trust comes from the bundle living in `/Apps` (installed by the
user/installer).

## Capability model

Manifest declares capabilities (already reserved as `caps` in
[app-package-format.md](app-package-format.md)):

```
caps = window, fs:home, fs:bundle, spawn, time
```

Proposed capability set (start small, grow):

| cap | grants | enforced by |
|---|---|---|
| `window` | `win_create`/present/events, mmap for its back buffer | WIN_* + SYS_MMAP gated |
| `fs:bundle` | read its own `/Apps/<X>.app/**` (resources) | path check in `fs_open` |
| `fs:home` | read/write under `/Users/user/**` | path check |
| `fs:system` | read `/System/**` (fonts, etc.) | path check (read-only) |
| `spawn` | `fork`/`exec` (most apps don't need it) | gated in sched |
| `time` | `SYS_TIME`/uptime | trivially allowed |
| `notify` | post notifications to the WM | WM message gated |
| (none) | a "compute only" app: window + its bundle, nothing else | default-deny the rest |

A process with no declared cap for an operation gets `-EPERM` (and the attempt can
be logged / surfaced as a permission prompt later). The boot chain (init, twm,
shell, term) runs **unsandboxed** (full caps) — they are the OS.

### Enforcement mechanism

- `struct task` gains a `caps` bitmask (+ a small path-jail descriptor for `fs:*`).
- The **trusted launcher sets caps at exec.** Since the kernel loads programs by
  path, the launcher (twm, the future Settings/installer) passes the manifest's
  caps to a new `SYS_EXEC`-with-caps (or sets them on the child before exec). caps
  can only be *dropped*, never raised, by an already-sandboxed task.
- Syscalls consult `current->caps`: `fs_open` checks the resolved absolute path
  against the task's allowed roots; `sched_fork`/`exec` check `spawn`; `win_*`
  check `window`; etc. The checks live at the syscall entry (`traps.c`), the same
  place that already routes them.
- A **path jail** for `fs:home`/`fs:bundle` is just "resolve, then verify the slot
  is within an allowed directory subtree" — `fs.c` already has `is_within` for the
  rename cycle check, so the primitive exists.

## Phasing

1. **Declare** — `caps` in manifests, parsed by the launcher; **advisory only**
   (logged, not enforced). No behaviour change. Establishes the vocabulary.
2. **Enforce coarse** — kernel `task.caps`; gate `spawn`, `window`, `notify`, and
   read-only `/System`. Boot chain keeps full caps. Apps that over-reach get
   `-EPERM`; verify the bundled apps still work with least privilege.
3. **FS jails** — `fs:bundle` / `fs:home` path enforcement; an app sees only its
   bundle + the user's home (and a private `~/.config/<app>` it always owns).
4. **Runtime prompts** — a WM permission dialog that grants a cap on the fly
   (Android-style), persisted in the registry per app.
5. **Resource limits** — per-app caps on task count / mmap size (the pools are
   RAM-bounded; add per-app accounting) so a runaway app can't exhaust the system.

## Realistic limits

No multi-user yet (everything is "user"), so `fs:home` is one home. No code
signing — trust is "it's installed in `/Apps`." No fine-grained MMU tricks beyond
the per-process address space we already have. The win is real though: because the
syscall is the *only* interface and I/O is already brokered, a capability check at
that one boundary genuinely sandboxes an app — far simpler than retrofitting
isolation onto a system with ambient shared state.

## Ties

- Bundles + `caps`: [app-package-format.md](app-package-format.md).
- Permission persistence + prompts: [settings.md](settings.md) (registry).
- FS jail roots: [filesystem-layout.md](filesystem-layout.md) (`/System`, `/Users/user`).
