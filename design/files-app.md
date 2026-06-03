# Design guideline — tOS Files app: feature completeness

> Status: **design guideline + implementation spec.** This is the plan for turning
> `user/files/files.cpp` from a single-pane directory lister into a *real* file
> manager — the app-level chrome and capabilities (view modes, location bar, tabs,
> search/filter, status bar, editable Places, rich Get Info, Trash, tags, thumbnails,
> undo, background jobs). Almost none of this is built yet; the baseline section is
> honest. Implement in the phases at the end; keep `make test` green at every step.
>
> **Companion — read first:** [`files-and-desktop.md`](files-and-desktop.md) owns the
> *foundation*: the shared `ui::FileView` component, the multi-selection contract,
> the path-reference clipboard (folder-aware copy/cut/paste), inline rename, the DnD
> protocol, and the desktop-as-a-Files-view. **This doc does not re-specify those** —
> it builds the application *on top of* them. Where a feature here needs a foundation
> piece (e.g. multi-select for "selected count" in the status bar), it says so and
> links there.
>
> Other companions: [`ui.md`](ui.md) (toolkit + desktop chrome), [`filesystem-layout.md`](filesystem-layout.md)
> (the on-disk tree), [`system-ownership.md`](system-ownership.md) (who may delete what),
> [`settings.md`](settings.md) (the registry), [`testing.md`](testing.md).

## Why this doc exists

We studied a mature file manager (KDE Dolphin) to enumerate what a complete one
actually does, then filtered to **what tOS Files needs** — adapted to our look and
ownership model, not copied. tOS Files is **macOS-Finder-styled** (the ⌘ modifier
maps to **Ctrl**, Super as an alias) on our slate-blue palette — see
[[feedback-tos-keep-palette-no-material]]; no Material, no KDE chrome. Where a
behaviour is "obvious Finder," we say so rather than re-derive it.

We own the whole stack. So when a feature needs something the kernel/fs/toolkit
doesn't have yet (file timestamps, free-space query, an fs-change signal, a tag
store), that missing piece is **part of this plan**, not a blocker — collected in
[Supporting-stack work](#supporting-stack-work-we-own-this-too). "We don't support
it" is never a reason to leave it out; it's a reason to build it.

## What we have today (the honest baseline)

`user/files/files.cpp` (~720 lines) today is a competent *single* view:

- A **fixed sidebar** of 8 hardcoded Favorites (Home, Desktop, …, Computer), with a
  sync highlight on the current path. Not editable, no sections, no volumes/Trash/tags.
- A toolbar: **back / fwd / up**, **New Folder**, **Delete**, and an **Info** toggle.
- **One list view** (detail rows: icon + name + size). No icon grid, no columns view,
  no column (Miller) view, no zoom/size control.
- A right **Details/Info pane** (single-click to inspect: icon, kind, size, where).
- A **right-click context menu** + an **Open With** chooser that remembers the default
  app per extension in the registry (`open.default.<ext>`).
- **Navigation history** (toolbar arrows + mouse side buttons via `on_nav`).
- **Copy/Cut/Paste of a single file** via the clipboard *byte* ring (cannot do folders;
  see the foundation doc's "switch to path references").
- **New Folder** and **immediate recursive Delete** (`rmrf` — no Trash, irreversible).
- Sort is **hardcoded**: dirs first, then case-sensitive ASCII by name. Not changeable.
- A **static path label** (`pathlbl`) — not clickable, not editable, no breadcrumb.

What it is missing is everything below.

## The gap at a glance

Priority: **P1** = a file manager is incomplete without it; **P2** = expected, high
value; **P3** = polish / power-user. "Foundation" = specified in
[`files-and-desktop.md`](files-and-desktop.md), listed here only for completeness.

| Capability | Today | Target | Pri |
|---|---|---|---|
| Multi-selection (Ctrl/Shift/marquee) | single int | foundation | **P1** (foundation) |
| Folder-aware copy/cut/paste (path refs) | files only | foundation | **P1** (foundation) |
| Inline **rename** | none | foundation | **P1** (foundation) |
| **Icon (grid) view** | none | §1 | **P1** |
| **List / Details view** w/ sortable columns | size only | §1 | **P1** |
| **Column (Miller) view** | none | §1 | P2 |
| **Gallery / preview view** | none | §1 | P3 |
| **Zoom / icon-size** control | none | §1 | **P1** |
| **Sort by** name/kind/size/date + asc/desc | hardcoded | §2 | **P1** |
| **Show in groups** | none | §2 | P3 |
| **Per-folder view memory** | none | §2 | P2 |
| **Location bar**: breadcrumb + editable path | static label | §3 | **P1** |
| **Tabs** | none | §4 | P2 |
| **Split / dual pane** | none | §4 | P2 |
| **Filter bar** (live name filter) | none | §5 | **P1** |
| **Search** (recursive, Spotlight-integrated) | none | §5 | P2 |
| **Status bar** (counts, size, free space, progress) | none | §6 | **P1** |
| **Editable Places** sidebar + sections + volumes | fixed 8 | §7 | **P1** |
| **Rich Get Info** (dates, owner, recursive size, perms) | thin pane | §8 | P2 |
| **Trash** (move-to-trash, restore, empty) | immediate rm | §9 | **P1** |
| **Tags / labels** (colored, filterable) | none | §10 | P3 |
| **Thumbnails** + **Quick Look** (Space) | none | §11 | P2 |
| **Richer file-type icons** + custom folder icons | 5 icons | §11 | P2 |
| **Duplicate**, **New File / templates** | none | §12 | P2 |
| **Undo / redo** of file ops | none | §12 | P2 |
| **Background jobs + progress** (long copy/delete) | blocking | §12 | P2 |
| **Keyboard + menu map** | ad hoc | §13 | P2 |

---

## 1. View modes & zoom

A Finder/Dolphin view is one of several *renderers over the same directory model*.
The shared `ui::FileView` (foundation doc) already separates data from renderer — it
defines `LIST` and `ICONS`. This section fleshes those out and adds two more.

- **Icon view (`ICONS`)** — a grid of large icons with the name centred below; the
  Finder default and the desktop's only mode. Wraps to the pane width, column-major.
  This is the headline missing mode — most users expect the grid first.
- **List / Details view (`LIST`)** — the current rows, upgraded to **multiple,
  user-chosen, sortable columns** (Name, Kind, Size, Date Modified, Date Added,
  owner; see §2/§8). A **header row** whose cells sort on click (and show the
  asc/desc caret) and are **resizable**; column set + widths persist per folder (§2).
- **Column view (Miller columns)** — horizontally scrolling panes, one per path level;
  selecting a folder in column *n* opens its contents in column *n+1*; the last column
  is a preview. This is *the* Finder navigation idiom for deep trees. P2.
- **Gallery view** — a large preview of the focused item with a filmstrip of the rest;
  for browsing images/docs. P3, rides on thumbnails (§11).

**Zoom / icon size.** A control (status-bar slider §6, and Ctrl `+`/`-`/`0`) scales
icon-grid tiles and list-row thumbnails through a few discrete levels. `FileView`
already smooth-scales icons via `ugfx_blit_scaled`; zoom just picks the target box.
Persists per folder (§2).

**Switching** lives in the toolbar (a segmented Icon/List/Column control, Finder-style)
and on the View menu (§13), with shortcuts (Ctrl 1/2/3/4). The mode is a `FileView`
property; switching never re-reads the directory, only re-renders.

## 2. Sorting, grouping & per-folder view memory

Replace the hardcoded comparator with a **sort key + direction**, chosen by the user:

- **Sort by**: Name, Kind, Size, **Date Modified**, **Date Added/Created**, Tag, owner.
  Dates need fs timestamps — see [Supporting-stack work](#supporting-stack-work-we-own-this-too).
- **Direction**: ascending/descending, toggled from the menu or by clicking the active
  list-view column header.
- **Folders first** (toggle, default on — today's behaviour) and **Hidden last**.
- **Natural/numeric** name compare (so `file2` < `file10`) — small polish over today's
  raw ASCII memcmp.
- **Show in Groups** — partition the view by the sort key (e.g. by Kind, or by size
  bucket, or first letter) with group headers. P3 but cheap once sorting is data-driven.

**Per-folder view memory.** Finder/Dolphin remember each folder's view mode, sort, zoom,
column set + widths, and "show hidden". Persist these in the **registry** (we already
use it for `open.default.<ext>`) under a per-path key, e.g.
`view.<path>.mode/sort/dir/zoom/cols`, with a global default for unseen folders and a
"use as default for all folders" action. The registry is layered (user/lib) — a folder
override falls back to the user default falls back to the lib default. See
[`settings.md`](settings.md).

## 3. The location bar (breadcrumb + editable path)

The static `pathlbl` becomes a real **location bar**, the single most-used navigation
surface after the sidebar:

- **Breadcrumb mode** (default): each path segment is a **clickable button**
  (`/ Users / user / Documents`) that navigates to that ancestor; an optional small
  **chevron** per segment opens a menu of that folder's *sibling* subfolders for quick
  lateral jumps (Dolphin's breadcrumb sub-menus). Ellipsize the middle when the path is
  too long for the bar.
- **Editable mode**: click the empty part of the bar (or Ctrl L) to turn it into a
  `ui::TextField` showing the literal path; type a path and Enter to **go to folder**,
  with **tab/inline completion** against `readdir` of the partial parent. Esc reverts to
  breadcrumb. (Foundation gives us `ui::TextField` already — see the spotlight/notepad reuse.)
- It lives in the toolbar between the nav arrows and the search field; back/fwd/up keep
  working as today and just re-render it.

This replaces "I can only get places via the sidebar or by drilling in" with "type or
click to anywhere," which is table-stakes for a file manager.

## 4. Tabs & split view

- **Tabs** (Finder ⌘T). One window, several folders; a tab strip under the toolbar; each
  tab owns its own `FileView` + history + view settings. New Tab (Ctrl T), Close Tab
  (Ctrl W), Next/Prev (Ctrl Tab / Ctrl Shift Tab), **Open in New Tab** from the context
  menu, **reopen closed tab** (Ctrl Shift T). Middle-click a folder = open in new tab.
- **Split / dual pane** (Dolphin's signature; Finder lacks it but it's invaluable for
  moving files): the content area splits into **two FileViews side by side**, each with
  its own path; **Copy/Move to Other View** acts between them, and drag between panes
  (foundation DnD) moves files. One pane is "active" (keyboard focus). Toggle on the
  toolbar / Ctrl `*`.

Both are pure app-level composition over `FileView` instances — no kernel work — but
tabs need a tab-strip widget and split needs a draggable splitter (small toolkit
additions, both reusable). See [[feedback-ok-multi-file-modules]]: it's fine to split
Files into `files.cpp` + `fileview.*` + `locationbar.*` + `tabstrip.*` rather than one
mega-file.

## 5. Filter bar & search

Two different things, both missing:

- **Filter bar** (Dolphin `/`, Finder has the search field do this for the current
  folder). A slim bar (toggle from the toolbar or `/`) with a `ui::TextField`; as you
  type it **live-filters the current folder's entries** by substring (case-insensitive),
  optionally by kind. No fs traversal — it just hides non-matching rows in the existing
  `FileView`. Cheap, immediate, very high value. Ship this first.
- **Search** (recursive find). A search field in the toolbar; typing runs a **recursive
  walk** from a chosen **scope** ("This Folder" / "Everywhere" / Home) matching name
  substrings (later: by kind, by tag, by size). Results render in the same `FileView`
  (each row shows its path). **Integrate with Spotlight** — tOS already has a Spotlight
  app and the kernel hook (`KEY_SUPER_SPACE`); Files' search and Spotlight should share
  one **indexer/walk** so we don't write the traversal twice, and "Show in Files" from a
  Spotlight hit opens the containing folder with the item selected. Long searches run as
  a background job with the result list streaming in (§12) and a Stop button (§6). P2.

## 6. Status bar

A bottom bar (Finder/Dolphin both have one) — currently absent. Shows, left-to-right:

- **Item count** for the folder, and on selection **"N of M selected, <total size>"**
  (needs multi-select — foundation; needs recursive folder size — §8/supporting work).
- **Free space** on the volume with a thin usage bar (Dolphin's
  `StatusBarSpaceInfo`). Needs a **statfs** syscall — supporting work.
- The **zoom slider** (§1), right-aligned.
- A **progress area** for the active background job (copy/move/delete/search) with a
  **Stop** button (§12). Hidden when idle; appears only if a job runs >~0.5s (Dolphin's
  delayed-progress trick avoids flicker on fast ops).

## 7. The sidebar / Places (editable, sectioned)

Today's 8 hardcoded rows become a real **Places sidebar**, Finder/Dolphin-style:

- **Sections** with headers: **Favorites** (user shortcuts), **Locations** (the volume(s)
  / disks, Computer root, Network later), **Tags** (§10), and **Trash** (§9). Each
  section is collapsible.
- **Editable Favorites**: **Add to Places** (from the context menu or drag a folder onto
  the sidebar — foundation DnD), **remove**, **rename** the shortcut, and **reorder** by
  drag. Persist the list in the registry (a `places.*` list) so it survives reboots and
  isn't recompiled into the binary.
- **Volumes** show a free-space bar inline (statfs). **Trash** shows full/empty state and
  accepts drops (drag-to-trash). **Eject/unmount** when removable storage exists (future).
- Keeps the current **sync highlight** of the active path, and gains **drag-drop onto a
  place** = move/copy there (foundation DnD).

## 8. Rich Get Info / Properties

The Details pane is thin (icon, kind, size, where). A real **Get Info** (Ctrl I; Finder
⌘I) — either the side pane upgraded or a small floating panel — shows:

- **Large preview / thumbnail** (§11), the editable **name** (inline rename from Info),
  **Kind**, **Size** — *recursive* for folders ("Calculate" / auto, with a running total
  as the walk progresses), **Where** (path).
- **Created / Modified** dates (needs fs timestamps — supporting work).
- **Owner** (we already have `fstat.owner` / `TOS_UID_*`) and **permissions** — and
  whether the item is **system-owned** (read-only / undeletable per
  [`system-ownership.md`](system-ownership.md), shown as a lock badge).
- **Opens with** — the default app for this type with a chooser (reuse the existing
  Open-With + `open.default.<ext>` machinery) and "change for all of this type."
- A **tags** editor (§10).

For a multi-selection, show the combined count and total size. The Info pane stays a
toggle (today's Info button) and is shared with the desktop's "Get Info" popup
(foundation doc).

## 9. Trash

Replace irreversible `rmrf` with a two-tier model (Finder/Dolphin):

- **Move to Trash** (default Delete / Ctrl Backspace): move the item into a per-user
  **`~/.Trash`**, recording its **original path** (and deletion time) in a trash metadata
  file so it can be **restored** ("Put Back"). Within the same fs this is a cheap
  `rename_` into `~/.Trash` plus a metadata append.
- **Delete Immediately** (Shift+Delete / "Delete…" with confirm): the current recursive
  permanent delete, kept for power users and for items already in Trash.
- **Trash as a place**: a Trash entry in the sidebar (§7) that opens a `FileView` of
  `~/.Trash`; from there **Put Back**, **Empty Trash**, or delete individual items.
  Drag-to-trash (foundation DnD) and a full/empty Trash icon in the dock/sidebar.
- **System-owned** items can't be trashed — greyed out, not an error
  ([`system-ownership.md`](system-ownership.md)).

Collision handling in Trash dedupes (`name`, `name 2`, …) so re-trashing the same name
doesn't clobber a previous one and Put Back can still find the right origin.

## 10. Tags & labels

macOS colored tags / Finder labels — assign one or more tags to a file, then **filter or
search by tag** and see a **Tags section** in the sidebar (§7). tOS has no fs xattrs, so
back tags with a **registry/sidecar tag store** keyed by path (a `tags.<path>` entry, or
a single `~/.tags` index), with a fixed palette of named colors first and custom names
later. Set tags from the context menu and Get Info (§8); show a colored dot on the icon.
P3 — clean follow-up once Get Info and search exist.

## 11. Thumbnails, Quick Look & richer iconography

- **Thumbnails**: render real previews for images instead of the generic image glyph —
  for our native image format (`load_icon_argb` already decodes it) and any later formats,
  downscaled with the existing `ugfx_blit_scaled`. Generate **off the UI path** (a small
  job) and **cache** them (a `~/.cache/thumbnails` keyed by path+size, or in RAM per
  session) so scrolling a big folder stays smooth. Feeds the icon view, list thumbnails,
  Get Info preview, and gallery/column previews.
- **Quick Look** (Space) — a centred overlay previewing the focused item (image full-size,
  text head, folder summary) without opening an app; Esc/Space closes. P2, rides on the
  same preview pipeline.
- **Richer file-type icons.** We ship **5** baked icons (`FILEICON_FOLDER/TEXT/EXEC/
  IMAGE/FILE`). Grow the set and the **extension→icon map** (archives, audio, code, pdf,
  app-document icons from a manifest), plus **per-app document icons** and **custom folder
  icons** (set via Get Info, stored like tags). This is mostly art + a lookup table.

## 12. File-operation completeness

Beyond the foundation's copy/cut/paste/rename:

- **Duplicate** (Ctrl D) — `cp_r` an item next to itself as "X copy"; the foundation's
  recursive copy makes this a one-liner.
- **New File / templates** — alongside New Folder: New Text File (and a small template
  set), then **New Folder with Selection** (Finder: wrap the selection in a new folder).
- **Undo / redo** of file operations (Dolphin's `KIO` undo, Finder ⌘Z) — keep a small
  **op journal** (create/rename/move/copy/trash) with inverse actions, so the last
  operation(s) can be undone. Trash + rename make most undo trivial (move back / rename
  back); copy-undo deletes the created files. High QoL, P2.
- **Background jobs + progress.** Long copy/move/delete/search must **not block the UI**.
  Model file ops as **jobs** that run incrementally (a step per event-loop tick, or a
  worker), reporting progress to the status bar (§6) with **cancel**. Pair with **conflict
  prompts** (Replace / Skip / Keep Both / Apply to All) when a destination exists, instead
  of today's silent "copy of X". This is the difference between "froze copying a folder"
  and a real file manager.

## 13. Keyboard & menu map

Make shortcuts and menus first-class (today's are ad hoc). Finder-flavoured, ⌘→Ctrl:

| Action | Key | | Action | Key |
|---|---|---|---|---|
| New Folder | Ctrl Shift N | | Find / Search | Ctrl F |
| New Tab | Ctrl T | | Filter | `/` |
| Close Tab | Ctrl W | | Get Info | Ctrl I |
| Open | Enter / dbl-click | | Rename | Enter / F2 |
| Open in New Tab | Ctrl-click | | Go to Folder (edit path) | Ctrl L |
| Up / Back / Fwd | Ctrl ↑ / ⌫ / mouse | | Select All | Ctrl A |
| Icon/List/Column view | Ctrl 1/2/3 | | Copy / Cut / Paste | Ctrl C/X/V |
| Zoom in/out/reset | Ctrl +/-/0 | | Duplicate | Ctrl D |
| Quick Look | Space | | Move to Trash / Delete | ⌫ / Shift+⌫ |
| Undo / Redo | Ctrl Z / Ctrl Shift Z | | Toggle Info pane | Ctrl I (today's button) |

Menus (a menu-bar API is a separate NEXT_STEPS item — see [`ui.md`](ui.md)): **File**
(New, New Tab, Open With, Get Info, Move to Trash…), **Edit** (Undo, Cut/Copy/Paste,
Select All, Rename), **View** (mode, sort, zoom, show hidden, show in groups, toggle
panes), **Go** (Back/Fwd/Up, the Favorites), **Help**.

## Supporting-stack work (we own this too)

Features above that need new kernel/fs/toolkit support. These are **in scope** — we build
them — and several unlock more than just Files:

| Need | For | Sketch |
|---|---|---|
| **File timestamps** (created/modified) in tosfs + `struct dirent`/`fstat` | Sort by date (§2), Get Info dates (§8), Date columns (§1) | Add `mtime`/`ctime` to the tosfs inode + the dirent/stat ABI; set on write/create; a clock source. The single biggest fs gap. |
| **statfs / free-space** syscall | Status-bar free space (§6), volume bars (§7) | Return total/free blocks for a path's volume. |
| **fs-change notification** (inotify-like `WEV`/signal) | Live refresh of Files + desktop without the 1 s poll | Kernel notifies a watcher when a watched dir changes; replaces the poll in [`files-and-desktop.md`](files-and-desktop.md). |
| **Recursive size** helper | Folder sizes in Get Info/status bar (§6/§8) | A `du`-style walk (job, cached); pairs with the `cp_r`/`rm_r` walks. |
| **Tag / sidecar metadata store** | Tags (§10), custom folder icons (§11), free desktop icon positions | A path-keyed store (registry-backed or a `~/.metadata` index) since tosfs has no xattrs. |
| **Thumbnail cache** | Thumbnails/Quick Look/gallery (§11) | `~/.cache/thumbnails` keyed by path+mtime+size; generated off the UI path. |
| **Toolkit widgets**: list-view **column header** (sortable, resizable), **tab strip**, **splitter**, **breadcrumb bar**, **rename overlay** (foundation) | §1/§3/§4 | Each is small, reusable beyond Files; keep them in `user/lib`. |
| **Background-job primitive** | Progress + non-blocking ops (§12), search (§5) | A cooperative job run from the event loop (or a worker proc) reporting progress; reused by search and copy. |
| **Move-across-fs** in clipboard paste | Folder cut/paste, Trash (§9) | Prefer `rename_` within a volume; fall back to `cp_r` + `rm_r` across volumes. |

## Phasing (keep `make test` green)

Builds on the foundation phases in [`files-and-desktop.md`](files-and-desktop.md)
(shared FileView + multi-select + path-ref clipboard + rename land there first).

1. **Location bar + filter bar + status bar.** Breadcrumb/editable path (§3), live filter
   (§5), status bar with item/selection counts (§6, minus free space). All app-level, no
   kernel work, immediately transforms day-to-day use.
2. **Icon view + zoom + data-driven sort + per-folder memory.** Real grid view, zoom
   control, Sort-by menu + sortable list columns (§1/§2), persisted in the registry.
3. **Trash + operation completeness.** `~/.Trash` move/restore/empty (§9), Duplicate /
   New File (§12), conflict prompts; backed by the foundation's recursive copy.
4. **fs timestamps + statfs + rich Get Info.** Supporting fs/kernel work, then Date
   sort/columns (§1/§2), free-space in the status bar/volumes (§6/§7), full Get Info (§8).
5. **Tabs + split view.** Tab strip + splitter widgets; Copy/Move to Other View (§4).
6. **Search (Spotlight-shared) + background jobs + thumbnails.** Recursive search (§5),
   non-blocking jobs/progress (§12), thumbnail cache + Quick Look (§11).
7. **Tags, undo/redo, grouping, column/gallery views.** The power-user polish layer
   (§10/§12/§2/§1) and the fs-change notification to drop the poll.

## Testing strategy (pyramid — see [`testing.md`](testing.md))

Honour [[feedback-testing-pyramid]]: pure logic as host unit tests, a few e2e canaries,
screenshots for UI polish.

- **Unit (host, no QEMU):** the sort comparators (each key + direction + folders-first,
  on a fixed entry list → expected order), the filter/search matcher, path breadcrumb
  splitting + completion, the Trash metadata round-trip (trash → restore restores the
  original path), the conflict-name deduper, and the recursive-size/copy planners (shared
  with the foundation).
- **e2e (QEMU canaries):** drive by serial markers the way the dock/Spotlight tests do —
  `[files] view icons`, `[files] sort kind`, `[files] filter N`, `[files] trash <name>` /
  `[files] restore <name>`, `[files] tab N`. One journey per phase: filter a folder; switch
  to icon view + sort by size; trash then restore a file; open a second tab.
- **Screenshots:** icon view, the breadcrumb, the status bar, Get Info, Quick Look — UI
  polish is judged by eye ([[feedback-tos-visible-ui-not-primitives]]).

## Out of scope (for now)

Network/remote filesystems and mounts, archive create/extract (zip/tar), file
compression, symlinks/aliases, version-control overlays, "Compare files", batch-rename
tooling, multi-monitor, and a touch/selection mode. Each is a clean follow-up once the
view modes, navigation, Trash, search, and tags above exist.
