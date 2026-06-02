# tOS — next steps

The road ahead. How the system works **today** is in [PROJECT.md](PROJECT.md);
this file tracks what's **left** plus a terse log of what's landed. Every item
keeps `make test` green (BIOS + UEFI) before it's checked off.

**Status:** **19 e2e smoke/journey tests** (`make test`, BIOS+UEFI under QEMU) + **28 host
unit tests** (`make unit`, no QEMU) — restructured from a flat 49-test all-e2e suite into a
pyramid; the policy + tiers live in [`design/testing.md`](design/testing.md). tOS is
early-to-mid development — see [`design/roadmap.md`](design/roadmap.md) for the phased plan.

Legend: `[ ]` not started · `[~]` partial · `[x]` done (see the changelog).

---

## Open — the road ahead

### Toolkit & desktop UI
- [x] **Status-bar cluster (ui.md phase 2).** *(done — see changelog)*
- [x] **Notifications / toasts (ui.md phase 3).** *(done — see changelog)*
- [x] **Notification center — dirty-rect artifacts (bug).** *(done — see changelog;
  Clear now invalidates the OLD tall panel before shrinking, and CC/toast hover repaint
  their full shadow halo.)*
- [~] **Notification interaction model + expand affordance.** (a)(b)(c) **done** (see
  changelog): the toast expands **only via its chevron button** (a whole-toast click is
  consumed but no longer toggles); the **Notification Center rows now carry their own
  per-row chevron + expand** (collapsible rows only, wrapping the full body and growing
  the panel); the chevron is the **baked Lucide `chevron-down`/`chevron-up` glyph**
  (`tools/genstatus.py` → `statusicons.h`, blit via `draw_status_glyph`/`ugfx_blit_tint`).
  Still open: (d) *(lower priority)* **clicking a notification focuses/opens its sender**
  — needs a small **notification API** (the poster declares an app/pid + a click action
  carried on `struct notif`, so twm can route the click). Design it in [`design/ui.md`](design/ui.md).
- [~] **Menu-bar API for apps (#6).** The menu-tile machinery + the **app tile**
  (the focused app's name → About / Quit, Quit = WEV_CLOSE) are **done** (compositor-
  owned; see changelog). Still open: an **app→WM protocol** so apps declare their *own*
  File / Edit / Help tiles + custom items (`SYS_WIN_SETMENU` + a `WEV_MENU` selection
  event + a `ui::Window` menu API). See [`design/ui.md`](design/ui.md).
- [x] **OS-logo dropdown (#8).** *(done — the logo opens the system menu: About This
  tOS / Preferences… / Restart / Shut Down. See changelog.)*
- [ ] **Desktop UI roadmap (beyond fullscreen).** Status-bar items, a
  notification/toast system, app menus (#6), dock magnification, and hiding a
  window's *own* chrome in fullscreen for a purer macOS look. See
  [`design/ui.md`](design/ui.md).
- [ ] **Grow the toolkit + port more apps.** Add a layout system and menus, then
  port `term` / `fastfetch` (and new apps) onto the toolkit.
- [~] **Files app polish.** **Ctrl+C/X/V done** — Copy/Cut/Paste over the clipboard ring,
  as keybinds *and* context-menu items (Paste appears when a file is on the clipboard);
  Cut+cross-dir Paste moves the file (deletes the source), Paste dedupes (`copy of X`)
  rather than clobbering (see changelog). Still open: **rename** (needs an in-window text
  entry), **scrolling for long directories** (Global ScrollBar), and **drag-to-move**
  between folders (DnD foundation).
- [ ] **Live resize preview + content reflow.** Resize snaps on mouse-up and keeps
  cells at their (row,col) without reflowing wrapped lines. A live outline while
  dragging the grip, and reflowing long lines, would polish it.
- [x] **macOS-style Alt-Tab animation (#7).** *(done — a centred card of window tiles
  with an animated selection highlight; hold-Alt / release-to-commit via the modifier
  mask, plus click-a-tile / Enter to commit, arrows to navigate, and ESC to cancel.
  See changelog.)*
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
Ctrl+Backspace / Ctrl+Delete word-delete, the Delete key, **and shift-select**.
**Still needs foundations:**
- [x] **Shift-select** (Shift+arrow / Shift+click extend the selection). *(done — the
  toolkit now sees Shift: nav keys carry the xterm modifier param decoded by the CSI
  reader, and a press packs `WEV_MOUSE_SHIFT` for Shift+click. See changelog.)*
- [ ] **I-beam cursor over selectable text.** Blocked on an **app→compositor
  cursor-shape protocol** (none exists — twm composites the cursor and doesn't know
  about widget regions; the only cursor API today is the text-console `paint_cursor`).
- [ ] **System primary-selection + cross-app text drag.** Blocked on the **DnD
  protocol**. Lets a selection be the paste source and be dragged between windows.

### Input / event foundations (unblock the above)
- [x] **Richer key events.** *(done.)* `WEV_KEY` now carries **modifier flags** in its
  upper bits (`WEV_KEY_PACK`/`WEV_KEY_MODS`; legacy readers still mask `a & 0xff`),
  `kbd_mods()` exposes the live `KMOD_*` mask as a syscall, and the compositor posts
  **`WEV_KEYUP`** when a modifier is released (drives the Alt-Tab overlay). A mouse press
  also packs **`WEV_MOUSE_SHIFT`** for shift+click. This unblocked shift-select + clean
  chord routing. (A full per-key key-UP for non-modifier keys is still future, if needed.)
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
  clipboard ring (`SYS_CLIP_*`) — **landed in `TextField`** (^C/^X/^V/^A) and the
  **terminal** (mouse click-drag grid selection + **Ctrl+Shift+C/X/V**, so plain Ctrl+C
  stays `^C` to the pty; `term_copy`/`term_paste`, covered by `t_term_copy` +
  `t_term_paste`) **and the Files manager** (plain Ctrl+C/X/V copy/cut/paste of files
  over the clipboard ring; see changelog). The chord set is now consistent across all
  three editors.
- [ ] **Pocket Dimension — a per-session shelf (Super+D).** ⏸️ **SET ASIDE — do not
  implement unless explicitly requested.** A slide-out panel from the
  **left** screen edge holding media stashed for later (typed payloads: file ref / text
  / image+thumbnail). *Interim with no DnD:* an "Add to Pocket" action + the Super+D
  shelf with per-item buttons; add the drag gesture once DnD lands. Complements the
  clipboard (transient single payload) with a curated multi-item staging shelf.

### Platform / runtime / storage
- [ ] **A real shell + scripting (replace the hardcoded command dispatch).** Today
  `user/shell/shell.c` is one ~530-line `if/else if (streq(line, ...))` chain: every
  command is a baked-in special case (it only falls through to `fork`+`exec` for an
  external program), and there's no scripting, variables, quoting, pipes, redirection,
  or globbing. Build a proper shell instead, roughly:
  1. **First step (low-risk cleanup):** drop the `help` command and trim the one-off
     demo/diagnostic built-ins that don't earn their place (`spawn`/`fork`/`smp`/
     `crash`/`colors`/`ticker`-style demos) — prefer turning real utilities into
     external programs in `/System/bin` over hardcoding them in the shell.
  2. A real **lexer + parser** (tokenise → an AST), so a line is parsed, not
     string-matched: **quoting** (`'…'`/`"…"`), **variables + environment** (`$VAR`,
     `export`, assignment), **`$(…)`/backtick** command substitution.
  3. An execution model: a small set of true **builtins** (`cd`, `export`, `exit`, …)
     vs **external commands resolved on `PATH`** (`/System/bin` first); **pipes** (`|`),
     **redirection** (`>` `>>` `<`), and **`;` / `&&` / `||`** sequencing; **globbing**
     (`*`/`?`); background jobs (`&`) + basic job control.
  4. **Shell scripts** — a shell *is* its own interpreter (no separate VM needed): add
     control flow (`if`/`for`/`while`/`case`, functions) and the ability to **source a
     `.sh` file** (run a file of commands; `#!` shebang dispatch for executables). With
     (2)–(3) in place this is mostly reusing the parser + an input source.
  Bash-compatibility is the north star, but a clean POSIX-sh-like core first is the
  pragmatic path. Big win for usability and for running the OS's own setup as scripts.
- [x] **Use all of RAM — 3 GB cap removed.** *(done — see changelog.)* `vmm` now parses
  the firmware **e820** map (read in-kernel from QEMU `fw_cfg`'s `etc/e820`, no boot-code
  changes) into sorted RAM regions and builds a **multi-region** identity map + frame
  allocator that **skips the sub-4 GiB PCI hole and uses the RAM remapped above 4 GiB**.
  The Makefile cap is gone (`MEM ?= 8G`). Proven by `memtest` (a new shell tool) touching
  3.7 GiB @4G and 7.8 GiB @8G with **zero mismatches** — i.e. allocations cross the hole
  into real above-4G RAM. **UEFI follow-up landed (2026-06-02):** the loader now reads the
  firmware **GetMemoryMap** to size its transition identity map to *all* of RAM — without it
  OVMF loaded the EFI app above 4 GiB and the loader's old 0–4 GiB map #PF'd on the CR3 switch
  before the kernel ran (the `MEM ?= 8G` default was unbootable on UEFI). See changelog.
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
  [`design/installation.md`](design/installation.md). Note: the live env boots from the
  build image (firmware loads it), so it needs **no new driver to run** — only *writing*
  the install needs a writable block device. Start with the existing ATA second disk (or
  **virtio-blk** in QEMU) to build/test the flow, then ride the Phase-4 storage drivers
  for metal.
- [ ] **Device drivers (roadmap Phase 4).** Today: PCI enum, ATA PIO, PS/2, VBE/GOP fb.
  To reach real install targets + bare metal, VM-first so the installer is QEMU-testable:
  **virtio-blk** → **AHCI/SATA+DMA** → **NVMe** → **GPT + ESP(FAT) writer** → **USB
  (xHCI + core + HID + mass-storage)** → **ACPI (uACPI/LAI for MADT + shutdown)** →
  **virtio-net/e1000 + TCP/IP**. Graphics stays **software-rendered on the firmware fb**
  on metal; GPU accel is **VM-only** (virtio-gpu + virgl/Venus). Linux drivers **don't
  port** (no stable ABI, GPLv2) — reference the specs / OSDev / BSD drivers and clean-room
  it. Full rationale + priority table in [`design/roadmap.md`](design/roadmap.md) Phase 4.
- [ ] **Growable / smarter filesystem.** Files are stored contiguously and a metadata
  change rewrites the whole slot table; the partition is fixed-size. Extent/indirect
  blocks, a runtime-sized partition, and journaling the table would make it robust.
- [~] **Terminal scrollback paging.** Done via the scroll wheel **and Shift-PgUp/PgDn
  keyboard paging** (256-row ring; see changelog). Still future: a configurable ring size.

### Smaller ideas
- A heap / `sbrk` for user programs (today a program is its static image + stack).
- Calibrate the LAPIC timer instead of a fixed count.

---

## Done (changelog)

Terse one-liners; the prose lives in git history + PROJECT.md. Newest first.

- **UEFI boot fixed above 4 GiB — loader identity-maps ALL of RAM (2026-06-02).** With the
  `MEM ?= 8G` default the UEFI path was unbootable: OVMF loads the EFI app high in RAM
  (~5 GiB, above the 4 GiB PCI hole), but `uefi/uefi.c`'s transition page tables only mapped
  the low **0–4 GiB**, so the instant `mov %cr3` installed them the next instruction fetch
  (still at the loader's ~5 GiB RIP) `#PF`'d — and the kernel's `*bi` read would have too.
  The kernel's e820 work taught the *kernel* about above-4G RAM but never the loader. Fix:
  the loader reads the firmware **GetMemoryMap** (`ram_top`), finds the top of RAM, and
  identity-maps `[0, top)` with one PD per GiB from a single AllocatePages arena (`build_tables`)
  — scales with the machine, no fixed cap. Verified: 8G UEFI now reaches `desktop ready`
  (`identity-mapping 9 GiB`, kernel `2 regions across the 4 GiB hole`). `t_ram_scales` gained a
  **6G case and now also runs on the UEFI path** as the regression guard (the kernel-only path
  can't catch a loader bug). `make test` **30/30** (BIOS 19 + UEFI 11).

- **Settings app uses Lucide glyphs + toolkit Button icon support (2026-06-02).** The Settings
  app was plain centred-text rows — visually inconsistent with the Lucide menu bar above it.
  Added optional **leading-icon + right-aligned value** support to `ui::Button` (`icon`/`icon_sz`/
  `icon_tint`/`value`/`value_fg`; with neither set it stays a centred button, so every existing
  caller is unaffected), and a sibling generator **`tools/genglyphs.py` → `user/lib/glyphs.h`**
  that bakes a reusable Lucide app-glyph set (layers/image/palette/panel-bottom/panel-top/clock/
  timer/calendar/settings/toggles) as tintable white masks blitted via `ugfx_blit_tint` — same
  pipeline as the status cluster's `genstatus.py`/`statusicons.h`. Each settings row now shows a
  themed Lucide glyph, a left-aligned label and its value (On-toggles read in the accent).
  Screenshot-verified; toggling still fires (`[settings] set …`). `make test` 30/30.

- **Multi-region frame pool: use RAM across the 4 GiB hole (2026-06-02).** The pool mapped
  RAM as one contiguous span `[0, ram_size)`, so above ~3 GiB it walked into the PC PCI/MMIO
  hole (handing out MMIO "frames") and ignored the RAM QEMU remaps above 4 GiB — hence the
  3 GB Makefile cap. Now `vmm` reads the firmware **e820** map in-kernel from QEMU `fw_cfg`
  (`etc/e820`; **no bootloader/`boot_info` changes**), parses it into 2 MiB-aligned sorted
  RAM regions (`ram[]`), and the identity map (`build_low_map`, per-page `frame_in_ram`) +
  bump allocator (`bump_next`) + contig allocator are all **multi-region**: they map only
  real RAM and **skip the hole**, crossing into the above-4G region as the pool fills.
  `vmm_ram_bytes` reports real RAM (hole excluded). The `MEM:=3G` cap is gone (`MEM ?= 8G`).
  New `memtest` shell tool mmaps most of RAM and write/read-verifies it; **PASS, 0 mismatches**
  at 4G (3.7 GiB, crosses the hole) and 8G (7.8 GiB). BIOS suite 19/19.

- **Notification toast: shadow-smear killed + X dismiss + global shadow switch (2026-06-02).**
  The toast drop-shadow smeared on hover and left a ghost after it vanished: its `dirty_toast`
  band had 8/4 px margins but the shadow bleeds ~18 px, so the halo's left/top strip never got
  cleaned. Fixed by covering the full halo AND a `toast_kill()` that lingers the footprint in
  the dirty set for a few frames after death (pixel-diff verified: **zero residue**). Added a
  baked Lucide **`x` dismiss button** beside the chevron (instant close). And, because soft
  shadows + damage tracking are a perennial smear source across dock/windows/CC/bar, a single
  **global kill switch** (`ugfx_set_shadows`, every shadow routes through `ugfx_shadow`) wired
  to the **`ui.shadows` registry key** — `reg set ui.shadows false` removes ALL shadows at once
  (verified). Default on (the toast is fixed).

- **Files manager Ctrl+C/X/V copy/cut/paste (2026-06-02).** The Files app had only a
  context-menu "Copy"; added full clipboard editing over the kernel ring (`SYS_CLIP_*`):
  **Ctrl+C** copies the selected file's bytes (`CLIP_FILE`), **Ctrl+X** copies + remembers
  the source for a move, **Ctrl+V** writes the active clip into the current dir — deduping
  to `copy of X` instead of clobbering, and a pending Cut `rmrf`s the source after a
  successful cross-dir write. Same actions added to the right-click menu (Copy/Cut, plus a
  **Paste** item that appears only when a file is on the clipboard). The chords flow through
  the existing `on_key` (the ListView ignores ^C/^X/^V so they fall through to the app).
  Screenshot + disk-proven: Ctrl+C→Ctrl+V produced `copy of zzzclip.txt` (visible in Files,
  read back via `ls`), and a Cut→cross-dir Paste moved a file into `Documents`. BIOS 19/19.

- **Shift-select completed: Shift+click + verification (2026-06-02).** Shift+arrow / word /
  Home / End selection already worked (nav keys carry the xterm modifier param; the toolkit
  CSI decoder extracts Shift). The missing piece was **Shift+click**: the compositor now ORs
  a new `WEV_MOUSE_SHIFT` (button bit 0x80) into a forwarded **press** when Shift is held, and
  `TextField::on_mouse` reads it to **extend** the selection (seed the anchor from the caret,
  move the caret to the click) instead of resetting it — flowing through `on_mouse(x,y,btn)`
  with no signature change. Screenshot-verified (Shift+Left then Shift+Ctrl+Left highlighted
  "fox jumps" in Notepad). Closes the **shift-select** + **richer-key-events** items; BIOS 19/19.

- **Terminal Shift-PgUp/PgDn scrollback paging (2026-06-02).** The 256-row scrollback was
  scroll-wheel-only; added keyboard paging. The keyboard (`kernel/drivers/keyboard.c`) now
  emits the new app-level bytes `KEY_TERM_PGUP`/`KEY_TERM_PGDN` for **Shift+PgUp/PgDn** (via
  a `key_page()` helper), while plain PgUp/PgDn still forward `ESC[5~`/`ESC[6~` to the app.
  The terminal (`user/term/term.c`) consumes those bytes to page `view_off` by a screenful
  (`rows-1`, one line of overlap), clamped to the ring — like xterm. Screenshot-verified
  (paged back to the boot banner + a buried marker line, then back to live); BIOS 19/19.

- **Notification expand affordance: Lucide chevron + per-row expand (2026-06-02).** The
  toast/center expand control is now the baked **Lucide `chevron-down`/`chevron-up`** glyph
  (added to `tools/genstatus.py` → `user/lib/statusicons.h`, blitted tinted via
  `draw_status_glyph`) instead of the old `"^"/"v"` text. The toast **expands only via the
  chevron** now — a click anywhere on it is still consumed (never leaks to a window behind)
  but only the chevron hitbox toggles. The **Notification Center rows gained their own
  per-row chevron + expand**: a row whose body would truncate shows a chevron; clicking it
  wraps the full body (`tu_wrap`, ≤`NC_MAXLINES`) and grows that row — and the panel —
  while the rest stay collapsed. Per-row state is `note_exp[]` (ring-indexed, reset when a
  slot is reused or on Clear); `nc_layout`/`draw_nc` now sum **per-row heights**, and the
  expand toggle re-uses the shadow-halo dirty-before/after-resize pattern. Screenshot-verified
  (toast collapse/expand, row collapse/expand, no shadow artifacts); BIOS 19/19.

- **Notification-center dirty-rect artifacts fixed (2026-06-02).** Two SHADOW-HALO-INVARIANT
  violations in `user/twm/twm.c`. (1) **Clear left a ghost list:** `dirty_nc()` re-runs
  `nc_layout()` *after* `notes_n` is zeroed, so it only invalidated the new short rect and
  the taller old list hung as stale rows. Fix: `dirty_nc()` the OLD (tall) panel + halo
  *before* zeroing `notes_n`, then the NEW short one — the union repaints the vacated rows.
  (2) **Hover smeared the shadow:** the cursor-move repaint used the CC *body* rect
  (`add_dirty(cc_x,cc_y,cc_w,cc_h)`), not the `*_SHADOW_SP`-padded `shadow_box`, and the
  live toast wasn't repainted on move at all — so sweeping the cursor through their
  drop-shadows punched holes. Fix: `dirty_cc()` (halo-padded) on move + `dirty_toast()`
  while a toast is live. Screenshot-verified (clear → no ghost rows, clean halo); BIOS 19/19.

- **Crisp icons: smooth resampler + hi-res masters + Lucide bar glyphs (2026-06-01).**
  The launchpad tiles looked blocky next to the dock because `ugfx_blit_scaled` was
  **nearest-neighbour** and the icons were baked at only 48px — the dock blits 48→48
  (1:1, crisp) but the launchpad upscaled 48→64 (jaggy). Fixes: (1) `ugfx_blit_scaled`
  is now a **premultiplied-alpha resampler** — area-average on downscale, bilinear on
  up — so every scaled icon (launchpad, switcher, Files lists) is smooth; the Files
  app's private NN copy now forwards to it. (2) `.app` bundle `icon.argb` are baked at
  a **128px master** (genicons.py; in-binary fallback arrays stay 48px, file-type icons
  bumped to 64px); dock/switcher/launchpad scale the master *down* to their display
  size → crisp everywhere. (3) The hand-stacked 1px-rectangle status glyphs
  (volume/battery/wifi/bell) are replaced by **Lucide (MIT) line icons** — fetched +
  rendered hi-res via `tools/genstatus.py` (librsvg + PIL Lanczos), baked as tintable
  white alpha masks in `user/lib/statusicons.h`, blitted with the new `ugfx_blit_tint`
  (recoloured to the theme ink / accent). The 128px bundles overflowed the 1 MiB tosfs
  image, so the disk grew to **2 MiB** (`TOSFS_DISK_SECTORS` 2048→4096, synced in
  `boot/stage1.asm` + the Makefile UEFI `UFS_SECTORS`), and `fs_sread/fs_swrite` now
  **chunk** into ≤128-sector ATA transfers so the directory table can exceed the 8-bit
  ATA count — lifting the ~1.5 MiB FS ceiling so the disk scales. BIOS suite 19/19 green
  (incl. fs_persist/crud/many_files); launchpad + bar screenshot-verified. The Control
  Center status item also moved to Lucide `sliders-horizontal` (`draw_cc_button`), so the
  whole menu-bar cluster is now one consistent icon set.

- **Test suite rebuilt as a pyramid (2026-06-01).** The suite was a flat **49 e2e tests**
  that each booted the whole OS under QEMU and drove it over serial — the inverted-pyramid
  anti-pattern (heavy, flaky, used even for pure logic). Rebuilt into tiers
  ([`design/testing.md`](design/testing.md)): a **host unit-test harness** (`tests/unit/`,
  `make unit`) compiles pure tOS logic with the host `cc` and runs in ms — **28 unit tests**
  so far covering the launcher search filter, word-jump/selection index math, and the toast
  word-wrap (incl. a regression test for the space-dropping bug). The pure logic was lifted
  into a dependency-free `user/lib/textutil.h` (`tu_ci_contains`/`tu_wordch`/`tu_word_*`/
  `tu_wrap`) that the toolkit, launchers, and twm now delegate to. The **e2e set was pruned
  49 → 19**: the 9 fs micro-ops merged into one `t_fs_crud` journey, window/clipboard tests
  into `t_window_mgmt`, the driver smokes into `t_drivers`, dir-persistence folded into
  `t_fs_persist`, and ~20 per-feature GUI tests deleted (their logic is now unit-tested).
  `make check` runs both. Net: adding a feature usually adds a cheap unit test, not a boot.
- **macOS-style Alt-Tab switcher (#7, 2026-06-01).** The interim Super/Alt+Tab MRU
  switcher (which raised focus on every press) became a real **overlay**: Alt+Tab opens
  a centred frosted card of window **tiles** (app icon + title) and steps the selection
  through the MRU snapshot **without changing focus**, with the selection highlight
  **easing between tiles** as you cycle. It **commits** (focuses the pick) on Alt release
  after a genuine hold (`kbd_mods()` polled each frame), or a **click** on a tile, or
  **Enter**, or a linger timeout (the fallback the test harness uses since QEMU can't
  hold a modifier); **ESC cancels**; **arrows** / **Shift+Tab** walk it backward. Drawn
  above windows/dock (`draw_switcher`, dirty-tracked with its shadow halo). Traces
  `[twm] altswitch open/sel/commit/cancel`; `t_alt_tab` (open → sel → commit → focus) +
  overlay screenshot; `t_window_switch` still green. Built on the richer key-events
  foundation (the compositor polls the `SYS_KBD_MODS` modifier mask each frame).
- **Terminal copy-path coverage (2026-06-01).** The terminal's mouse grid selection +
  **Ctrl+Shift+C/X/V** clipboard chords were already wired (`keyboard.c` emits
  `KEY_TERM_COPY/CUT/PASTE` only when Ctrl+Shift is held, so plain Ctrl+C stays `^C`;
  `term_copy`/`term_paste` over `SYS_CLIP_*`), but only the *paste* direction had a test.
  Added a `[term] copy <n>` trace + a `[term] grid <fw> <fh> <cols> <rows>` metrics trace
  and a deterministic `t_term_copy`: `clear` to fix the layout, `echo <token>` so the
  output lands on row 1, drag-select that row off the `[twm] win` client rect, copy, and
  assert the copied byte count equals the token length. BIOS suite green + selection
  screenshot.
- **Notification QoL (2026-06-01).** Four polish passes on the toast/center: (1) **hover
  pauses** the auto-dismiss — while the cursor is over a toast it snaps fully open and
  freezes (`[twm] toast pause`), resuming when the pointer leaves; (2) a **collapsible
  toast** — when the body is truncated the card shows a `v`/`^` chevron and clicking it
  expands the toast to the word-wrapped full body (`toast_wrap`, capped at
  `TOAST_MAXLINES`; `[twm] toast expand 1/0`); (3) when the **center is open** a new
  notification no longer pops a toast — it **slides into the top of the list**
  (`nc_slide`; `[twm] notif slide`); (4) a header **Clear** button empties the center
  ring (`[twm] notifcenter clear`). `dirty_toast` now covers the worst-case expanded
  height so expand/collapse never smears; the open-center trace carries the panel
  origin. Extended `t_notifications` (pause + expand + slide + clear); BIOS suite green
  + expanded-toast & center screenshots.
- **Notifications / toasts (ui.md phase 3, 2026-06-01).** A `notify(title, body)` SDK call
  (new `SYS_NOTIFY`) posts to a global kernel ring (`kernel/ipc.c`); the compositor drains
  it (`SYS_WM_NOTIFY` / `wm_poll_notify`, compositor-only) and turns each into a **top-right
  toast** (solid raised card with shadow + accent stripe; slides in / holds ~3.7s / slides
  out — no alpha, so the dirty-rect math stays simple) plus a **notification-center** ring
  (last 8) shown in a frosted panel toggled by a **bell** status item (with an unseen
  badge). Opening the center clears the badge and supersedes any live toast. Shell gained
  `notify <text>` to post one. twm traces `[twm] notify <title>` + `[twm] notifcenter open
  <n>`. `t_notifications`; suite 45/45 + toast & center screenshots.
- **Status-bar cluster (ui.md phase 2, 2026-06-01).** The top bar's right side gained
  a macOS-style status cluster: vector placeholder glyphs for network (ascending signal
  bars), volume (speaker + waves), and battery (shell + nub + charge fill), drawn
  monochrome in the slate ink, plus a **registry-driven clock**. `build_clock()` reads
  `clock.format` (24h|12h), `clock.seconds`, and `clock.weekday` live, so settings drive
  the format (shipped defaults: 24h + weekday + seconds → "Fri 14:09:09"). `cc_layout()`
  lays the cluster out right-to-left into fixed slots (reserving the worst-case clock
  width) so the glyphs + CC button never jitter as the seconds tick; the per-second
  dirty rect was widened to cover the whole cluster. twm traces `[twm] statusbar net …
  vol … bat … cc …` + `[twm] clk "…"`. `t_statusbar`; suite 44/44 + screenshot.
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
- [`design/virtio-net.md`](design/virtio-net.md) — first NIC (virtio-net) + the stack/sockets layering above it.
- [`design/virtio-gpu.md`](design/virtio-gpu.md) — virtual GPU: 2D presentation now, virgl/Venus accel later (VM-only).

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
