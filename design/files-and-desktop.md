# Design guideline — tOS Files manager & the desktop

> Status: **design guideline + implementation spec.** This is the plan for the Files
> app as the canonical file-management surface, and for the **desktop as a live Files
> view of `~/Desktop`** — one shared component, one selection contract. Almost none of
> this is built yet; the "Current state" section is the honest baseline. Implement in
> the phases at the end; keep `make test` green at every step.
>
> Companion docs: [`ui.md`](ui.md) (toolkit + desktop chrome), [`filesystem-layout.md`](filesystem-layout.md)
> (the on-disk tree), [`system-ownership.md`](system-ownership.md) (who may delete what),
> [`app-runtime.md`](app-runtime.md) (capabilities). The selection contract here is the
> file-side twin of the global text-interaction contract in [`ui.md`](ui.md) / NEXT_STEPS.

## Why this doc exists

The Files app should be a *real* file manager and the desktop should **be Files** — the
icons on the desktop are the contents of `~/Desktop`, and they behave exactly like items
in a Files window (select, multi-select, copy/cut/paste incl. folders, rename, context
menu, drag-to-move). Rather than enumerate every behaviour ad hoc, this doc fixes the
model once: a **shared FileView component** and a **single selection contract**, used by
both surfaces. New behaviours are added to the component, not re-implemented per surface.

Reference point: **macOS Finder** semantics (adapted — tOS has no ⌘ key, so the modifier
that is ⌘ on macOS maps to **Ctrl** here, with **Super** as an alias where free). Where a
behaviour is "obvious Finder," we just say so instead of re-deriving it.

## Current state (the honest baseline)

> **Update (2026-06-13):** this baseline has largely been overtaken — multi-select,
> rename, and drag-to-move have since landed (see the per-bullet notes + CHANGELOG).
> The list below is kept as the original honest starting point.

What exists in `user/files/files.cpp` + the toolkit today:

- ~~**Single selection only.**~~ **Multi-select landed (2026-06-13).** The list-view
  selection is now a shared set (`user/lib/filesel.h`: a row-index set + anchor + cursor
  implementing the contract below — click/Ctrl/Shift/marquee/Select-All/cursor moves,
  unit-tested in `t_filesel`). `ListView` grew an optional `is_sel` predicate so it paints
  the whole set; the Files list reads `kbd_mods()` at click time to route plain/Ctrl/Shift
  clicks, shows "N selected", and Ctrl+A / Edit ▸ Select All select all. The **rubber-band marquee**
  is wired too (a drag over empty list space live-selects the row band, via a new `Window::on_release`
  hook). The **icon and gallery views** share the same set (each got an `is_sel` predicate; click +
  Ctrl+A multi-select work there too). Deferred polish: the marquee's rubber-band rectangle and a 2D
  grid marquee.
- **Copy/Cut/Paste handles files only.** `copy_sel()` does `clip_put(CLIP_FILE, name,
  bytes, len)` — it stuffs the file's *bytes* into the clipboard ring, so it cannot
  represent a directory (the code says as much). `paste()` writes those bytes back;
  Cut+Paste `rmrf`s the source. Folders are unsupported.
- **No rename, no New File** (there is a New Folder button); no Trash.
- **The desktop renders no files.** `twm` paints the wallpaper + dock + menu bar; the
  `icons[]` array there is *dock launchers*, not `~/Desktop` contents. The desktop is
  not a folder view at all.
- **No drag-and-drop.** Apps only see clicks (NEXT_STEPS "DnD protocol" is unbuilt), so
  there is no drag-to-move between folders or onto the desktop. The toolkit *does* expose
  the hooks (`Window::on_drag(x,y,btn)`, `Window::on_context(x,y)`) — they're just unused
  for selection/DnD in Files.

## The shared FileView component (the core move)

Factor the directory view out of the Files **window** into a reusable component the
toolkit owns — call it `ui::FileView` (or a `user/lib/fileview.*` module). It is the one
place that knows how to show a directory and act on it; the Files window and the desktop
are thin shells around it.

A `FileView` owns:

- **A root path + entries** (`readdir`, sorted dirs-first), each with name/type/size/icon.
- **A layout mode**: `LIST` (rows; Files' main pane) or `ICONS` (a grid; the desktop and
  Files' future icon view). Same data, two renderers.
- **The selection set** (see the contract below) — a multi-selection, not one index.
- **Clipboard ops** (copy/cut/paste of the *whole selection*, files **and** folders).
- **Context menu** construction (Open, Open With, Rename, Cut/Copy, Paste, New Folder,
  Move to Trash / Delete, Get Info) — built from the selection + clipboard state.
- **Rename** (an in-place `TextField` overlay on the focused item).
- **DnD source/target** behaviour (drag the selection out; accept a drop onto a folder
  icon or empty space) once the DnD protocol lands.

The Files **window** = chrome (toolbar: back/fwd/up, path bar, Info pane, sidebar) wrapped
around one `FileView(LIST)` rooted at the navigated path, with the rich toolkit affordances
(sidebar shortcuts, history, details pane, rename, DnD).

The **desktop** is a different beast — see "[The desktop is part of the compositor](#the-desktop-is-part-of-the-compositor-not-a-window-not-an-app)"
below. It is drawn by `twm` itself (a peer of the dock), not a Files window, and it
re-implements only the small icon-grid subset it needs rather than embedding the C++
`FileView` (the compositor is C). They share the *idea* — `~/Desktop` as a grid of icons —
not the code.

## The selection contract (shared by every FileView and surface)

A selection is a **set** of entry indices with an **anchor** (the pivot for range
extends) and a **cursor** (the keyboard-focused item). Pointer + keyboard map onto it the
Finder way (⌘→Ctrl, with Super as an alias):

| Gesture | Result |
|---|---|
| Click an item | Replace selection with just it; anchor = cursor = it |
| **Ctrl/Super-click** an item | **Toggle** it in/out of the selection (anchor moves to it) |
| **Shift-click** an item | **Range-select** anchor→it (replaces, unless Ctrl held = additive range) |
| Click empty space | Clear the selection |
| **Drag on empty space (marquee)** | Rubber-band select items the rect touches (replace; +Ctrl/Shift = add) |
| Ctrl/Super + A | Select all |
| Esc | Clear selection (or cancel a rename/drag) |
| Arrow keys | Move the cursor (replace selection); **Shift+arrow** extends from anchor |
| Double-click / Enter | Open the cursor item (dir → navigate; file → default app) |
| F2 / Enter-on-rename / slow second click | Rename the cursor item |
| Space | Get Info / Quick Look (future) |

Rubber-band selection is **intra-window** — a mouse drag inside the FileView's own
surface, producing a selection rect; it needs **no** cross-app DnD protocol and is
buildable now via `Window::on_drag`. (Dragging items *out* of the window is DnD; see
below.) The same contract is the file-side analogue of the text selection contract in
[`ui.md`](ui.md), so "select / Ctrl-multi-select / Shift-range / marquee" reads
identically whether you're selecting characters or icons.

## File operations (on the whole selection, files **and** folders)

### Clipboard: switch from "bytes" to "path references"

The current "copy the file's bytes into the clipboard" model cannot represent a folder and
wastes RAM on big files. Redesign the file clipboard payload as a **list of source path
references**, not contents:

- `clip_put(CLIP_FILEREF, label, paths, len)` where `paths` is a NUL-separated list of
  absolute source paths (the whole selection), and a flag records **copy vs cut**.
  (Reuse `CLIP_FILE` and treat its data as a path list, or add `CLIP_FILEREF` — prefer a
  distinct type so a "paste as text" never dumps raw bytes.)
- **Paste** = the file manager walks each source path and **recursively copies** it into
  the destination dir (a real `cp -r`: a directory is recreated and its children copied;
  the existing `rm_r`/`rmrf` shows the walk pattern — add the copy twin `cp_r`).
- **Cut** then paste = copy then delete the sources (a move; within the same fs prefer a
  cheap `rename`). Same-dir paste of a cut item is a no-op (as today).
- **Name collisions** dedupe to `copy of X` rather than clobbering (as today), extended to
  multi-item and folders.
- **Multi-item**: copy/cut/paste act on the entire selection set, not one row.

This makes folder copy/paste fall out of the same path as files, and makes the clipboard a
small reference (cheap), with the heavy lifting done by the recursive walk at paste time.

### Other operations

- **Open**: dir → navigate (Files) / open a new Files window (desktop); file → its default
  app (by extension / manifest), as today's `enter()` does.
- **Delete**: recursive for dirs. Two tiers (Finder-like): **Move to Trash** (`~/.Trash`,
  reversible — future) vs **Delete immediately** (Shift-Delete). Ship Delete-immediately
  first; add Trash later. Deletes of **system-owned** items must fail — see
  [`system-ownership.md`](system-ownership.md); Files greys those out instead of erroring.
- **Rename**: an in-place `TextField` over the item label; Enter commits (`rename`), Esc
  cancels. Needs the toolkit rename-overlay (one new widget, reused by both surfaces).
- **New Folder / New File**: create + immediately enter rename on the new item.
- **Get Info**: the Files details pane; on the desktop, a small popup.

## Drag-and-drop

> **Status: the DnD protocol landed 2026-06-11** (kernel `drag.c` + twm ghost/routing +
> toolkit `begin_drag`/`on_drop`/`on_drag_over`/`on_press` + `WEV_DRAG`/`WEV_DROP`; see
> [`ui.md`](ui.md) and CHANGELOG). **Built:** Files drag-to-move (list view — drag a
> file/folder onto a folder row → move); cross-app **text drag** (toolkit-wide); **drag-reorder
> Places** (§7 — a `DRAG_PLACE` drag of a Favorites row, ghost chip + an accent insertion line,
> the new order persisted to the registry). Still open: icon/gallery-view drag sources,
> inter-window + onto-desktop drags (the desktop layer below), copy-on-Ctrl, Esc-to-cancel.

Two distinct things share the word "drag":

1. **Marquee selection** — drag on *empty space* to rubber-band. Intra-window, **buildable
   now** (no protocol). Specified above.
2. **Drag-to-move/copy items** — press on a selected item and drag it to another folder,
   onto a folder icon, between a Files window and the desktop, or to the dock. This crosses
   window boundaries, so it needs the **DnD protocol** (NEXT_STEPS "Drag-and-drop
   protocol", still unbuilt). Minimum the protocol must provide:
   - A drag **source** starts a session with a **typed payload** (here: the selection's
     path list, `DRAG_FILES`).
   - The compositor drags a **ghost** (a translucent stack of the dragged icons) under the
     cursor and hit-tests **drop targets** (a folder icon, a FileView's empty space, the
     dock, the desktop), highlighting the target.
   - **Drop** delivers the payload to the target window as a `WEV_DROP` carrying the
     payload + the drop point; the target performs the move (or copy if **Ctrl/Opt** is
     held — Finder uses Opt-drag = copy, plain drag = move within a volume).
   - Drag to the desktop = move into `~/Desktop`; drag off the desktop into a window =
     move out. Because both are FileViews over real dirs, "drag between them" is just
     "move a path from dir A to dir B."

   This same protocol unlocks cross-app text drag and the Pocket Dimension shelf
   (NEXT_STEPS), so design it once, generally (payload types: files, text, image bytes).

## The desktop is part of the compositor (not a window, not an app)

**The desktop is a feature of `twm`, a peer of the dock and the control center** — it
lives in `user/twm/desktop.c`, drawn by `compose()` directly over the wallpaper, below
every window. This mirrors how real shells are built: in KDE Plasma the *desktop
containment* and the *panel containment* (the task bar) are both hosted by **plasmashell**,
the one shell process — KWin only composites; the desktop is **not** a launched program
(see `inspiration/plasma-desktop`, `containments/desktop`, `"KPackageStructure":
"Plasma/Applet"`). In tOS `twm` is *both* the compositor and the shell — it already owns
the wallpaper, the bar and the dock — so the desktop belongs in it too.

Architecture:

- **No window, no `WIN_DESKTOP` flag, no separate process.** twm scans `~/Desktop` at
  startup (`desktop_init`) and paints its icon grid in `draw_desktop()`, called by
  `compose()` right after the wallpaper and before any window. The wallpaper simply shows
  wherever there is no icon — there is nothing to colour-key, no surface to blit, no IPC
  snapshot to reconcile, and the desktop is never in the z-order so it is never focusable.
  (This replaces an earlier, over-engineered draft — a bottom-pinned `WIN_DESKTOP` window
  owned by a standalone `desktop` app, colour-keyed through `ugfx_blit_round_key`. It was
  scrapped: letting an app's *window* declare itself "the desktop" is the same mistake as
  letting an app's *manifest* pin itself to the dock — desktop-ness is shell policy, not an
  app's to claim. Folding it into twm also deleted a whole process, a window flag, the
  colour-key path, and the snapshot/launch plumbing.)
- **Why not literally reuse the Files `FileView`?** `FileView` is C++ on the `ui::` toolkit;
  twm is C. They can't share a binary component, so the desktop re-implements the *small*
  slice it needs (read a dir, lay out an icon grid, select, double-click-open) in ~150 lines
  of C. The Files window keeps the rich `FileView` (sidebar, history, details, rename, DnD);
  the desktop is deliberately the lightweight subset. They share the *idea* (`~/Desktop` as
  an icon grid), not the code — the right trade for keeping file-management logic out of the
  compositor.
- **Desktop specifics (in `desktop.c`)**:
  - **Auto-grid** layout (top-left, row-major), inset from the work-area edges. **Free icon
    positions** (drag an icon anywhere, remembered) is a follow-up — persist per-item
    `(x,y)` in a dotfile or the registry; absence ⇒ auto-grid.
  - **Single-click** selects an icon (translucent pill); **clicking empty space** deselects.
  - **Double-click**: open — a folder opens navigated-to in Files (the path is handed off via
    `/tmp/.open-doc`, then twm's capped `launch()` starts Files); a file opens Files at the
    Desktop. (Open-with-default-app is a follow-up.)
  - **Live updates**: re-scan `~/Desktop` once a second (`desktop_tick`, the same cadence twm
    re-reads the registry), repainting only when a cheap content signature changes; a file
    dropped in from the shell or Files appears within a second. An fs-change notification
    (inotify-like `WEV`/signal) to avoid the poll is future work.
  - **Right-click empty desktop** → context menu (New Folder, Paste, Change Wallpaper…,
    Clean Up) and **multi-select / marquee / clipboard / rename on the desktop** are
    follow-ups, reimplemented in `desktop.c` as needed (they do not come "free" — that
    convenience was the cost of not sharing the C++ component).

## Gaps this design opens (and where they're closed)

| Gap | Where |
|---|---|
| Multi-selection set + anchor/cursor | toolkit (`FileView` + `ListView`/new `IconView`) |
| Rubber-band marquee | `Window::on_drag` (exists) → FileView |
| Path-reference, multi-item, folder-aware clipboard | `CLIP_FILEREF` + `cp_r`/recursive paste |
| Rename overlay widget | toolkit (one new widget, shared) |
| Desktop rendering `~/Desktop` | `user/twm/desktop.c` — a twm feature, drawn over the wallpaper in `compose()` (no window, no `WIN_DESKTOP`, no separate app) |
| Drag-to-move between folders/desktop/dock | **DnD protocol** (NEXT_STEPS) — typed payload + ghost + `WEV_DROP` |
| Deleting system files must fail | [`system-ownership.md`](system-ownership.md) |
| Live folder change without polling | future fs-change notification |

## Phasing (keep `make test` green)

1. **Shared FileView + multi-select + marquee (list view).** *Landed 2026-06-13:* the selection set
   (`filesel.h`, unit-tested) + Ctrl/Shift-click + Ctrl+A wired into the Files list view (`is_sel`
   predicate, `list_pick`, "N selected"), **plus the rubber-band marquee** (a new `Window::on_release`
   hook + Files `on_press`/`on_drag` live-select the row band a drag over empty space covers). No
   kernel changes. Existing Files tests still pass; multi-select is screenshot-verified (Ctrl+A) +
   unit-pinned (held-modifier clicks can't be driven by the harness), the marquee is e2e-verified via
   the drag helper. **Icon + gallery views** share the same set too (each got the `is_sel` predicate;
   click + Ctrl+A multi-select work there, screenshot-verified). *Left here:* the marquee's rubber-band
   **rectangle** (cosmetic) and a 2D **grid marquee** (the list marquee is 1D-contiguous; a grid rect
   selects a non-contiguous set, so it needs a per-tile hit loop rather than `fsel_marquee`).
2. **Folder/multi-item copy-cut-paste + rename + New File.** *Landed 2026-06-13.* The clipboard is a
   `CLIP_FILEREF` path-reference list (the whole selection, files + folders); `copy_sel` gathers the
   multi-set, `paste` recursively copies each source via `copy_across`/`copy_tree` (deduped; a Cut moves
   by deleting the source), each item journaled. Rename (`renamefld`) + New Folder/New File already
   existed. Verified: a folder with a nested subdir + file copied through the clipboard, whole tree
   confirmed on disk (boot + screenshot); the dedupe naming (`dupname.h`) + the set algebra (`filesel.h`)
   are unit-tested.
3. **The desktop, in twm.** *Landed 2026-06-14.* `user/twm/desktop.c` — a compositor feature
   (peer of the dock), **not** a window or a separate app. twm scans `~/Desktop` at startup
   and paints the icon grid over the wallpaper in `compose()`; single-click selects,
   double-click opens (folder → Files navigated there), and a once-a-second re-scan picks up
   files dropped in live. Verified by a disposable boot (`[twm] desktop items N` /
   `[twm] desktop reload N`) + screenshots. *An earlier `WIN_DESKTOP` window + standalone
   `desktop` app draft was scrapped — see the architecture section.* *Left here:* the
   right-click desktop context menu, and multi-select / marquee / clipboard / rename on the
   desktop (reimplemented in `desktop.c` when needed).
4. **DnD protocol → drag-to-move.** General typed-payload DnD (files/text/image); wire
   FileView as source + target; drag between folders, window↔desktop, onto folder icons and
   the dock. Tests: drag a file from a Files window to the desktop and confirm the move.

## Testing strategy (pyramid — see [`testing.md`](testing.md))

- **Unit (host, no QEMU):** the selection-set algebra (click/ctrl/shift/marquee → resulting
  set) and the recursive-copy *planner* (source tree → list of (mkdir/copy) ops, collision
  dedupe) are pure logic — unit-test them in `tests/unit/`.
- **e2e (QEMU, a few canaries):** one multi-select + folder-paste journey; one desktop
  journey (file appears on the desktop, opens). Drive by the serial markers the surfaces
  print (`[files] paste …`, and the compositor's `[twm] desktop items N` /
  `[twm] desktop reload N` / `[twm] desktop open …`) the way the dock/Spotlight tests do.

## Out of scope (for now)

Trash with restore, free (persisted) icon positions, Quick Look previews, tags/labels,
column/gallery view, search, symlinks/aliases, and multi-monitor. Each is a clean follow-up
once the shared FileView + selection contract + DnD exist.
