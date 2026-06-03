# tOS — next steps

How the system works **today** is in [PROJECT.md](PROJECT.md); this file tracks what's
**left** plus a terse log of what's landed. Every item keeps `make test` green (BIOS +
UEFI) before it's checked off.

**Status:** `make test` **40/40** (29 e2e journeys on BIOS + a UEFI subset 11) + **62 host
unit tests** (`make unit`, no QEMU). Pyramid policy in [`design/testing.md`](design/testing.md);
the phased plan in [`design/roadmap.md`](design/roadmap.md). tOS is early-to-mid development.

Legend: `[ ]` not started · `[~]` partial · `[⏸]` set aside (don't build unless asked).

---

## Open — the road ahead

### Toolkit & desktop UI
- [ ] **Files + Desktop suite (#10).** A shared `ui::FileView` powering both the Files window
  and a new bottom-pinned `WIN_DESKTOP` layer over `~/Desktop`: **multi-select** (Ctrl/Shift-click
  + rubber-band marquee — single-select today), **folder/multi-item copy-cut-paste** (today's
  `CLIP_FILE`-of-bytes can't hold a directory → path-reference clipboard + recursive `cp_r`),
  **rename**, context menus, and **drag-to-move** (needs DnD). **Keyboard shortcuts:** F2 rename,
  Ctrl+N new folder, Enter/Ctrl+O open, Delete (or Backspace) remove, Ctrl+A select-all,
  Backspace/Alt+← up a directory, plus the existing Ctrl+C/X/V — surfaced in the context menu and a
  menu bar (#6) so the accelerators show. → [`files-and-desktop.md`](design/files-and-desktop.md)
- [~] **App menus (#6).** **Done:** the app→WM protocol — `SYS_WIN_SETMENU` (a `struct winmenu`
  of up to 5 menus × 8 items), `SYS_WM_GETMENU`, a `WEV_MENU` event, and the `ui::Window`
  `menu_begin/menu_add/menu_item/menu_commit` + `on_menu()` API; twm draws a bar tile per menu
  (kind-3 dropdown) and routes a pick back to the app. **Per-item state + accelerators:** items
  carry `WMI_DISABLED`/`WMI_CHECKED` flags and a Ctrl-accelerator letter — the dropdown greys
  disabled rows (no hover, non-clickable), draws a leading ✓ for checked rows, and shows the
  accelerator (e.g. `^S`) right-aligned; the compositor intercepts `Ctrl+<letter>` for the focused
  window and fires the matching enabled item as a `WEV_MENU` (opt-in per declared menu, so
  menuless apps keep their raw Ctrl chords). Runtime toggles via `menu_set_checked/_set_enabled`.
  Notepad ships File [New ^N, Save ^S] / Edit [Select All ^A, Undo ^Z, Redo ^Y] / View [✓ Status
  Bar]; **Files** ships File [New Folder ^N, Refresh] / Edit [Copy ^C, Cut ^X, Paste ^V, Delete] /
  Go [Up, Back, Forward]; **Terminal** (a raw-syscall app, proving the protocol isn't toolkit-only)
  ships Edit [Copy, Paste, Clear] with no Ctrl accelerators so the shell keeps ^C. **Left:**
  submenus (needs a `struct winmenu` ABI bump — deferred until something needs nesting). →
  [`ui.md`](design/ui.md)
- [ ] **Grow the toolkit + port apps.** A layout system; `fastfetch` and new apps onto the toolkit.
  (term + Files now carry real menu bars; the toolkit layout system is the remaining piece.)
- [~] **File open/save dialog (reusable picker).** **Done:** an in-process `ui::FileDialog` modal in
  the toolkit (Open + Save modes), built on the toolkit's own `ListView` + `TextField` so every app
  gets the same chrome — a **Favorites sidebar**, an **Up button**, a path bar, the directory list,
  and (Save) a name field — without re-implementing it. It honours system ownership (the Save button
  greys when the folder isn't user-writable) and raises a nested `ui::ConfirmDialog` **Replace /
  Cancel** when overwriting. Notepad wired it up: **File > Open…** (`^O`) and **File > Save As…**
  drive it; e2e `t_file_dialog`. **Left:** (a) the chosen design was the **in-process modal**, not a
  standalone picker **process** returning a path over IPC — revisit the process variant only when a
  non-toolkit app (e.g. the raw-syscall terminal) needs a picker; (b) **Rename** in the overwrite
  warning (it's Replace/Cancel today, the design wanted Replace/**Rename**/Cancel); (c) once the
  Files suite's shared `ui::FileView` lands (#10), refactor the dialog's hand-rolled list onto it so
  there's one directory-view component. → ties into [`files-and-desktop.md`](design/files-and-desktop.md)
- [~] **Notepad redesign: tabs + session autosave (#5).** The filename still sits in an editable
  field at the top. Rework it:
  - [x] **Unsaved-changes guard** — **DONE.** New (and the window Close) on a dirty buffer now raise
    a modal **Save / Discard / Cancel** sheet (the reusable `ui::ConfirmDialog`) instead of silently
    nuking; Save writes first, then performs the deferred New/Quit. Backed by a `Window::on_close()`
    veto hook + an `editor.on_change` dirty flag. e2e `t_notepad_guard`.
  - **Tabs** — drop the top filename field; each note is a tab. A **`+` button** beside the tabs and
    **File > New** both open a fresh **"untitled"** tab; switch + close tabs; the title bar / active
    tab shows the note's name (or "untitled").
  - **Session autosave** — periodically cache each tab's text **and** the app state (open tabs, the
    active one, per-tab name + dirty flag) to a per-user draft store (e.g.
    `/Users/user/.cache/notepad/`, like Windows Notepad's draft restore), so relaunching restores the
    whole session — even notes that were never explicitly saved.
  - **Save / Open flow** — the reusable **file open/save dialog** above now exists and is wired into
    Notepad as **File > Open…** / **File > Save As…**. Remaining for the tabs rework: route a plain
    **Save on an untitled note** through the Save dialog (today `^S` still writes straight to
    `Documents/untitled.txt`). → [`ui.md`](design/ui.md)

### Global text-interaction contract
The toolkit owns the in-window text contract: anything in `TextField` is inherited by every
toolkit app for free. **Done:** blink caret, drag-select, Ctrl+A, double-click word-select,
Ctrl+←/→ word-jump, Ctrl+Backspace/Delete word-delete, Delete, shift-select, **undo/redo
(Ctrl+Z / Ctrl+Y)**. **Left:**
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
- _(nothing queued right now)_

---

## Done (changelog)

Terse one-liners, newest first; the prose lives in git history + PROJECT.md.

- **Reusable file picker `ui::FileDialog` #4 (2026-06-03).** A new toolkit modal — the system's one
  Open/Save browser. Built on the toolkit's own `ListView` + `TextField` (no atlas dep — lean vector
  folder/file glyphs), so any app gets the same chrome for free: a **Favorites sidebar**
  (Home/Desktop/Documents/Downloads/Pictures/Applications/Computer), an **Up button** + a path bar
  (right-truncated), the dirs-first directory list, and — in **Save** mode — a name field that opens
  pre-filled + select-all'd so the first keystroke replaces the suggestion. Added LAST to a Window
  (like `ConfirmDialog`), it grabs focus, forwards keys to the embedded name field (a new
  `TextField::force_focus` keeps its caret blinking while the modal owns focus), and swallows stray
  clicks. **Open** picks an existing file (the OK button greys until a file is selected; activating a
  folder navigates); **Save** browses + names, greys OK when the folder isn't user-writable
  (`tos_may_write` via `kernel/fs/perm.h`), and raises a nested **Replace / Cancel**
  `ui::ConfirmDialog` on an overwrite. `on_pick(ctx, path)` returns the chosen absolute path (or
  nullptr on Cancel); markers `[filedialog] open …/cd …/pick …/cancel`. **Notepad** wired it up:
  File > Open… (`^O`) loads a note, File > Save As… writes one (File menu is now New/Open/Save/Save
  As). e2e `t_file_dialog` (Save As → type a name → Enter → read back off disk); screenshot-verified
  (both Open + Save chrome). BIOS 29/29 + UEFI 11/11 + 62 unit.
- **Notepad default save location = Documents (2026-06-03).** A bare note name now resolves to
  `/Users/user/Documents/<name>` instead of the home root, so saved notes stop littering `$HOME`;
  `resolve_path` `mkdir`s `Documents` defensively (init already seeds it). Absolute paths are
  untouched. The three e2e notepad checks (`t_notepad_edit_save`, `t_notepad_undo`,
  `t_notepad_guard`) follow the path to `/Users/user/Documents/untitled.txt` and read it back as
  `cat Documents/untitled.txt`. When the file picker (#4) lands, this becomes its default folder.
  BIOS 28/28 + UEFI 11/11 + 62 unit (no test count change — paths shifted, not added).
- **Notepad unsaved-changes guard + reusable ConfirmDialog #5 (2026-06-03).** New (and the
  compositor's Close button) on a dirty buffer no longer silently nukes it. New toolkit widget
  `ui::ConfirmDialog`: a modal sheet (dim scrim + centred card + up to 3 buttons, the primary
  accent-filled on the right) that grabs keyboard focus while open (Enter = primary, swallows the
  rest) and captures every click (added last to the window, rect = whole window, like Files' Popup);
  `on_choice(ctx, idx)` returns the button index. Window gained an `on_close()` veto hook (return
  false to keep the window open). Notepad tracks a `dirty` flag via `editor.on_change`, splits
  `new_note()` into a guarded entry + `do_new()`, and defers New/Quit until the **Save / Discard /
  Cancel** answer (Save writes first). The dialog reports its button centres (`[ui] dlgbtn i x y`,
  client-relative) so tests can click them. e2e `t_notepad_guard` exercises the Discard (click) and
  Save (Enter) paths; screenshot-verified. The reusable dialog also unblocks the file dialog's
  overwrite warning (#4). BIOS 28/28 + UEFI 11/11 + 62 unit.
- **Terminal + Files ported onto the app-menu API #6 (2026-06-03).** Two more apps now carry real
  menu bars. **Files** (a `ui::Window`) declares File [New Folder ^N, Refresh] / Edit [Copy ^C, Cut
  ^X, Paste ^V, Delete] / Go [Up, Back, Forward] via `menu_begin/menu_add/menu_item/menu_commit`,
  routed in `on_menu` to the same actions the toolbar + right-click already run; the ^C/^X/^V/^N
  accelerators are intercepted by the compositor and arrive as `WEV_MENU` picks (`[files] menu m i`
  trace). **Terminal** — a raw-syscall app, *not* the toolkit — builds a `struct winmenu` by hand
  and calls `win_setmenu(win, &m)`, then handles `WEV_MENU` in its event loop: Edit [Copy, Paste,
  Clear] with **no** Ctrl accelerators on purpose, so a plain ^C still reaches the shell as an
  interrupt (`[term] menu i` trace). Proves the menu protocol is app-agnostic, not toolkit-only.
  e2e `t_files_menu` (Ctrl+N → New Folder lands on disk) + `t_term_menu` (click Edit > Clear →
  WEV_MENU); both menu bars screenshot-verified. BIOS 27/27 + UEFI 11/11 + 62 unit. Still left
  under #6: submenus (a `struct winmenu` ABI bump, deferred until a use appears).
- **TextField undo/redo — the global text contract (2026-06-03).** Every toolkit text field now
  inherits Ctrl+Z / Ctrl+Y. `TextField` carries two bounded ring stacks of insert/delete span
  records (`{op, pos, span text, caret-before}`); `ins`/`del_range` record each mutation, `undo`/
  `redo` pop one stack and apply the inverse (which re-records onto the other, so the chain is fully
  reversible) and restore the caret. A run of single-char typing or backspacing **coalesces** into
  one step (one Ctrl+Z drops the whole word), broken by a newline or a caret jump/click; a fresh
  edit clears the redo stack; `set_text` resets the history. The subtle merge rule is factored into
  a pure `user/lib/editlog.h` (`el_coalesce_kind`) shared by the widget and the new host unit test
  `t_editlog` (16 checks). Notepad's **Edit > Undo** (was declared-but-disabled) is enabled with
  accelerator `^Z` and a **Redo `^Y`** item added — the compositor routes the chords as menu picks
  for the focused window, and the same raw `^Z`/`^Y` bytes drive undo/redo in any non-menu app
  (Spotlight, Files name fields). e2e `t_notepad_undo` (type → Ctrl+Z → 0-byte save → Ctrl+Y →
  8-byte save → read back). BIOS 25/25 + UEFI 11/11 + 62 unit; screenshot-verified (Edit dropdown
  shows enabled Undo ^Z / Redo ^Y).
- **User-program heap — confirmed already done (2026-06-03).** The "a program is its static image
  + stack" note was stale: `user/lib/libc.c` already ships a full growable heap over `SYS_MMAP`
  (`malloc/free/realloc/calloc`, an address-sorted free list with first-fit + split + boundary
  coalescing, arena grown in ≥1 MiB mmap chunks) — `operator new`/`delete` (crt.cpp) sit on it, and
  twm/Files/ui all allocate through it. An mmap-backed heap supersedes a `sbrk`; the stale "smaller
  idea" bullet was removed. (Making the terminal scrollback ring runtime-sized still sits under
  *Terminal scrollback*.)
- **LAPIC timer calibrated against the PIT (2026-06-03).** The AP preemption timer was a magic
  QEMU-tuned count (`1000000`, ~62.5 Hz). `lapic_timer_calibrate(hz)` (apic.c) now measures the
  local timer's real rate over a PIT-channel-2 one-shot window (gated + polled via port 0x61 bit5,
  so no IRQs — it runs at boot with interrupts still off) and returns the divide-by-16 count for a
  defined rate; `smp_init` calls it once on the BSP (`LAPIC_PREEMPT_HZ` = 100, matching the BSP's
  PIT tick) and the APs reuse the result. Implausible readings / a watchdog timeout fall back to
  the old fixed count, so the worst case is unchanged. Measured count 626723 on QEMU (≈625000
  expected: 1 GHz APIC ÷16 ÷100) — `[smp] lapic timer calibrated: count N (~100 hz preempt)`.
- **Live resize + reflow — verified done (2026-06-03).** twm already streams `WEV_RESIZE` to the
  app as the grip is dragged (throttled to >7px) and sends the exact size on release (twm.c live-
  resize block); the terminal recomputes its grid via `setup_surface` and the toolkit's multiline
  `TextField` re-wraps from the current width on each redraw, so content reflows **live**, not on
  mouse-up. Screenshot-verified (notepad text re-wrapped as the window narrowed). A separate XOR
  "ghost outline" is unnecessary now that the content itself resizes live; the stale open bullet
  was removed.
- **App-menu accelerators + checkmarks + disabled items #6 (2026-06-03).** `struct winmenu`
  items gained a `flags` byte (`WMI_DISABLED`/`WMI_CHECKED`) and an `accel` letter. `ui::Window`'s
  `menu_item(label, accel=0, flags=0)` plus `menu_set_checked/_set_enabled/_is_checked` declare and
  live-toggle them. twm's dropdown greys disabled rows (no hover, ignored clicks), draws a leading
  ✓ for checked rows (a two-stroke `draw_check`, no line primitive), and right-aligns the `^X`
  accelerator hint; the key loop intercepts `Ctrl+<letter>` for the focused window and fires the
  matching enabled item as a `WEV_MENU` (opt-in per declared menu — menuless apps keep raw chords;
  Backspace/Tab/Enter/Esc arrive without Ctrl so never match). `menu_sig` folds in flags+accel so a
  runtime toggle re-publishes. Notepad now ships File [New ^N, Save ^S] / Edit [Select All ^A, Undo
  disabled] / View [✓ Status Bar]. `[twm] accel <L> <m> <i>` trace; `t_app_menu` extended with the
  Ctrl+N accelerator path. Build + unit (46) + screenshot-verified.
- **App menus #6 (2026-06-02).** App→WM menu protocol: `struct winmenu` (≤5 menus × ≤8 items) set
  via `SYS_WIN_SETMENU`, read by the compositor via `SYS_WM_GETMENU`, with `WEV_MENU` delivering a
  pick back to the app. `ui::Window` gained `menu_begin/menu_add/menu_item/menu_commit` + an
  `on_menu(menu,item)` hook; twm fetches the focused window's menu each frame, draws a tile per
  top-level menu after the app name (a kind-3 dropdown), and posts `WEV_MENU` on a pick. Notepad
  declares File [New, Save] / Edit [Select All]. `[twm] appmenu`/`menu app` traces; `t_app_menu`.
- **Maximize hides both bars + hover-reveal (2026-06-02).** Fullscreen (green button /
  double-click title / new **Super+F**) now makes the client fill the **whole** screen (`W×H`,
  was `W×(H−TH)`); the window's own title bar becomes a sliding overlay that hides **with** the
  menu bar as one "top group". A top-edge hover reveals both together and HOLDS while the cursor
  stays in the revealed band (so you can reach the traffic lights); diving into the content
  retracts them. Centralized window geometry in `is_fs`/`client_rect`/`outer_rect`/`in_client`
  helpers + `fs_titlebar_y`; rewired every compositor input path. `[twm] fullscreen <t> 0|1` and
  `[twm] topbar shown|hidden` traces; `t_fullscreen`. 34/34 + 46 unit.
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
