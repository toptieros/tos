# tOS — next steps

How the system works **today** is in [PROJECT.md](PROJECT.md); this file tracks what's
**left** plus a terse log of what's landed. Every item keeps `make test` green (BIOS +
UEFI) before it's checked off.

**Status:** `make test` **33/33** (22 e2e journeys: BIOS 22 + a UEFI subset 11) + **46 host
unit tests** (`make unit`, no QEMU). Pyramid policy in [`design/testing.md`](design/testing.md);
the phased plan in [`design/roadmap.md`](design/roadmap.md). tOS is early-to-mid development.

Legend: `[ ]` not started · `[~]` partial · `[⏸]` set aside (don't build unless asked).

---

## Open — the road ahead

### Toolkit & desktop UI
- [ ] **Maximize hides both bars + hover-reveal.** Fullscreen must hide **both** the menu bar
  **and** the window's own title bar (client goes full-height; today it's `W×(H−TH)`, leaving
  the app bar visible). Top-edge hover reveals both together; they retract only when the cursor
  leaves the title-bar band **downward** into content. → [`ui.md`](design/ui.md)
- [ ] **Files + Desktop suite (#10).** A shared `ui::FileView` powering both the Files window
  and a new bottom-pinned `WIN_DESKTOP` layer over `~/Desktop`: **multi-select** (Ctrl/Shift-click
  + rubber-band marquee — single-select today), **folder/multi-item copy-cut-paste** (today's
  `CLIP_FILE`-of-bytes can't hold a directory → path-reference clipboard + recursive `cp_r`),
  **rename**, context menus, and **drag-to-move** (needs DnD). → [`files-and-desktop.md`](design/files-and-desktop.md)
- [ ] **App menus (#6).** An app→WM protocol so apps declare their own File/Edit/Help tiles
  (`SYS_WIN_SETMENU` + a `WEV_MENU` event + a `ui::Window` menu API). The menu-bar machinery +
  the app tile (About/Quit) are already done. → [`ui.md`](design/ui.md)
- [ ] **Grow the toolkit + port apps.** A layout system + menus, then port `term` / `fastfetch`
  (and new apps) onto the toolkit.
- [ ] **Live resize preview + reflow.** A live outline while dragging the grip, and reflowing
  wrapped lines (resize snaps on mouse-up today, no reflow).

### Global text-interaction contract
The toolkit owns the in-window text contract: anything in `TextField` is inherited by every
toolkit app for free. **Done:** blink caret, drag-select, Ctrl+A, double-click word-select,
Ctrl+←/→ word-jump, Ctrl+Backspace/Delete word-delete, Delete, shift-select. **Left:**
- [ ] **I-beam cursor over selectable text.** Blocked on an app→compositor cursor-shape protocol
  (twm composites the cursor and doesn't know widget regions).
- [ ] **Primary selection + cross-app text drag.** Blocked on the DnD protocol.

### Input / event foundations
- [ ] **Drag-and-drop protocol.** A source starts a drag with a **typed payload** (file path /
  text / image bytes); the compositor drags a ghost + hit-tests drop targets; drop delivers the
  payload (a `WEV_DROP`). Unlocks Files drag-to-move, the desktop, cross-app text drag, and Pocket
  Dimension. (Richer key events — modifier flags, `WEV_KEYUP`, `WEV_MOUSE_SHIFT` — are done.)

### System & security
- [~] **System ownership (#1).** **Done:** tosfs v3 carries a per-entry `owner`; tasks carry a
  `uid` (init=system, the desktop session drops to user); the mutating fs syscalls enforce
  `tos_may_write()`; the shell prints `permission denied (system file)` and ships an `id` builtin.
  **Remaining (folded into the Files suite below):** the Files/desktop **lock badge** + greyed
  actions on system-owned items (reads `fstat.owner`). → [`system-ownership.md`](design/system-ownership.md)
- [ ] **Capability sandbox.** Wire the manifest `caps` field to a per-task capability set checked
  at the syscall boundary (fs jails, spawn/window/notify gating). → [`app-runtime.md`](design/app-runtime.md)
- [~] **Ctrl+C/X/V everywhere.** Landed in `TextField`, the terminal (Ctrl+Shift+C/X/V), and Files
  (files). Remaining: **folders** — folded into the Files + Desktop suite above.
- [⏸] **Pocket Dimension (Super+D).** A left-edge per-session shelf of stashed typed payloads.
  Don't implement unless explicitly requested; needs DnD.

### Platform / runtime / storage
- [⏸] **Real shell + scripting.** Replace `shell.c`'s hardcoded `if/else` dispatch with a real
  lexer/parser + exec model (quoting, `$VAR`/env, pipes, redirection, `;`/`&&`/`||`, globbing,
  background `&`, scripts). First step: drop `help`, move demo/diagnostic builtins to `/System/bin`
  programs. Big effort; **not this round** unless asked.
- [ ] **Userspace runtime + SDK sysroot.** sysroot + `tos-cc`/`tos-c++`; a hosted C++ runtime
  (STL/exceptions/RTTI/unwind); `libposix`; a QPA-style framebuffer/input shim. The line between
  "teaching OS" and "runs third-party software." → [`app-porting.md`](design/app-porting.md)
- [ ] **Installer (live → install).** Raw block-write to a target disk, in-OS mkfs/partitioning,
  copy `/System`+`/Apps`, seed `/Users` + registry. Start on virtio-blk/ATA-slave. → [`installation.md`](design/installation.md)
- [ ] **Device drivers (Phase 4).** virtio-blk → AHCI/SATA+DMA → NVMe → GPT/ESP(FAT) writer → USB
  (xHCI+HID+MSC) → ACPI (uACPI/LAI) → virtio-net/e1000 + TCP/IP. GPU accel is VM-only. → [`roadmap.md`](design/roadmap.md)
- [ ] **Growable filesystem.** Files are contiguous and a metadata change rewrites the whole slot
  table; the partition is fixed-size. Want extent/indirect blocks, a runtime-sized partition, and a
  journaled table.
- [~] **Terminal scrollback.** Wheel + Shift-PgUp/PgDn over a 256-row ring done; future: a
  configurable ring size.

### Smaller ideas
- A heap / `sbrk` for user programs (a program is its static image + stack today).
- Calibrate the LAPIC timer instead of a fixed count.

---

## Done (changelog)

Terse one-liners, newest first; the prose lives in git history + PROJECT.md.

- **System ownership #1 (2026-06-02).** tosfs bumped to **v3**: every entry carries an `owner`
  uid (+ a reserved `mode` byte); `mkfs` stamps `/Users` + `/tmp` = user, the rest = system
  (shared rule in `kernel/fs/perm.h`, unit-tested). Tasks carry a `uid` (inherited on fork); init
  (pid 1) is `system` and drops the whole desktop session to `user` via a new `SYS_SETUID` before
  launching twm. The mutating fs syscalls (unlink/open-create/mkdir/rmdir/rename) enforce
  `tos_may_write()`, so the user can no longer delete or modify `/System` or `/Apps`; reads/exec
  stay open. `SYS_GETUID` + `fstat.owner` exposed; shell `rm` prints `permission denied (system
  file)` and gains an `id` builtin. Unit `t_perm` (18) + e2e `t_system_ownership`. 33/33 + 46 unit.
- **Notification click routing (2026-06-02).** `struct notif` gained a `target` field; new
  `notify_to(title, body, target)` (plain `notify()` posts an empty target = no routing).
  Clicking a toast body or a notification-center row (`notif_activate` + `nc_row_at`) focuses the
  target app's window (restoring a minimized one) or launches it from the catalog; `[twm] notif
  open <app>` trace. Shell `notify <app> <body>` posts a routed toast. `t_notif_click_routing`.
- **Dock pinned | running divider (2026-06-02).** `rebuild_dock()` records the boundary index
  (`dock_runsep`) after the last pinned tile; `draw_dock()` draws a faint 1px vertical separator
  in the gap before the first running-unpinned tile, shown only when one exists. `[twm] docksep`
  trace; asserted in `t_notepad_edit_save`.
- **Launchers mutually exclusive (2026-06-02).** `dismiss_launchers(except)` at every launcher
  summon path (the three Super hotkeys + the dock Launchpad button) closes the other two, so
  Spotlight can't float over the clipboard/Launchpad. Focus telemetry now tracks the window
  *id* too, so a freed slot reused by a new window in the same frame still reports the focus
  change + repaints chrome; new `[twm] unmap <title>` trace. `t_launchers_exclusive`. 31/31.
- **UEFI boot fixed above 4 GiB (2026-06-02).** The loader only identity-mapped 0–4 GiB, so at
  `MEM ?= 8G` OVMF's high-loaded app `#PF`'d on the CR3 switch; it now reads the UEFI
  `GetMemoryMap` and maps all of RAM (`ram_top`/`build_tables`). `t_ram_scales` gained a 6G case +
  runs on UEFI. 30/30.
- **Settings app uses Lucide glyphs (2026-06-02).** `ui::Button` gained optional `icon`/`value`;
  new `tools/genglyphs.py` → `user/lib/glyphs.h` bakes a reusable Lucide app-glyph set.
- **Multi-region frame pool across the 4 GiB hole (2026-06-02).** `vmm` reads the e820 map
  (`fw_cfg`), maps only real RAM + skips the PCI hole, uses RAM above 4 GiB; `MEM ?= 8G`; `memtest`.
- **Notification toast: shadow-smear fixed + Lucide `x` dismiss + global `ugfx_set_shadows`
  (`ui.shadows` key) (2026-06-02).**
- **Files Ctrl+C/X/V (files) over the clip ring + context menu (2026-06-02).**
- **Shift-select completed — Shift+click via `WEV_MOUSE_SHIFT` (2026-06-02).**
- **Terminal Shift-PgUp/PgDn scrollback paging (2026-06-02).**
- **Notification expand: Lucide chevron + per-row expand in the center (2026-06-02).**
- **Notification-center dirty-rect / shadow-halo fixes (2026-06-02).**
- **Crisp icons: premultiplied resampler + 128px masters + Lucide bar glyphs; tosfs 1→2 MiB
  (2026-06-01).**
- **Test suite rebuilt as a pyramid: 49 e2e → 19 e2e + 28 unit; `textutil.h` (2026-06-01).**
- **macOS-style Alt-Tab switcher overlay (#7, 2026-06-01).**
- **Terminal copy-path test coverage — `t_term_copy` (2026-06-01).**
- **Notification QoL: hover-pause, collapsible toast, slide-into-open-center, Clear (2026-06-01).**
- **Notifications / toasts: `notify()` → `SYS_NOTIFY` → toast + center + bell (2026-06-01).**
- **Status-bar cluster + registry-driven clock (2026-06-01).**
- **Single-sourced shadow-halo extents via `ugfx_elevation_extent` (2026-06-01).**
- **Global ScrollBar: `ugfx_scroll_thumb` + `ui::ScrollBar` (Files/Spotlight thumbs) (2026-06-01).**
- **Notepad / global text editing (#5): blink caret, word-select, word-jump/delete, Delete
  (2026-06-01).**
- **Rounded, bigger search bars (#14, 2026-06-01).**
- **Frosted-glass UI pass: `ugfx_frost` blur under bar/dock/CC/Launchpad (2026-05-31).**
- **Launchpad polish (#11): centred grid + type-to-filter (2026-05-31).**
- **Draggable scroll indicator (#12, 2026-05-31).**
- **Spotlight keyboard QoL (#13): arrows/Tab walk results (2026-05-31).**
- **Scroll-wheel support (#9): `WEV_SCROLL` (2026-05-30).**
- **Launchpad/Spotlight real icons (#2), dynamic dock (#3), Hello removed (#4) (2026-05-30).**
- **Window UX: single-instance `summon`, Super+V clipboard, Super+Tab switcher (2026-05-30).**
- **Hierarchical filesystem (tosfs v2, parent-indexed) + Dolphin-style Files app.**
- **Compositor desktop: `kernel/ipc.c` protocol; twm back-buffered compositor; `term`; fastfetch.**
- **C++ application SDK: freestanding crt + heap + libc + `ulib`/`sys` + `ui::` toolkit.**
- **UI-modernization pass: hover feedback, AA borders / state layers / elevation (2026-05-29).**
- **macOS-style system layout: `/System` `/Apps` `/Users`; registry; real cursors; fullscreen.**
- **Kernel foundations: delete+free-map, per-task fd tables, ELF loader, MBR, sleep, fork/exec +
  pids/wait/zombies, fine-grained SMP, RTC/PCI/speaker/reboot, VBE fb, AA system font, test suite.**

---

## Design guidelines (`design/`)

Read [`roadmap.md`](design/roadmap.md) first — the strategic plan and current-stage assessment.

- **Implemented:** [`filesystem-layout.md`](design/filesystem-layout.md) ·
  [`app-package-format.md`](design/app-package-format.md) · [`settings.md`](design/settings.md) ·
  [`testing.md`](design/testing.md) (the test pyramid).
- **Planned:** [`ui.md`](design/ui.md) (desktop chrome, iconography, fullscreen, dock) ·
  [`files-and-desktop.md`](design/files-and-desktop.md) (Files + the desktop-as-Finder) ·
  [`system-ownership.md`](design/system-ownership.md) (who may delete what) ·
  [`app-runtime.md`](design/app-runtime.md) (capability sandbox) ·
  [`app-porting.md`](design/app-porting.md) (sysroot/libposix) ·
  [`installation.md`](design/installation.md) · [`virtio-net.md`](design/virtio-net.md) ·
  [`virtio-gpu.md`](design/virtio-gpu.md).

---

## Known issues (history, not blocking)

- **BIOS real-mode load envelope `#UD` — FIXED (2026-05-29).** The chunked disk-read loop ran
  `mov ah,0x42` before `push ax`, baking `0x42` into the sectors-remaining counter; one-line
  ordering fix in `boot/stage1.asm`. (Debug via `-d int,cpu_reset` + `pmemsave` dumps; gdb breaks
  in the boot sector are unreliable — the loop self-modifies the DAP page.)
- **Flaky UEFI tests under host load — hardened (2026-05-29).** Tight inject schedules can outrun
  OVMF+TCG; the harness now retries (`t_mouse` re-injects, `line_for` retypes). Environmental.
- **tmpfs scratch leak — FIXED.** `Tos.stop()` removes the per-run scratch disk / OVMF-vars / serial
  log it created (a caller-supplied scratch is left alone).
