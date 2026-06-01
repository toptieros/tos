# tOS — next steps

The road ahead. How the system works **today** is in [PROJECT.md](PROJECT.md);
this file tracks what's **left** plus a terse log of what's landed. Every item
keeps `make test` green (BIOS + UEFI) before it's checked off.

**Status:** BIOS suite **43/43**. tOS is early-to-mid development — see
[`design/roadmap.md`](design/roadmap.md) for the phased big picture.

Legend: `[ ]` not started · `[~]` partial · `[x]` done (see the changelog).

---

## Open — the road ahead

### Toolkit & desktop UI
- [ ] **Menu-bar API for apps (#6).** When an app is focused the top-left shows its
  name; design an API letting an app add **menu tiles** there (first tile = app name
  → About / Preferences / Quit; apps add File / Edit / Help, macOS-style). The "app
  menus" half of the Desktop UI roadmap. See [`design/ui.md`](design/ui.md).
- [ ] **OS-logo dropdown (#8).** Clicking the logo at top-left opens a dropdown
  (Preferences / Help / About; items can be inert placeholders at first). Shares the
  menu-tile machinery from #6.
- [ ] **Desktop UI roadmap (beyond fullscreen).** Status-bar items, a
  notification/toast system, app menus (#6), dock magnification, and hiding a
  window's *own* chrome in fullscreen for a purer macOS look. See
  [`design/ui.md`](design/ui.md).
- [ ] **Grow the toolkit + port more apps.** Add a layout system and menus, then
  port `term` / `fastfetch` (and new apps) onto the toolkit.
- [ ] **Files app polish.** Rename (needs an in-window text entry), scrolling for
  long directories, inline Ctrl+C/X/V (cut/paste over the clipboard ring), and
  drag-to-move between folders. Today Files has a "Copy" action but not the keybinds,
  cut, paste, or drag. Overlaps the Global ScrollBar and the DnD foundation.
- [ ] **Live resize preview + content reflow.** Resize snaps on mouse-up and keeps
  cells at their (row,col) without reflowing wrapped lines. A live outline while
  dragging the grip, and reflowing long lines, would polish it.
- [ ] **macOS-style Alt-Tab animation (#7).** Windows shown side-by-side, moving
  between them as you cycle — the full overlay the interim Super+Tab switcher left as
  future. Needs the **richer key events** foundation (hold-Alt / release-to-commit).
- [ ] **Desktop-as-a-folder + selection suite (#10).** Treat the desktop as
  `/Users/user/Desktop`: right-click context menu, the directory's files as icons,
  click-select / Ctrl-multi-select. Same selection model in Files. Builds on the
  global text/icon selection contract below.

### Global text-interaction contract
The toolkit **is** the OS-level layer for in-window text: anything in `TextField` is
inherited by every toolkit app (Notepad + both search boxes) for free — same
philosophy as the Global ScrollBar. This is the #10 design decision ("the OS/toolkit
owns the selection + cursor contract"). **Already global** (done): blinking caret,
drag-select, Ctrl+A select-all, double-click word-select, Ctrl+←/→ word-jump,
Ctrl+Backspace / Ctrl+Delete word-delete, the Delete key. **Still needs foundations:**
- [ ] **Shift-select** (Shift+arrow / Shift+click extend the selection). Blocked on
  **richer key events** — the toolkit can't see Shift today (`on_key` even notes it:
  "the toolkit doesn't surface Shift yet").
- [ ] **I-beam cursor over selectable text.** Blocked on an **app→compositor
  cursor-shape protocol** (none exists — twm composites the cursor and doesn't know
  about widget regions; the only cursor API today is the text-console `paint_cursor`).
- [ ] **System primary-selection + cross-app text drag.** Blocked on the **DnD
  protocol**. Lets a selection be the paste source and be dragged between windows.

### Input / event foundations (unblock the above)
- [ ] **Richer key events.** `WEV_KEY` carries a single byte, key-DOWN only. Widen it
  to also carry **modifier flags** (Ctrl/Alt/Shift/Super) and emit **key-UP** events.
  `keyboard.c` already tracks modifier state for ASCII translation — it just doesn't
  surface it. Unblocks shift-select, the Alt-Tab overlay, and clean chord routing.
- [ ] **Drag-and-drop protocol.** Apps only see clicks. Real DnD — a source starts a
  drag with a *typed payload* (file path / text / image bytes), the compositor drags a
  "ghost", drop delivers the payload to the target window or a drop-zone. Unlocks
  inter-window drag, cross-app text drag, and Pocket Dimension.

### System & security
- [ ] **System ownership / hidden `system` user (#1).** Today a user can delete
  system apps and files (e.g. the clipboard). Introduce an owning model: a hidden
  `system` account owns `/System` + system apps; other users get normal use but
  cannot modify/delete them. Ties into capability enforcement and
  [`design/app-runtime.md`](design/app-runtime.md).
- [ ] **Capability enforcement (app sandbox).** Wire the manifest `caps` field to a
  per-task capability set the kernel checks at the syscall boundary (fs jails,
  `spawn` / `window` / `notify` gating). See [`design/app-runtime.md`](design/app-runtime.md).
- [~] **Ctrl+C / X / V everywhere.** GUI text widgets map the chords over the kernel
  clipboard ring (`SYS_CLIP_*`) — **landed in `TextField`** (^C/^X/^V/^A). Still open:
  **terminal** grid selection + paste using **Ctrl+Shift+C/V** (so plain Ctrl+C stays
  `^C` to the pty), and Files-level copy/cut/paste (see Files app polish).
- [ ] **Pocket Dimension — a per-session shelf (Super+D).** A slide-out panel from the
  **left** screen edge holding media stashed for later (typed payloads: file ref / text
  / image+thumbnail). *Interim with no DnD:* an "Add to Pocket" action + the Super+D
  shelf with per-item buttons; add the drag gesture once DnD lands. Complements the
  clipboard (transient single payload) with a curated multi-item staging shelf.

### Platform / runtime / storage
- [ ] **Use all of RAM — remove the 3 GB cap (don't raise it).** The frame pool maps
  RAM as one contiguous block from address 0, which only holds *below* the PC PCI hole
  (~3.5 GB). Parse an **e820 / UEFI memory map** (passed via `boot_info`) and make
  `vmm` build a **multi-region** frame pool that also maps the RAM remapped above the
  4 GB hole. Then delete the `MEM` cap in the Makefile entirely.
- [ ] **A real userspace runtime + SDK sysroot (the substrate for porting anything).**
  Userspace is **freestanding** today (tiny libc, `-fno-exceptions -fno-rtti`, no STL,
  no dynamic linker, no POSIX). Roughly in order: (1) a reusable **sysroot +
  `tos-cc`/`tos-c++`/`tos-link`**; (2) a **hosted C++ runtime** (libstdc++/libc++ with
  exceptions, RTTI, STL, `__cxa_*` unwind); (3) **`libposix`** (pthreads, fds,
  `open/read/write/close`, `mmap`, signals, `dlopen`); (4) a **QPA-style platform
  layer** (framebuffer + input shim). The main thing between "teaching OS" and "runs
  third-party software." See [`design/app-porting.md`](design/app-porting.md).
- [ ] **Installer (live → install).** Treat the running system as a live medium and
  install onto a target disk: ATA slave + raw block write, in-OS mkfs + partitioning,
  `copytree /System+/Apps`, seed `/Users` + the registry. See
  [`design/installation.md`](design/installation.md).
- [ ] **Growable / smarter filesystem.** Files are stored contiguously and a metadata
  change rewrites the whole slot table; the partition is fixed-size. Extent/indirect
  blocks, a runtime-sized partition, and journaling the table would make it robust.
- [~] **Terminal scrollback paging.** Done via the scroll wheel (256-row ring). Still
  future: **Shift-PgUp/PgDn** keyboard paging and a configurable ring size.

### Smaller ideas
- A heap / `sbrk` for user programs (today a program is its static image + stack).
- More drivers as needed (networking, AHCI/NVMe, a real serial console for input).
- Calibrate the LAPIC timer instead of a fixed count.

---

## Done (changelog)

Terse one-liners; the prose lives in git history + PROJECT.md. Newest first.

- **Single-sourced shadow halo extents (2026-06-01).** Every drop-shadow dirty/cull
  rect (windows, dock, Control Center, Launchpad) now comes from one twm
  `shadow_box(x,y,w,h,spread,dy)` helper fed by a new `ugfx_elevation_extent(level,
  &spread, &dy)` query — so the hand-tuned `DOCK_SH`/`CC_SHADOW_*`/`TH_SHADOW_SP`
  margins can no longer drift (removed `DOCK_SH` entirely; the elevation level is the
  named `DOCK_ELEVATION`). The formula only ever *grows* coverage vs. the old symmetric
  boxes, so it can't smear, and it fixes a latent window-bottom gap (the +5/6px offset
  wasn't in the old halo). twm-internal; zero app-porting impact. Suite 43/43 +
  drag-a-window-over-the-dock screenshot (clean shadows, no residue).
- **Global ScrollBar (2026-06-01).** One scrollbar everywhere. A new C primitive
  `ugfx_scroll_thumb` is the single thumb renderer; a `ui::ScrollBar` helper
  (set/hit/top_from_y/draw) wraps it for toolkit containers. `TextField` was
  refactored onto it (the inline `sb_*` cache is gone; `[ui] sbtop` + `t_scrollbar_drag`
  preserved) and `ListView` adopted it — so **Files + Spotlight now have a real
  draggable thumb** (click the track to jump, drag when focused). The terminal's
  scrollback indicator draws through the same primitive, so every scrollbar in the OS
  looks identical. Verified by screenshot (Files list thumb) + full BIOS suite 43/43.
- **Notepad / global text editing (#5, 2026-06-01).** Caret now **always blinks**
  (Window pulses a repaint at the blink cadence while a caret widget is focused, so it
  no longer freezes when idle); **double-click word-select** (`select_word`, telemetry
  `[ui] word a b`); **Ctrl+←/→ word-jump** and **Ctrl+Backspace / Ctrl+Delete
  word-delete** (`word_prev`/`word_next`, telemetry `[ui] wjump/wdel`); the **Delete
  key** now works in the toolkit (the decoder used to drop `ESC[3~`). Mechanism: the
  keyboard emits xterm modifier sequences (`ESC[1;5C/D`, `ESC[3;5~`) and `^W` for
  Ctrl+Backspace; the toolkit's CSI decoder gained parameter/modifier parsing. All in
  the shared `TextField`, so every toolkit app inherits it. `t_notepad_wordedit`.
- **Rounded, bigger search bars (#14, 2026-06-01).** `TextField` gained a public
  **`radius`** (default `TH_R_SM`) and now fills a rounded well (`ugfx_rrect_aa`) and
  vertically centres single-line text; Spotlight + Launchpad use `TH_R_PILL` and a
  taller field — Google-search-box proportions, our slate palette.
- **Frosted-glass UI pass (2026-05-31).** Real backdrop blur (`ugfx_frost`) under the
  bar / dock / Control Center / Launchpad; precomputed wallpaper; colour-key
  (`ugfx_blit_round_key`) so the frost shows through the Launchpad; iOS-style CC
  switches; dirty-rect hardening via `expand_to_panels` (grows any panel-touching
  dirty rect to the whole panel so the blur can't smear).
- **Launchpad polish (#11, 2026-05-31).** Fixed-cell centred grid (`Grid::recentre`),
  focused search field with type-to-filter (`app_match` → `filt[]`). `t_launchpad_search`.
- **Draggable scroll indicator (#12, 2026-05-31).** The `TextField` right-edge thumb is
  clickable + grab-draggable (`sb_geom`/`sb_set_top_from_y`, `[ui] sbtop=N`).
  `t_scrollbar_drag`.
- **Spotlight keyboard QoL (#13, 2026-05-31).** Arrows + Tab walk the results instead
  of dismissing the popup (twm peeks the byte after `ESC` — `[`/`O` = forward the
  sequence). `t_spotlight_nav`.
- **Scroll-wheel support (#9, 2026-05-30).** PS/2 Z-byte decode → `WEV_SCROLL`;
  `Widget::on_scroll`; ListView + multiline TextField scroll; `term` 256-row scrollback.
- **Launchpad / Spotlight real icons (#2), dynamic dock (#3), Hello removed (#4)
  (2026-05-30).** `app_scan` loads `icon.argb`; lone-Super toggles Launchpad with a
  `WIN_OVERLAY` dim scrim; dock = Launchpad button + pinned + transient running tiles.
- **Window UX (2026-05-30).** Single-instance `summon`, Super+V clipboard summon,
  Super+Tab MRU window switcher. `t_clipboard_summon`, `t_window_switch`.
- **Hierarchical filesystem + Files app.** tosfs v2 (parent-indexed slot table,
  per-task cwd, `mkdir`/`cd`/`pwd`/`mv`/`cp`/`rm -r`/`tree`, `mkfs` packs a tree); a
  Dolphin-style Files app driven by forwarded client-area clicks.
- **Compositor desktop.** Kernel compositor protocol (shared window surfaces + ptys,
  `kernel/ipc.c`); twm = back-buffered compositor (AA rounded windows, soft shadows,
  translucent bar, centred dock, drag/resize/min/max/close, software cursor); `term` =
  standalone ANSI terminal running the shell over a pty; `fastfetch` banner; full PS/2
  keyboard with ANSI escape sequences.
- **C++ application SDK.** Freestanding C++ crt + heap; libc core (coalescing malloc,
  printf); the system API (`ulib` + `sys`); a retained-mode widget toolkit
  (`ui.{h,cpp}`: Window, Widget/Label/Button/Panel/ListView/TextView, multiline
  TextField, wheel scrolling) styled from `theme.h`.
- **UI-modernization pass (2026-05-29).** Hover feedback (twm forwards `btn==0` moves,
  `0xfff`=leave), AA inner borders + state layers + elevation shadows
  (`ugfx_rrect_border`/`state_layer`/`elevation`), surface/border/state/rounding tokens.
- **macOS-style system layout.** `/System` + `/Apps` (`.app` bundles) + `/Users/user`
  tree; layered key=value registry (`reg` cmd); real macOS cursors; maximize =
  fullscreen with auto-hiding bar/dock (edge-reveal).
- **Kernel foundations (README "Next" checklist).** Delete + free-sector map, per-task
  fd tables, ELF loader, MBR partition, timed sleeps, fork/exec + pids/wait/zombies,
  fine-grained SMP (per-CPU run queues + load balancing); limits scale with hardware;
  RTC/`date`, PCI enumeration, PC speaker, reboot/poweroff; BIOS VBE framebuffer; one
  AA JetBrains Mono system font (kernel console + GUI); integration test suite. Kernel
  organized into `arch/ mm/ drivers/ fs/`.

---

## Design guidelines (`design/`)

- [`design/roadmap.md`](design/roadmap.md) — **the strategic plan** (read first): an
  honest current-stage assessment and the phased path (foundation → userspace runtime
  → security → networking → desktop → self-hosting).

Implemented:
- [`design/filesystem-layout.md`](design/filesystem-layout.md) — the `/System`
  `/Apps` `/Users/user` tree; program resolution falls back to `/System/bin`; `init`
  self-heals the skeleton; the shell starts in `~`.
- [`design/app-package-format.md`](design/app-package-format.md) — directory bundles
  `/Apps/<Name>.app/{manifest,icon.argb,bin/<exe>}`, built by `tools/mkapp.py`.
  (Single-file zip `.app` still future.)
- [`design/settings.md`](design/settings.md) — a layered key=value registry, system
  defaults + per-user overrides, `reg` shell command. (Change-bus + Settings app future.)

Planned:
- [`design/ui.md`](design/ui.md) — macOS desktop modernization (status bar,
  notifications, app menus, dock polish).
- [`design/app-runtime.md`](design/app-runtime.md) — app execution + capability sandbox.
- [`design/app-porting.md`](design/app-porting.md) — retargeting external apps (sysroot,
  `libposix`, framebuffer shim).
- [`design/installation.md`](design/installation.md) — installing the live system to disk.

---

## Known issues (documented for later debug, not blocking)

- **BIOS boot past the real-mode load envelope — FIXED (2026-05-29).** Once the kernel
  passed ~127 sectors the BIOS path wedged in a `#UD` loop. Root cause: the chunked
  disk-read loop kept the sectors-remaining counter in `AX` but ran `mov ah, 0x42`
  **before** `push ax`, baking `0x42` into the counter's high byte so the loop never
  terminated and walked off into garbage. Fix: move `push ax` above `mov ah, 0x42` in
  `boot/stage1.asm`'s `.kread` (one-line ordering fix). Debug recipe that nailed it:
  `-d int,cpu_reset` showed the `#UD` flood; `-monitor` + `pmemsave` diffed
  `0x10000`/`0x7e00` against the built images; the live `dap_kernel` fields showed
  `count=64 seg=0xe800` (should've been `count=3 seg=0x2060`). NB: gdb breakpoints in
  the boot sector are unreliable (the loop self-modifies the DAP page) — use dumps.

- **Flaky UEFI tests under host load — hardened (2026-05-29).** `t_mouse`, `t_move`,
  `t_write_then_cat` can fail on the **UEFI** path when the host is heavily loaded
  (tight keystroke/mouse-injection schedules outrun OVMF+TCG). Environmental, not a
  code bug (the BIOS variants pass every run; an isolated run passes). The harness is
  now load-tolerant: `t_mouse` retries the inject+report cycle until the position
  actually changes; `Tos.line_for(cmd, text)` retypes a shell line whose reply doesn't
  appear. A dropped packet now costs another lap instead of a failure.

- **tmpfs scratch leak — FIXED.** `Tos.stop()` now removes the per-run scratch disk /
  OVMF-vars / serial-log it created (a caller-supplied scratch path is left alone), so
  `/tmp` (tmpfs = RAM) no longer grows unbounded across many runs.
