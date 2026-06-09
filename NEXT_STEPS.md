# tOS — next steps

How the system works **today** is in [PROJECT.md](PROJECT.md); this file tracks what's
**left** plus a terse log of what's landed. Every item keeps `make test` green (BIOS +
UEFI) before it's checked off.

**Status:** `make test` **46/46** (35 e2e journeys on BIOS + a UEFI subset 11) + **135 host
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
- [~] **Files app feature-completeness (files-app.md).** Turning Files from a single-pane lister into
  a real manager. **Done (2026-06-06):** **filter bar** + **status bar** (earlier); **location bar §3** —
  the static path label is now a clickable **breadcrumb** (segment chips → ancestor, chevron separators,
  middle-ellipsis on overflow) with an **editable path** mode (click the empty bar / Ctrl+L → field,
  Enter = go, Esc = revert); pure split `pathbar.h` (unit `t_pathbar`), e2e `t_files_breadcrumb`.
  **Data-driven sort §2** — a **View ▸ Sort by Name/Kind/Size + Reversed + Folders First** menu replacing
  the hardcoded comparator (natural/numeric name compare); pure `filesort.h` (unit `t_filesort`), e2e
  `t_files_sort` (under a dedicated **Sort** menu). **Icon (grid) view + zoom §1** — a new `IconGrid`
  renders the same model as a wrapping icon grid (large icon + centred name), toggled via **View ▸ as
  Icons / as List** with **Zoom In/Out/Actual Size**; selection is shared with the list; e2e
  `t_files_iconview`. **In-place rename §foundation** — Edit ▸ Rename / context-menu Rename drops an
  inline field over the selected tile (list *or* icon view) with the name pre-selected; **New Folder**
  enters rename immediately (Finder-style); Enter commits (`rename_()` on disk), Esc cancels,
  **click-away commits**; e2e `t_files_rename`, logs `[files] renaming … / rename <old> -> <new>`.
  **Path-bar click-away** — clicking outside the editable path field now reverts it the same as Esc
  (`[files] pathleave`), checked in `t_files_breadcrumb`. **Per-folder view memory §2** — each folder
  remembers its view mode + sort + zoom in the **registry** (one value per path, `view.<path>`, hashed
  when too long for `REG_KEYMAX`; a stable `view.default` covers unseen folders); restored on navigate,
  persisted on every view/sort/zoom change; the menu-check re-sync was **batched into one `win_setmenu`**
  (was 7/nav, which raced menu clicks). Pure codec `viewmem.h` (unit `t_viewmem`), e2e `t_files_viewmem`,
  logs `[files] viewmem <path> <mode> zoom <z>`. **Trash §9** — Delete in a normal folder now **moves**
  the item to `~/.Trash` (a `rename_()`, so it works for whole directories) instead of destroying it; a
  hidden sidecar `~/.Trash/.trashinfo` records each item's origin so the context-menu **Put Back** restores
  it where it came from; **Empty Trash** (File menu + Trash context menu) removes for good; Delete *inside*
  the Trash deletes immediately. A **Trash** place anchors the sidebar; dotfiles (incl. `.Trash`/`.trashinfo`)
  are now hidden from listings and the status "N items" count. Pure sidecar codec `trashinfo.h` (unit
  `t_trashinfo`), e2e `t_files_trash` (full move → Put Back → Empty round-trip, confirmed on disk), logs
  `[files] trash <name> / untrash <name> / trash empty`; new geometry canaries `[files] listrect` +
  `[files] ctxmenu` (and a `rightclick` harness helper) let e2e drive list rows and context menus.
  **Duplicate + New File §12** — **Duplicate** (Edit ▸ / context menu) clones the selected item beside
  itself as Finder-style **"X copy" / "X copy 2"** — files copy their bytes, folders copy **recursively**
  (`copy_tree`); **New File** (File ▸ / context menu) drops an empty `newfile.txt` and enters rename like
  New Folder. Enabling empty files needed a **tosfs fix**: `close_l` now persists a real **0-byte entry**
  (start_lba 0 / size 0, like a directory) instead of discarding empty writes — so New File, truncate-to-
  empty, and copying an empty file all work. Pure name codec `dupname.h` (unit `t_dupname`), e2e
  `t_files_newdup` (New File + file Duplicate + recursive folder Duplicate, all confirmed on disk), logs
  `[files] duplicate <name>`. **fs timestamps §8** — every tosfs entry now carries a packed `mtime`
  (`fstime.h`, unit `t_fstime`), stamped from the CMOS RTC on create/write/mkdir (+ build time for
  shipped files via `mkfs`), plumbed through `dirent`/`fstat` to the Details pane's **Modified** line.
  **statfs §6/§7** — `SYS_STATFS` returns the tosfs volume's total/free bytes (sector bitmap); shown
  as the shell's `df` + a **"<n> free"** Details-pane footer; pure formatter `humansize.h` (unit
  `t_humansize`), e2e `t_statfs`. **rich Get Info §8** — selecting an item now fills the Details pane
  with a folder's **recursive size + item count** (a `du`-style `dir_usage` walk), the **Owner**
  (System / You from `fstat.owner`), a **"Read only" lock badge** for system-owned items, and **Opens
  with** (the default app for the type); folder size also lands on the status bar; **Ctrl+I** toggles
  the pane. Pure helpers `fileinfo.h` (owner label / lock rule / item-count, unit `t_fileinfo`), e2e
  `t_files_getinfo` (recursive folder size + a locked system item, screenshot-verified), canary
  `[files] sel <name> (ro|rw) owner=<uid> size=<n> [items=<n>]`. **Left, cheapest-first:**
  tabs/split §4, column/gallery views §1, search/thumbnails §11, tags/undo §10/§12.
  → [`files-app.md`](design/files-app.md)
- [~] **App menus (#6).** **Done:** the app→WM protocol — `SYS_WIN_SETMENU` (a `struct winmenu`
  of up to 5 menus × 8 items), `SYS_WM_GETMENU`, a `WEV_MENU` event, and the `ui::Window`
  `menu_begin/menu_add/menu_item/menu_commit` + `on_menu()` API; twm draws a bar tile per menu
  (kind-3 dropdown) and routes a pick back to the app. **Per-item state + accelerators:** items
  carry `WMI_DISABLED`/`WMI_CHECKED` flags and a Ctrl-accelerator letter — the dropdown greys
  disabled rows (no hover, non-clickable), draws a leading ✓ for checked rows, and shows the
  accelerator (e.g. `^S`) right-aligned; the compositor intercepts `Ctrl+<letter>` for the focused
  window and fires the matching enabled item as a `WEV_MENU` (opt-in per declared menu, so
  menuless apps keep their raw Ctrl chords). Runtime toggles via `menu_set_checked/_set_enabled`.
  Notepad ships File [New ^N, Open ^O, Save ^S, Save As, Close Tab ^W] / Edit [Select All ^A,
  Undo ^Z, Redo ^Y] / View [✓ Status Bar]; **Files** ships File [New Folder ^N, Refresh] /
  Edit [Copy ^C, Cut ^X, Paste ^V, Delete] /
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
  drive it; e2e `t_file_dialog`. The overwrite warning is **Replace / Keep Both / Cancel** — Keep
  Both dedupes to `name (N).ext` (`fd_dedup`) rather than clobbering. **Retired (2026-06-06):** the
  in-process modal looked *plain* (hand-drawn `fd_folder`/`fd_file` shapes, a re-implemented sidebar);
  it has been **deleted** and replaced by the **Files app launched as a picker process** (#11 below) so
  the dialog *is* Files (real `icons.h` icons, sidebar, breadcrumb, filter — like a Windows dialog
  resembles Explorer).
- [x] **File picker → Files-as-picker process (#11) — DONE (2026-06-06).** Retire `ui::FileDialog`; the system Open/Save
  picker becomes the **Files app** run in a *picker mode* with parameters, returning the chosen path
  to the caller. Mechanism extends the existing `/tmp/.open-doc` hand-off (`sys_open_with`): a request
  temp file in, a result temp file out, caller notices the picker exited via `trywait()`. Gets Files'
  whole design + feature set for free and can't visually drift from it. Full design (channels, SDK,
  picker-mode layout, modality, tests, risks) → [`file-picker.md`](design/file-picker.md). Phases,
  cheapest-first — **all (1)–(6) DONE (2026-06-06)**:
  - [x] **(1) SDK + codec.** `struct pick_req` + the key=value codec (`pickreq_encode`/`pickreq_parse` +
    the `ext`-filter `pickreq_ext_match`) live in pure header `user/lib/pickreq.h`; `sys_pick_begin` /
    `sys_pick_poll` (caller) + `sys_pick_req` (Files) in `user/lib/sys.{h,c}`; `/tmp/.picker-req`
    (key=value) in, `/tmp/.picker-res` (path or empty) out; begin unlinks stale res, poll wraps
    `trywait()`. Host unit test `tests/unit/t_pickreq.c` (encode/parse round-trip + ext predicate).
  - [x] **(2) Files picker mode.** Startup checks `sys_pick_req()` first; if set, a 560×420 dialog-shaped
    window + a picker footer (Name field on save, Cancel / Open·Save), extension filter (dirs always
    shown — `pickreq_ext_match`), ownership-greyed Save (`tos_may_write`+`getuid`), overwrite via the
    existing `ui::ConfirmDialog` (Replace / Keep Both / Cancel), New Folder kept / Delete·Open-With
    hidden, no menu bar. Writes the result + exits on pick/cancel/close. Logs `[files] picker …` /
    `[files] picked …` / `[files] pick cancel`. Open + save both done.
  - [x] **(3) Migrate notepad + tests.** `open_open()`/`save_as()` → `sys_pick_begin` (`start_pick`);
    `on_tick` polls `sys_pick_poll`; the embedded `ui::FileDialog` is gone. New e2e `t_file_picker`
    (save-with-rename + overwrite/Keep-Both in the picker window + open-mode round-trip);
    `t_notepad_edit_save`/`t_notepad_undo`/`t_notepad_guard`/`t_app_menu` rewired via `_accept_save_picker`
    to the `[files] picker/picked …` markers (read-back persistence checks kept).
  - [x] **(4) Delete `ui::FileDialog`** + its `fd_*` glyph/dedup helpers — done; one picker, no dead code.
  - [x] **(5) Modality polish (2026-06-06).** New `WIN_MODAL` flag: twm keeps the picker topmost +
    focused, draws a full-screen dim scrim behind it (reusing the Launchpad scrim path) and **swallows
    input outside it** (mouse clicks + the focus-stealing keys Alt-Tab / clipboard / Spotlight / Launchpad),
    so the windows behind are inert. Implemented **without** `wininfo.parent` — a system-wide scrim needs no
    kernel ABI/pid-mapping change and gives the same modal feel (everything behind dims, not just the
    caller). `ui::Window::modal` → flag; Files sets it in picker mode. e2e: `t_file_picker` now asserts a
    click on the window behind is swallowed; screenshot-verified (parent dimmed, dialog lit on top).
  - [x] **(6) Hardening (2026-06-06).** Temp files are now pid-namespaced — `/tmp/.picker-<pid>.req/.res`
    keyed by the *caller's* pid, so two apps can have a picker open at once without clobbering. Added
    `SYS_GETPID`/`SYS_GETPPID` (kernel `sched_getpid`/`sched_ppid`, ulib `getpid`/`getppid`): the caller
    names the files from `getpid()`, the picker (its fork+exec child) derives the same pid from `getppid()`.
    Path-building centralised in `sys.c` (`picker_path`) + a new `sys_pick_result()` so Files no longer
    hardcodes the result path. All picker e2e (`t_file_picker`/`t_notepad_*`/`t_app_menu`) green.
- [x] **Notepad redesign: tabs + session autosave (#5).** **DONE** — Notepad is now a tabbed editor.
  - [x] **Close UX (refined per use)** — closing the **window** never prompts: the session autosave
    already holds every tab + its unsaved contents, so `on_close` just flushes the latest draft and
    exits (a relaunch restores everything). Closing a **tab** (the tab's × or **File > Close Tab ^W**)
    is what asks about unsaved work — a dirty tab raises the modal **Save / Discard / Cancel** sheet
    (`ui::ConfirmDialog`); **Discard** drops it, **Save** on a named tab writes to its path then
    closes, and **Save** on a never-saved tab opens the picker to choose where (the tab closes once
    the pick succeeds). Explicitly closing the *last* tab clears the draft store so a relaunch starts
    fresh. e2e `t_notepad_guard`.
  - [x] **Tabs** — the top filename field is gone; each note is a tab in a `TabBar` strip (active
    highlighted, dirty shows a dot, each has a × to close) + a trailing **`+`** (also **File > New /
    ^N**); switch/close per tab; one shared `editor` swaps the active tab's text in/out (a `loading`
    flag stops a load from dirtying the tab). The active tab shows the note's name. *(The window
    title bar stays "Notepad" — there's no win-set-title syscall yet; the tab is the per-note name.)*
    *(Per-tab undo history isn't preserved across a switch — acceptable for v1.)* Screenshot-verified.
  - [x] **Session autosave** — a new `Window::on_tick()` hook drives a periodic draft (~1.8 s, only
    when the session changed) of every tab's text + the layout (open tabs, active one, per-tab
    name/dirty, the untitled counter) to `/Users/user/.cache/notepad/` (`session` + `tab<i>`). On a
    bare relaunch Notepad rebuilds the whole session — even never-saved notes. Two-boot e2e
    `t_notepad_session`.
  - [x] **Save / Open flow** — the reusable picker is wired as **File > Open…** (`^O`) / **File >
    Save As…**. **Save / ^S** writes a named note straight to its path; a **never-saved** note opens
    the picker (rooted at `~/Documents`) so you always choose where the first time — a draft sitting
    in the autosave store doesn't count as "saved". e2e `t_notepad_edit_save` (Save→picker→accept) +
    `t_notepad_undo` (first save→picker, later saves write direct). → [`ui.md`](design/ui.md)

### Global text-interaction contract
The toolkit owns the in-window text contract: anything in `TextField` is inherited by every
toolkit app for free. **Done:** blink caret, drag-select, Ctrl+A, double-click word-select,
Ctrl+←/→ word-jump, Ctrl+Backspace/Delete word-delete, Delete, shift-select, **undo/redo
(Ctrl+Z / Ctrl+Y)**. *(2026-06-06: fixed Ctrl+Backspace closing Notepad — the kernel collapsed it
to the bare ^W byte 0x17, which the compositor matched as Notepad's "Close Tab ^W" accelerator;
it now emits `ESC[127~`, forwarded to the app and decoded to word-delete. e2e `t_notepad_wordedit`.)*
**Left:**
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

- **Files rich Get Info / Properties §8 (2026-06-09).** Selecting an item now fills the **Details
  pane** with a real Get Info: a folder's **recursive size + item count** (a `du`-style `dir_usage`
  walk — heap-listed per level like `copy_tree`; the volume is tiny so it runs synchronously on
  selection), the **Owner** (System / You, from `fstat.owner`), a gold **"Read only" lock badge** for
  system-owned items (the `tos_may_write` rule), and **Opens with** (the type's default app from
  `open.default.<ext>`). The selected folder's size also shows on the status bar; **Ctrl+I** (and the
  toolbar Info button) toggle the pane. Pure helpers live in `user/lib/fileinfo.h` (owner label / lock
  rule / singular-plural item count — unit `t_fileinfo`, 14 checks); e2e `t_files_getinfo` stages a
  nested tree and a system folder, asserting the `[files] sel <name> (ro|rw) owner=<uid> size=<n>
  [items=<n>]` canary, screenshot-verified for both a user folder ("34 B, 3 items", Owner: You) and a
  locked one (/Apps "Read only", Owner: System).
- **statfs / free-space §6/§7 (2026-06-08).** New `SYS_STATFS` (69) fills a `struct statfs`
  (total/free/block bytes) from the tosfs sector bitmap. Surfaced two ways: the shell's **`df`**
  (Size/Used/Free) and a **"<n> free"** footer in the Files **Details pane** (the status bar's
  own bottom-right corner sits under the dock, so the always-visible Details column is the better
  home). 1024-based formatting is a pure header `humansize.h` (unit `t_humansize`, rounds tenths);
  e2e `t_statfs` drives `df` + a post-write re-check.
- **tosfs file timestamps (mtime) §8 (2026-06-08).** Every tosfs entry now carries a packed
  modification time — a FAT-style 32-bit `year:12 month:4 day:5 hour:5 min:6` (`kernel/fstime.h`,
  pure + unit `t_fstime`), stamped from the CMOS RTC (`rtc_now`) on file create/overwrite + mkdir,
  and on files shipped in the image by the host `mkfs` (build time). Plumbed through `struct
  tosfs_ent` → `struct dirent` (readdir) + `struct fstat` (stat) → the Files **Details pane**, which
  now shows a **Modified** line (screenshot-verified: the minute tracks the menu-bar clock). FS
  regression (struct grew): `t_fs_crud` / `t_fs_persist` / `t_many_files` / `t_files_*` all green.
- **Files Duplicate + New File §12 + tosfs 0-byte files (2026-06-08).** **Duplicate** (Edit ▸ Duplicate /
  Ctrl D / context menu) clones the selected item beside itself as Finder-style **"X copy"** — files copy
  their bytes, folders copy **recursively** (new `copy_tree`, heap-allocates each level's listing so deep
  trees don't blow the stack). **New File** (File ▸ New File / context menu) drops an empty `newfile.txt`
  and enters rename like New Folder. tosfs couldn't hold empty files — `close_l` discarded any 0-byte write —
  so it now writes a real **0-byte entry** (start_lba 0 / size 0, like a dir); New File / truncate-to-empty /
  copying an empty file all persist now. Pure name codec `dupname.h` (unit `t_dupname`, 12 checks), e2e
  `t_files_newdup` (New File + file Duplicate + recursive folder Duplicate, all confirmed on disk). Menu
  reindex: File = New Folder / **New File** / Refresh / Empty Trash; Edit gains **Duplicate**; the folder
  context menu gains Duplicate (shifted t_files_trash's Delete/Empty-Trash indices, updated to match).
- **Userspace de-clutter — split the big single files into modules (2026-06-08).** The Makefile now
  compiles + links **every `.c`/`.cpp` in an app's dir** (a generic `app_objs` wildcard rule), so an app
  can outgrow one file just by dropping another source in. **twm** (was 2325 lines) is now `twm.c` (core:
  shared state, helpers, desktop, window draw, compose, launch/focus, the event loop) plus a shared
  `twm.h` contract and the feature files **`bar.c`** (top menu bar + status glyphs), **`dock.c`** (dock +
  /Apps catalog), **`controlcenter.c`**, **`notify.c`** (toast + notification center), **`switcher.c`**
  (Alt-Tab), **`menubar.c`** (dropdowns). Panel hit-tests/animation the event loop used to inline now live
  with their feature (`menu_row_at`, `nc_clear_hit`, `toast_click`, `toast_tick`, `switcher_tick`). **Files**
  (was 1570) split its custom widgets into header `fileswidgets.h` and the path/icon helpers into
  `filesutil.{h,cpp}`, leaving `files.cpp` as `FilesApp` + `app_main`. **ui.cpp** (was 785) pulled the
  330-line `TextField` widget into `ui_textfield.cpp` (the shared `TF_BLINK` caret period moved to `ui.h`).
  No behaviour change: full unit suite + the e2e suite stay green (the 3 "did-not-launch" flakes pass in
  isolation, per the known host-load flakiness). Kernel stays single-file by design; `ugfx.c`/`term.c`/
  `shell.c`/`notepad.cpp` left as cohesive single units (notepad's `TabBar` is back-coupled to its app
  struct, so it doesn't split cleanly).
- **Files Trash §9 — move / Put Back / Empty (2026-06-08).** Delete in a normal folder now **moves** the
  item to `~/.Trash` via `rename_()` (works for whole directories too) instead of destroying it, recording
  its origin in a hidden `~/.Trash/.trashinfo` sidecar so the context-menu **Put Back** restores it where
  it came from; **Empty Trash** (File menu + Trash context menu) and Delete-inside-the-Trash remove for
  good. New **Trash** sidebar place; **dotfiles are now hidden** from listings (so `.Trash`/`.trashinfo`
  don't show) and excluded from the status "N items" count (new `nshown`). Pure sidecar codec
  `trashinfo.h` (host unit `t_trashinfo`, 24 checks); e2e `t_files_trash` drives the full move → Put Back →
  Empty round-trip and confirms each move on disk from the shell. Two reusable e2e canaries landed:
  `[files] listrect x y w rowh` (click a list row) and `[files] ctxmenu px py rowh n` (click a context-menu
  item, clamp-aware), plus a `rightclick` harness helper (twm already forwards the right button as a
  context request). Screenshot-verified (sidebar Trash, two trashed folders, Put Back/Delete Immediately/
  Empty Trash menu).
- **Exit-fullscreen no longer leaves black areas (2026-06-06).** Toggling a toolkit app out of
  fullscreen left most of the window black until you hovered around to repaint it piecemeal. Cause:
  the `WEV_RESIZE` handler in `ui::Window::run` only set `dirty`, not `dmg_full` — so if any other
  event in the same drain (e.g. a hover while the cursor crossed the shrinking window) set a *partial*
  damage rect, `redraw()` did a partial paint and the rest of the freshly-resized surface stayed
  stale/black. **Fix:** the resize handler now calls `invalidate()` (whole surface). Fixes every
  toolkit app's fullscreen restore, not just Files. Screenshot-verified (clean full render, cursor
  parked off-window).
- **Picker hardening — pid-namespaced temp files + `getpid`/`getppid` (2026-06-06).** The picker handoff
  files moved from the fixed `/tmp/.picker-req/.res` to `/tmp/.picker-<callerpid>.req/.res`, so concurrent
  pickers from different apps can't clobber each other. Added `SYS_GETPID`/`SYS_GETPPID` (67/68) with
  `sched_getpid`/`sched_ppid` in the kernel and `getpid`/`getppid` in ulib: the caller names the files from
  its own pid, and the picker — a `sys_launch` (fork+exec) child of the caller — derives the *same* pid via
  `getppid()`. Naming centralised in `sys.c` `picker_path()`; new `sys_pick_result()` retires the hardcoded
  path in Files' `finish_pick`. Completes the picker (#11) track. All picker e2e green.
- **Picker modality — `WIN_MODAL` (2026-06-06).** The Files Open/Save picker is now a real modal: a new
  `WIN_MODAL` window flag (kernel `syscall.h`, SDK `ui::Window::modal`, set by Files in picker mode) tells
  twm to keep it topmost + focused, paint a full-screen dim scrim behind it (the `modal_slot()`/`modal_on`
  path next to the Launchpad overlay), and **swallow input outside it** — mouse clicks on other windows/bar/
  dock and the focus-stealing keys (Alt-Tab, clipboard, Spotlight, Launchpad) are dropped so the dimmed
  windows are inert. Deliberately skipped `wininfo.parent`: a system-wide scrim needs no kernel ABI change
  and looks better (everything behind dims). e2e `t_file_picker` gained a click-behind-is-swallowed
  assertion; `t_launchers_exclusive`/`t_alt_tab` still green. Screenshot-verified.
- **Files per-folder view memory §2 (2026-06-06).** Each folder now remembers its view mode + sort +
  zoom across navigations, stored in the registry as one value per path (`view.<path>`, hashed past
  `REG_KEYMAX`), with a stable `view.default` for never-visited folders. Restored in `load_path()`,
  persisted in `set_view`/`set_sort`/`set_zoom`. The on-navigate menu-check re-sync was batched into a
  **single `win_setmenu`** (`sync_menus()` + `menu_check_local`) — the old per-item `menu_set_checked`
  re-published 7×/nav and raced menu-open clicks. Pure codec `viewmem.h` (unit `t_viewmem`, 22 checks),
  e2e `t_files_viewmem`, logs `[files] viewmem <path> <mode> zoom <z>`. Screenshot-verified (icon view
  restored, clean render).
- **Files in-place rename + path-bar/rename click-away (2026-06-06).** Inline rename over the selected
  tile in list or icon view (New Folder enters it Finder-style); Enter/click-away commit, Esc cancels.
  Clicking outside the editable path bar now reverts it like Esc. `dispatch_mouse` made virtual so
  `FilesApp` can hook click-away. e2e `t_files_rename` + extended `t_files_breadcrumb`.
- **Incremental directory flush — kills the notepad-autosave freeze (2026-06-06).** Found the real
  culprit behind "saving freezes the desktop": `flush_super()` rewrote the *entire* tosfs directory
  table on every file create/delete/rename/close. On today's 4096-sector disk that table is
  **378 sectors / 189 KB**, so each `close()` blasted 189 KB through polled PIO (≈97k VM-exits under
  KVM, ~15-20 ms on the single core) just to record one changed entry — and a single notepad autosave
  closes one draft file *per open tab* plus a session file, i.e. several × 189 KB per idle pause.
  This dwarfs the file data itself and is what the preemptible-syscall work (2026-06-05) couldn't
  cure: it made the write *preemptible* but not *smaller*. **Fix:** `flush_super_ent(slot)` writes
  only the 1-2 sectors that hold the one entry a mutating op changed (every op flushes a single slot
  immediately, so this is byte-identical on disk) — a save's metadata I/O drops ~190× (189 KB → ≤1 KB),
  making an autosave imperceptible. Pure kernel change in `fs.c`; build clean, unit 62/62, e2e 37/37
  (the 4 fails were pre-existing load flakes, all green re-run alone). Caveat: this does not make I/O
  *async*, so a very large file's *data* write still scales with its size — the async/DMA + write-back
  cache cure in Known issues remains the full fix for big writes.

- **Preemptible syscalls — long disk ops no longer freeze the machine; short writes still hitch (2026-06-05).** The
  "typing in notepad freezes the entire OS / the mouse locks up" report. Root cause (measured on
  KVM, where each 16-bit `rep insw/outsw` word is a VM-exit, so a 128-sector PIO read ≈ 20M cycles
  ≈ 5–7 ms): **every syscall ran with interrupts disabled** — the `int 0x80` gate is an *interrupt*
  gate and `isr_common` never `sti`'d — and `fs_lock`/`ata_lock` were `spin_lock_irqsave`, so a slow
  polled-PIO disk transfer held IF=0 for its whole duration. On a single core the timer then never
  fired, so the scheduler never ran the compositor and the cursor locked up for the length of the
  transfer (notepad autosaving on every typing pause = repeated freezes). **Fix:** (1) run syscalls
  **preemptibly** — `sti` in the `0x80` dispatch; the scheduler already parks a half-finished kernel
  frame per task (`tasks[].krsp` in `do_switch`), and every kernel lock is `irqsave`, so a timer
  preemption can never land inside a critical section. (2) New **`spin_lock_preempt` /
  `spin_unlock_preempt`** (`kernel/arch/spinlock.h`) — a mutual-exclusion lock that leaves IF
  untouched — for `fs_lock` + `ata_lock`, so the disk transfer itself stays preemptible. (3) **Moved
  all disk I/O out of `sched_lock`**: `sched_exit` flushes+closes files *before* the lock, and
  `sched_spawn`/`sched_exec` build the address space + read the ELF *before* the lock — this keeps
  those paths preemptible (also fixing a pre-existing multi-ms **app-launch freeze**) and avoids a
  single-CPU deadlock (an IF=0 spinner on a preempt-lock held by a preempted task). (4) Preemptible
  syscalls exposed one **lost-wakeup**: `SYS_READ` checked the input ring empty and then blocked as
  two steps, so a keystroke could arrive + wake between them and be lost — the check+block is now
  interrupt-atomic. (5) Notepad autosave debounce widened (~0.6 s → ~1.3 s idle). `make test`
  40–41/41 (the lone miss a known compositor toast-timing flake, green in isolation) + 62 unit.
  **What this does NOT fix (verified by the user — the cursor still freezes on save):** a syscall is
  only actually *preempted* if it spans a **10 ms** (100 Hz) timer tick. App launches / large reads
  (tens of ms) now do, so they no longer freeze — but a **short synchronous write that finishes
  inside one tick still blocks everything, cursor included**, for its duration, because the timer
  never fires mid-write. notepad's autosave is a handful of small writes (~a few ms) that mostly
  complete within a tick, so it **still freezes the cursor briefly on every save**. The fix is
  correct architecture (and removes a deadlock + the multi-ms launch freeze) but is the wrong layer
  for the autosave hitch — the real cure is **async/DMA disk + a write-back cache** so the UI task
  never blocks on the platter. See Known issues.
- **Damage-rect presents + notepad save/close fixes (2026-06-03).** Fixed a notepad lag regression
  (hovering the tab strip while typing pinned the loop at the frame cap, each frame re-blitting the
  whole client surface). New **`win_present_rect(id,x,y,w,h)`** syscall (#66): the kernel accumulates
  a per-window damage rect (union of partial presents, reset each compositor snapshot; full
  `win_present` ⇒ whole-surface), carried in `struct wmwin`, and twm composites **only** that
  sub-rect instead of the whole window. The toolkit tracks a damage rect per frame — `invalidate()`
  = whole, `invalidate(rect)` = union; hover state, the blinking caret, and `TextField` typing now
  invalidate just their widget's rect, and `Window::redraw()` clips + `win_present_rect`s that band
  (skipping non-overlapping widgets). Backward-compatible: a full present is just a whole-rect
  damage, so every existing app is unchanged. **Save bug:** quitting with a dirty background tab used
  to silently drop it (quit-Save only wrote the active tab). **Close/save UX reworked per use:**
  closing the **window** never prompts (the autosave draft already holds every tab + unsaved
  contents — `on_close` flushes + exits, relaunch restores); closing a **tab** is what guards, and
  **Save** on a never-saved tab (or **Ctrl+S** on one) opens the **picker** to choose where (a draft
  ≠ saved); closing the *last* tab clears the draft store. **Autosave debounced** so a disk write
  never stalls active typing (flush after ~0.6 s idle, or a ~3 s backstop). Tests updated
  (`t_notepad_edit_save`/`_undo`/`_guard`/`t_app_menu` now drive the picker). Screenshot-verified
  (hover-while-typing clean, picker renders). BIOS 30/30 + UEFI 11/11 + 62 unit.
- **Notepad tabs + session autosave #5 (2026-06-03).** Notepad is now a tabbed editor. The top
  filename field is gone; each note is a tab in an app-local `TabBar` strip (active accent-edged,
  dirty shows a dot, each with a ×) + a trailing `+`; **File > New / ^N** opens a fresh untitled tab
  (no guard — opening a tab can't lose data), **File > Close Tab / ^W** (or the ×) closes one. One
  shared `editor` swaps the active tab's text in/out, gated by a `loading` flag so a load doesn't
  dirty the tab; a `Tab{name,named,dirty,text,caret}` model holds the rest. The unsaved-changes guard
  moved from New to **tab/window close** (`t_notepad_guard` reworked: two dirty tabs, Discard one +
  Save the other — which also proves per-tab content isolation). **Session autosave:** a new toolkit
  `Window::on_tick()` hook drives a periodic draft (~1.8 s, only when changed) of every tab's text +
  the layout to `~/.cache/notepad/` (`session` + `tab<i>`); a bare relaunch rebuilds the whole
  session, even never-saved notes — two-boot e2e `t_notepad_session` (restore markers carry each
  tab's loaded byte count). **Save** is a quick-save (named → its path; untitled → `~/Documents`),
  **Save As…**/**Open…** use the picker. Window title stays "Notepad" (no set-title syscall yet);
  per-tab undo isn't kept across a switch (v1). Screenshot-verified. BIOS 30/30 + UEFI 11/11 + 62 unit.
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
  (`tos_may_write` via `kernel/fs/perm.h`), and on an overwrite raises a nested **Replace / Keep
  Both / Cancel** `ui::ConfirmDialog` — **Keep Both** dedupes to `name (N).ext` (`fd_dedup`) instead
  of clobbering. `on_pick(ctx, path)` returns the chosen absolute path (or nullptr on Cancel);
  markers `[filedialog] open …/cd …/pick …/cancel`. **Notepad** wired it up: File > Open… (`^O`)
  loads a note, File > Save As… writes one (File menu is now New/Open/Save/Save As). e2e
  `t_file_dialog` (Save As → type a name → Enter → read back; then Save As the same name → Keep Both
  → `picked (2).txt`); screenshot-verified (Open + Save + the 3-button overwrite warning). BIOS 29/29
  + UEFI 11/11 + 62 unit.
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
