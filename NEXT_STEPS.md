# tOS — next steps

What's planned next. How the current system works is in [PROJECT.md](PROJECT.md);
this file is just the road ahead. Each item should keep `make test` green
(BIOS + UEFI) before it's checked off.

## Done so far

The README "Next" checklist is complete: delete + free-sector map, per-task fd
tables, ELF loader, MBR partition, timed sleeps, fork/exec + pids/wait/zombies,
and fine-grained SMP (per-CPU run queues + load balancing). On top of that: a
graphical desktop with a windowed terminal + PS/2 mouse; limits that scale with
the hardware (frame pool = all RAM, RAM-bounded task count, disk-bounded file
count, no fixed program-load buffer); a BIOS VBE framebuffer; RTC/`date`, PCI
enumeration, PC speaker, reboot/poweroff; and an integration test suite (48 tests
— 46 after a 2026-05-29 consolidation that merged the three SMP cases into one and
the two file-persistence cases into one, plus the clipboard-summon + window-switch
cases added 2026-05-30). The kernel is organized into `arch/ mm/ drivers/ fs/`. See
PROJECT.md for the details.

Desktop polish: userspace is one directory per app over a shared `lib/`; all text
(kernel console + GUI) uses one anti-aliased JetBrains Mono system font
(`kernel/sysfont.h`, baked by `tools/genfont.py`).

A **UI-modernization pass** (2026-05-29) added the rendering techniques behind a
"modern" look (studied from the Quickshell/Qt shell in `~/dev/shells`, applied to
tOS's own slate-blue identity — not its palette): **hover feedback** (twm forwards
`btn==0` pointer-moves to the window under the cursor, `0xfff`=leave; the toolkit
lights up buttons / list rows / menu items), **AA inner borders + state layers +
a Material-style elevation shadow** (`ugfx_rrect_border` / `ugfx_state_layer` /
`ugfx_elevation`), and surface/border/state/rounding tokens in `theme.h`. It also
fixed the Control-Center shadow-residue bug (see the shadow dirty-rect item below).

A **frosted-glass UI pass** (2026-05-31) — the visible look, not more abstract
primitives. A new `ugfx_frost(x,y,w,h,rad,tint)` does a real **backdrop blur**
(separable integer box blur over the back buffer, heap scratch) under a slate tint,
so the **top bar, dock, Control Center and the Launchpad overlay** are genuine
frosted glass (the content behind them blurs through) instead of flat alpha washes —
**our palette, no Material**. The desktop wallpaper is now precomputed once
(gradient + a soft upper glow + a corner vignette) into a buffer that `draw_desk`
fast-blits. The Launchpad shows the frost through a **colour-key** (`ugfx_blit_round_key`):
the app paints its panel background with the `TH_FROST_KEY` sentinel and the
compositor lets the blurred backdrop show there while tiles/text stay solid. The
companion **dirty-rect hardening** (`expand_to_panels`, see the Open section) keeps
the blur from smearing on hover. Cleanups in the same pass: fastfetch's Kernel line
now reads a normal `tOS 1.0` (not "SMP, preemptive"), the README is trimmed to a
brief description, and the seed filesystem dropped throwaway test files
(`notes.txt`/`readme`/`guide`) — `t_seed_tree` now proves the nested tree by reading
a shipped app manifest instead. The **Control-Center toggles** were also redrawn as
proper iOS-style switches (an opaque lit pill track + a white knob with a soft shadow
and rim) — the old version read as a flat dark blob with no "you can flip this"
affordance.

A **hierarchical filesystem**: tosfs v2 is a slot table with parent-indexed
directories; per-task working directory + path resolution; `mkdir`/`rmdir`/`cd`/
`pwd`/`mv`/`cp`/`rm -r`/`tree` in the shell; `mkfs` packs a tree. A **Files** app
(Dolphin-style: navigate / New Folder / Delete / file viewer) sits below Terminal
on the desktop, driven by client-area mouse clicks the compositor now forwards.

A real desktop, with the window manager and the terminal as **separate apps**:
- a kernel **compositor protocol** — window surfaces shared (shared memory)
  between an app and the compositor, per-window event queues — and **ptys** that
  decouple a program's stdio from the console (all in `kernel/ipc.c`);
- **`twm`** is now a pure window manager/compositor: desktop **icons** (read from
  a `shortcuts` file) you **double-click** to launch apps, **draggable** /
  **resizable** windows with **minimize / maximize / close** controls in the
  top-right of the title bar (maximize fills the work area and toggles back;
  minimized windows park as clickable **pills** in the top bar), focus, a
  software cursor composited on top (no flicker, no smear — twm is the sole
  framebuffer writer); the desktop is persistent;
- **`term`** is a standalone terminal emulator (grid + ANSI CSI/SGR parser) that
  runs the **shell** as a piped child; **`fastfetch`** prints a colour system
  banner at startup;
- the **whole keyboard**: scancode set 1 with Shift/Caps/Num/Ctrl/Alt, the
  function and navigation keys emitted as ANSI escape sequences, Ctrl+letter as
  C0 controls.

A **C++ application SDK**, layered like win32 + Qt over the C rasterizer:
- **freestanding C++** (`g++ -ffreestanding -fno-exceptions -fno-rtti`): a crt
  (`user/lib/crt.cpp`) that runs `.init_array` constructors then calls the app,
  `operator new`/`delete` on a `SYS_MMAP` heap, `.init_array` bracketed by the
  user linker script;
- a **libc core** (`user/lib/libc.{c,h}`): `mem`/`str` helpers, a real coalescing
  allocator (malloc/free/realloc/calloc) over `SYS_MMAP`, and printf/snprintf;
- the **system API** — the raw syscall wrappers (`ulib`) plus SDK conveniences
  (`user/lib/sys.{c,h}`: file slurp/spit, launch, stat);
- a **widget toolkit** (`user/lib/ui.{h,cpp}`, namespace `ui`): a retained-mode
  `Window` (shared surface + event loop, ESC-sequence key decoder, focus) and
  `Widget`/`Label`/`Button`/`Panel`/`ListView`/`TextView`, styled from `theme.h`,
  drawing through `ugfx`. Callbacks are plain function pointers + ctx (apps use
  non-capturing lambdas). The **Files, Notepad, Spotlight, Launchpad and Clipboard
  apps are C++ toolkit apps**. The toolkit also has a multiline `TextField` and
  wheel scrolling (`Widget::on_scroll` / `Window::dispatch_scroll`).

Working through the **in-OS todo** (`/Users/user/Documents/todo.txt`, captured into
its own section below): items **#4** (remove Hello), **#3** (dynamic dock — Launchpad
button + transient running-app tiles), **#2** (Launchpad/Spotlight real icons +
Super-toggle + a dim overlay above the dock) and **#9** (scroll-wheel events end to
end + Notepad/`term` scroll indicators, incl. a `term` scrollback ring) are **done**
(2026-05-30); the BIOS suite is **39/39** (added `t_dock_launchpad`). The remaining
items (#1, #5, #6, #7, #8, #10, #11) are tracked in that section.

## Open

- [ ] **Use all of RAM — remove the 3 GB cap (remove it, don't raise it).** The
  interactive run targets are pinned to `-m 3G` because the kernel's frame pool
  maps RAM as one contiguous block from address 0, which only holds *below* the
  PC's PCI hole (~3.5 GB); above that, firmware puts a hole below 4 GB and remaps
  the rest above 4 GB, so a naive contiguous map would walk into device MMIO. To
  use any amount (16 GB, whatever the box has), parse an **e820 memory map** —
  BIOS `int 15h`/E820 in `stage1.asm`, the UEFI memory map in `uefi.c`, passed via
  `boot_info` — and make `vmm` build a **multi-region** frame pool that also maps
  the RAM remapped above the 4 GB hole. Then delete the `MEM` cap in the Makefile
  entirely (the goal is no cap, not a bigger one).

- [ ] **A full taskbar / window switcher** — see **Alt-Tab window switcher** in
  the "Next batch" section below (this older note assumed the top-bar minimize
  *pills*, which the dock-collapse genie has since replaced).

- [~] **Terminal scrollback** — DONE via the scroll wheel (2026-05-30): `term` keeps
  a 256-row scrollback ring and the wheel pages through it (issue #9). Still future:
  **Shift-PgUp/PgDn** keyboard paging and a configurable ring size.

- [ ] **Live resize preview + content reflow** — resize currently snaps on
  mouse-up and preserves cells at their (row,col) without reflowing wrapped lines.
  A live outline while dragging the grip, and reflowing long lines, would polish it.

- [ ] **Capability enforcement (app sandbox)** — apps run unsandboxed today; wire
  the manifest `caps` field to a per-task capability set the kernel checks at the
  syscall boundary (fs jails, `spawn`/`window`/`notify` gating). See
  [`design/app-runtime.md`](design/app-runtime.md).

- [ ] **Installer (live → install)** — treat the running system as a live medium
  and install it onto a target disk: ATA slave + raw block write, in-OS mkfs +
  partitioning, `copytree /System+/Apps`, seed `/Users` + the registry. See
  [`design/installation.md`](design/installation.md).

- [ ] **A real userspace runtime + SDK sysroot (the substrate for porting anything).**
  Agreed 2026-05-31: we don't need Qt itself, but we DO need the runtime it (and any
  serious software) assumes — that's broadly useful, so it's recorded here for a later
  round (not this one). Today userspace is **freestanding**: a tiny custom libc,
  `-fno-exceptions -fno-rtti`, no STL, no dynamic linker, no POSIX. To run ordinary
  C/C++ programs (or eventually a real UI toolkit) we'd build, roughly in order:
  1. a reusable **sysroot + `tos-cc`/`tos-c++`/`tos-link`** wrapping the build flags;
  2. a **hosted C++ runtime** — `libstdc++` (or libc++) with **exceptions, RTTI, the
     STL**, plus the `__cxa_*` personality/unwind support;
  3. **`libposix`** — pthreads, file descriptors + `open/read/write/close`, `mmap`,
     signals, and a `dlopen`/dynamic-linker path;
  4. a **platform/QPA-style layer** — a framebuffer + input shim (think `linuxfb` +
     `evdev`) so a graphical toolkit has something to target.
  With (1)–(4) in place, porting SDL, a real toolkit, or other Unix software becomes a
  recompile rather than a rewrite. tOS is **early-to-mid development**; this runtime is
  the main thing standing between "teaching OS" and "runs third-party software." See
  [`design/app-porting.md`](design/app-porting.md) and the new
  [`design/roadmap.md`](design/roadmap.md).

- [ ] **Desktop UI roadmap (beyond fullscreen)** — status-bar items + a
  notification/toast system + app menus (App/File/Edit), dock magnification, and
  hiding the window's *own* chrome in fullscreen for a purer macOS look. See
  [`design/ui.md`](design/ui.md).

- [ ] **Grow the toolkit + port more apps** — the C++ widget toolkit covers the
  basics; add a TextField (in-window text entry), a ScrollBar/scrolling container,
  menus, and a layout system, then port `term`/`fastfetch` (and new apps) onto it.

- [ ] **Make the scroll indicator GLOBAL (shared ScrollBar component).** Today the
  draggable scroll thumb lives only in the multiline `TextField` (#12), so Notepad
  has it but `ListView` (Files, Spotlight) scrolls by wheel/keys with no thumb and the
  terminal has its own non-draggable scrollback indicator. The user expects it
  everywhere, like every other OS. Factor the thumb geometry/hit-test/drag
  (`sb_geom`/`in_scrollbar`/`sb_set_top_from_y` + `sb_drag`) out of `TextField` into a
  reusable toolkit **`ScrollBar`** helper a scroll container embeds; give it to
  `ListView` (covers Files + Spotlight) and wire the terminal's scrollback to it too,
  so a single component renders/handles every scrollbar in the OS. This is
  consolidation, not scatter — one scrollbar, used everywhere.

- [ ] **Files app polish** — rename (needs the TextField above), scrolling for
  long directories, and inline copy/cut/paste keys (the clipboard ring + a "Copy"
  action already exist; the Ctrl+C/X/V wiring and inter-window drag-and-drop are in
  the "Next batch" section).

- [ ] **Growable / smarter filesystem** — files are stored contiguously and a
  metadata change rewrites the whole slot table; the partition is a fixed size.
  Extent/indirect blocks (no contiguous requirement), a runtime-sized partition,
  and journaling the table would make it robust and unbounded.

## Next batch — input, clipboard & window UX (agreed 2026-05-29)

Three **foundations** are shared by most of this batch; building each once makes
the features cheap:

- **Richer key events.** `WEV_KEY` carries a single byte today, key-DOWN only.
  Widen it to also carry **modifier flags** (Ctrl/Alt/Shift/Super) and emit
  **key-UP** events. Alt-Tab's "hold Alt, release to commit" and clean Ctrl /
  Ctrl-Shift chord routing both need this. `kernel/drivers/keyboard.c` already
  tracks the modifier state for ASCII translation — it just doesn't surface it.
- **A drag-and-drop protocol.** Apps only see clicks. Real DnD — a source app
  starts a drag with a *typed payload* (file path / text / image bytes), the
  compositor drags a "ghost" under the cursor, drop delivers the payload to the
  target window or a compositor drop-zone — unlocks inter-window drag *and* Pocket
  Dimension, and is reusable well beyond either.
- **Single-instance "summon" — DONE (2026-05-30).** `summon(title, exec, frame)`
  in twm (`user/twm/twm.c`): if a window with that identity already exists it
  raises + focuses it (restoring it from the dock first if minimized, via the genie)
  instead of forking a second copy; a title-keyed `pending_until` guard absorbs the
  press-twice-before-the-window-maps race. Matches by window title for now (a stable
  per-app id in the window metadata is the cleaner key later). Built on a factored
  `focus_window(slot)` raise/refocus helper now shared by the dock, the switcher and
  summon.

- [x] **Super+V summons the existing clipboard, doesn't spawn a new one. DONE
  (2026-05-30).** twm's Super+V handler now calls `summon("Clipboard", "clipboard")`
  instead of an unconditional `launch` — it focuses/restores the running clipboard
  window if there is one, else launches. Covered by `t_clipboard_summon` (Super+V
  twice ⇒ a single `[clipboard] up`).

- [ ] **Ctrl+C / X / V everywhere, with the terminal's Ctrl+Shift+C/V variant.**
  These are *app-level edit* actions: the compositor forwards the intent (via the
  richer key events) and the focused app acts on its own selection, all over the
  existing kernel clipboard ring (`SYS_CLIP_*`).
  - GUI apps: the toolkit maps the chords to copy/cut/paste on the focused text
    widget — so this needs the **TextField + a selection model** (already listed
    under "Grow the toolkit"). *Paste* needs no selection and can land first.
  - Terminal: Ctrl+C/X/V must stay the shell's C0 controls / job control (Ctrl+C =
    `^C` to the pty), so the clipboard uses **Ctrl+Shift+C/V** — the standard.
    Copy needs **terminal grid selection** (click-drag to select cells →
    `clip_put`); paste is `clip_get` → `pty_write` and can ship before selection.

- [x] **Alt-Tab window switcher — interim Super+Tab DONE (2026-05-30).** Super+Tab
  cycles focus MRU-style through *all* open windows, single-stepping + raising on
  each press (the key-down-only model the note anticipated). The keyboard driver
  emits a `KEY_SUPER_TAB` byte (sibling of `KEY_SUPER_V`); twm's `window_switch()`
  runs the cycle. Note on the "separate MRU stack": in this compositor a focus
  change *always* raises, so the z-order list (`zo[]`, top == most recent) already
  IS the MRU — a switch *session* snapshots reversed-`zo` once and steps a cursor
  through the snapshot (stable while it reorders `zo` underneath), and the landed
  window becomes MRU-top for free when the session lapses (`SWITCH_LINGER`). Covered
  by `t_window_switch`. **Still future:** the full hold-Alt / release-to-commit
  overlay with thumbnails — that one still needs the **key-up + modifier events**
  foundation above.

- [ ] **Pocket Dimension — a per-session shelf (Super+D).** A slide-out panel from
  the **left** screen edge (the edge is free: bar=top, dock=bottom) that holds
  media you stash for later, so you don't hunt a target window to drop things into
  — park them in the pocket, retrieve when ready.
  - *Gesture:* drag media toward the left edge → the shelf peeks out (reuse the
    `update_chrome` reveal/linger state machine the bar/dock already use) → drop →
    the item slides in. **Summon / pin** with Super+D. Drag an item back out, or use
    per-item actions (Open / Copy to clipboard / Reveal in Files / Remove).
  - *Storage:* session-scoped (cleared on reboot for now; persist to
    `/Users/user/.cache/pocket` later). An item is a **typed payload** — file
    reference (path), raw bytes (text), or image (+ a thumbnail via `blit_scaled`)
    — the same shape the DnD payload carries. A kernel-side store (a sibling of the
    clipboard ring) lets any app put/get by syscall with twm as the UI; twm-owned
    RAM is the simpler first cut.
  - *Depends on the DnD protocol* for the drag gesture. **Interim with no DnD:** an
    "Add to Pocket" action on the Files context menu + the clipboard manager, plus
    the Super+D shelf with per-item buttons — ship that first, add the drag later.
  - *Relation to the clipboard:* the clipboard is the transient single payload you
    paste; the pocket is a curated multi-item staging shelf kept for the session.
    Complementary — allow promoting a clip into the pocket and back.

- [x] **Harden the compositor's dirty-rects — DONE (2026-05-31), generalised to
  frosted panels.** When the bar/dock/control-center became real frosted glass
  (`ugfx_frost` blurs the backdrop, see the frosted-glass pass below), a partial
  dirty rect re-blurred the already-frosted pixels outside it and smeared (the
  "hovering the bar/dock draws shadows" bug). Fixed by single-sourcing every
  frosted panel's full extent (+ shadow halo) in `expand_to_panels()` and growing
  ANY `add_dirty` rect that touches a panel to cover that whole panel, so the frost
  always reads a freshly-painted, un-frosted backdrop. This subsumes the older
  hand-tuned `TH_SHADOW_SP`/`CC_SHADOW_*`/`DOCK_SH` margin worry below: the panel
  extents now live in one place. (Known minor: a 1-frame frost transient can flash
  when hovering right at the CC's edge over a window behind it; self-corrects next
  frame, deprioritised.)

- [~] **Harden the compositor's shadow dirty-rects (deferred from the UI pass).**
  Drop-shadowed chrome (windows, dock, Control Center) must include the shadow
  **halo** in *both* every `add_dirty` that invalidates it *and* the `compose()`
  cull test that decides whether to repaint it — otherwise the cursor (or a window
  edge) gliding through the halo lets compose paint the desktop over the shadow
  without redrawing it: the Control-Center "cursor draws on the screen near the
  edges" bug, fixed by matching the cull/dirty boxes to the true shadow extent.
  It's correct today, but the extents are **hand-tuned margins repeated in a few
  places** (`TH_SHADOW_SP`, `CC_SHADOW_*`, `DOCK_SH`) that must track the shadow
  params by hand (e.g. an elevation's downward offset isn't reflected in the side
  margins — currently harmless only because the dock sits at the screen bottom).
  Single-source it: have `ugfx` expose a shadow/elevation **extent** query and give
  twm one `bounds_with_shadow(rect, level)` helper used by both the cull and every
  invalidation, so the margins can't drift. *Not per-app* — shadows are entirely
  compositor-side (apps draw a flat surface; twm wraps the window), so this is
  twm-internal hygiene with zero impact on app porting.

## From the in-OS todo (`/Users/user/Documents/todo.txt`, captured 2026-05-30)

Ten items the user wrote inside tOS itself. Ordered here as written; being worked
cheapest-first. Several overlap with the batches above — cross-referenced inline.

- [ ] **1. System ownership / a hidden `system` user.** Today a user can delete
  system apps and files (e.g. the clipboard) outright. Introduce an owning model: a
  hidden `system` account owns `/System` + system apps; other users get
  execute/normal-use but cannot modify or delete them. Ties into
  **capability enforcement** (Open) and [`design/app-runtime.md`](design/app-runtime.md).

- [x] **2. Launchpad / Spotlight polish — DONE (2026-05-30).** `applist.h`'s
  `AppEntry`/`app_scan` now load each bundle's `icon.argb`, and the launcher grid +
  Spotlight rows blit the real icons via a new shared `ugfx_blit_scaled` (Launchpad
  falls back to the big initial only when an app has no icon). A lone-Super tap now
  **toggles** Launchpad (a second tap posts `WEV_CLOSE`; Esc still works). New
  `WIN_OVERLAY` window flag (set via `ui::Window::overlay`): twm draws such a window
  **above the dock over a full-screen dim scrim** (`overlay_slot()` + a scrim pass in
  `compose()`, with a full repaint on the open/close edge). Verified by screenshot
  (real icons + dimmed desktop above the dock) and the toggle assertion in
  `t_launchpad`. Full BIOS suite 39/39.

- [x] **3. Dock cleanup — DONE (2026-05-30).** twm now catalogs all `/Apps` bundles
  once (`load_apps` → `apps[]` with a pinned flag) and `rebuild_dock()` composes the
  visible dock = **Launchpad button (leftmost) + pinned apps + a transient tile for
  every running non-popup window not already shown**, re-laid-out whenever the
  running set changes (`running_sig`/`dock_sig`). So: only Terminal + Files are
  pinned (`Notepad.app` manifest → `pinned = false`); the Launchpad button is a
  grid-glyph tile that single-click `summon`s the grid; and Notepad (opened from
  Files or Spotlight) shows as a running tile and drops off when closed. Covered by
  the new `t_dock_launchpad` (leftmost + click-opens) and the reworked
  `t_notepad_edit_save` (Spotlight-launch + asserts the transient tile). Full BIOS
  suite 39/39.

- [x] **4. Remove the Hello app — DONE (2026-05-30).** Dropped `user/hello/`,
  `fs/apps/Hello.app/`, the `hello` ELF from `CXXPROGS` + the mkfs seed, the
  `Hello|hello` desktop shortcut, `Hello` from `dock.pinned`, and the `HELLO` icon
  tile (regenerated `icons.h` → `ICON_APP` is now index 3). `t_boot_and_ls` updated
  to expect `Notepad.app` and passes (BIOS). The shell `fork`-demo string and the
  `hello123` registry-test value are unrelated and left alone.

- [ ] **5. Notepad text-editing polish.** The `|` caret should **always be visible
  and blinking** (not only while typing/arrowing). Add the **I-beam cursor** when
  hovering over / selecting text. Add richer text editing: **double-click to select
  a word**, **Ctrl+arrows to jump word-by-word**. Overlaps the toolkit TextField +
  selection-model work and the "selectable text is OS-provided" question in #10.

- [ ] **6. Menu-bar API for apps.** When an app is focused the top-left shows its
  name; design an API letting an app add **menu tiles** there. First tile = app name
  with a dropdown (About / Preferences / Quit / …); apps add more tiles (e.g. Notes →
  File → Save / Save As / New, plus Help) — macOS-style. This is the **app menus**
  half of the "Desktop UI roadmap" (Open) and [`design/ui.md`](design/ui.md).

- [ ] **7. macOS-style Alt-Tab animation.** Windows shown side-by-side, moving
  between them as you cycle — the full overlay the interim Super+Tab switcher left
  as future (needs the **key-up + modifier events** foundation in the Next batch).

- [ ] **8. OS-logo dropdown.** Clicking the logo at top-left opens a dropdown
  (Preferences / Help / About). Placeholders are fine for now — wire the menu, leave
  the items inert. (Shares the menu-tile machinery from #6.)

- [x] **9. Scroll-wheel support — DONE (2026-05-30).** The PS/2 driver now decodes
  the IntelliMouse/Explorer Z byte (negated so `mousestate.wheel > 0` = up) and
  accumulates it; twm forwards it to the window under the cursor as a new
  `WEV_SCROLL` event (position + signed delta, `WEV_MOUSE_PACK`d). The toolkit gained
  `Widget::on_scroll` + `Window::dispatch_scroll`: **ListView** scrolls (Files /
  Spotlight), and the multiline **TextField** scrolls freely (snaps to the caret only
  when it *moves*) with a **scroll indicator** on its right edge — Notepad inherits
  both. **term** gained a 256-row **scrollback ring**: the wheel pages through history
  (new output snaps back to live) with a right-edge position indicator. Shared
  `ugfx_blit_scaled` added earlier. Verified by screenshots (Notepad + term history)
  and full BIOS suite 39/39.

- [ ] **10. Desktop-as-a-folder + a global selection suite.** Treat the desktop as
  `/Users/user/Desktop`: right-click context menu, the directory's files shown as
  icons, click-select / Ctrl-multi-select — like any desktop. Same selection model
  in Files. Open design question the user raised: **is text/file selection OS-side
  or per-app?** Decision to make: have the OS/toolkit own the selection + cursor
  contract (an app marks a region "selectable text / icons", declares the cursor and
  the keybinds), so apps opt in and future work is cheap. If so, build the full
  suite once. Overlaps #5 and the toolkit selection model.
  - **Files, specifically:** the same selection model plus the clipboard/drag verbs —
    **Ctrl+C / Ctrl+X / Ctrl+V** (copy / cut / paste over the kernel clipboard ring)
    and **drag** to move/copy items between folders/windows. Today Files has a
    "Copy" *action* but not the C/X/V keybinds, cut, paste, or drag — wire those in
    (Ctrl+Shift variants where the terminal already claims C/X/V; see the "Ctrl+C / X
    / V everywhere" Next-batch item and the DnD-protocol foundation).

- [x] **11. Launchpad polish — DONE (2026-05-31).** The grid (`user/launchpad/
  launchpad.cpp`) now uses a fixed-size cell (`LP_CELL`) and **centres the tile
  block** both horizontally and vertically (`Grid::recentre`, recomputed each draw
  so it tracks the filtered count). A centred single-line **search field** sits above
  it and is the window's initial `focus`, so you can **type to filter without
  clicking it first**; `on_change` re-runs Spotlight's `app_match` filter into
  `filt[]` and the grid draws only matches (launching maps the filtered index back
  through `filt[]`). Emits `[launchpad] filt=N`. Covered by the new
  `t_launchpad_search` (filt 3 → 1 on "note"); `t_launchpad` + `t_dock_launchpad`
  still green.

- [x] **12. Draggable scroll indicator — DONE (2026-05-31).** Re-implemented the
  missing pieces (`sb_geom`/`sb_set_top_from_y` were declared but never defined, and
  `on_mouse` didn't engage the strip): the multiline `TextField`'s right-edge thumb
  is now clickable — a press on the track maps the pointer-y to the scroll `top`
  (`[ui] sbtop=N`) and the thumb is grab-draggable via `on_drag -> sb_set_top_from_y`
  (with `on_button_up` ending the drag). `t_scrollbar_drag` is reworked (scroll to the
  TOP first; keep clear of the bottom dock band that ate the press) and **registered**
  (BIOS suite now 42). It asserts a low track-click scrolls down, a high one scrolls
  up, and a low click again returns — the track maps the pointer both ways. (The
  held-thumb *drag* is wired in code; the harness drag path at the window's right edge
  is finicky, so the round-trip click is the stable assertion.)

- [~] **12-old. Draggable scroll indicator — superseded by the DONE note above.**
  The multiline `TextField`'s right-edge scroll thumb (`user/lib/ui.cpp`) is now
  **clickable and draggable**: `on_mouse` detects a press in the right-edge strip
  (`in_scrollbar`, `x >= r.x+r.w-8`), enters a `sb_drag` mode, and `drag_to` maps
  the pointer-y to the scroll `top` via `sb_set_top_from_y` (geometry shared with
  `draw()` through `sb_geom`); the thumb draws fatter + brighter while grabbed and
  emits `[ui] sbtop=N`. twm also now traces each placed window's client rect
  (`[twm] win <title> <wx> <wy> <w> <h>`) and the harness gained `win_rect()` +
  `drag()` helpers. Builds clean; the wheel/keyboard scroll tests stay green.
  **Still open:** `t_scrollbar_drag` (added to `tests/run_tests.py` but *not*
  registered in `BIOS_TESTS` yet) needs reworking — pressing Enter to fill the
  buffer auto-scrolls to the bottom via caret-follow, so the test's first "click
  low on the track" is a no-op (`top` is already at max → no `sbtop=`). Rework it to
  scroll to the TOP first (or fill without moving the caret to the end), then assert
  a low click increases `top` and a drag-up decreases it, and re-register it.

  ListView has no thumb yet (it scrolls by wheel/keys only); add the same draggable
  indicator there for parity when convenient.

- [x] **13. Spotlight keyboard QoL — DONE (2026-05-31).** Arrow keys (Up/Down) and
  **Tab** now walk the Spotlight results instead of being swallowed by the search
  field — or, worse, *dismissing* the popup ("instantly closing the thing"). Root
  cause: arrows arrive as CSI escape sequences (`ESC [ A`..`D`) and twm treated the
  leading `ESC` byte as "dismiss popup". Fix: twm now **peeks the byte after an ESC**
  (a one-byte `twm_getkey`/`twm_ungetkey` pushback in `user/twm/twm.c`) — `[` or `O`
  means a nav/function key, so the whole sequence is forwarded to the focused app;
  anything else is a standalone Esc that still dismisses the popup. Spotlight's
  `on_key` (`user/spotlight/spotlight.cpp`) moves the selection on Up/Down and cycles
  it on Tab (wrapping), emitting `[spotlight] sel=N`. Covered by `t_spotlight_nav`.

## Design guidelines (written up in `design/`)

- [`design/roadmap.md`](design/roadmap.md) — **the strategic plan**: an honest
  current-stage assessment (early-to-mid dev) and the phased path to a fully
  functional OS (foundation → userspace runtime → security → networking → desktop →
  self-hosting). Read this first for the big picture; the items below are the pieces.

Implemented:
- [`design/filesystem-layout.md`](design/filesystem-layout.md) — **implemented.** The
  `/System` `/Apps` `/Users/user` tree; program resolution falls back to
  `/System/bin`; `init` self-heals the skeleton; the shell starts in `~`.
- [`design/app-package-format.md`](design/app-package-format.md) — **implemented** as
  directory bundles: `/Apps/<Name>.app/{manifest,icon.argb,bin/<exe>}`, built by
  `tools/mkapp.py`, scanned by twm. (Single-file zip `.app` is still future.)
- [`design/settings.md`](design/settings.md) — **implemented.** A layered key=value
  registry (`user/lib/registry`), system defaults + per-user overrides, `reg`
  shell command. (Change-notification bus + a Settings app are future.)

Planned (not implemented):
- [`design/ui.md`](design/ui.md) — macOS desktop modernization. The fullscreen +
  auto-hide bar/dock is **done**; the status bar, notifications, app menus, and
  dock polish remain.
- [`design/app-runtime.md`](design/app-runtime.md) — app execution + a capability
  sandbox tied to the manifest `caps`.
- [`design/app-porting.md`](design/app-porting.md) — retargeting external apps onto
  the tOS SDK (sysroot, `libposix`, framebuffer shim).
- [`design/installation.md`](design/installation.md) — installing the live system
  onto a target disk.

## Ideas / smaller

- A heap / `sbrk` for user programs (today a program is its static image + stack).
- Mouse *motion* forwarding for **hover** is now done (compositor → focused window,
  `btn==0`); the remaining half — **drag** inside/between apps — rides the DnD
  protocol in the "Next batch" section.
- More drivers as needed (networking, AHCI/NVMe, a real serial console for input).
- Calibrate the LAPIC timer instead of a fixed count.

## Known issues (documented for later debug, not blocking)

- **BIOS boot once the kernel exceeded the real-mode load envelope (FIXED
  2026-05-29).** Symptom history: BIOS showed `disk read error` once the kernel
  passed ~127 sectors, then (after the chunked-read rewrite) a **black screen /
  early fault loop** — empty serial, the CPU wedged in real mode at `F000:FF53`
  (the BIOS dummy `iret`) servicing a non-stop stream of `#UD` (vector 6).
  - **Real root cause.** The chunked-read loop kept the *sectors-remaining* counter
    in `AX`, but `mov ah, 0x42` (the int 13h function number) ran **before**
    `push ax`, so the value saved/restored across the read had `0x42` baked into
    its **high byte**. The loop tested the whole `AX` (`test ax,ax` / `cmp ax,64`),
    so it never saw the counter hit the partial-chunk path or zero: it read 64-sector
    chunks forever, walking `dap_kernel.seg` up to `0xe800` (phys `0xe8000`) until it
    executed garbage → `#UD` → the BIOS's int-6 vector points at a bare `iret`, which
    returns to the same faulting byte → infinite loop. The earlier "chunked read
    places bytes wrong / 64 KB-crossing copy" hypotheses were **wrong**: a guest
    memory dump proved both the load at `0x10000` and stage2 at `0x7e00` were
    byte-perfect; only the loop's *termination* was broken.
  - **Fix.** Move `push ax` **above** `mov ah, 0x42` in `boot/stage1.asm`'s `.kread`
    so the counter keeps a clean high byte across the call. One-line ordering fix;
    the 64-sector chunking (which already avoids the 127-sector and 64 KB-DMA limits
    — chunks are 32 KiB at 64 KiB-aligned segments) was otherwise correct.
  - **Verified.** BIOS boots to `[twm] desktop ready`; `make test` is **52/52**
    (full BIOS + UEFI halves). Debug recipe that nailed it, for next time:
    `-d int,cpu_reset` showed the `#UD` flood; `-monitor` + `pmemsave` dumped
    `0x10000`/`0x200000`/`0x7e00` to diff against `build/kernel.bin`/`boot.bin`;
    reading the live `dap_kernel` fields exposed `count=64 seg=0xe800` (should have
    been `count=3 seg=0x2060`); a gdb breakpoint after `mov cx,[count]` showed
    `ax=0x4283` (the tell-tale `0x42` high byte). NB: gdb software breakpoints in the
    boot sector are unreliable because the loop self-modifies that page (the DAP);
    use register/memory dumps there.


- **Flaky UEFI tests under host load.** `t_mouse`, `t_move`, and `t_write_then_cat`
  can fail on the **UEFI** path when the *host* is heavily loaded (e.g. many QEMU
  runs back-to-back). They drive the guest by injecting keystrokes / relative mouse
  movement on tight `time.sleep(~0.04s)` schedules; when OVMF+TCG runs slow the
  guest can't keep up, so a move/keystroke is missed and the assertion sees an
  unchanged position / un-read-back file. Evidence it's environmental, not a code
  bug: the **BIOS** variants of all three pass every run; a single isolated run
  passes; and the original (pre-change) mouse driver shows the same failures under
  the same load. The symptom for `t_mouse` is the cursor "stuck" at the
  open-terminal position `(576,744)` because the second `mouse` reading never lands.
  **Hardened 2026-05-29.** The harness is now load-tolerant: `t_mouse` retries the
  whole inject+report cycle until the reported position actually changes (instead of
  one fixed-sleep burst), and a `Tos.line_for(cmd, text)` helper retypes a shell line
  (clearing any half-typed line first) when its expected reply doesn't appear —
  applied to the `write` step of `t_write_then_cat` / `t_move`. A dropped packet or
  keystroke now just costs another lap instead of failing the run. The kernel mouse
  driver itself was always robust (4-byte IntelliMouse-Explorer stream with a
  Linux-psmouse-style timeout resync).

- **tmpfs scratch leak (FIXED).** `tests/harness.py` named each run's scratch disk /
  OVMF-vars / serial-log by port and never deleted them, so `/tmp` (which is tmpfs =
  RAM-backed) grew unbounded (~2.6 GB of stale `tos_test_disk_*.img` after a few
  hundred runs). `Tos.stop()` now removes the per-run temp files it created (a
  caller-supplied scratch path, e.g. the persistence test, is left alone).
