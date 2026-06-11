# Design guideline — the tOS terminal emulator

> Status: **partly built + roadmap.** A working terminal emulator ships today
> (`user/term/term.c`, ~425 lines): it owns a character grid, runs the shell as a
> piped child over a kernel **pty**, parses a useful slice of ANSI, renders with a
> glyph diff, and handles selection/scrollback/resize. This doc takes
> [`ghostty`](https://ghostty.org) as the model — *fast, native, standards-
> compliant* — and lays out how to grow our ad-hoc emulator into a VT that real
> terminal programs (`less`, a line editor, a `vi`/`htop`-class TUI) can actually
> drive. The lesson we take from Ghostty is its **correctness architecture**, not
> its GPU renderer: a software glyph-diff blit is the right call for a 2D
> compositor on a hobby OS.
>
> Companion: [`shell.md`](shell.md) (the program `term` hosts — and which *emits*
> the ANSI this doc *interprets*; they reinforce each other), the kernel **pty**
> (`kernel/ipc.c`, already built — the hard kernel part is done), [`ui.md`](ui.md)
> (window chrome / title / menu bar), [`settings.md`](settings.md) (palette,
> cursor, scrollback config), [`filesystem-layout.md`](filesystem-layout.md)
> (where a terminfo entry would live).

## Where we are today

`term.c` is a genuine little terminal, and one architectural choice is already
right: **`term` is an ordinary app that owns the grid.** It asks twm for a window
(a shared pixel surface), so the compositor can move/restack/resize the window
freely without `term` losing a character — the display state lives in the app, not
the compositor. What it does:

- **A character grid + shadow grid.** Cells are `char/fg/bg` bytes at a fixed
  `STRIDE`; `render()` is a **glyph diff** — it repaints only cells whose
  char/fg/bg/selection changed. Cheap and flicker-free on a software framebuffer.
- **A 3-state ANSI parser** (`S_NORMAL`/`S_ESC`/`S_CSI`) handling: **SGR** (16
  colours + bold, and `38;5`/`48;5` truncated to 16), cursor moves (`CUP`, `CUU/D/F/B`),
  **erase** display (`ED`) and line-right (`EL`), CR/LF/BS/TAB(8)/BEL.
- **Scrollback** — a 256-row ring; mouse wheel + Shift+PgUp/PgDn page through it
  with a margin thumb.
- **Mouse selection → clipboard** (Ctrl+Shift+C, reading-order, trailing-space
  trimmed) and **paste** (Ctrl+Shift+V → pty), an **Edit** menu, a blinking I-beam
  cursor managed as an overlay, and **resize** that preserves content (fixed
  stride; only `cols/rows` and the cursor clamp change).

## The gap: what real programs need that we lack

Our parser is ad-hoc and silently drops everything it doesn't recognise, and the
cell model is 8-bit char + 4-bit colour. That's fine for the shell's own output,
but it locks out most terminal software:

| Missing | Unlocks | Notes |
|---|---|---|
| **Alternate screen** (`?1049`/`?47`) | `vi`/`less`/`htop`-class **full-screen TUIs** | a second screen buffer, swapped on enter/leave; the biggest single unlock |
| **Scroll regions** (`DECSTBM`) + **IL/DL/ICH/DCH/ECH** | line editors, `less`, anything that inserts/deletes lines | today only full scroll + erase exist |
| **SGR truecolor + attributes** (`38;2;r;g;b`, italic/underline/reverse/dim/strike/blink) | correct colour output; `ls`/diff/syntax colour | we only do 16-colour + bold |
| **DEC private modes** (`?25` cursor, `?2004` bracketed paste, mouse `1000/1002/1003/1006`, `?7` wrap, `?6` origin) | safe paste, mouse-aware apps, cursor hide | no mode table at all today |
| **UTF-8 / Unicode** | any non-ASCII output | cells are raw bytes; `≥0x20` stored as-is |
| **DEC special-graphics charset** (`ESC ( 0`, SO/SI) | **box-drawing** — our own `tree`, and every TUI border | line-drawing glyphs render as letters now |
| **Save/restore cursor** (`DECSC`/`DECRC`), tab stops (`HTS`/`TBC`), `index`/`reverseIndex`, `REP` | prompts, editors, `tput` users | hardcoded 8-col tabs, no save/restore |
| **OSC** — title (`0/2`), clipboard (`52`), cwd (`7`), hyperlinks (`8`) | window title, app-driven copy, "open new tab here", clickable links | no OSC parsing |
| **TERM + terminfo** | curses/readline apps query real capabilities | no `TERM`, no terminfo entry |

## The architecture lesson from Ghostty

Ghostty's terminal core is a clean four-stage pipeline, and adopting that shape is
worth more than any single feature — it's what makes adding the features above
*safe and uniform* instead of more `else if` chains:

```
pty bytes ─► Parser ──actions──► Stream/handler ──ops──► Terminal + Screen ──► Renderer ─► surface
            (byte→event)        (event→operation)      (the state)            (glyph diff)
```

1. **Parser — the canonical Williams DEC ANSI state machine.** Ghostty's
   `Parser.zig`/`parse_table.zig` implements the well-known DEC parser: states
   `ground · escape · escape_intermediate · csi_entry · csi_param · csi_intermediate
   · csi_ignore · dcs_* · osc_string · sos_pm_apc_string`, emitting actions
   (`print` a codepoint, `execute` a C0/C1 byte, `csi_dispatch`, `esc_dispatch`,
   `osc_dispatch`, `dcs_*`, `apc_*`). The point: it is **total** — it never wedges
   on malformed or partial input, it handles every sequence class the *same* way,
   and it's a published spec rather than guesswork. **This replaces our 3-state
   `feed()`.** It's a refactor, not a rewrite of behaviour: the same operations get
   called, but through a robust front end with room for OSC/DCS/intermediates.
2. **Stream/handler split.** The parser knows *nothing* about what a sequence
   *means*; a handler turns actions into terminal operations (`setCursorPos`,
   `eraseLine`, `scrollUp`, `setAttribute`, `switchScreen`, …). Keeping these
   separate is what lets the parser stay a pure, testable state machine.
3. **Terminal + Screen state.** The grid, cursor, modes, tab stops, scroll region,
   styles, and the primary/alternate screens. Ghostty's `Terminal.zig` exposes
   exactly the op vocabulary we need to grow into (`insertLines`/`deleteLines`,
   `setTopAndBottomMargin`, `saveCursor`/`restoreCursor`, `index`/`reverseIndex`,
   `configureCharset`, `switchScreen`, `setTitle`, …).
4. **Renderer.** Ghostty is GPU; **we stay software** (glyph diff, as now) — that's
   correct for our compositor and not a dead end (2D is bandwidth-bound, and damage
   tracking already exists). We borrow the *cell/style model*, not the GPU path.

What we deliberately **don't** copy: the GPU renderer, `PageList` paged scrollback
(our fixed ring is fine), ref-counted style interning at Ghostty's scale, ligature
shaping, and the Kitty graphics/sixel image protocols. Those are performance/scale
machinery a hobby OS terminal doesn't need yet.

## Subsystems to build

- **VT parser** (state machine + dispatch table) — the front end above; everything
  else hangs off it.
- **Modes table** — a small DEC-private/ANSI mode set: `cursor_visible(25)`,
  `alt_screen(1049/47/1047)`, `bracketed_paste(2004)`, mouse `1000/1002/1003` +
  `sgr(1006)`, `origin(6)`, `wraparound(7)`, `reverse_colors(5)`, `cursor_keys(1)`,
  `linefeed(20)`. Each is a bit; set/reset via `CSI ? Pm h/l`.
- **Cell + style model** — a cell carries a **codepoint** + a **style** (fg/bg as a
  palette index *or* 24-bit colour, plus attribute bits: bold/dim/italic/underline/
  reverse/strike/blink). Replaces the per-cell `fg/bg` bytes; enables truecolor and
  attributes. A small style table (or packed style) keeps cells compact.
- **Alternate screen** — a second `Screen` swapped in on `?1049` (save cursor, clear,
  enter) and out on leave; the primary keeps scrollback, the alt doesn't. The key to
  full-screen apps.
- **Editing ops** — `IL/DL` (insert/delete line), `ICH/DCH/ECH` (insert/delete/erase
  char), `DECSTBM` scroll region, `IND/RI`, `DECSC/DECRC`, `REP`, and real tab stops
  (`HTS`/`TBC`).
- **UTF-8 decoder** — incremental byte→codepoint decode feeding the cell's codepoint.
  Rendering beyond ASCII is **gated on font coverage** (`sysfont.h` is limited): step
  one is *decode correctly and render what the font has, box-glyph the rest*; wide
  (CJK) and combining characters come later with a real font.
- **DEC special-graphics charset** — `ESC ( 0` + SO/SI mapping so box-drawing
  codepoints render as lines (needed by our own `tree` and every TUI border).
- **Mouse reporting** — when a mouse mode is on, encode twm mouse events into pty
  input (`1000` click / `1002` drag / `1003` any, `1006` SGR encoding); keep local
  selection working with a Shift override, like xterm.
- **OSC** — `0/2` set the twm window **title** (needs a small `win_settitle`, the
  sibling of the existing `win_setmenu`); `52` route app copy into `clip_put`
  (clipboard already exists); `7` record the cwd (for "new terminal here"); `8`
  clickable hyperlinks.
- **Cursor styles** — `DECSCUSR` (block/bar/underline, blink/steady); we already
  draw a bar, so this is choosing the shape on request.
- **Selection upgrades** — word (double-click) / line (triple-click) selection,
  selection that survives scrollback.

## Kernel / SDK dependencies (honest)

- **The pty already exists** — the hard kernel piece is done; this is almost entirely
  userspace work in `term.c` + the SDK.
- **`TERM`/terminfo** depends on the **environment mechanism** that
  [`shell.md`](shell.md) also needs (env passing to a child). Until env exists, apps
  assume a fixed baseline; once it does, set `TERM=tos` and ship a terminfo entry
  under `/System/share/terminfo` describing what we actually support.
- **Window title** — add `win_settitle(wid, str)` to the WM protocol (mirrors
  `win_setmenu`) for OSC 0/2.
- **Font coverage** — full Unicode is bounded by the font; a richer font + glyph
  handling is a separate, larger effort. Decode now, expand glyph coverage later.
- **Clipboard** (`clip_put`/`clip_get`) already exists — OSC 52 just wires to it.

## Phasing (keep `make test` green)

1. **Swap in the Williams parser** — refactor `feed()` into the table-driven state
   machine, dispatching to the *current* operations. **No behaviour change**, pure
   robustness + headroom; a test feeds malformed/partial sequences and asserts the
   grid never corrupts.
2. **SGR completeness** — the cell/style model: 256-colour + 24-bit truecolor +
   italic/underline/reverse/dim/strike/blink. Most visible correctness win.
3. **Editing ops + modes** — `IL/DL/ICH/DCH/ECH`, `DECSTBM`, `DECSC/DECRC`,
   `index/reverseIndex`, tab stops, and the mode table (`?25`, `?7`, `?6`). Makes
   `less`/`readline`/line-editors behave.
4. **Alternate screen + bracketed paste** (`?1049`, `?2004`) — unlocks full-screen
   TUIs (a future `vi`/`htop`).
5. **UTF-8 decode** (render what the font covers; box the rest).
6. **Charsets (line drawing) + mouse reporting + OSC** (title / clipboard / cwd /
   hyperlinks).
7. **TERM + terminfo** (once env lands) + `DECSCUSR` cursor styles + word/line
   selection.

## Out of scope (for now)

GPU rendering (software glyph-diff is the right model here), the **Kitty graphics**
and **sixel** image protocols, ligatures / complex text shaping, full Unicode
grapheme clustering + wide-character width tables (basic UTF-8 first), `tmux` control
mode, `PageList`-style paged scrollback (the ring suffices), and **reflow on resize**
(we keep the fixed-stride grid; rewrapping long lines on resize is a later nicety).

## Ties

- The program it hosts, and which emits the ANSI it renders (so a richer terminal
  directly benefits the shell's highlighting / completion pager):
  [`shell.md`](shell.md) — they share the **env** dependency.
- The byte-stream transport: the kernel **pty** (`kernel/ipc.c`).
- Window title / menu / chrome: [`ui.md`](ui.md) (and the `win_settitle` addition).
- Palette / cursor / scrollback config (kept minimal, per the configurability ethos
  in [`shell.md`](shell.md)): [`settings.md`](settings.md). **Scrollback depth is wired:**
  `term.scrollback` in the registry sizes the heap ring at startup (default 256 rows).
- Where a terminfo entry lives: [`filesystem-layout.md`](filesystem-layout.md)
  (`/System/share`).
