# Compositor — lessons from the inspiration shells

> Status: **design + roadmap.** This is the home for compositor *architecture* and
> the window-management feature roadmap. The desktop *chrome* (menu bar, dock,
> notifications, fullscreen/auto-hide look) is in [`ui.md`](ui.md); this doc is about
> what twm *is* under the chrome and where it goes next. Everything here keeps the
> house rules: native C drawing (no Qt — see the runtime note below), the slate-blue
> [`theme.h`](../user/lib/theme.h) palette (no Material), the dirty-rect single-writer
> compositor, and the testing pyramid (units + one disposable boot + a screenshot,
> not a new permanent e2e per feature).

## Sources observed

Our compositor is **`user/twm/`** (`twm.c` core + the feature files `bar.c` /
`dock.c` / `controlcenter.c` / `notify.c` / `switcher.c` / `menubar.c` /
`desktop.c`). It is a back-buffered, dirty-rect compositor that owns the framebuffer
and composites each app's shared-memory surface; full architecture in
[PROJECT.md](../PROJECT.md) ("The desktop: a compositor + separate apps") and the
header [`twm.h`](../user/twm/twm.h).

The inspiration folder (`~/dev/os/inspiration/`, a sibling of the repo, not checked
in) holds two compositor-relevant sources:

- **`plasma-desktop/`** — KDE's desktop *shell* (plasmashell): the panel / desktop /
  folder **containments**, and the **applets** (taskmanager, pager, kickoff/kicker,
  showdesktop, minimizeall, trash, window-list…). This is the primary one — it is the
  direct analog to twm's chrome and was already the model for our `~/Desktop`
  containment ([`files-and-desktop.md`](files-and-desktop.md)).
- **`wayland/`** — the wire *protocol* + reference client/server library
  (`protocol/wayland.xml`, `src/`). Not "a compositor" itself, but the canonical
  statement of the compositor↔client contract; mined below for architecture lessons.

**The key framing:** twm is *both* halves of a Plasma system in one process — it is
the **compositor** (KWin's job: surfaces, stacking, input routing, effects) **and**
the **shell** (plasmashell's job: panel, dock, launchers, widgets). So we borrow from
both layers, but always collapsed into twm's single event loop and dirty-rect model.

---

## The free lunch: we already own every window's pixels

The single most important observation from comparing the two. In Plasma/Wayland a
panel applet that wants a **live thumbnail** of a window has to go the long way round —
PipeWire screen-casting (`taskmanager/qml/PipeWireThumbnail.qml`) or a KWin
window-thumbnail item — because clients' buffers are *not* shared with the shell.

twm has no such wall. Every app surface is mapped **into the compositor** read-only at
a stable per-id vaddr (`struct cwin.vaddr`, refreshed each frame from
`SYS_WM_WINDOWS`; PROJECT.md "Window surfaces"). `draw_window()` already blits straight
from `c->vaddr`. That means **a live, scaled thumbnail of any window is free** — a
box-downscale of the bytes we already hold, no new syscall, no protocol, no per-app
code.

That one fact turns three otherwise-expensive Plasma features into cheap wins for us:
window **previews on dock hover**, a **richer Alt-Tab** with real contents, and a
full-screen **Overview / Present-Windows** expose. They all share one new helper:

```c
/* twm.c — downscale a window's live surface into a dst rect of the back buffer.
 * Integer box filter (no FPU), clipped to cur_clip like everything else. Source is
 * c->vaddr (w×h, tightly packed); honors the title-bar offset so the thumbnail shows
 * the chrome-less client, matching what draw_window paints. */
void ugfx_blit_scaled(int dx, int dy, int dw, int dh,
                      const uint32_t *src, int sw, int sh, int stride);
```

Build that once and B/C below are mostly layout.

---

## Candidate features — ranked by leverage ÷ cost

Each entry: **Plasma model → twm today → tOS design → cost → how it's tested.**
Ordered so the cheap, high-value, mutually-unblocking ones come first.

### A. Live window previews — dock hover + a real switcher  *(cheapest real win)*

- **Plasma.** Hovering a task shows a tooltip with a window **thumbnail**
  (`taskmanager/qml/ToolTipInstance.qml`, `thumbnailSourceItem` / `winId`); a *grouped*
  task expands to a **GroupDialog** of all its windows' thumbnails.
- **twm today.** Dock tiles are static icons; the Alt-Tab `switcher.c` card shows an
  icon + title, no contents.
- **tOS design.** Use `ugfx_blit_scaled` from the free-lunch helper.
  1. **Dock hover preview:** after a tile is hovered ~`HIDE_LINGER` frames, float a
     small frosted card above it (reuse the `shadow_box` + `ugfx_frost` panel idiom)
     containing the scaled live surface of that app's window(s). It's a transient
     overlay drawn in `compose()` after the dock, dirtied like the toast.
  2. **Switcher previews:** swap the switcher card's per-entry icon for a scaled
     surface thumbnail. Pure drawing change in `draw_switcher()`.
- **Cost.** Low once `ugfx_blit_scaled` exists. No new IPC.
- **Test.** Unit-test the box-filter (known src → known averaged dst pixels). One boot:
  open two windows, Alt-Tab, screenshot the card showing real contents.

### B. Overview / Present-Windows (expose)  *(headline UX, builds on A)*

- **Plasma/KWin.** "Present Windows"/"Overview" tiles every window un-overlapped for
  pointer/keyboard pick; bound to a hot corner or Meta+W.
- **twm today.** Only linear Alt-Tab MRU (`switcher.c`).
- **tOS design.** A new compositor mode `overview` (a peer of `sw_overlay`). On a
  hotkey (e.g. a Super chord) or a hot corner (H below): dim the desktop, lay the
  non-minimized windows out in a **non-overlapping grid** scaled to fit, each cell a
  live `ugfx_blit_scaled` thumbnail + title. Click/Enter focuses + exits; Esc cancels.
  Reuses the switcher's input-grab pattern and the modal scrim. Grid cell count scales
  to the live window set — **no fixed cap on the layout** (the existing `MAXW=8` window
  ceiling is the only bound, and that's a separate concern), per the "no artificial
  caps" rule.
- **Cost.** Medium — a layout function + one new mode in the input loop. Drawing is A.
- **Test.** Unit-test the grid packer (N rects in WxH → no overlap, all on-screen).
  One boot: 3 windows, trigger overview, screenshot the tiled field; click a cell,
  assert `[twm] focus <title>`.

### C. Quick-tile / edge snapping  *(high value, isolated)*

- **Plasma/KWin.** Drag a window to the top → maximize; to a side → half-screen; to a
  corner → quarter. Also keyboard (Meta+arrows).
- **twm today.** Dragging is free-floating (`drag` path in the loop); only the green
  button / double-click does full maximize (`toggle_max`).
- **tOS design.** During a title-bar drag, when the cursor enters an `EDGE`-px band,
  paint a translucent **snap preview** rect (the accent-tinted ghost we already draw
  for minimize is the visual vocabulary). On release in that band, set the window's
  geometry to the work-area half/quarter/full via the same `WEV_RESIZE` + reposition
  `toggle_max` already uses. Restore-on-undrag remembers floating geometry exactly like
  `c->sx/sy/sw/sh`. The "work area" is `bar_h..H` per ui.md's dynamic work-area note.
- **Cost.** Medium — drag-state additions + one preview draw; no new IPC (apps already
  honor `WEV_RESIZE`).
- **Test.** Drag a window to the left edge via the harness drag helper; assert the
  reported `[twm] win` client rect is the left half. Screenshot the snap preview.

### D. Show-Desktop / Peek / Minimize-All  *(trivial, satisfying)*

- **Plasma.** `showdesktop` + `minimizeall` applets: a toggle that minimizes everything
  (and a *peek* that hides windows transiently and restores them on a second click —
  `PeekController.qml` / `MinimizeAllController.qml`).
- **twm today.** Per-window minimize only (the genie into the dock).
- **tOS design.** A status/dock affordance (or a Super chord) that toggles **minimize
  all**: record the set of currently-visible windows, minimize them; toggle again to
  restore exactly that set. "Peek" is the same but without committing the minimize
  (just stop drawing windows in `compose()` while held) — cheap because compose already
  clips per-rect. Reuses the existing `min` flag + restore genie.
- **Cost.** Very low.
- **Test.** Open 2 windows, fire the toggle, assert both `[twm] ... min`; fire again,
  assert both restored. Screenshot bare desktop mid-peek.

### E. Dock as a real task manager  *(incremental polish, several sub-wins)*

Plasma's `taskmanager` is the richest applet; several pieces map cleanly onto our dock
(`dock.c`) and the data twm already has:

- **Grouping + group popup.** *Plasma:* `groupingStrategy`, GroupDialog lists a group's
  windows. *twm:* the dock already collapses by `app_for_title` / `find_app_window`;
  add a **hover/long-press group popup** (same card as feature A) when an app has >1
  window, each row a live preview that focuses that specific window. *Free-lunch reuse.*
- **Progress + count badge + urgency.** *Plasma:* `smartlauncheritem` exposes
  `progress`, `count`, `urgent` (from the Unity LauncherEntry / notification protocol)
  → a progress bar across the tile, a count badge (`TaskBadgeOverlay.qml`), and an
  attention pulse. *twm:* extend the `notify()` channel (or a sibling WM message) so an
  app can post `{progress 0..100, count N, attention bool}` keyed to its window; the
  dock draws a thin accent progress arc under the tile, a small count pill, and a
  one-shot **attention pulse** (a few frames of accent glow — we already animate dock
  hover lift, so the damage path exists). Keep glyphs Lucide, tints from `theme.*`.
- **Audio indicator.** *Plasma:* `AudioStream.qml` shows a per-app playing/mute glyph.
  *twm:* deferred until there's an audio subsystem — note it, don't build it.
- **Cost.** Grouping popup: low (it's A again). Progress/count/attention: low-medium
  (one small message + draw). Audio: parked.
- **Test.** A tiny test app posts progress/count/attention; screenshot the tile arc +
  badge + pulse; unit-test the badge/arc geometry math.

### F. Virtual desktops + a pager  *(big model change — do it deliberately)*

- **Plasma.** Multiple **virtual desktops**; the `pager` applet shows them as cells,
  current highlighted, click to switch, **drag a window between cells**, optional wrap
  and drag-to-switch (`pagermodel.cpp`: `currentDesktop`, `VirtualDesktop`).
- **twm today.** One workspace; `zo[]`/`nz` is the only stacking.
- **tOS design.** Give each window a `desktop` index and keep a `cur_desktop`. The
  compositor draws/inputs only windows on `cur_desktop` (filter the `compose()` and
  hit-test loops by `c->desktop == cur_desktop`); the dock's running set filters the
  same way. A small **pager** in the menu bar's status cluster (or left of the dock)
  draws N cells with a current highlight, click to switch, and accepts a window
  **dropped onto a cell** (we already have the full DnD machinery — drag a *window* the
  way Files drags a file). Switching can ride a cheap **slide** transition (J). Desktop
  count is dynamic (add/remove), **never a hard-coded wall**.
  - *Activities* (Plasma's orthogonal second axis — separate widget/window *sets*) are
    **out of scope**: they're a power-user concept with heavy state; virtual desktops
    cover the actual need.
- **Cost.** Medium-high — touches every place that iterates windows. Worth gating
  behind its own task; it unblocks J's slide and the desktop-grid variant of B.
- **Test.** Unit: window-on-desktop filter predicate. Boot: open a window on desk 1,
  switch to desk 2 (assert it's not composited / focus is desktop), switch back
  (assert refocus). Screenshot the pager highlight moving.

### G. Panel visibility modes  *(refines existing auto-hide)*

- **Plasma.** A panel has several visibility modes beyond on/off: **Always Visible**
  (reserves a strut), **Auto-Hide** (off-screen, edge-reveal), **Windows Can Cover**
  (panel normally on top but a raised window may go over it, reveal on edge), and
  **Windows Go Below** (panel always drawn, windows maximize *under* it without
  reserving a strut).
- **twm today.** Binary per ui.md: permanently shown, or fullscreen/registry auto-hide
  with an edge-reveal slide (`update_chrome`). That's "Always Visible" + "Auto-Hide".
- **tOS design.** Add the two middle modes as registry options for the bar/dock:
  - *Windows-go-below* = our current "shown" behaviour (dock floats over content,
    reserves no strut) — already the default; just name it.
  - *Dodge-windows* (the genuinely new, nicest one): keep the bar/dock shown **until a
    window's geometry would overlap its band**, then auto-hide *only* for that window —
    a context-sensitive hide that feels less twitchy than pure edge auto-hide. We
    already compute every window's outer rect each frame; OR the auto-hide trigger with
    "a visible window intersects the bar/dock band."
- **Cost.** Low — it's an extra predicate feeding the existing `want_bar`/`want_dock`
  in `update_chrome`.
- **Test.** Registry-set dodge mode; move a window under the dock band, assert
  `[twm] ...` hide telemetry; move it away, assert reveal. Screenshot both states.

### H. Screen edges / hot corners  *(small glue, big discoverability)*

- **Plasma/KWin.** ScreenEdges trigger actions (top-left → Overview, a side →
  desktop switch, etc.).
- **twm today.** Edge detection exists but only for chrome reveal (`EDGE` in
  `update_chrome`).
- **tOS design.** Generalize edge/corner detection in the main loop into a tiny
  **hot-corner** table mapping a corner dwell to an action (Overview B, Show-Desktop D,
  notification center). Registry-configurable, default a couple of sane bindings.
- **Cost.** Very low (the cursor position is already sampled every frame).
- **Test.** Park the cursor in the configured corner, assert the bound action's
  telemetry (`[twm] overview` etc.).

### I. Anchored popups (the Wayland `xdg_positioner` lesson)  *(correctness)*

- **Wayland.** `xdg_positioner` places a popup by an **anchor rect on the parent + a
  gravity + constraint-adjustment** rules (flip / slide / resize) so a menu never falls
  off-screen and always hangs off the thing that spawned it.
- **twm today.** `WIN_POPUP` windows are **centered** (`cw[k].wx = (W-ow)/2`), and
  menu-bar dropdowns are hand-placed at the tile x with manual clamping (`menubar.c`).
  Fine for the menu bar, wrong for future context menus / tooltips / combo-box
  dropdowns, which must hang off their trigger and **flip** when they'd clip an edge.
- **tOS design.** Add an optional **anchor** to popup creation: an `{x,y,w,h}` anchor
  rect + a gravity hint; twm places the popup adjacent to the anchor and, if it would
  clip, **flips to the opposite side, then slides to fit** — the xdg-positioner
  algorithm, minus the parts we don't need. The menu-bar dropdown becomes the first
  consumer (drop its bespoke clamp), context menus the next.
- **Cost.** Low-medium — a placement helper + one field on the popup metadata.
- **Test.** Unit-test the positioner (anchor near each edge → expected flipped/slid
  rect). Boot: open a menu near the right edge, screenshot it opening leftward.

### J. Cheap compositor effects worth faking

KWin's effect pipeline is enormous; most of it is GPU eye-candy we explicitly skip
(see below). But a handful are cheap on a dirty-rect CPU compositor and add a lot of
"alive":

- **Fade/scale on open & close.** We already animate minimize/restore (the genie). Add
  a short scale-up-from-90%-with-alpha on map and the reverse on close — same
  `anim`/`draw_ghost` machinery, a new `AN_OPEN`/`AN_CLOSE` kind.
- **Dim inactive windows.** A subtle alpha veil over non-focused windows (KWin's "Dim
  Inactive"). One `ugfx_fill_a` per non-top window in `compose()`, behind the focused
  one. Registry toggle, off by default.
- **Dialog-parent dim.** We already scrim behind a `WIN_MODAL` picker; that *is* KWin's
  "Dim Screen for Dialog Parent." Just note we have it.
- **Slide on desktop switch** (pairs with F): translate the outgoing/incoming desktop's
  windows horizontally over a few frames.
- **Cost.** Low each; all reuse existing animation + alpha-fill primitives.
- **Test.** Screenshot mid-open-animation; toggle dim-inactive and screenshot the veil.

### K. Widget / applet model + "Edit Mode"  *(longer-term, architectural)*

- **Plasma.** The desktop and panel are **containments** that host **applets**
  (plugins); an **Edit Mode** (`toolboxes/`, `ConfigOverlay.qml`) lets you add / move /
  configure widgets from a **widget explorer**.
- **twm today.** Bar/dock/control-center/notify are **hard-coded feature files** — a
  deliberate, simpler choice than a plugin system, and right for now.
- **tOS design (sketch, not near-term).** *If* we ever want user-arrangeable chrome,
  the minimal version is a **status-cluster applet list** + a **control-center tile
  grid** described by registry entries, each backed by a C draw/click vtable in twm —
  an internal "applet" interface, **not** a dynamic-plugin ABI (that needs the Phase-2
  userspace runtime; [`app-runtime.md`](app-runtime.md)). Keep it as a known evolution
  path, don't build speculatively.
- **Cost.** High. Parked behind the runtime.

---

## Architecture lessons from Wayland (protocol-level)

Even though twm's shared-memory model is simpler and already works, `wayland.xml`
encodes contract decisions worth adopting as we grow:

- **Role/surface separation & explicit popups.** Wayland splits a bare surface from its
  *role* (toplevel / popup / subsurface). twm conflates this in `cwin` flags
  (`WIN_POPUP`/`WIN_OVERLAY`/`WIN_MODAL`). As popups gain anchoring (I), formalize a
  **popup-with-parent** so a popup tracks its owner (auto-close when the parent loses
  focus / closes) — today launchers are dismissed by an ad-hoc `dismiss_launchers`.
- **Serial-stamped input & grabs.** Wayland tags input events with **serials** so a
  client's request ("start a drag", "show this popup") can be validated against "the
  event that authorized it." twm's DnD/grab is trust-by-construction (we *are* the
  compositor); fine, but if a window ever starts a drag/grab on its own, a serial check
  is the right gate. Note for later, not now.
- **Frame callbacks vs. fixed sleep.** Wayland clients draw on a **frame callback**
  (compositor says "now"), throttling to the display. twm sleeps a fixed `12ms`
  (`sleep_ms(12)`) and apps `SYS_WIN_PRESENT` freely. A frame-callback-style "you may
  draw" signal would cut wasted redraws and is the cleaner long-term throttle — a small
  IPC addition, low priority while everything fits in budget.
- **Buffer-relative damage (we already do this).** Wayland's `wl_surface.damage_buffer`
  is exactly our per-present `dmgx/dmgy/dmgw/dmgh` sub-rect (PROJECT.md). Good — we
  independently arrived at the right design; keep it.
- **Output scale / transform (HiDPI).** Wayland carries per-output `scale` so clients
  render crisp at 2×. twm assumes 1×. Real HiDPI is a future track (it touches the
  toolkit, not just twm) — record it, defer it. Listed in ui.md's "out of scope" too.

---

## Deliberately NOT borrowing (for now)

- **Activities** (Plasma's second window-set axis) — virtual desktops (F) cover the
  need; activities are heavy state for marginal gain.
- **A GPU effects pipeline / compositing redirection** — wobbly windows, blur shaders,
  cube, etc. Our "blur" is a CPU frost (`ugfx_frost`) and that's the right ceiling for
  a software compositor; the cheap effects in J are the whole budget.
- **Multiple monitors (live)** — out of scope (ui.md), a `W`/`H` + output-geometry
  generalization for later.
- **A dynamic applet/plugin ABI** — gated on the Phase-2 userspace runtime; the C-vtable
  sketch in K is the most we'd do before then.

---

## Phasing (cheapest-first, each keeps `make test` green)

Ordered by leverage ÷ cost and by what unblocks what:

1. **`ugfx_blit_scaled`** — the free-lunch primitive. Unblocks A/B/E-grouping.
2. **A. Live previews** (dock hover + switcher) — immediate "alive" payoff on (1).
3. **D. Show-desktop / peek / minimize-all** — trivial, satisfying.
4. **C. Quick-tile / edge snapping** — isolated, high value.
5. **H. Hot corners** + **B. Overview** — corners give Overview a home; Overview is the
   headline that (1)+(2) make cheap.
6. **E. Task-manager polish** (grouping popup, progress/count/attention) — incremental.
7. **G. Panel visibility modes** (dodge-windows) + **J. cheap effects** (fade/scale,
   dim-inactive) — polish passes.
8. **I. Anchored popups** — correctness investment; pays off when context menus land.
9. **F. Virtual desktops + pager** — the big model change; do it as its own task. Its
   slide transition reuses J.
10. **K. Applet/edit-mode** + **Wayland frame-callbacks / HiDPI** — parked behind the
    Phase-2 runtime.

Per the testing pyramid ([`testing.md`](testing.md)): each item lands with host
**unit** checks for its pure math (box filter, grid packer, snap geometry, positioner,
desktop filter), **one disposable boot** that exercises it, and a **screenshot** — not
a new permanent e2e journey. Reuse the existing twm telemetry (`[twm] focus …`,
`[twm] win …`, `[twm] … min`) for assertions; add a terse line per new mode
(`[twm] overview`, `[twm] desktop N`) on the same pattern.

## Cross-references

- Desktop chrome look + fullscreen/auto-hide: [`ui.md`](ui.md)
- `~/Desktop` containment (the Plasma folder-view analog, landed): [`files-and-desktop.md`](files-and-desktop.md)
- Where this sits in the overall plan: [`roadmap.md`](roadmap.md) (Phase 5, desktop maturity)
- Why a plugin ABI waits: [`app-runtime.md`](app-runtime.md)
- Compositor internals + syscall map: [PROJECT.md](../PROJECT.md), [`twm.h`](../user/twm/twm.h)
