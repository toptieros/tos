# tOS — changelog

What has **landed**, plus the history of resolved issues. What's *left* is in
[NEXT_STEPS.md](NEXT_STEPS.md); how the system works today is in [PROJECT.md](PROJECT.md).

## Landed work (newest first)

Terse one-liners; the full prose lives in git history and the design/ docs.

- **Terminal configurable scrollback ring size (2026-06-11).** The scrollback ring is now
  **heap-allocated at startup and sized from `term.scrollback`** in the registry (default 256,
  clamped up to at least one screenful), replacing the compiled-in 256-row static array — so the
  history depth scales with the setting instead of hitting a fixed wall. New seed key in
  `fs/etc/registry`; `sbch/sbfg/sbbg` became heap pointers + a runtime `sb_rows`; `[term]
  scrollback <n> rows` canary at startup. Disposable boot confirmed a non-default seed (1000)
  flows through.
- **Files lock badges + greyed actions on system-owned items #1 (2026-06-11).** Finishes the
  System-ownership UI: a gold **padlock badge** now sits at the lower-right of every
  system-owned item's icon in the list / details / icon / split views, and the context menu
  **greys Cut / Rename / Delete** on those items (a new `disabled` flag on the `Popup` rows).
  The keyboard/toolbar paths (`do_delete` / `copy_sel(cut)` / `start_rename`) short-circuit with
  a status-bar **deny-flash** (`[files] denied <name>`) instead of a silently-refused syscall.
  Owner now rides on `struct dirent` (filled by `readdir` from the tosfs entry), so the badge
  reads it **without a per-row `SYS_STAT`**; the Get Info pane's "Read only" badge was already
  there. New `G_LOCK` glyph + cached caller uid. Disposable boot + screenshots (`/Apps` bundles
  badged; the greyed Rename/Delete menu).
- **Filter/search bar click-away dismiss §5 (2026-06-11).** Clicking outside an open-but-empty
  filter/search bar dismisses it, like Esc — routed click-first so the click lands in the
  pre-close layout, then the chosen row is re-selected (close's `apply_filter` resets `sel`).
  A bar holding a live filter, or search results, is untouched by clicks; Esc remains the way
  out. Disposable boot `tests/repro_filterdismiss.py` + screenshots.
- **Background jobs + conflict prompts §12 and recursive search §5 (2026-06-11).** Cross-pane
  copy/move now runs as a **chunked job on the window tick** (`Window::on_tick` → 4 items/tick
  over a pre-collected tree list): the status bar shows "Copying k of n..." plus a 2px accent
  **permille band**, Esc cancels, and a colliding destination raises **Replace / Keep Both /
  Skip** (the shared `ConfirmDialog`; `[files] job conflict/start/done/skip/cancel`) before
  anything copies — moves carry tags and journal OP_MOVE for undo. **File ▸ Find** (^F) arms
  the **filter bar — which existed but was never wired or even add()ed to the window** — and
  Enter walks the tree from the current folder as the same kind of job (2 dirs/tick, dotfiles +
  `.app` skipped), streaming hits into the view with "in <dir>" status and double-click
  jump-to-hit (`[files] search start/done/open/close`). En route: the **`crumbend` e2e canary
  clamped inside the breadcrumb bar** (a long path pushed the click target past the bar's edge
  → dead clicks) and **`listrect` re-emitted when the filter bar opens/closes** (it shifts the
  list 29px). Verified: units + one disposable boot (`tests/repro_jobs.py`) + 4 screenshots
  (conflict card, frozen mid-copy band, results, jump); smoke 13/13.
- **Gallery view §1 (2026-06-10).** View ▸ as Gallery (item 7, appended; `[files] view
  gallery`): a full-size decoded preview (cached one image at a time) over a filmstrip of §11
  thumbnails — wheel pans, ←/→/Enter + double-click work, round-trips through §2 view memory;
  split view forces list. Disposable boot `tests/repro_gallery.py` + screenshots.
- **Thumbnails + Quick Look §11; lone-Esc toolkit fix (2026-06-10).** Real 96px previews for
  `.argb` images (pure `thumb.h` fit + box-average scale, unit `t_thumb`; eager per-folder RAM
  cache) in list rows, icon tiles and the gallery; **Space** toggles a Quick Look scrim+card
  (image fitted / text head / icon+summary), any click or Esc dismisses. Found en route: the
  toolkit **never delivered a bare Esc keypress** (the ANSI escape-sequence latch ate it);
  `ui::Window::run` now flushes a lone Esc after two idle drains as `UK_ESC` — fixes every
  in-app Esc binding OS-wide. Disposable boot `tests/repro_quicklook.py` + screenshots.
- **Tags / labels §10 + a scrolling sidebar (2026-06-10).** Finder-style colored tags on a
  `~/.tags` sidecar (no fs xattrs): pure codec `tagstore.h` (get/set/move; unit `t_tagstore`),
  carried across rename / move-to-trash / Put Back. Context-menu **"Tags..."** opens a
  stay-open picker (Popup grew per-item `checked` + colour `dot` + an `on_toggle` callback;
  Open With's shared toggle untouched) that writes through per flip (`[files] tags <path>
  <mask>`); details rows draw the dots overlapped, right-aligned in the Name column; the
  sidebar's **Tags section** (collapsed by default) filters the listing per color, click
  again to clear (`[files] tagfilter <name|off> <n>`, status "N of M shown"). Found via the
  verification boot: the expanded sections **overflowed past the pinned Trash row** and
  misrouted clicks to Trash — the sidebar now wheel-scrolls by whole rows, clips at the
  Trash divider, and `side_dump` reports only on-screen rows. Verified the NEW way: units +
  one disposable boot (`tests/repro_tags.py`) + screenshots; smoke tier 13/13, no new e2e.
- **Test-suite restructure: smoke tier + in-OS selftest (2026-06-10).** The suite had
  crept back to "boot full QEMU for every little thing" (56 boots per `make test`, re-run
  per increment). Now: **`make test` = a smoke tier** of 13 deliberate journeys
  (`SMOKE_BIOS`/`SMOKE_UEFI_LIST`); the full catalog moved behind **`make test-all`**
  (`--all`) as the release / cross-cutting-change gate. New **`selftest`** userspace
  program (`/System/bin/selftest`, shell command) runs **46 native fs / registry /
  fork-wait / statfs / clipboard assertions in one boot** — replacing a boot-per-area in
  the smoke tier — and prints `selftest: N/N OK` (failures name the expression). Policy
  sharpened in design/testing.md: features verify with **units + one disposable ad-hoc
  boot + screenshot**; new permanent e2e only for a critical journey or a pinned bug, and
  new in-OS checks go into `selftest` first.
- **Cursor hints + the press-vs-hover compositor bug; folder sizes + column resize (2026-06-10).**
  Root-caused a real twm input bug: on the press frame the hover block posted the hover-leave
  packet (0xfff,0xfff,0), which the toolkit reads as a **button-up** — so a widget grab (the
  header divider) was cancelled before the first `WEV_MOUSE_DRAG` arrived. Fix: hover posting is
  frozen while a button is held, and the release edge posts an explicit btn-0 packet to the
  `cdrag` owner (the leave/enter pair had been, by accident, the app's only up signal). Probe
  kept: `tests/repro_hdr_drag.py`. On top, **global cursor hints**: `SYS_WIN_SETCURSOR` (70) +
  per-window snapshot plumbing; twm shows the hint over that client (held live mid-drag); the
  toolkit's `Widget::cursor`/`cursor_at(x,y)` relays the hot widget's shape — every TextField
  OS-wide shows an I-beam (term's hardcoded title hack removed), the header divider shows **⇔**
  (the resize affordance; the hover highlight was dropped per user preference). And in Files:
  `load_dir` fills directory sizes via the recursive `dir_usage` walk (real Size cells + size
  sort for folders/.apps), and **View ▸ Info** (item 6, ^I) toggles the inspector. e2e
  `t_files_details` extended (divider drag widens Name ≥40 px; size-desc ranks a stuffed folder
  on top; screenshot with the ⇔ cursor).
- **Editable Places sidebar §7 (2026-06-10).** The 8 hardcoded sidebar rows became a sectioned
  list: **Favorites** (registry-backed editable pins — pure `places.h` codec, unit `t_places`;
  context-menu **Add to / Remove from Places**) + **Locations** (the volume row carries a statfs
  used-space bar), collapsible section headers, Trash pinned at the bottom. Shared `vrows`/`vrow`
  row model so draw + hit-test agree; rows dumped for e2e (`[files] siderow`, deduped). e2e
  `t_files_places` (pin → navigates → collapse/expand → survives in the on-disk registry →
  remove; screenshot). Still open: drag-reorder + pin rename (want DnD / inline-field plumbing).

- **Files undo / redo of file ops §12 (2026-06-10).** Edit ▸ Undo / Redo (Ctrl+Z/Y) invert the
  last file op via a pure journal (`undojournal.h`, unit `t_undojournal`; cap 24, a push
  truncates the redo tail) interpreting RENAME / MOVE / CREATE / COPY / TRASH with the existing
  fs helpers; the menu items grey via can-undo/can-redo. Menu caps `WINMENU_ITEMS`/`MENU_MAXI`
  8 → 12. e2e `t_files_undo`; canaries `[files] undo|redo <type> <path>`. Also fixed
  `_files_menu_open` clicking another app's same-named menu tile (take the *last* `[twm] appmenu`
  match, not the first).
- **Files details / column view §1 (2026-06-09).** List mode is a real details view: a
  Name | Kind | Size | Date Modified header — click sorts (▲/▼ caret, re-click flips), dividers
  drag-resize (Date fills the rest), widths remembered per folder; new `FSORT_DATE` off
  `dirent.mtime`. Pure width math `colfit.h` (unit `t_colfit`); `filesort.h`/`viewmem.h` extended
  compatibly. e2e `t_files_details`. Left: a Sort-menu Date item, grouping, column add/remove.
- **Files split / dual pane §4 (2026-06-09).** View ▸ Split View: a second lean pane (its own
  path/listing/selection), splitter + active-pane accent, **Copy / Move to Other Pane** (Edit +
  context menus, split-only) via `copy_tree` + dedupe. e2e `t_files_split`; canaries
  `[files] split / pane2 cd / copy-across / listrect2`. The shared `_files_nav` e2e helper now
  retries the whole open→type→Enter as a unit (was the top flake in nav-heavy runs).
- **Files tabs §4 (2026-06-09).** A tab strip of folder pills (each tab keeps its own folder +
  history + selection; hidden with one tab): New Tab ^T / the strip's + / Open in New Tab
  (context menu); the pill's × or ^W closes; relabels on navigate; growable heap store. `TabStrip`
  widget + pure `tabtitle.h` (unit `t_tabtitle`); e2e `t_files_tabs`; canaries
  `[files] tab new|sel|close` + `tabbar/tabpos`.
- **Files rich Get Info / Properties §8 (2026-06-09).** Selecting fills the Details pane with a
  folder's recursive size + item count (the `du`-style `dir_usage` walk), the Owner (System /
  You), a gold "Read only" lock badge (the `tos_may_write` rule), and Opens-with; Ctrl+I toggles
  the pane. Pure `fileinfo.h` (unit `t_fileinfo`); e2e `t_files_getinfo` (screenshots: a user
  folder + locked /Apps); canary `[files] sel <name> (ro|rw) owner=<uid> size=<n> [items=<n>]`.
- **statfs / free-space §6/§7 (2026-06-08).** New `SYS_STATFS` (69) off the tosfs sector bitmap;
  surfaced as the shell's `df` and a "<n> free" Details-pane footer. Pure formatter `humansize.h`
  (unit `t_humansize`); e2e `t_statfs`.
- **tosfs file timestamps (mtime) §8 (2026-06-08).** Every entry carries a packed FAT-style
  mtime (`kernel/fstime.h`, unit `t_fstime`), stamped from the CMOS RTC on create/write/mkdir
  (+ build time via mkfs), plumbed through `dirent`/`fstat` to the Details pane's Modified line.
- **Files Duplicate + New File §12 + tosfs 0-byte files (2026-06-08).** Duplicate clones
  Finder-style "X copy" (folders recursively via the new `copy_tree`); New File drops an empty
  `newfile.txt` and enters rename — which needed tosfs to persist real **0-byte entries**
  (`close_l` used to discard empty writes). Pure `dupname.h` (unit `t_dupname`); e2e
  `t_files_newdup`.
- **Userspace de-clutter — the big single files split into modules (2026-06-08).** The Makefile
  now compiles every `.c`/`.cpp` in an app's dir; twm (2325 lines) became `twm.c` + `twm.h` +
  `bar.c`/`dock.c`/`controlcenter.c`/`notify.c`/`switcher.c`/`menubar.c`; Files split out
  `fileswidgets.h` + `filesutil.{h,cpp}`; `TextField` moved to `ui_textfield.cpp`. No behaviour
  change (suites green).
- **Files Trash §9 — move / Put Back / Empty (2026-06-08).** Delete moves to `~/.Trash`
  (`rename_`, whole dirs too) with a `.trashinfo` origin sidecar (pure `trashinfo.h`, unit
  `t_trashinfo`); Put Back restores to the origin, Empty Trash / delete-inside-Trash remove for
  good; dotfiles hidden from listings + counts. e2e `t_files_trash`; the reusable
  `[files] listrect`/`ctxmenu` canaries + the `rightclick` harness helper landed here.
- **Exit-fullscreen no longer leaves black areas (2026-06-06).** `WEV_RESIZE` only set partial
  damage, so any same-drain hover made `redraw()` partial-paint the freshly resized surface; the
  resize handler now `invalidate()`s the whole surface (fixes every toolkit app's restore).
- **Picker hardening + modality (2026-06-06).** Pid-namespaced `/tmp/.picker-<pid>.req/.res`
  (new `SYS_GETPID`/`SYS_GETPPID` 67/68; naming centralised in `sys.c`) so concurrent pickers
  can't clobber each other; and `WIN_MODAL` — twm keeps the picker topmost + focused, dims
  everything behind a full-screen scrim, and swallows input outside it (no `wininfo.parent`
  ABI change needed). e2e `t_file_picker` + `t_launchers_exclusive`/`t_alt_tab` green.
- **Files per-folder view memory §2 (2026-06-06).** View mode + sort + zoom per folder in the
  registry (`view.<path>`, hashed past `REG_KEYMAX`; a stable `view.default` fallback); the
  on-navigate menu re-sync batched into one `win_setmenu` (the per-item republish raced menu
  clicks). Pure codec `viewmem.h` (unit `t_viewmem`); e2e `t_files_viewmem`.
- **Files in-place rename + path-bar/rename click-away (2026-06-06).** Inline rename over the selected
  tile in list or icon view (New Folder enters it Finder-style); Enter/click-away commit, Esc cancels.
  Clicking outside the editable path bar now reverts it like Esc. `dispatch_mouse` made virtual so
  `FilesApp` can hook click-away. e2e `t_files_rename` + extended `t_files_breadcrumb`.
- **Incremental directory flush — kills the notepad-autosave freeze (2026-06-06).** The real
  culprit behind "saving freezes the desktop": `flush_super()` rewrote the entire **189 KB**
  tosfs directory table through polled PIO on every file close (several times per autosave).
  New `flush_super_ent(slot)` writes only the 1–2 sectors holding the changed entry (~190× less
  metadata I/O), byte-identical on disk. Large *data* writes stay synchronous — see Known issues.

- **Preemptible syscalls — long disk ops no longer freeze the machine (2026-06-05).** Every
  syscall ran IF=0 (interrupt gate, no `sti`) with `irqsave` fs/ata locks, so a slow polled-PIO
  transfer starved the timer and froze the single-core desktop (the "typing in notepad freezes
  the OS" report). Fix: `sti` in the 0x80 dispatch; a new IF-preserving `spin_lock_preempt` for
  `fs_lock`/`ata_lock`; all disk I/O moved out of `sched_lock` (also fixing a multi-ms app-launch
  freeze and a 1-CPU deadlock); a `SYS_READ` lost-wakeup made check+block interrupt-atomic.
  Caveat: a write finishing inside one 10 ms tick still blocks — the full cure is async/DMA +
  a write-back cache (Known issues).
- **Damage-rect presents + notepad save/close fixes (2026-06-03).** New `win_present_rect` (66):
  a per-window damage union carried in `struct wmwin`; twm composites only that sub-rect, and the
  toolkit invalidates per-widget (typing/caret/hover no longer re-blit the whole client). Also
  fixed quit dropping a dirty background tab; close/save UX reworked (window close never prompts —
  the autosave draft holds everything; closing a *tab* guards); autosave debounced.
- **Notepad tabs + session autosave #5 (2026-06-03).** The tabbed editor + a new toolkit
  `Window::on_tick()` hook driving the session autosave to `~/.cache/notepad/`; a bare relaunch
  rebuilds the whole session, even never-saved notes (two-boot e2e `t_notepad_session`); the
  unsaved guard moved to tab/window close (`t_notepad_guard`).
- **Reusable file picker `ui::FileDialog` #4 (2026-06-03).** The in-process toolkit Open/Save
  modal (Favorites sidebar, Up + path bar, Save name field, ownership-greyed OK,
  Replace/Keep-Both/Cancel overwrite). *(Retired + deleted 2026-06-06 — replaced by
  Files-as-picker; the entry stays for the history.)*
- **Notepad default save location = Documents (2026-06-03).** Bare note names resolve to
  `~/Documents/<name>` instead of littering `$HOME`.
- **Notepad unsaved-changes guard + reusable `ui::ConfirmDialog` #5 (2026-06-03).** A modal
  sheet (dim scrim + up to 3 buttons, Enter = primary, swallows outside clicks) + a
  `Window::on_close()` veto hook; Notepad defers New/Quit on a dirty buffer until
  Save / Discard / Cancel answers. e2e `t_notepad_guard`; `[ui] dlgbtn` canary.
- **Terminal + Files ported onto the app-menu API #6 (2026-06-03).** Files declares File/Edit/Go
  via the toolkit; Terminal builds `struct winmenu` by hand (a raw-syscall app — proving the
  protocol isn't toolkit-only) with no Ctrl accelerators so ^C still interrupts the shell.
  e2e `t_files_menu` + `t_term_menu`.
- **TextField undo/redo — the global text contract (2026-06-03).** Bounded ring stacks of
  insert/delete span records with typing-run coalescing (pure `editlog.h`, unit `t_editlog`);
  every toolkit field inherits Ctrl+Z/Y; Notepad's Edit ▸ Undo enabled + Redo ^Y added.
  e2e `t_notepad_undo`.
- **User-program heap — confirmed already done (2026-06-03).** `user/lib/libc.c` already ships a
  full mmap-backed `malloc/free/realloc` (first-fit free list, coalescing, ≥1 MiB arenas); the
  stale "needs sbrk" bullet was removed.
- **LAPIC timer calibrated against the PIT (2026-06-03).** `lapic_timer_calibrate(hz)` measures
  the local timer over a gated PIT channel-2 one-shot at boot (no IRQs needed), replacing the
  magic QEMU-tuned count; implausible readings fall back to the old constant.
- **Live resize + reflow — verified already done (2026-06-03).** twm already streams `WEV_RESIZE`
  mid-drag and term/toolkit apps reflow live; the stale open bullet was removed.
- **App-menu accelerators + checkmarks + disabled items #6 (2026-06-03).** `winmenu` items gained
  `flags` (`WMI_DISABLED`/`WMI_CHECKED`) + an accel letter; twm greys/✓-marks rows, right-aligns
  `^X`, and fires `Ctrl+<letter>` as the matching `WEV_MENU` (opt-in per declared menu).
  `t_app_menu` extended.
- **App menus #6 (2026-06-02).** The app→WM menu protocol: `struct winmenu` via
  `SYS_WIN_SETMENU`/`SYS_WM_GETMENU`, `WEV_MENU` picks, the `ui::Window` menu API + `on_menu()`;
  twm draws a dropdown tile per menu. `t_app_menu`.
- **Maximize hides both bars + hover-reveal (2026-06-02).** Fullscreen fills the whole screen;
  the title bar + menu bar hide as one top group, revealed (and held) by a top-edge hover.
  Window geometry centralised in `is_fs`/`client_rect`/`outer_rect`/`in_client`. `t_fullscreen`.
- **System ownership #1 (2026-06-02).** tosfs v3: a per-entry `owner` uid; tasks carry a `uid`
  (init = system; the desktop session drops to user via `SYS_SETUID`); the mutating fs syscalls
  enforce `tos_may_write` (unit `t_perm`, e2e `t_system_ownership`); the shell prints
  `permission denied (system file)` and gains an `id` builtin.
- **Notification click routing (2026-06-02).** `notify_to(title, body, target)`: clicking a toast
  or a notification-center row focuses (or launches) the target app. `t_notif_click_routing`.
- **Dock pinned | running divider (2026-06-02).** `rebuild_dock()` records the boundary index
  (`dock_runsep`) after the last pinned tile; `draw_dock()` draws a faint 1px vertical separator
  in the gap before the first running-unpinned tile, shown only when one exists. `[twm] docksep`
  trace; asserted in `t_notepad_edit_save`.
- **Launchers mutually exclusive (2026-06-02).** `dismiss_launchers(except)` at every summon
  path; focus telemetry tracks the window *id* across slot reuse. `t_launchers_exclusive`.
- **UEFI boot fixed above 4 GiB (2026-06-02).** The loader identity-mapped only 0–4 GiB, so
  `MEM=8G` #PF'd on the CR3 switch; it now maps all of RAM from `GetMemoryMap`. `t_ram_scales`
  gained a 6G case.
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

## Issue history (resolved / not blocking)

- **BIOS real-mode load envelope `#UD` — FIXED (2026-05-29).** The chunked disk-read loop ran
  `mov ah,0x42` before `push ax`, baking `0x42` into the sectors-remaining counter; one-line
  ordering fix in `boot/stage1.asm`. (Debug via `-d int,cpu_reset` + `pmemsave` dumps; gdb breaks
  in the boot sector are unreliable — the loop self-modifies the DAP page.)
- **Flaky UEFI tests under host load — hardened (2026-05-29).** Tight inject schedules can outrun
  OVMF+TCG; the harness now retries (`t_mouse` re-injects, `line_for` retypes). Environmental.
- **tmpfs scratch leak — FIXED.** `Tos.stop()` removes the per-run scratch disk / OVMF-vars / serial
  log it created (a caller-supplied scratch is left alone).
- **Synchronous disk I/O can still freeze the desktop on *large* writes — MOSTLY FIXED for the
  everyday case (2026-06-06).** The headline symptom (notepad autosave freezes the cursor) is fixed:
  it was dominated not by the file data but by `flush_super()` rewriting the whole **189 KB** directory
  table on every close — now an incremental per-entry flush of ≤1 KB (see Done). What *remains* is the
  residual architecture: writes are still **synchronous polled PIO**, and a syscall is only preempted
  if it spans a **10 ms** timer tick, so writing a genuinely large file's *data* (tens of KB+) still
  busy-waits the single core for its duration (every transferred word is a VM-exit under KVM; raising
  `TIMER_HZ` barely helps — a 3 ms write still freezes ~3 ms at 1 kHz). For typical notes this is now
  imperceptible; the full cure for big writes is still, in order of payoff: **(a)** an **async / DMA**
  block driver (virtio-blk or AHCI+DMA — see the Phase-4 driver item) so a transfer is one descriptor,
  not thousands of `inb`/`outb`; **(b)** a **write-back buffer cache** that returns immediately and
  flushes on idle/sync, so the app never blocks on the platter; **(c)** make the writer not block the
  UI thread (background the flush). Cheaper palliatives already in place: autosave is debounced to a
  real ~1.3 s pause and only drafts. (More CPU cores would let *other* tasks run during a writer's
  blocking write, but won't stop the *writer* itself from hitching — only async I/O does that.)
