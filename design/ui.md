# Design guideline — tOS desktop UI modernization

> Status: **design + roadmap.** The current desktop (twm) has a translucent top
> bar with a logo + active-app title + clock, a centered dock of app tiles, and
> floating windows with macOS-style traffic-light controls. This is the plan to
> turn it into a polished, macOS-like environment: a real menu/status bar, a
> notification + widget system, a cleaner dock, and proper fullscreen with
> auto-hiding chrome.

## Target experience

```
┌────────────────────────────────────────────────────────────────────────┐
│  logo  App   File Edit View        ⌗      ▣ 42%  🔉  Fri 14:09   🔔      │  ← menu / status bar
├────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│                         desktop / windows                                │
│                                                       ┌──────────────┐   │
│                                                       │ notification │   │  ← toast, top-right
│                                                       └──────────────┘   │
│                                                                          │
│                         ╭───────────────────────────╮                   │
│                         │  ▦  ▦  ▦  │  ▦   ▦         │                   │  ← dock (pinned | running)
│                         ╰───────────────────────────╯                   │
└────────────────────────────────────────────────────────────────────────┘
```

- **Top bar (menu + status).** Left: the OS logo as an "action" menu (about,
  sleep, restart, shut down) and the **focused app's name + menus** (App / File /
  Edit / View) — the macOS app-menu model. Right: **status items** (battery/■,
  volume, network later), a **clock**, and a **notifications** bell that opens a
  notification center. The bar is the single source of global actions.
- **Dock.** macOS layout: **Launchpad, then the pinned apps on the left**, then a
  **thin vertical separator**, then the **running-but-unpinned apps to its right**, with a
  focus underline and a minimized dot (already partly there). The separator appears **only
  when ≥1 running-unpinned app exists** (no trailing divider on an all-pinned dock).
  `rebuild_dock()` already emits the clusters in this order — it just needs to record the
  boundary (index after the last pinned tile) and the renderer draws a 1px divider there when
  the running cluster is non-empty. Rounded translucent panel, subtle magnification on hover
  (optional, later).
- **Launchers are a single-instance group.** Spotlight (Super+Space), Launchpad (Super tap)
  and the Clipboard manager (Super+V) are transient, modal-lite launchers — **only one is up
  at a time.** Summoning any of them first dismisses the others (a `dismiss_launchers(except)`
  helper at the top of each `summon()`), so you can never have Spotlight floating over
  Launchpad. Summoning the one that's already open still toggles it closed (current Launchpad
  behaviour), generalised to all three.
- **Notifications + widgets.** Apps post a notification (title + body + optional
  icon); twm shows a **toast** top-right that auto-dismisses, and stacks recent
  ones in a **notification center** panel toggled from the bell. Widgets (clock,
  system stats) are the same drawing primitives in a slide-over panel — later.
- **Window management.** Keep floating windows with shadows + traffic lights;
  polish snapping, focus ring, and the open/close/minimize animations. **Maximize
  becomes true fullscreen** (below).

## Fullscreen + auto-hide (the headline change)

Today "maximize" fills the *work area* (between the bar and dock). The new
behaviour, macOS-style:

- **Fullscreen** (green button / double-click title): the window takes the **whole
  screen** — the top bar, the dock, **and the window's own title bar (its close/max/min
  traffic lights)** are all **hidden**, so the app content fills the entire display. Toggle
  off restores the floating geometry (as today).
  - *Today's gap:* `toggle_max()` resizes the maxed client to `W × (H − TH)`, leaving the
    app's own title bar permanently on-screen in fullscreen — which reads as "strange." The
    fix: a maxed window's client is **full height (`W × H`)** and its title bar joins the
    auto-hidden set (it slides up off-screen with the menu bar, not drawn in the work area).
- **Top reveal = both bars together.** While anything is fullscreen (or global bar
  auto-hide is on), the menu bar **and** the focused fullscreen window's title bar are one
  reveal group: a cursor within `EDGE` px of the **top** slides **both** down together (menu
  bar above, app title bar just below it); the dock reveals independently on a **bottom**
  edge hover. A slide animation, not a pop.
- **Hide when leaving the title bar downward.** The top group does **not** retract on any
  small move — it retracts only when the cursor crosses **out of the title-bar band into the
  app content** (moving down past the bottom of the app's title bar), after a short `LINGER`.
  So you can travel from "hover top → menu bar → app title bar → press a traffic light" and
  the chrome stays; the moment you dive into the content it slides away. (The dock keeps the
  simple bottom-edge reveal/leave behaviour.)

### State machine (two reveal groups: the TOP group and the dock)

The **top group** = the menu bar **+** the focused fullscreen window's own title bar (they
reveal/hide as one). The **dock** is independent.

```
HIDDEN  --cursor in reveal zone-->  REVEALING --(anim done)--> SHOWN
SHOWN   --cursor leaves hold zone + LINGER ticks--> HIDING --(anim done)--> HIDDEN
```

- `auto_hide(top)  = any_fullscreen() || reg_bool("ui.bar.autohide")`.
  `auto_hide(dock) = any_fullscreen() || reg_bool("ui.dock.autohide")`.
  When false the element is permanently `SHOWN` (current behaviour — no regression).
- **Reveal zone.** top group: `y < EDGE`; dock: `y >= H - EDGE`.
- **Hold zone** (where `SHOWN` does *not* retract):
  - top group = the **whole revealed band** it occupies — the menu-bar strip **plus** the
    app's title-bar strip below it. The cursor leaving *downward into content* (`y` past the
    bottom of the app title bar) is what starts `HIDING`; horizontal moves within the band
    keep it shown, so you can reach the traffic lights.
  - dock = the strip the dock occupies (bottom-edge leave starts `HIDING`).
- Animation: lerp an offset — the menu bar slides `-bar_h..0`, the app title bar slides with
  it (its top tracks `bar_bottom`), the dock slides `+dock_h..0` — over ~8 frames; the damage
  rects already drive compositing. (`bar_linger`/`dock_linger` + `SLIDE`/`HIDE_LINGER` in
  `twm.c` are the existing knobs; add a `topbar_linger` shared by the menu bar + app title
  bar, keyed off "cursor below the app title bar.")

### Compositor changes

- The **work area** becomes dynamic: `work_top = bar_visible ? bar_h : 0`,
  `work_bottom = H` (dock floats over content, doesn't reserve space — like macOS).
  Window maximize/tile uses the work area; fullscreen ignores it (full `H`).
- `draw`/`compose` gain a per-element visible-offset so a partially-slid bar/dock
  renders correctly and only its moving band is dirtied.
- A `fullscreen` flag on `struct cwin` (distinct from the current `maxed`), plus a
  saved geometry to restore to.
- The cursor-position handling already runs every frame; add edge detection there.

## Notification API

A small kernel/IPC addition or a twm-owned channel:

```c
/* userspace */
void notify(const char *title, const char *body);   /* posts to the WM */
```

twm keeps a ring of recent notifications, renders the newest as a top-right toast
(fade in, ~4 s, fade out), and lists them in the notification-center panel. Start
with a twm syscall/`WM_POST`-style message carrying a small struct; no persistence
needed initially.

## Phasing (keep `make test` green; tests drive the dock by its serial-reported
icon coordinates, so layout changes are safe as long as those are still printed)

1. **Fullscreen + auto-hide bar/dock + dock cleanup** — the headline, highest-value
   change. (Tracked as a concrete implementation task.)
2. **Status bar items** — clock already there; add a right-side status cluster and
   wire the clock format from the registry ([settings.md](settings.md)).
3. **Notifications** — `notify()` + toasts + a simple center.
4. **App menus** — the focused app advertises a menu; the bar shows App/File/Edit.
   Needs an app→WM menu protocol (extends the `ui::` toolkit).
5. **Widgets / notification center panel** — slide-over with clock + system stats.
6. **Dock magnification + open/close polish** — eye-candy last.

## Iconography — Lucide glyphs everywhere (guideline)

Every glyph in the UI is a **Lucide (MIT) line icon** — the clean, minimal set that
matches the macOS-ish chrome (NOT Material). Glyphs are fetched + rendered once by a
generator and baked into a committed header as **white ARGB alpha masks** (only the
alpha matters), then drawn recoloured to a theme token via `ugfx_blit_tint`. Two
parallel sets:

- **`tools/genstatus.py` → `user/lib/statusicons.h`** — the menu-bar status cluster
  (wifi/volume/battery/bell, the Control Center `sliders-horizontal`, the notification
  `chevron-down`/`up` + `x`). Drawn by twm via `draw_status_glyph`.
- **`tools/genglyphs.py` → `user/lib/glyphs.h`** — the general toolkit app-glyph set any
  `ui::` app can use (the Settings rows' leading icons, etc.). `ui::Button` takes an
  optional `icon` (a `glyphs_argb[...]` mask) + `icon_tint`; with an icon (or a `value`)
  it left-aligns its label for a settings-row look.

**Guideline:** a new app/affordance that wants an icon uses a Lucide glyph from one of
these headers — add the slug to the generator's list and regenerate (needs network +
`rsvg-convert` + PIL; commit the header so the OS build needs none of them). Don't
hand-draw glyphs or mix in another icon family. Tint to `TH_TEXT` (ink), `TH_MUTED`
(secondary), or `TH_ACCENT` (interactive) so icons track the theme.

## Toolkit layout — `ui::Layout` (header-only, in `ui.h`)

The toolkit is **retained-mode with a flat `kids` list of absolute rects**: each app's
`layout()`/`layout_widgets()` computes every widget's `r` and the window blits them.
Hand-computing those rects (a running `y` cursor + `w - 2*pad` widths) is the repetitive,
error-prone part. **`ui::Layout`** owns it: a one-axis linear placer — a **column**
(`LAY_COL`) or **row** (`LAY_ROW`) — that splits a bounds rect among its items.

```cpp
ui::Layout col(ui::LAY_COL, /*gap*/8, /*pad*/14);
col.add(&topbar, 36);     // fixed extent (px along the main axis)
col.add(&body,    0);     // extent 0 = STRETCH: shares the leftover evenly
col.space(12);            // reserve a gap with no widget
col.arrange(ui::Rect{0,0,w,h});   // writes each widget->r in place
ui::Rect b = col.rect_of(1);      // query a slot (e.g. to nest another Layout)
```

- **Fixed vs stretch.** `extent > 0` pins a size; `extent == 0` stretches, and the
  leftover (after fixed items + gaps) is split among stretch items, the remainder spread
  one px at a time so it sums exactly. `space(n)` reserves a region with no widget
  (`add(nullptr, n)`), queryable via `rect_of(i)`.
- **Nesting is just `rect_of`.** Compose a UI by arranging an outer Layout, then feeding
  a child's slot rect to an inner one (column → row → column…). No tree, no retained
  layout objects — call `arrange()` from `layout()`/`on_resize()` each time.
- **In use:** **Settings** lays its full-bleed top bar + the rows column out with it
  (replacing a hand-rolled y-cursor); new toolkit apps should reach for it instead of
  open-coding rects. Cap: `MAXI = 24` items per Layout.

> A linear placer (not flex/grid/constraints) is deliberate — it covers the bars, lists,
> and sidebars tOS apps actually have, stays a few dozen lines, and needs no allocation.
> Note `fastfetch` is **not** a toolkit app: it is a CLI *package* (the shell login
> banner) per [`packaging.md`](packaging.md)'s app-vs-package split, not a `/Apps` bundle.

## Drag-and-drop (toolkit hooks)

The DnD protocol (kernel `drag.c` + the compositor; **landed 2026-06-11**) surfaces in
the toolkit as three `ui::Window` members:

- **`begin_drag(type, label, data, len)`** — a source arms a typed payload
  (`DRAG_FILES`/`DRAG_TEXT`/`DRAG_IMAGE`) from its `on_drag()` once the gesture leaves a
  draggable item. `label` is the text the compositor paints in the **ghost chip**.
- **`on_drag_over(x, y)`** (virtual) — fires while a drag hovers this window so it can
  highlight a drop zone; `x < 0` means the drag left. Default: ignored.
- **`on_drop(x, y, type, data, len)`** (virtual) — the drop landed here; read the typed
  payload and act (move the files, insert the text). The toolkit reads the bytes via
  `drag_payload()` and hands them in.

The compositor owns the visual session (the ghost + hit-test + `WEV_DRAG`/`WEV_DROP`
routing), so an app only writes these hooks — the same brokered model as windows/input.
First consumer: **Files** drag-to-move (`user/files/files.cpp`).

## Out of scope (for now)

Multiple monitors, a real compositor effects pipeline (blur behind translucency is
faked with alpha fills), and theming beyond the registry's `theme.*` keys. The
toolkit (`ui::`) grows to support menus/toasts as part of phases 3–4.
