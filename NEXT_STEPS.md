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
  onto the desktop). **Keyboard shortcuts:** F2 rename,
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
  `Fastfetch.app` GUI bundle was reverted). **Left:** convert the other toolkit apps' panes to
  `ui::Layout` as they're touched; row/grid sugar only if an app needs it.
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
  chip screenshotted mid-flight). **Left:** *move* semantics (delete source on same-field drop) +
  the X11-style **primary selection** (middle-click paste of the last selection).

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
  `Window::drop_mods`; Ctrl+drop copies, leaving the source) both landed too. **Left:**
  icon/gallery-view drag sources (list view today). Unlocks the desktop + Pocket Dimension.

### System & security
- [~] **System ownership (#1).** **Done:** tosfs v3 carries a per-entry `owner`; tasks carry a
  `uid` (init=system, the desktop session drops to user); the mutating fs syscalls enforce
  `tos_may_write()`; the shell prints `permission denied (system file)` and ships an `id` builtin.
  Files now shows a **gold padlock badge** on system-owned items in the list/icon/split views
  (off the new `dirent.owner`, no per-row stat) and **greys Cut/Rename/Delete** in the context
  menu, with a status-bar deny-flash on the keyboard/toolbar paths (2026-06-11).
  **Remaining (folded into the Desktop suite below):** the same lock badge on the future
  `WIN_DESKTOP` layer (waits on that layer existing). → [`system-ownership.md`](design/system-ownership.md)
- [~] **Capability sandbox.** **Done (Phase 1 Declare + Phase 2 coarse-enforce, 2026-06-11):**
  a per-task `caps` bitmask (`kernel/cap.h`, shared with userspace via `syscall.h`); init/the
  boot chain run at `CAP_ALL`, `fork` inherits, `exec` keeps; `SYS_SETCAPS` (drop-only) +
  `SYS_GETCAPS`. The trusted launcher (twm's `launch()`) reads each bundle manifest's `caps`
  (`appcaps.h`) and **confines the child to it at exec** — apps default to `CAP_NORMAL` (every
  low-risk cap, none of the dangerous ones). The kernel **enforces `CAP_NOTIFY`** at the
  `SYS_NOTIFY` boundary (the one dangerous cap with a real syscall today); the `selftest`
  `group_caps` proves a normal app is confined and the notify gate refuses it. All four bundle
  manifests declare `caps`. **Left:** Phase 3 fs path-jails (`fs:home`/`fs:bundle`), and Phase 4
  the runtime permission prompts + Settings review/revoke (needs the device caps to exist).
  → [`app-runtime.md`](design/app-runtime.md)
- [~] **Ctrl+C/X/V everywhere.** Landed in `TextField`, the terminal (Ctrl+Shift+C/X/V), and Files
  (files). Remaining: **folders** — folded into the Files + Desktop suite above.
- [⏸] **Pocket Dimension (Super+D).** A left-edge per-session shelf of stashed typed payloads.
  Don't implement unless explicitly requested; needs DnD.

### Platform / runtime / storage
- [⏸] **Real shell + scripting.** Replace `shell.c`'s hardcoded `if/else` dispatch with a real
  lexer/parser + exec model (quoting, `$VAR`/env, pipes, redirection, `;`/`&&`/`||`, globbing,
  background `&`, scripts). First step: drop `help`, move demo/diagnostic builtins to `/System/bin`
  programs. Big effort; **not this round** unless asked.
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
