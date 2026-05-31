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
- **Dock.** Cleaner: pinned apps, a thin separator, then running-but-unpinned
  apps, with a focus underline and a minimized dot (already partly there). Rounded
  translucent panel, subtle magnification on hover (optional, later).
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
  screen** — the top bar and dock are **hidden**. Toggle off restores the floating
  geometry (as today).
- **Edge reveal.** While anything is fullscreen (or if the user enables global
  dock/bar auto-hide), the bar and dock auto-hide and **reveal on a screen-edge
  hover**: cursor within `EDGE` px of the **top** slides the bar down; within
  `EDGE` px of the **bottom** slides the dock up. They retract when the cursor
  leaves the revealed strip (after a short `LINGER`). A slide animation, not a
  pop.

### State machine (per chrome element: bar, dock)

```
HIDDEN  --cursor in edge zone-->  REVEALING --(anim done)--> SHOWN
SHOWN   --cursor leaves + LINGER ticks--> HIDING --(anim done)--> HIDDEN
```

- `auto_hide = fullscreen_active || reg_bool("ui.bar.autohide") (resp. dock)`.
  When `auto_hide` is false the element is permanently `SHOWN` (current behaviour).
- `reveal_zone(bar)` = `y < EDGE`; `reveal_zone(dock)` = `y >= H - EDGE`.
- An element that is `SHOWN` also counts the strip it occupies as its hover zone,
  so it doesn't retract while the pointer is on it.
- Animation: lerp an offset (bar slides in `-bar_h..0`, dock slides in
  `+dock_h..0`) over ~8 frames; the damage rects already drive compositing.

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

## Out of scope (for now)

Multiple monitors, a real compositor effects pipeline (blur behind translucency is
faked with alpha fills), and theming beyond the registry's `theme.*` keys. The
toolkit (`ui::`) grows to support menus/toasts as part of phases 3–4.
