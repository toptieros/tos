# tOS — next steps

How the system works **today** is in [PROJECT.md](PROJECT.md); what has **landed** is in
[CHANGELOG.md](CHANGELOG.md). This file tracks only what's **left**. Every item keeps
`make test` green (BIOS + UEFI) before it's checked off.

**Status:** `make check` = **344 host unit checks** (`make unit`, no QEMU) + the **smoke
tier** (13 deliberate boots: 10 BIOS incl. the in-OS `selftest` batch of 46 native checks,
3 UEFI). The full catalog (46 BIOS + 11 UEFI journeys) is `make test-all` — the release /
cross-cutting-change gate, **not** the per-increment loop. Pyramid + tier policy in
[`design/testing.md`](design/testing.md); the phased plan in
[`design/roadmap.md`](design/roadmap.md). tOS is early-to-mid development.

Legend: `[ ]` not started · `[~]` partial · `[⏸]` set aside (don't build unless asked).

---

## Open — the road ahead

### Toolkit & desktop UI
- [ ] **Files + Desktop suite (#10).** A shared `ui::FileView` powering both the Files window
  and a new bottom-pinned `WIN_DESKTOP` layer over `~/Desktop`: **multi-select** (Ctrl/Shift-click
  + rubber-band marquee — single-select today), **folder/multi-item copy-cut-paste** (today's
  `CLIP_FILE`-of-bytes can't hold a directory → path-reference clipboard + recursive `cp_r`),
  **rename**, context menus, and **drag-to-move** (the DnD protocol landed 2026-06-11; Files
  list-view drag-a-file/folder-onto-a-folder works — left: icon/gallery sources, inter-window,
  onto the desktop). Icon/gallery-view drag **sources** + onto-folder-tile drops landed 2026-06-13
  (`view_row_at`); left here: inter-window drags and onto the future desktop layer. **Keyboard shortcuts:** F2 rename,
  Ctrl+N new folder, Enter/Ctrl+O open, Delete (or Backspace) remove, Ctrl+A select-all,
  Backspace/Alt+← up a directory, plus the existing Ctrl+C/X/V — surfaced in the context menu and a
  menu bar (#6) so the accelerators show. → [`files-and-desktop.md`](design/files-and-desktop.md)
- [~] **Files app follow-ons (files-app.md).** The planned catalog landed 2026-06-11 (see
  CHANGELOG.md). **Done 2026-06-11:** the interactive **§6 status bar** — a clickable **zoom
  slider** (icon view) + `+`/`-`/`0` shortcuts, and a **Stop button** that halts a running job
  (the click twin of Esc). **Still open in the doc, none scheduled:** Miller columns §1 ·
  multi-select · Show in Groups §2 · templates / New Folder
  with Selection §12 · Apply-to-All conflicts §12 · jobs for Paste/Duplicate/Delete §12 ·
  search scopes + a Spotlight-shared indexer §5 · richer icon art §11.
  → [`files-app.md`](design/files-app.md)
- [~] **App menus (#6).** **Done:** the app→WM protocol — `SYS_WIN_SETMENU`/`SYS_WM_GETMENU`
  (≤5 menus × 12 items), `WEV_MENU` picks, the `ui::Window` menu API + `on_menu()`; per-item
  `WMI_DISABLED`/`WMI_CHECKED` flags and Ctrl-accelerator letters (greyed rows, ✓ marks; the
  compositor intercepts `Ctrl+<letter>` for the focused window, opt-in per declared menu).
  Notepad / Files / Terminal ship bars (Terminal builds `struct winmenu` raw — the protocol
  isn't toolkit-only — and declares no Ctrl accelerators so ^C still reaches the shell).
  **Left:** submenus (a `struct winmenu` ABI bump — deferred until something needs nesting).
  → [`ui.md`](design/ui.md)
- [~] **Grow the toolkit + port apps.** **Done 2026-06-11:** a reusable header-only
  **`ui::Layout`** (column/row, fixed + stretch extents, even gaps, nest via `rect_of`) so
  toolkit apps stop hand-computing rects in `layout()`/`layout_widgets()`; **Settings** now
  lays its top bar + rows out with it (was a hand-rolled y-cursor) — see [`ui.md`](design/ui.md).
  **Correction:** `fastfetch` is a CLI **_package_** (a headless system-info tool → `/System/bin`,
  the shell's login banner), **not** a dock app — per [`packaging.md`](design/packaging.md)'s
  app-vs-package split it stays on the shell, so it was **not** ported to the toolkit (a brief
  `Fastfetch.app` GUI bundle was reverted). **Done 2026-06-13:** **Notepad** now lays its content
  stack (tab bar / editor-stretch / status band) out with `ui::Layout` — pixel-identical, the
  status inset derived via `rect_of` (boot-screenshot verified). **Findings:** Clipboard /
  Launchpad / Spotlight are intentionally left hand-rolled (centred-pill / asymmetric-margin
  overlays a symmetric-pad placer doesn't simplify — see [`ui.md`](design/ui.md)); the one layout
  that genuinely benefits, **Files' `layout_widgets()`**, is rewritten onto `ui::Layout` as part of
  the Files + Desktop suite below, where it's heavily touched anyway. **Left:** convert app panes to
  `ui::Layout` opportunistically as they're touched; row/grid sugar only if an app needs it.
### Global text-interaction contract
The toolkit owns the in-window text contract: anything in `TextField` is inherited by every
toolkit app for free. **Done:** blink caret, drag-select, Ctrl+A, double-click word-select,
Ctrl+←/→ word-jump, Ctrl+Backspace/Delete word-delete, Delete, shift-select, undo/redo
(Ctrl+Z / Ctrl+Y), and the **I-beam cursor over every text field** (2026-06-10, via the
`SYS_WIN_SETCURSOR` cursor-hint protocol + `Widget::cursor_at` — was blocked on exactly that
protocol). **Left:**
- [x] **Cross-app text drag (2026-06-11).** A `TextField` selection, pressed-and-dragged from
  *within* the selection, arms `begin_drag(DRAG_TEXT, preview, bytes, n)`; twm runs the ghost +
  routing; the drop target's default `Window::on_drop` routes `DRAG_TEXT` to the `Widget` under the
  cursor via `accept_text_drop(x,y,s,n)`, which `TextField` overrides to insert at the drop point
  (**copy** semantics for v1). Because the payload lives in the kernel and twm routes `WEV_DROP` by
  window-under-cursor, any toolkit text field is a source *and* a target for free (Notepad's editor,
  unmodified, gets text-drop). Verified: select "world", drag it left → "worldhello world" (ghost
  chip screenshotted mid-flight).
  - [x] **Move semantics + primary selection (2026-06-13).** A drop back into the **same** field now
    **moves** the text (deletes the source span, leaves the moved run selected) unless **Ctrl** is held
    (then it copies, mirroring the Files file-drag's copy-on-Ctrl); a cross-field drop still copies (v1).
    The same-field source is tracked in `ui_textfield.cpp` (`s_text_src` + the span); the post-delete
    insert offset is the pure `tu_textmove_dest(a,b,p)` in `textutil.h` (unit-tested, `t_textutil`). The
    X11-style **primary selection** landed too: every `TextField` selection path (drag-select, double-click
    word, Ctrl+A, shift-arrows) updates a shared primary buffer, and a **middle-click** (`WEV_MOUSE_MID`,
    bit2 — twm now forwards the middle button, like it does the right button) pastes it at the click via
    `TextField::paste_primary`. Verified by `repro_textdrag.py` (move → 11 B, middle-paste → 16 B).

### Input / event foundations
- [x] **Drag-and-drop protocol (2026-06-11).** A source arms a **typed payload** (`DRAG_FILES` /
  `DRAG_TEXT` / `DRAG_IMAGE` / `DRAG_PLACE`) with `begin_drag()`; the kernel holds the bytes
  (`kernel/drag.c`, mirrors the clipboard); the compositor (twm) draws a **ghost chip**, hit-tests
  windows, posts `WEV_DRAG` (highlight) + `WEV_DROP` (release); the target reads the payload via
  `drag_payload()` in its toolkit `on_drop(x,y,type,data,len)` hook (plus the new window-level
  `on_press(x,y,btn)` hook so an app can note where a gesture began). **Consumers:** Files
  drag-to-move (file/folder onto a folder row → moved); cross-app **text drag** (toolkit-wide);
  Files **drag-reorder Places** (`DRAG_PLACE`, ghost chip + accent insertion line, persisted to the
  registry — verified Downloads→above Desktop). **Esc-to-cancel** a drag (twm clears the session, no
  drop) and **copy-on-Ctrl** for the Files file-drag (mods ride in the `WEV_DROP`'s packed byte →
  `Window::drop_mods`; Ctrl+drop copies, leaving the source) both landed too.
  - [x] **Icon/gallery-view drag sources (2026-06-13).** The Files file-drag now works in **every**
    view, not just list: the source arms `DRAG_FILES` from `cur_sel()` (the active view's selection),
    and the drop hit-tests with a new view-aware `view_row_at()` (`grid.index_at` / `gal.index_at` /
    `list_row_at`), so a file/folder TILE drops onto a folder TILE — the icon tile under the drag gets
    the same accent drop-target ring the list row does. Verified by `repro_icondrag.py` (drag doc.txt's
    tile onto the dest folder tile → moved on disk; ghost + tile-highlight screenshotted). Unlocks the
    desktop + Pocket Dimension.

### System & security
- [~] **System ownership (#1).** **Done:** tosfs v3 carries a per-entry `owner`; tasks carry a
  `uid` (init=system, the desktop session drops to user); the mutating fs syscalls enforce
  `tos_may_write()`; the shell prints `permission denied (system file)` and ships an `id` builtin.
  Files now shows a **gold padlock badge** on system-owned items in the list/icon/split views
  (off the new `dirent.owner`, no per-row stat) and **greys Cut/Rename/Delete** in the context
  menu, with a status-bar deny-flash on the keyboard/toolbar paths (2026-06-11).
  **Remaining (folded into the Desktop suite below):** the same lock badge on the future
  `WIN_DESKTOP` layer (waits on that layer existing). → [`system-ownership.md`](design/system-ownership.md)
- [ ] **Authentication & UAC elevation (system-ownership.md Phase 3).** Ownership Phase 1–2 landed
  (per-entry `owner`, per-task `uid`, `may_write()` enforcement, the Files lock badge), but the
  human-facing override is unbuilt: a **hashed user password** in a system-owned `/System/etc/auth`
  (set at install / first boot); a **system-drawn trusted prompt** — drawn by the compositor /
  authenticator, *never* the requesting app — that collects it (the same mechanism the capability
  runtime-permission prompts in Phase 4 reuse); a short-lived **elevated** task state
  (sudo-timestamp style); the mutating fs syscalls returning a distinct **"needs elevation"** result
  (not a flat `-EPERM`) that drives the prompt-then-retry flow; and the **App-Store-vs-terminal
  two-route** rule (a trusted first-party installer holds a standing install capability and doesn't
  prompt; `rm /System/...` or a hand-typed `tos package add` does). UAC, not sudo — no root account.
  → [`system-ownership.md`](design/system-ownership.md)
- [~] **Capability sandbox.** **Done (Phase 1 Declare + Phase 2 coarse-enforce, 2026-06-11):**
  a per-task `caps` bitmask (`kernel/cap.h`, shared with userspace via `syscall.h`); init/the
  boot chain run at `CAP_ALL`, `fork` inherits, `exec` keeps; `SYS_SETCAPS` (drop-only) +
  `SYS_GETCAPS`. The trusted launcher (twm's `launch()`) reads each bundle manifest's `caps`
  (`appcaps.h`) and **confines the child to it at exec** — apps default to `CAP_NORMAL` (every
  low-risk cap, none of the dangerous ones). The kernel **enforces `CAP_NOTIFY`** at the
  `SYS_NOTIFY` boundary (the one dangerous cap with a real syscall today); the `selftest`
  `group_caps` proves a normal app is confined and the notify gate refuses it. All four bundle
  manifests declare `caps`. **Done (Phase 3 fs path-jails, 2026-06-13):** the mutating *and* reading
  fs syscalls (open/read via the fd, mkdir/rmdir/unlink/rename/chdir/stat/readdir) now gate path
  access on the fs caps — the kernel walks a resolved slot to its top-level ancestor and requires the
  region's cap (`/System`→`CAP_FS_SYSTEM`, `/Users`→`CAP_FS_HOME`, `/Apps`→`CAP_FS_BUNDLE`; root/`/tmp`
  ungated). The pure region→cap table is `cap_fs_region_need` in `cap.h` (unit-tested, `t_cap`); the
  slot-walk jail is `cap_may_reach` in `fs.c`. A task holding all three fs caps (init + every
  `CAP_NORMAL` app) takes a fast-path, so the jail only bites a cap-dropped app — proven by the new
  `selftest group_fsjail` (a child that drops `CAP_FS_SYSTEM` can't open/stat/list `/System` but still
  writes home). **Left:** a *precise* per-bundle `fs:bundle` (an app sees only its OWN `/Apps/<x>.app`,
  not all of `/Apps` — needs the kernel to know each task's bundle root), and Phase 4 the runtime
  permission prompts + Settings review/revoke (needs the device caps + the trusted prompt to exist).
  → [`app-runtime.md`](design/app-runtime.md)
- [~] **Ctrl+C/X/V everywhere.** Landed in `TextField`, the terminal (Ctrl+Shift+C/X/V), and Files
  (files). Remaining: **folders** — folded into the Files + Desktop suite above.
- [⏸] **Pocket Dimension (Super+D).** A left-edge per-session shelf of stashed typed payloads.
  Don't implement unless explicitly requested; needs DnD.

### Platform / runtime / storage
- [ ] **Shell: interactive "fish feel" + the argv blocker + scripting (shell.md).** `shell.c` is a
  line editor over a fixed `if/else` builtin table — no parser, completion, highlighting, or argv.
  Three bands, cheapest-first:
  - [x] **Interactive layer — no kernel change (band 1 landed 2026-06-13).** Full-line redraw
    with synchronous **syntax highlighting** (first token green if it's a builtin or a
    `/System/bin` program, red if not — `cmd_known`), **Tab completion** (command names from the
    `shellcmds.h` catalog for the first token, `readdir` path completion otherwise; extends the
    common prefix, completes + adds a space/`/` when unique, lists the candidates when ambiguous),
    **history persisted** under `~/.config/shell_history` (`hist_load`/`hist_save`), and the emacs
    motions **Ctrl+A/E/B/F/U/K/W/L**. Only active when stdio is a pty (`isatty`); the bare
    console / serial tests keep the plain incremental editor. Pure helpers are unit-tested
    (`t_shellcmds`), the IO is screenshot-verified. **Deferred (serial-test fragility):**
    history **autosuggestions** (dim inline suffix) and **Ctrl+R** reverse-search both echo
    *previous commands* into the COM1-mirrored stream the harness substring-matches; and a Tab
    **grid pager with per-command descriptions** (the candidate list is a plain wrapped row today).
  - [x] **argv passing — the shared blocker (band 2 landed 2026-06-13).** `vmm_create_user` now
    splits the launch string at the first space: the first token is the ELF path (`fs_find`), and
    the **whole** string is seeded into the data page as the command line — so `exec("ls /tmp")`
    works with **no new syscall** (chosen over `SYS_EXEC2`; zero-arg execs like `exec("twm")` are
    byte-identical). Userspace reads it via `cmdline()` / `getargs()` (`user/lib/argv.h`'s pure
    `argv_split`, unit-tested in `t_argv`); the shell now **execs an unknown first token that
    resolves to a `/System/bin` program, passing the full argv** (`line_is_extprog` → `run_prog`).
    Proven by the new `/System/bin/args` demo (`args hello world foo` → `argc=4`, screenshot).
    **Next:** migrate the file utilities (`ls`/`cat`/`cp`/…) out of the shell into `/System/bin`
    programs, and wire the `tos` CLI (packaging below) onto the same argv.
  - [⏸] **The language.** A small fish-shaped grammar behind a real tokenizer/parser: quoting,
    `$VAR`/env, command substitution `(…)`, globbing, pipes + redirection (needs `SYS_PIPE`/dup),
    `;`/`and`/`or`, control flow + functions. Big effort + a user heap; **not this round** unless
    asked. → [`shell.md`](design/shell.md)
- [ ] **Userspace runtime + SDK sysroot.** sysroot + `tos-cc`/`tos-c++`; a hosted C++ runtime
  (STL/exceptions/RTTI/unwind); `libposix`; a QPA-style framebuffer/input shim. The line between
  "teaching OS" and "runs third-party software." → [`app-porting.md`](design/app-porting.md)
- [x] **Installer (live → install) — v1 (2026-06-12).** Whole-disk clone of the boot disk onto a
  virtio-blk target through the block layer (`install` shell cmd → `SYS_INSTALL` → `kernel/install.c`),
  MBR verify-read. **fs-on-bdev landed too:** the root fs + ELF loader read through the block layer and
  the mount scans every disk's MBR for the tosfs partition, so the install **boots independently off
  virtio-blk** (verified: clone → boot-as-IDE → boot-as-virtio, root mounted from virtio0, no IDE
  disk). **Left for a "real" installer:** mkfs/partition the target instead of cloning (copy
  `/System`+`/Apps`, seed `/Users` + registry), and GPT/ESP for UEFI targets. → [`installation.md`](design/installation.md)
- [~] **Device drivers (Phase 4).** **Done:** **virtio-blk** (legacy virtio-pci, polled DMA) +
  a **block-device layer** (`blockdev.c`; ata0 + virtio0 + ahci0 + nvme0); **AHCI/SATA + DMA**
  (`ahci.c`: MMIO ABAR, command-list/PRDT DMA, READ/WRITE DMA EXT + IDENTIFY; new `vmm_map_mmio()`
  for BARs in the PCI hole; installs onto + boots straight off a SATA disk); **NVMe** (`nvme.c`:
  MMIO regs, admin + I/O queue pair, PRP DMA, IDENTIFY ns; boots straight off an NVMe namespace);
  **ACPI** (`acpi.c`: RSDP→RSDT/XSDT→MADT CPU topology + FADT poweroff/reset, no AML; SMP discovers
  CPUs from the MADT; real S5 poweroff verified working alone); **virtio-net** (`virtio_net.c`: the
  first NIC — legacy virtio-pci, RX+TX queues, MAC, raw Ethernet frames; ARP round-trip self-test);
  **net stack** (`net/net.c` + `dhcp.c` + `tcp.c`: native ARP+IPv4+ICMP+UDP+DHCP+**TCP**) + **userspace
  networking** (CAP_NET-gated `SYS_NET_*` syscalls + shell `ping`/`get`; **tOS fetches a file over the
  network** via HTTP/1.0 — the Phase 4 exit criterion) (2026-06-12); **e1000** — a second NIC
  (`drivers/e1000.c`, Intel 8254x: MMIO BAR0, legacy RX/TX rings, MAC from RAL/RAH) behind a new
  NIC-agnostic **`netif` layer** (`net/netif.{c,h}`) the stack drives instead of naming a driver, first
  NIC up wins; an e1000-only box leases/pings/fetches through the same path, virtio unchanged (2026-06-13);
  **TCP server** — passive open (`net_tcp_listen`/`accept`) + CAP_NET-gated `SYS_NET_LISTEN`/`ACCEPT`
  (83/84) + a shell `serve <port>` one-shot HTTP server; **tOS serves a page over the network** (the
  host fetches it through a SLIRP host-forward), one connection at a time (2026-06-13);
  **serial console input** — COM1 RX on IRQ4 feeds the keyboard ring (CR/CRLF→Enter, DEL→backspace),
  so the shell runs **headless** over the serial line (`repro_serial_input.py`; harness gained an
  opt-in `serial_socket` mode), output already mirrors to COM1 (2026-06-13).
  **Next:** a fuller **sockets layer** — *multi-connection* state (a TCB table) + `bind`, so several
  clients/servers coexist; TCP retransmit/windowing for lossy links; GPT/ESP(FAT) writer; USB
  (xHCI+HID+MSC). GPU accel is VM-only.
  → [`roadmap.md`](design/roadmap.md)
  - **ACPI under UEFI — ✅ done 2026-06-13.** The UEFI loader now pulls the RSDP from the EFI ACPI
    config table (ACPI 2.0 GUID, 1.0 fallback) and hands its physical address to the kernel through
    `boot_info.acpi_rsdp`; `acpi_init()` validates+uses that before the legacy scan (BIOS passes 0
    → unchanged legacy EBDA/BIOS-ROM scan). Verified: UEFI now logs `[acpi] (UEFI handoff) rev 2
    (XSDT), 4 CPU(s) via MADT` and SMP comes up `4 of 4` from the MADT (was `[acpi] no RSDP` →
    `fw_cfg` fallback); BIOS still `[acpi] (scan) rev 0 (RSDT)`, PM1a=0x604, no regression. Full
    uACPI/LAI (AML, runtime ACPI, `_PRT`/IRQ routing) remains the long-term path.
- [ ] **Growable filesystem.** Files are contiguous and a metadata change rewrites the whole slot
  table; the partition is fixed-size. Want extent/indirect blocks, a runtime-sized partition, and a
  journaled table.
- [x] **Terminal scrollback.** Wheel + Shift-PgUp/PgDn over the ring done; the ring is now
  **heap-allocated and sized from `term.scrollback`** in the registry (default 256, clamped to
  at least one screenful), so the depth scales with the setting — no compiled-in wall (2026-06-11).
- [ ] **Terminal emulator → a real VT (terminal.md).** `term.c`'s 3-state ANSI parser handles the
  shell's own output but silently drops everything else, locking out real terminal programs. Grow it
  on Ghostty's correctness shape (parser → handler → terminal/screen → glyph-diff renderer; the
  renderer stays **software**): swap `feed()` for the **Williams DEC parser** (total — never wedges
  on malformed/partial input); a **cell/style model** with **256-colour + 24-bit truecolor** +
  italic/underline/reverse/dim/strike; **editing ops** (IL/DL/ICH/DCH/ECH, `DECSTBM` scroll region,
  DECSC/DECRC, index/reverseIndex, real tab stops); a **DEC mode table** (`?25` cursor, `?7` wrap,
  `?6` origin, `?2004` bracketed paste, mouse `1000/1002/1003/1006`); the **alternate screen**
  (`?1049`) — the single biggest unlock (full-screen TUIs: a future `vi`/`less`/`htop`); **UTF-8
  decode** (render what the font covers, box the rest); the **line-drawing charset** (`ESC ( 0`,
  needed by our own `tree`); **mouse reporting**; **OSC** (window title via a new `win_settitle`,
  clipboard `52` → the existing `clip_put`, cwd `7`, hyperlinks `8`); `DECSCUSR` cursor styles.
  `TERM`/terminfo waits on the env mechanism shared with the shell. Almost all userspace in `term.c`
  + the SDK — the pty is done. → [`terminal.md`](design/terminal.md)
- [ ] **virtio-gpu (virtio-gpu.md) — VM-only.** A paravirtual GPU for a better *presentation* path.
  **Level A (2D):** real modesetting (pick resolution — also unlocks VM window resize), a
  host-composited **hardware cursor**, and damage-driven `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`
  scanout (twm's existing dirty rects map one-to-one; the rasteriser doesn't change). Reuses the
  PCI/virtqueue transport the NIC drivers already have. **Level B (3D)** — GL/Vulkan via
  virgl/Venus — is a separate stretch. Metal stays on the software compositor.
  → [`virtio-gpu.md`](design/virtio-gpu.md)

### Packaging & software distribution
- [ ] **`tos` — apps, packages & the shared install engine (packaging.md).** The `.app` bundle
  format is built (four `/Apps/*.app` bundles, `tools/mkapp.py`, twm's `/Apps` scan), but the
  **install engine + CLI** the rest hangs off is not. Build: a **`.tpkg`** package format (the
  headless sibling of `.app`, same `key=value` manifest grammar) + `tools/mkpkg.py`; a shared
  **`copytree`** (also the OS installer's primitive); a **receipts DB** under `/System/var/tos/db/`
  (the exact file list per unit → exact uninstall, reverse-dep safety, upgrade-as-diff); and the
  **`tos` umbrella** (`tos app install/uninstall/list/info`, `tos package add/remove/list/info/
  upgrade`) — one engine, two front-ends. Phase 1 (`tos app install/uninstall` of a local `.app` →
  copytree into `/Apps` + a twm rescan) sits almost entirely on what exists. The real
  `/System/bin/tos` binary needs **argv passing** (shell, above); until then it can live as a shell
  built-in calling the same engine *library*. The elevation gate ties to the auth work above.
  → [`packaging.md`](design/packaging.md)
- [ ] **Software repository + remote update — `tos get` / `tos sync` (repository.md).** Now
  **unblocked** — networking + an HTTP `get` landed 2026-06-12. A *dumb static* repo (an `index`
  catalog in the manifest grammar + `apps/`/`pkg/`/`system/<channel>/` artifacts, each with an
  `sha256`); the **client** half of `tos` (`get install` / `search` / `sync` / `upgrade`) that
  resolves a name, downloads + **verifies the hash**, and installs through the packaging engine
  above — exercisable against a **local-mirror path** before a packet is sent. The payoff is the
  **remote system upgrade**: userland (`twm`/`term`/`shell`/`/System/bin`/`/Apps`) hot-swapped
  **write-new-then-rename** + live relaunch (no reboot); a **kernel** bump **staged** to the boot
  partition (the installer's raw block writer) + "reboot to finish." A `make repo` host target emits
  a servable tree straight from the build. Signing/TLS + channels are later hardening.
  → [`repository.md`](design/repository.md)

### Smaller ideas
- _(nothing queued right now)_

### Known issues (to investigate)
- ⚠ **One-off kernel #GP at `isr_restore`'s `iretq` (2026-06-11).** Seen once while typing in
  the shell right after `tests/repro_jobs.py`'s 40-file copy jobs **with a QEMU monitor
  `stop`/`cont` freeze around the mid-copy screenshot**: `#GP vector=13 err=0xea9c
  rip=isr_restore` — i.e. an iretq into a corrupted/garbage interrupt frame, kernel halted.
  Not reproduced since (same workload passed clean both **without** stop/cont and on a retry
  **with** it), so it's intermittent; the repro no longer uses stop/cont. If it recurs, suspect
  the frame/stack handoff in `isr_dispatch`'s return path under preemptible syscalls + heavy
  ATA PIO, or timer behaviour across a vm pause — not the Files app (userspace can't corrupt a
  kernel frame legally).
- ⚠ **Reported desktop freeze on a cross-pane click-drag in Files split view (2026-06-09).** A user
  reported that, in split view, pressing-and-holding inside one pane and dragging onto the other pane
  freezes the whole desktop (input stops everywhere, not just Files — "the Files bug nukes the whole
  OS"). **Status: not reliably reproducible** — neither the user nor I can trigger it on demand.
  **What's ruled out:** the Files-app side of the drag is benign. A left-drag from pane 1 into pane 2 is
  forwarded by twm to the Files window as `WEV_MOUSE_DRAG` packets (drag-capture via `cdrag`); on the app
  side `ListView::on_drag` is a no-op unless the scrollbar thumb is grabbed and the window-level
  rubber-band `on_drag` is unimplemented, so the drag only produces the initial `[files] sel` and then
  nothing — no loop, no `[EXCEPTION]`/`PANIC`. `SplitDecor` is decorative with a zero rect and never
  receives the drag; `cdrag` is cleared on button-up. **The danger, regardless of trigger:** a userspace
  app must *never* be able to wedge the compositor — so if real, the locus is twm's input loop, not Files.
  **Lead for next time:** the most likely mechanism is twm's `down`/`cdrag`/`last_b` getting stuck
  "button held" after an odd button-up ordering, which would suppress hover + swallow clicks
  desktop-wide → apparent freeze. The QEMU repro is confounded because the harness `drag()` uses
  *relative* `mouse_move` packets that can themselves desync the emulated PS/2 pointer/button (a state a
  real mouse self-heals), so harness "input stalls" after a drag aren't trustworthy evidence. Repro
  scaffold kept at `tests/repro_split_drag.py`. To investigate: instrument twm's button/`cdrag` state
  across a client drag and guarantee input servicing can't wedge on a malformed button sequence.
  **Update (2026-06-10):** the press-vs-hover fix (hover posting frozen while a button is held +
  an explicit release packet to the `cdrag` owner — see the changelog) reworked exactly this
  button/`cdrag` machinery; if the freeze is ever seen again, re-test on a build with that fix
  first.
