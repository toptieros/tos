# Design — File picker as the Files app (a picker *process* over IPC)

> Status: **design + roadmap.** Today the system Open/Save dialog is an in-process
> toolkit modal (`ui::FileDialog`, `user/lib/ui.{h,cpp}`). It works, but it looks
> *plain*: it draws its own crude folder/file shapes (`fd_folder` = a tabbed
> rectangle, `fd_file` = a dog-eared sheet) in a bare list, and it re-implements a
> sidebar + path bar that the Files app already does far better. This is the plan
> to retire it and make the picker **the Files app itself**, launched in a
> *picker mode* with parameters and returning the chosen path to the caller.

## Why this approach

The Files app (`user/files/files.cpp`) already has the design we want: real baked
icons (`icons.h` / `fileicons_argb[]` + per-`.app` bundle icons via
`load_icon_argb`), a locations sidebar, a breadcrumb/toolbar, type-aware icons
(`file_icon_for`), a live filter, a status bar, and navigation (back/forward/up).
Reusing it means the picker gets **all of that for free** and stays in lockstep
with Files forever — no second directory-browser to style and maintain.

The mental model is Windows: its Save/Open dialog *resembles Explorer*. (Strictly,
Windows does that with a shared component — `comdlg32`/`IFileDialog` — drawn
in-process, not by spawning `explorer.exe`. We go one step further and literally
run our Files binary, because we already have a complete Files app and a
dead-simple inter-app hand-off (`/tmp/.open-doc`, see below) to build on — so
"launch Files as the picker" is *less* code than extracting a shared component
would be, and it can't visually drift from Files.)

We own the whole stack (kernel, compositor, SDK, apps), so the plumbing this needs
— a structured launch argument and a result channel back to the caller — we just
add.

## How apps already hand off (the pattern we extend)

`sys_open_with(prog, path)` (`user/lib/sys.c`) is the existing "Open With" flow and
the template for everything here. It is *not* a syscall — it is a temp-file
hand-off:

```c
#define OPEN_DOC_PATH "/tmp/.open-doc"
int sys_open_with(const char *prog, const char *path){
    sys_spit(OPEN_DOC_PATH, path, strlen(path));  // write the document path
    return sys_launch(prog);                       // fork+exec the target -> pid
}
int sys_open_arg(char *out, int cap){              // the target, once at startup:
    char *d = sys_slurp(OPEN_DOC_PATH, &n);        //   read it,
    funlink(OPEN_DOC_PATH);                        //   consume it,
    ... copy first line into out ...               //   use it.
}
```

The picker is the same idea in **both directions**: a request file in, a result
file out, with the caller noticing the picker exited via `trywait()`.

(Bonus: those hand-off writes used to stutter the desktop because every file close
rewrote the whole 189 KB tosfs directory table; that's fixed — see the
"incremental directory flush" entry in `NEXT_STEPS.md` — so spawning a picker and
shuttling small temp files is now cheap.)

## Architecture

```
  caller (e.g. notepad)                         picker (Files in -picker mode)
  ─────────────────────                         ──────────────────────────────
  Ctrl+S / File>Save As
    sys_pick_begin(req) ── writes /tmp/.picker-req (mode,dir,name,ext,title,caller)
                        └─ unlinks any stale /tmp/.picker-res
                        └─ sys_launch("/Apps/Files.app/files") ──► fork+exec
                                                                     │
  ...event loop keeps running (window stays modal-dimmed)...        │ reads+consumes
                                                                     │ /tmp/.picker-req
                                                                     │ → picker mode
                                                                     │ browse / filter
                                                                     │ pick or cancel
                          /tmp/.picker-res ◄── writes chosen path ───┘ (empty=cancel)
  on_tick:                                                           proc_exit(0)
    sys_pick_poll(pid,out) ── trywait()==pid? ─► read+unlink /tmp/.picker-res
       └─ 1 + path  → on_pick(path)
       └─ -1        → cancelled (no/empty result file)
       └─  0        → still open, poll again next tick
```

Two processes, two temp files, one `trywait`. No new kernel object is *required*
for v1 (only the SDK + Files + caller change); the compositor modality (below) is a
separate, optional polish layer.

## The request channel — `/tmp/.picker-req`

A line-based `key=value` blob (trivial to write/parse with the existing string
helpers; forgiving of unknown keys for forward-compat):

```
mode=save            # "open" | "save"
dir=/Users/user/Documents
name=untitled.txt    # save: suggested filename (ignored for open)
ext=txt,md           # comma list of allowed extensions; empty = all files
title=Save Note      # window title (optional; default "Open"/"Save As")
caller=7             # caller pid — for modal parenting + result namespacing (v2)
```

SDK:

```c
struct pick_req {
    int  mode;              // PICK_OPEN | PICK_SAVE
    char dir[192];          // start directory (empty => ~)
    char name[64];          // suggested name (save)
    char ext[64];           // "txt,md" filter, or "" for all
    char title[32];         // window title (optional)
};
int  sys_pick_begin(const struct pick_req *r);   // write req, unlink stale res,
                                                 //   launch Files -> child pid, or -1
int  sys_pick_poll(int pid, char *out, int cap); //  1 = picked (out=path),
                                                 //  0 = still open,
                                                 // -1 = cancelled/closed
```

Files-side mirror of `sys_open_arg`:

```c
int sys_pick_req(struct pick_req *out);  // 1 + fills out if a request is pending
                                         // (consumes /tmp/.picker-req), else 0
```

## The result channel — `/tmp/.picker-res`

* On a confirmed pick, Files writes the **absolute** chosen path to
  `/tmp/.picker-res` and `proc_exit(0)`.
* On Cancel or window-close, Files writes nothing (or a 0-byte file) and exits.
* `sys_pick_begin` unlinks any stale `/tmp/.picker-res` *before* launching, so a
  previous run's result can never be mistaken for this one's.
* `sys_pick_poll` calls `trywait()`; when it returns the picker's pid (child
  reaped), it reads + unlinks `/tmp/.picker-res`: a non-empty path ⇒ return `1`,
  absent/empty ⇒ return `-1` (covers Cancel *and* a crashed picker). Until the
  child exits, `trywait()` returns 0 ⇒ poll again next tick.

v1 uses the single fixed pair (the picker is modal, so one is live at a time
per system). v2 namespaces both files by caller pid (`/tmp/.picker-<pid>.req/.res`)
to be robust if two apps ever open a picker at once.

## Files app — picker mode

When Files starts it checks `sys_pick_req()` (before its normal `sys_open_arg`
check). If a request is pending it enters **picker mode**, a layout variant of the
normal window — *not* a fork of the app:

* **Window:** smaller, dialog-shaped (≈ 560×420), titled from the request; created
  with `WIN_MODAL | parent=caller` (v2) or an ordinary window (v1).
* **Chrome reused as-is:** the locations sidebar, the directory `ListView` with its
  real icons, the breadcrumb/up, the live filter, the status bar.
* **Footer (new, picker-only):** `[ Name: ____ ]` (save mode only) on the left,
  `Cancel` + `Open`/`Save` on the right. Mirrors the in-process dialog's footer so
  the interaction is familiar.
* **Extension filter:** directories always shown (to navigate); files shown only
  when their extension ∈ `ext` (empty ⇒ all). Reuses `file_icon_for` for icons.
* **Activate semantics:**
  * open: double-click / `Open` on a file → write result, exit. Double-click a
    folder → navigate (unchanged).
  * save: `Save` → `join(dir, name)`; if it exists, reuse Files' existing
    `ui::ConfirmDialog` overwrite warning (**Replace / Keep Both / Cancel**) before
    writing the result + exiting.
* **Ownership:** saving into a system-owned dir (`/System`, `/Apps`) is blocked —
  Files already knows ownership (`stat_` + `tos_may_write`); grey the Save button
  there, exactly like `ui::FileDialog::dir_writable()` does today.
* **New Folder** stays available (handy mid-save); **Delete/Open-With** are hidden
  in picker mode.
* **Cancel / `WEV_CLOSE`** → empty result, exit.
* **Logging:** emit `[files] picker <mode> <dir>` on entry and `[files] picked
  <path>` / `[files] pick cancel` on exit, so e2e can drive it.

## Compositor — modality (phased polish, optional for v1)

> **Status: done (2026-06-06).** Shipped as a system-wide `WIN_MODAL` instead of the
> parent-linked design below: twm keeps the modal topmost + focused, dims the *whole*
> screen behind it (full-screen scrim, reusing the Launchpad path), and swallows input
> outside it. No `wininfo.parent` field / pid→window mapping was needed — see the
> NEXT_STEPS entry. The parent-only-dim variant below is left as the original idea.

For a real modal feel (parent dimmed + inert, picker on top + focused):

* Add `WIN_MODAL` to the window flags and a `parent` field to `struct wininfo`
  (the caller's window id or pid).
* The compositor: keeps the modal above its parent in z-order, draws a dim scrim
  over the **parent window** (reuse the Launchpad `WIN_OVERLAY` scrim mechanic),
  routes all input to the modal while it is up, and restores focus to the parent
  when the modal window is destroyed.

v1 skips this: the picker is just another top-level window. It already *works*
end-to-end and *looks* like Files — the user could click back to the caller
underneath, which is the only thing v2 fixes. Land function first, modality second.

## Caller migration & retiring `ui::FileDialog`

* **Notepad** is the only current caller. `open_open()` / `save_as()` switch from
  `dlg.open_dialog(...)` to `sys_pick_begin(...)`; `on_tick` polls
  `sys_pick_poll(pid, …)` and, on success, runs the same code the old
  `on_pick(ctx, path)` callback ran (open into a tab / `save_tab`). Drop the
  embedded `ui::FileDialog dlg;` member.
* Keep `ui::FileDialog` in the toolkit until notepad is migrated and the new path
  is proven by tests, then **delete it** (and its `fd_*` glyph helpers) — one
  picker, no dead code.
* A raw-syscall app (e.g. the terminal) can use the same `sys_pick_*` calls since
  they are plain SDK functions, not toolkit/Window-bound — which the in-process
  modal never offered. That portability is a side benefit of going process-based.

## Testing

* **Unit (pyramid base):** the request/result codec is pure string logic — host
  unit-test `pick_req` encode → parse round-trips and the `ext` filter predicate
  (`name_matches_ext`). No QEMU needed.
* **e2e `t_file_picker`** (replaces/extends `t_file_dialog`): launch notepad, type,
  Ctrl+S → assert the **Files** picker comes up (`[files] picker save …`),
  navigate, Save, then read the file back through a terminal to prove it hit disk.
  Drive buttons by **click**, not the Ctrl accelerator (the menu-accelerator race —
  see the project notes). Add an open-mode case too.
* **Update** `t_notepad_edit_save` / `t_notepad_undo`: they currently assert
  `[filedialog] open save …`; rewrite to the new `[files] picker …` / `[files]
  picked …` lines. Persistence assertions (read-back) stay.

## Risks & open questions

* **`/tmp` must exist + be user-writable** on a fresh disk. The Open-With flow
  already depends on it, so this is established — but confirm the mkfs seed creates
  `/tmp` (or have `sys_pick_begin` `fmkdir` it).
* **Launch latency:** spawning Files is an ELF load (a process), not an in-process
  modal — a perceptible beat before the picker appears. Acceptable post the
  disk-freeze fix (app launches are preemptible now); revisit only if it feels slow.
* **Single-picker assumption (v1):** the fixed `/tmp/.picker-req/.res` pair assumes
  one live picker. True while pickers are modal; v2 pid-namespacing removes the
  assumption.
* **Caller dies while the picker is open:** the picker just exits after writing a
  result nobody reads (the temp file is reaped on the next `sys_pick_begin`). The
  orphaned-child reparent path (`t_orphan_reparent`) already handles the process
  side.
* **Does userspace expose `getpid()`?** Needed for v2 namespacing + the `caller`
  field. If not, add a `SYS_GETPID` (trivial) when we get to v2.

## Implementation phases

See the tracked checklist in `NEXT_STEPS.md` ("File picker → Files-as-picker
process"). Order, cheapest-first: **(1)** SDK `sys_pick_*` + codec + unit tests →
**(2)** Files picker mode (open first, then save + overwrite reuse + ext filter) →
**(3)** migrate notepad + `t_file_picker` + update notepad tests → **(4)** delete
`ui::FileDialog` → **(5)** compositor `WIN_MODAL` parent-dim polish → **(6)** v2
pid-namespacing / `getpid`.
