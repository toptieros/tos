# Design guideline — tOS app runtime & sandbox model

> Status: **design / roadmap.** How tOS apps execute and how far we isolate them.
> Today an app is a trusted static ELF in ring 3; this is the plan to grow toward a
> real per-app sandbox with **Android-style runtime permissions** — the manifest
> *declares* what an app may request (camera, location, internet, notifications,
> files…), the user *grants at runtime* via a system prompt, and the grant *persists
> per-app and is revocable* — bounded by what a small OS can actually enforce.
>
> Companion: [`system-ownership.md`](system-ownership.md) (the *coarse* layer — who
> owns the bytes + UAC elevation; this doc's permission prompts **reuse its
> trusted-prompt mechanism**), [`app-package-format.md`](app-package-format.md) (where
> `caps` is declared), [`settings.md`](settings.md) (where grants persist).

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

tOS adopts the **Android runtime-permission** model. As in Android, capabilities
split into two tiers:

- **Normal caps** (`window`, `time`, `fs:bundle`) — low-risk, granted **at install**
  from the manifest with **no prompt**. The app just has them.
- **Dangerous caps** (`camera`, `location`, `mic`, `net`, `notify`, `fs:home`) —
  privacy- or system-sensitive. The manifest only **declares that the app may ask**;
  each starts **denied**, and is **granted at runtime** the first time the app uses it
  via a **system prompt** ("Allow *Maps* to use your location?"). The decision
  **persists per-app** (in the registry) and is **revocable** later in Settings — just
  like Android's app-info → permissions screen.

A trusted launcher installs the normal caps at exec; the kernel enforces all caps at
the syscall boundary (below); the runtime prompt simply flips a dangerous cap from
denied to granted. This is a plain **Allow / Deny tap — never the password**: it is
*not* system modification, so it does not cross the UAC threshold in
[`system-ownership.md`](system-ownership.md) (the password is for touching `/System`
/ installing software, not for an app asking to use the camera). We are NOT doing code
signing (no PKI); trust comes from the bundle living in `/Apps` (installed by the
user/installer), and from the user's explicit runtime grants.

> The grant prompt **must be drawn by the trusted authenticator / compositor, never by
> the requesting app** (an app can't fake "Allow camera?" or harvest the answer) — the
> same system-drawn-prompt rule as UAC elevation, sharing one mechanism with
> [`system-ownership.md`](system-ownership.md).

## Capability model

Manifest declares capabilities (already reserved as `caps` in
[app-package-format.md](app-package-format.md)):

```
caps = window, fs:home, fs:bundle, spawn, time
```

Proposed capability set (start small, grow). **Tier** = how it's granted: *normal*
(at install, no prompt) vs *dangerous* (runtime prompt, persisted, revocable):

| cap | tier | grants | enforced by |
|---|---|---|---|
| `window` | normal | `win_create`/present/events, mmap for its back buffer | WIN_* + SYS_MMAP gated |
| `fs:bundle` | normal | read its own `/Apps/<X>.app/**` (resources) | path check in `fs_open` |
| `time` | normal | `SYS_TIME`/uptime | trivially allowed |
| `spawn` | normal | `fork`/`exec` (most apps don't need it) | gated in sched |
| `fs:home` | dangerous | read/write under `/Users/user/**` (the user's files) | path check |
| `net` | dangerous | sockets / internet access | SYS_SOCKET* gated |
| `notify` | dangerous | post notifications to the WM | WM message gated |
| `camera` | dangerous | the camera device (when one exists) | device broker gated |
| `mic` | dangerous | the microphone (when one exists) | device broker gated |
| `location` | dangerous | a location provider (when one exists) | provider gated |
| `fs:system` | (system) | read `/System/**` (fonts, etc.) | path check (read-only) |
| (none) | — | a "compute only" app: window + its bundle, nothing else | default-deny the rest |

Normal caps the launcher installs at exec; **dangerous caps start denied** and are
flipped on by the first-use runtime prompt (Allow/Deny, persisted per app —
[`settings.md`](settings.md) — and revocable in Settings). A process attempting an
operation whose dangerous cap is still denied gets `-EPERM` (or triggers the prompt).
Several of these (`camera`/`mic`/`location`) are placeholders until the hardware
exists — declaring the vocabulary now keeps the model uniform. The boot chain (init,
twm, shell, term) runs **unsandboxed** (full caps) — they are the OS.

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

1. **Declare** — `caps` (normal + the dangerous ones the app may request) in
   manifests, parsed by the launcher; **advisory only** (logged, not enforced). No
   behaviour change. Establishes the vocabulary.
2. **Enforce coarse** — kernel `task.caps`; install the **normal** caps at exec; gate
   `spawn`, `window`, and read-only `/System`; **dangerous** caps default to denied.
   Boot chain keeps full caps. Apps that over-reach get `-EPERM`; verify the bundled
   apps still work with least privilege.
3. **FS jails** — `fs:bundle` / `fs:home` path enforcement; an app sees only its
   bundle + the user's home (and a private `~/.config/<app>` it always owns).
4. **Runtime prompts (the Android model proper)** — a **system-drawn** permission
   dialog (the shared trusted-prompt from [`system-ownership.md`](system-ownership.md))
   that grants a dangerous cap on first use, persisted per app in the registry, and a
   Settings screen to review/revoke. This is the user-facing heart of the model, not
   an afterthought.
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

- The coarse layer + the shared trusted prompt + UAC elevation:
  [system-ownership.md](system-ownership.md).
- Bundles + `caps`: [app-package-format.md](app-package-format.md).
- Permission persistence + prompts: [settings.md](settings.md) (registry).
- FS jail roots: [filesystem-layout.md](filesystem-layout.md) (`/System`, `/Users/user`).
