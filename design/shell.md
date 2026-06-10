# Design guideline — the tOS shell

> Status: **partly built + roadmap.** A working shell ships today
> (`user/shell/shell.c`): a line editor with history and cursor editing that
> dispatches a fixed table of built-in commands and `fork`+`exec`s external
> programs by name. This doc takes [`fish`](https://fishshell.com/) as the model
> — friendly, discoverable, highlighted, autosuggesting — and lays out how to grow
> the current command-dispatcher into a real interactive shell **and** a small
> scripting language, bounded by what a freestanding userspace can do today.
>
> Companion: [`packaging.md`](packaging.md) (the `tos` command the shell drives),
> the terminal/pty in `user/term/term.c` (the byte stream the shell edits over),
> [`settings.md`](settings.md) (the registry, where shell config lives),
> [`app-runtime.md`](app-runtime.md) (the caps a shell-launched process inherits).

## Where we are today

`shell.c` is ~570 lines and already does more than a first shell usually does:

- **A real line editor.** `readline()` keeps the on-screen line in sync with a
  buffer using only `printc` + non-destructive backspace; it decodes the arrow /
  Home / End / Delete keys from their ANSI escape sequences (`ESC [ …`), supports
  insert-anywhere cursor editing, and walks an in-memory **history ring** (64
  lines) on Up/Down with a preserved draft.
- **A fixed built-in table.** `help`, `ls`, `cd`, `pwd`, `mkdir`, `rmdir`, `tree`,
  `mv`, `cat`, `write`, `rm [-r]`, `cp`, `echo`, `clear`, `date`, `df`, `mem`,
  `uptime`, `sysinfo`, `uname`, `reg get/set/list`, `copy`/`paste`, `notify`,
  plus demos (`fork`, `smp`, `spawn`, `crash`, `beep`, `colors`, …). Dispatch is a
  long `if (streq(...)) … else if (starts(...))` chain.
- **External programs by name.** `run_prog()` does `fork()` → `exec(name)` →
  `wait_child()`; this is how `fastfetch`/`memtest` run. The prompt colours the
  user/host/cwd with ANSI and renders the home dir as `~`.
- **It runs as a process over a pty.** `term` owns the character grid and runs the
  shell as a piped child (`exec("shell")` after `pty_attach`); with no desktop,
  `init` runs the shell directly on the TTY. So the shell speaks a **byte stream**
  in and out, and the terminal already interprets ANSI colour + cursor moves.

What it is **not** yet: there is no parser, so there are **no pipes, no
redirection, no globbing, no variables, no command substitution, no scripting,
no tab completion, no syntax highlighting, and no autosuggestions.** Every line is
matched against the built-in table or treated as a bare program name. That is the
gap this doc closes.

## Design philosophy (fish, adapted to tOS)

fish's [design principles](https://fishshell.com/docs/current/design.html) map
remarkably well onto a teaching OS — they're about *user focus*, not about Unix
legacy. The ones we adopt, and how tOS reality bends them:

- **Discoverability.** *Everything tab-completable, every completion described;
  every error names what went wrong.* This is the single highest-value thing a
  small shell can copy: it turns "read the source to learn the commands" into
  "press Tab." Our `help` is a flat dump today; the target is that the shell
  *teaches itself.*
- **Responsiveness.** fish does highlighting/autosuggest I/O **asynchronously** so
  a slow disk never blocks a keystroke. tOS's filesystem is **in-memory-fast**
  (tosfs over a RAM-resident image, no spinning disk, no network in the hot path),
  so we can do highlight + history-suggest **synchronously** at first and stay
  responsive — and revisit async only if/when path completion reaches the network
  or a slow device. (Honest caveat: we have no userspace threads yet, so "async"
  would mean non-blocking polling, not background work.)
- **Configurability is the root of all evil.** *Be smart by default; expose few
  knobs.* This suits us — every config option is code we'd have to carry in a
  freestanding C program. The prompt, the colours, and the keybindings get **good
  built-in defaults** and only the minimum overrides (a couple of registry keys),
  not a `.fishrc` zoo.
- **Orthogonality + one input type.** fish has *one* kind of input — lists of
  commands — and `if`/`for`/`function`/variable-assignment are all just commands
  that read the rest of the line; every block ends with `end`. It has **functions,
  not aliases**, and **command substitution `(…)`, not three overlapping subshell
  syntaxes.** When we add a language (below) we copy this: the smallest orthogonal
  grammar that does the job, so learning the command/argument shape teaches the
  whole language.

We do **not** chase POSIX `sh` compatibility (fish doesn't either). tOS is a
closed, friendly world; a clean small language beats a `bash` clone.

## Architecture: layering features onto the existing editor

The current `readline()` already owns the redraw, so the interactive features
slot in as **decorations of the same redraw**, over the same pty/ANSI the prompt
already uses. No new kernel surface is needed for the interactive layer:

```
keystroke ──► read_key() ──► edit the line buffer ──► redraw:
                                                       ├─ syntax highlight (colour spans)
                                                       ├─ autosuggestion (dim suffix)
                                                       └─ cursor
            Tab ─────────────► completion engine ──► insert / show pager
            Enter ───────────► parse ──► execute (builtins + fork/exec pipeline)
```

### Interactive features (the fish "feel"), in priority order

1. **Syntax highlighting.** As the line is redrawn, colour the **first token**
   green if it resolves to a known command (a built-in, a program in `/System/bin`
   or `/Apps/*/bin`, see [`packaging.md`](packaging.md)) and **red** if it does
   not — fish's most loved feature and a live error before you even press Enter.
   Extend to: quoted strings, flags, and **paths that exist vs don't** (a red
   argument = "no such file"). Feasible synchronously given the fast FS; reuse the
   prompt's ANSI-colour approach in the redraw.
2. **Autosuggestions.** After the cursor, draw a **dimmed** suggestion (the most
   recent history line that has the current input as a prefix), accepted with `→`
   / `Ctrl+E` / `Ctrl+F`. History-based first (we already keep the ring); later,
   completion-based (suggest the single obvious completion).
3. **Tab completion with descriptions.** A completion engine that, for the first
   token, offers **command names** (built-ins + the `/System/bin` and `/Apps`
   catalogs) each with a one-line description, and for later tokens offers **path
   completion** (`readdir` the dirname, match the prefix). When there are many
   matches, show a **pager grid** (fish-style) instead of dumping them. This is the
   discoverability payoff: `tos <Tab>` lists `app`, `package`, `get`, `sync`…
4. **History that persists + prefix-searches.** Promote the in-memory ring to a
   **history file** under `/Users/user/.config/` (or a registry blob), loaded at
   startup; make Up do **prefix search** (type `git`, Up walks only `git …` lines)
   rather than raw scrollback.
5. **Abbreviations, not aliases.** When we add expansion, copy fish's
   **abbreviations** (`abbr gco "git checkout"`): they expand *in place, visibly*
   as you type Space/Enter, so the real command lands in history and highlighting —
   none of the "what does this alias hide?" confusion. (Orthogonality: one
   mechanism, not aliases *and* functions.)
6. **Prompt.** Today the prompt is hardcoded. Keep that as the **smart default**
   (`user`:`cwd`$, `~` for home, colourised). Add a *small* override path — a
   registry key with a tiny template (`{user} {cwd} $`) — and, once scripting
   exists, a `fish_prompt`-style function the user can define. No more than that
   (configurability-is-evil).

### Keybindings

The emacs-style motions are already half-present (Home/End/Del; arrows). Round out
the common set (`Ctrl+A/E` line start/end, `Ctrl+U/K` kill, `Ctrl+W` kill word,
`Ctrl+L` clear, `Ctrl+R` reverse history search) as fixed defaults. A `bind`
built-in to remap is a later, low-priority knob — defaults first.

## The shell language (scripting), longer-term

Today there is **no language** — just dispatch. The plan is a small fish-shaped
grammar, added in one go behind a real tokenizer/parser so the interactive layer
and scripts share it:

- **One input type: lists of commands.** A pipeline is `cmd | cmd | cmd`; `;` and
  newline separate; `and`/`or` (and `&&`/`||`) chain on exit status.
- **Quoting + expansion:** `'single'` literal, `"double"` with `$var`, `*`/`?`
  globbing via `readdir`, brace `{a,b}`, and **command substitution `(cmd)`** (the
  *only* substitution syntax — fish's orthogonality lesson).
- **Variables:** `set name value`, `$name`, lists are first-class; `set -x` to
  export into a child's environment (needs env passing — see below).
- **Control flow as commands, blocks closed by `end`:** `if cond; …; end`,
  `for x in …; …; end`, `while`, `switch`, and **functions** (`function f; …;
  end`) — *not* aliases.
- **Redirection** `>` `>>` `<` and **pipes** `|` between programs.

### Built-in vs external (the dividing line)

A command stays **built-in** only if it must mutate shell state or run in the
shell's own process: `cd`, `set`, `history`, `abbr`, `function`, `source`, `exit`,
and the flow keywords. Everything that's really a program — `ls`, `cat`, `cp`,
`tree`, `df`, … — should migrate to **external programs in `/System/bin`** once
argv passing exists (below), so the shell shrinks and the same tools work from
scripts and from `tos`. (This is fish's stance: keep the language core small,
push utilities out.) Until argv lands, they remain built-ins.

## What has to change (and the honest kernel dependencies)

The interactive layer (highlight/suggest/complete/history/prompt) needs **no new
kernel surface** — it's all redraw + `readdir` + ANSI. The *language* does:

1. **argv passing to `exec`/`spawn` — the blocker.** Today `exec(prog)` takes a
   single string used *both* as the ELF path *and* as the seed for the task's data
   page (`USER_DATA_VADDR`, see `kernel/syscall.h`; `kernel/mm/vmm.c` copies the
   whole `prog` string there as an "argv0-ish" value). So a program receives a
   name but **no arguments**. Real commands (`ls /tmp`, and crucially `tos package
   add git`) need argv. The clean fix: split the launch string into a **path
   (first token)** + an **argument tail**, load the ELF from the path, and seed the
   data page with the full argv (or add `SYS_EXEC2(path, argv)`). The delivery
   vehicle — a mapped data page — already exists; only the loader's "use the whole
   string as the path" assumption changes. This unblocks both external commands
   and the `tos` CLI ([`packaging.md`](packaging.md)).
2. **Pipes + redirection.** A `SYS_PIPE` (or reuse the pty machinery) plus an
   fd-dup primitive so a child's stdout can feed the next child's stdin and `>` can
   point stdout at a file. The pty already proves the kernel can wire one process's
   byte stream to another; pipelines generalise that.
3. **A heap (`malloc`/`sbrk`).** The current shell is all fixed stack buffers
   (`LINEMAX 512`, a 64-line history). A tokenizer/AST, variable lists, and
   completion candidate sets want dynamic allocation — ties into the userspace
   runtime work ([`roadmap.md`](roadmap.md) Phase 2; `libc` already has malloc, so
   this is mostly "grow the user heap").
4. **An environment block** for `set -x` to reach children (a second seeded page,
   or argv-style key=value passing).
5. **(Optional) non-blocking input** if highlight/complete ever touch a slow source
   — not needed while the FS is RAM-fast.

## Phasing (keep `make test` green)

1. **Discoverability first, no kernel change.** Add synchronous **syntax
   highlighting** (known-command green / unknown red) and **history
   autosuggestions** to `readline()`'s redraw. Biggest felt improvement for the
   least risk.
2. **Tab completion + pager.** Command-name completion from the built-in table +
   the `/System/bin` and `/Apps` catalogs, then path completion; a grid pager for
   many matches. Persist history to `~/.config`.
3. **argv passing** (kernel/loader change) → move the file utilities out to
   `/System/bin` programs; the shell keeps only state-mutating built-ins. This is
   also what `tos` needs.
4. **The language:** tokenizer/parser → variables + command substitution → pipes +
   redirection (needs `SYS_PIPE`/dup) → control flow + functions + abbreviations.
5. **Config + prompt overrides:** a startup script (`config` under `~/.config`) and
   a `fish_prompt`-style function once functions exist; keep defaults smart.

## Out of scope (for now)

POSIX `sh` compatibility, job control (`bg`/`fg`/`&` — we have `fork`/`wait` but no
session/job model), a web-based `fish_config`, universal variables synced across
sessions, and programmable per-command completion *specs* (start with generated
command + path completion; hand-written completions per tool can come later, like
fish's `share/completions`).

## Ties

- The command the shell most needs to drive: [`packaging.md`](packaging.md) (`tos
  app/package/get/sync`) — and its argv dependency is shared with this doc.
- The pty/byte-stream + ANSI grid: `user/term/term.c`.
- Where config + history persist: [`settings.md`](settings.md) (the registry) +
  `/Users/user/.config` ([`filesystem-layout.md`](filesystem-layout.md)).
- What a shell-launched process is allowed to do: [`app-runtime.md`](app-runtime.md)
  (the shell/boot chain runs unsandboxed; programs it launches inherit caps).
