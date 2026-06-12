# tOS — changelog

What has **landed**, plus the history of resolved issues. What's *left* is in
[NEXT_STEPS.md](NEXT_STEPS.md); how the system works today is in [PROJECT.md](PROJECT.md).

## Landed work (newest first)

Terse one-liners; the full prose lives in git history and the design/ docs.

- **Userspace networking: net syscalls + `ping`/`get` — Phase 4 exit criterion (2026-06-12).** Exposed
  the kernel TCP/IP stack to userspace via five **CAP_NET-gated** syscalls
  (`SYS_NET_PING`/`CONNECT`/`SEND`/`RECV`/`CLOSE`, 78–82) with `ulib` wrappers, and two shell commands:
  `ping <ip>` (ICMP) and `get <ip> <port> <path>` (an HTTP/1.0 GET over the TCP client). The Terminal
  manifest now declares the `net` cap, so the shell can use them (the cap stays **enforced** for every
  other app — the in-OS `selftest` drops to `CAP_NORMAL` and confirms net is then refused).
  **tOS now fetches a file over the network** from a userspace program — the Phase 4 networking exit
  criterion — verified end-to-end against a host HTTP server through SLIRP (`get` prints the response
  body). Also shortened the TCP poll budget so a recv loop spins fast instead of blocking per call.
- **Net L4: TCP client — Phase 4 #7 (2026-06-12).** `kernel/net/tcp.c`: a minimal single-connection
  TCP client (active open) on net.c's IPv4 layer — 3-way handshake (SYN/SYN-ACK/ACK), push/recv with
  the **mandatory pseudo-header checksum** and seq/ack tracking, and a FIN close. No retransmit/options
  yet (the local link is reliable), fixed window, one connection at a time. Added a generic
  `net_ip_tx` (IPv4 send via the gateway) shared by TCP. Boot self-test connects to a host echo server
  at 10.0.2.2:7777 (SLIRP aliases the host), sends `tOS-tcp`, reads the echo, and closes — verified
  `[net] TCP echo OK` against a real Python server in the probe. With no server it gets a fast RST and
  skips (not a failure), so the plain NIC probe is unaffected. Next: a sockets syscall layer for apps.
- **Net L4: UDP + DHCP — Phase 4 #7 (2026-06-12).** `kernel/net/net.c` gains **UDP** transmit
  (`net_udp_tx`, IPv4 checksum-0 datagrams), and `kernel/net/dhcp.c` is a tiny **DHCP client** that
  runs the full DORA exchange (DISCOVER → OFFER → REQUEST → ACK; BOOTP fixed fields + the options
  cookie/53/50/54/55). At boot the guest now **leases its address** instead of hardcoding it
  (`[net] DHCP lease 10.0.2.15` against QEMU's SLIRP DHCP server), then pings the gateway. Verified
  against `-netdev user`. Next up the stack: TCP + a sockets syscall layer.
- **Net L3: ARP + IPv4 + ICMP (ping) — Phase 4 #7 (2026-06-12).** `kernel/net/net.c` (new `kernel/net/`
  module): the first layer of a native TCP/IP stack on top of virtio-net. An **ARP** resolver + cache,
  **IPv4** framing with the internet checksum, and **ICMP** echo. `net_ping()` ARP-resolves the
  next hop (gateway), builds Ethernet→IPv4→ICMP, TXes it, and polls RX, demuxing replies. Boot
  self-test pings the SLIRP gateway 10.0.2.2 and gets the echo reply (`[net] ping 10.0.2.2: reply` /
  `[net] selftest OK`) — verified against `-netdev user`. Static config for now (guest 10.0.2.15, gw
  10.0.2.2); DHCP will replace it. Next: UDP → DHCP → TCP + a sockets syscall layer.
- **virtio-net — the first network driver, Phase 4 #7 (2026-06-12).** `kernel/drivers/virtio_net.c`:
  legacy virtio-pci NIC (transitional `virtio-net-pci`), reusing the split-virtqueue transport from
  virtio-blk. Two queues (RX queue 0 + TX queue 1), negotiates only `VIRTIO_NET_F_MAC` (offloads off,
  so a zeroed 10-byte `virtio_net_hdr` and plain Ethernet frames), reads the MAC from device config,
  pre-posts RX buffers, and exposes `virtio_net_tx`/`virtio_net_rx` (frames in, frames out -- the whole
  driver contract; the protocol stack lives above it). Buffers come from `vmm_alloc_surface`
  (identity-mapped, so the pointer is the DMA address -- no bounce). A **kernel boot self-test** TXes an
  ARP "who-has 10.0.2.2" (the SLIRP gateway) and polls RX for the reply: verified end-to-end against
  `-netdev user` -- `[virtio-net] up: MAC ...` then `[virtio-net] ARP reply: 10.0.2.2 is at
  52:55:0a:00:02:02` (QEMU's gateway MAC) + `selftest OK`. Absent a NIC it prints `none` and boots
  normally (smoke unchanged). Next: the TCP/IP stack (ARP→IPv4→ICMP→UDP→DHCP→TCP) + a sockets syscall.
- **ACPI: MADT topology + FADT poweroff/reset — Phase 4 #6 (2026-06-12).** `kernel/acpi.c`: real ACPI
  table parsing (no AML interpreter). Finds the RSDP (legacy EBDA/BIOS scan), walks the RSDT/XSDT, and
  pulls out the **MADT** (enabled CPUs' Local-APIC ids) and the **FADT** (PM1a control port + the `_S5`
  sleep type via the standard tiny byte scan, plus the RESET_REG). SMP now discovers CPUs from the
  MADT (real APIC ids that work on hardware/any VM) instead of the QEMU-only `fw_cfg` count, falling
  back to `fw_cfg`. `SYS_SHUTDOWN`/`SYS_REBOOT` issue a real ACPI S5 poweroff / ACPI reset, generalising
  the old hardcoded magic ports (which stay as a fallback) — on QEMU the parse independently derives
  PM1a=`0x604`, exactly the old constant. Tables are reached via `vmm_map_mmio` (so any e820 region,
  not just the RAM identity map). Verified: MADT reports the right CPU count (`-smp 4`→4, SMP 4/4 from
  MADT; `-smp 2`→2) and ACPI S5 poweroff **works on its own** (QEMU exits with the magic-port fallbacks
  patched out). Under UEFI the legacy RSDP scan finds nothing and it falls back cleanly (`[acpi] no
  RSDP` → `fw_cfg` + magic ports, no regression); follow-on is to pass the RSDP from the UEFI loader.
- **NVMe DMA block driver — Phase 4 #3 (2026-06-12).** `kernel/drivers/nvme.c`: a clean-room NVMe 1.x
  driver over the memory-mapped controller registers (BAR0/1, mapped with `vmm_map_mmio`). Probes PCI
  for an NVMe controller (class 01.08), enables memory-space + bus-master, disables then re-enables the
  controller, programs the **admin queue pair**, creates **one I/O queue pair** (Create I/O CQ then SQ),
  and `IDENTIFY`s namespace 1 for capacity + LBA size. Polled DMA via **PRPs** (PRP1 + a PRP-list page
  for transfers over 2 pages), bounce-buffered; completions tracked by **phase tag**. Registers as
  `nvme0` (512-byte-LBA namespaces; other LBA sizes are reported and skipped). A **kernel boot
  self-test** round-trips the last sector through the bdev API (`[nvme] selftest OK`). Verified
  end-to-end: tOS **boots straight off an NVMe namespace** (`-device nvme`) via the same fs-on-bdev +
  ELF-loader path — root mounts from `nvme0`, desktop comes up. How modern machines boot. Absent a
  controller it prints `[nvme] none` and boots normally (smoke tier unchanged). Next: GPT/ESP writer.
- **AHCI/SATA DMA block driver — Phase 4 #2 (2026-06-12).** `kernel/drivers/ahci.c`: a clean-room
  AHCI 1.x driver over the memory-mapped HBA register block (the ABAR, BAR5). Probes PCI for a SATA
  HBA (class 01.06), enables memory-space + bus-master, brings up the first port with a SATA disk
  (command list + received-FIS area + a slot-0 command table, all in identity-mapped frames so the
  pointers *are* the device-visible physical addresses), and does polled DMA via a single-entry PRDT:
  `READ DMA EXT`/`WRITE DMA EXT` for I/O and `IDENTIFY` for capacity, bounce-buffered like virtio.
  Registers as `ahci0`. Needed a new VMM primitive: **`vmm_map_mmio()`** — device BARs live in the PCI
  hole, *outside* the RAM identity map, so it carves cache-disabled 2 MiB huge pages from a dedicated
  higher-half PDPT slot (508); it's also wired into the live bootstrap page tables so MMIO probed
  during `kmain` (before the scheduler switches CR3) is reachable. A **kernel boot self-test** round-
  trips the last sector through the bdev API (`[ahci] selftest OK`). Verified end-to-end: tOS
  **installs onto and boots straight off a SATA disk** (`-device ich9-ahci`) via the same fs-on-bdev +
  ELF-loader path — root mounts from `ahci0`, desktop comes up, no IDE disk present. The realistic
  desktop install target; replaces PIO. Absent an HBA it prints `[ahci] none` and boots normally
  (smoke tier unchanged). Next in the batch: NVMe.
- **fs-on-bdev + true boot-from-installed-disk (#11, 2026-06-12).** The root fs (`kernel/fs/fs.c`)
  now reads/writes through the block layer and, at mount, **scans every registered block device's MBR
  for the tosfs partition** and binds to the first match (`fs_disk_bdev()`). The ELF program loader
  (`vmm.c`) routes through the same bdev. So tOS boots off **whichever disk carries the install** —
  IDE *or* virtio-blk. Proven: the installed virtio image, booted as the only (virtio) disk, mounts
  root from `virtio0` and reaches the desktop with no IDE disk present. Also fixed a virtio DMA bug
  this exposed: the device needs **physical** buffer addresses, but kernel `.bss`/stack buffers live
  in the higher half (virt != phys) — added a **bounce buffer** (DMA only touches identity-mapped
  memory; copy to/from the caller). Verified by a 3-phase boot (clone → boot-as-IDE → boot-as-virtio).
- **Live → disk installer (#11, 2026-06-12).** `kernel/install.c` + a shell `install` command
  (`SYS_INSTALL`): clones the boot disk (`bdev0`/ata0) onto a target block device (`bdev1`/virtio0)
  sector-for-sector through the block layer, then verify-reads the MBR. Phase 1 of the boot test
  installs onto a blank virtio-blk disk (`[install] done, verify OK`, 6144 sectors). The first
  concrete payoff of the storage stack.
- **Block-device layer (2026-06-12).** `kernel/drivers/blockdev.c`: a small registry of named disks
  (`bdev_register/read/write/find/dump`, 64-bit LBA) so the installer + future drivers target a disk
  by index instead of a specific driver. ATA registers as `ata0` (capacity via a new `ata_sectors()`
  IDENTIFY + `ata_bdev_*` chunking adapters), virtio-blk as `virtio0`. Additive — fs keeps its direct
  ATA boot path (no regression). The virtio self-test now runs *through* the bdev API.
- **virtio-blk DMA block driver — Phase 4 #1 (2026-06-11).** The first real (non-PIO) block device:
  `kernel/drivers/virtio_blk.c` over the legacy virtio-pci transport (QEMU's `if=virtio` transitional
  device). Probes PCI bus 0 for `1af4:1001/1042`, enables I/O+bus-master, negotiates
  ACK→DRIVER→DRIVER_OK, builds a split virtqueue (desc | avail | page-aligned used) in
  identity-mapped frames (phys == virt, so a descriptor address *is* the CPU pointer), and does
  polled read/write via a 3-descriptor request (header → data → status). Added `pci_write32` and
  `inw`. Hooked into `kmain` after `vmm_init`; a **kernel boot self-test** (Linux-style) round-trips
  the last sector non-destructively (save → write pattern → read back + compare → restore) and prints
  `[virtio-blk] selftest OK`. Absent a virtio disk it prints `none` and boots normally (smoke tier
  unchanged). Verified: a 16 MiB virtio disk reports 32768 sectors, self-test passes, IDE boot intact.
  Unblocks the installer (live → disk). Next in the batch: AHCI/SATA + DMA.
- **DnD polish: Esc-cancel + copy-on-Ctrl (2026-06-11).** A drag in flight now cancels on **Esc**
  (twm tears down the session, clears the drop-target highlight, erases the ghost — no drop is
  delivered; `[twm] drag cancel`). **Ctrl+drop copies** instead of moving in Files: the compositor
  already packs `kbd_mods` into the `WEV_DROP` byte, now surfaced to apps as `Window::drop_mods`, so
  `FilesWin::on_drop` does `copy_tree` (leaving the source) when Ctrl is held. Also fixed a latent
  bug the cancel exposed — a source's drag-arm flag is now reset on the next press (`on_press`), so a
  cancelled or dropped-elsewhere drag no longer wedges the next one. Verified in one boot
  (Esc-cancel leaves the file in place; Ctrl+drop duplicates it into the folder).
- **Drag-reorder Places (2026-06-11).** The Files sidebar's Favorites are now reorderable by drag,
  on the DnD keystone with a new `DRAG_PLACE` payload. A new window-level `Window::on_press(x,y,btn)`
  hook lets Files note which Favorites row a press landed on (fresh each gesture, so it never tangles
  with the file-list rubber-band/drag); a drag from it arms `DRAG_PLACE` (the row's path, ghost label
  = its name); twm runs the ghost; `FilesWin::on_drop` maps the drop y to an insertion gap and calls
  `places_move()`, then persists the order to the registry. The sidebar paints an accent insertion
  line at the live gap. Verified by a boot dragging "Downloads" above "Desktop" (reorder 3 → 1, new
  order + ghost + line screenshotted). Design: [`files-and-desktop.md`](design/files-and-desktop.md).
- **Cross-app text drag (2026-06-11).** The DnD keystone's second consumer, in the toolkit so every
  text field inherits it. Press-and-drag from *within* a `TextField` selection arms a `DRAG_TEXT`
  payload (preview label = the selected text); the drop target's default `Window::on_drop` routes
  `DRAG_TEXT` to the `Widget` under the cursor via the new `accept_text_drop()` hook, which
  `TextField` overrides to insert at the drop point (**copy** semantics). Payload-in-kernel +
  twm-routes-by-window-under-cursor means any field is both source and target with no per-app code
  (Notepad's editor, unchanged, drops text). Verified by a boot: select "world", drag it left →
  "worldhello world", with the translucent text ghost screenshotted mid-flight. Design:
  [`ui.md`](design/ui.md).
- **Drag-and-drop protocol — the keystone (2026-06-11).** A general DnD pipe spanning kernel →
  compositor → toolkit → app. A source arms a **typed payload** (`DRAG_FILES`/`DRAG_TEXT`/
  `DRAG_IMAGE`) with `begin_drag()`; the kernel holds the bytes in a single session
  (`kernel/drag.c`, mirrors `clipboard.c`, payload kept past `drag_end` so the target's post-drop
  read works); twm runs the **visual session** — a translucent **ghost chip** trailing the cursor,
  hit-tests the window under it, posts `WEV_DRAG` (drop-zone highlight, `0xfff` on leave) and
  `WEV_DROP` (release) — and the target reads the payload via `drag_payload()` in the toolkit's new
  `on_drop(x,y,type,data,len)` / `on_drag_over(x,y)` hooks (`SYS_DRAG_BEGIN/PAYLOAD/STATE/END`,
  73–76). **First consumer:** Files **drag-to-move** — drag a file/folder onto a folder row and it
  moves in (undoable `OP_MOVE`, accent drop-target highlight, system-owned items refuse to drag).
  Verified by a boot that dragged Downloads onto Desktop (the ghost visible mid-drag) + the full
  smoke tier. Design: [`files-and-desktop.md`](design/files-and-desktop.md), [`ui.md`](design/ui.md).
- **Toolkit `ui::Layout` + Settings ported to it (2026-06-11).** The `ui::` toolkit gained a
  header-only **linear layout placer** (`user/lib/ui.h`): a column (`LAY_COL`) or row (`LAY_ROW`)
  that splits a bounds rect among items with **fixed or stretch extents** (extent 0 stretches and
  shares the leftover, remainder spread 1px at a time to sum exactly), `space()` for empty gaps,
  and **nesting via `rect_of(i)`** — so apps stop hand-computing every widget rect (a running
  `y`-cursor + `w-2*pad` widths) in `layout()`. **Settings** now lays its full-bleed top bar +
  the 8 setting rows out with an outer column + a padded inner column (was a hand-rolled loop);
  verified by a boot screenshot. **Course-correction:** `fastfetch` is a CLI **package** (a
  headless system-info tool living in `/System/bin`, the shell login banner), **not** a desktop
  app — per [`packaging.md`](design/packaging.md)'s app-vs-package split — so a brief `Fastfetch.app`
  GUI/dock bundle was **reverted** and fastfetch stays a `/System/bin` CLI. Design: [`ui.md`](design/ui.md).
- **Capability sandbox — Phase 1 Declare + Phase 2 coarse enforcement (2026-06-11).** Apps are no
  longer implicitly all-powerful: `struct task` gained a `caps` bitmask (`kernel/cap.h`, shared
  with userspace through `syscall.h`). init/the boot chain (twm/term/shell) run at `CAP_ALL`;
  `fork` inherits caps, `exec` keeps them, and the new **drop-only** `SYS_SETCAPS` (+ `SYS_GETCAPS`)
  lets a task shed authority but never gain it. The trusted launcher — twm's `launch()` — reads
  the target bundle's manifest `caps` via the new `appcaps.h` mapping and **confines the forked
  child to exactly those bits before exec**; an app with no `caps` line gets `CAP_NORMAL` (window,
  fs:bundle/home/system, time, spawn — every low-risk cap, none of the dangerous ones). The kernel
  **enforces `CAP_NOTIFY`** at `SYS_NOTIFY` (the only dangerous cap with a live syscall today;
  net/camera/mic/location are declared but await the hardware). All four bundle manifests now
  declare `caps`. Verified by a new `selftest` `group_caps`: a normal app holds window/spawn/fs:home,
  **lacks notify/net, and the kernel refuses its `SYS_NOTIFY`** — while twm (CAP_ALL) still posts
  notifications. Design: [`app-runtime.md`](design/app-runtime.md).
- **Files interactive status bar — zoom slider, zoom shortcuts, Stop button §6 (2026-06-11).**
  The bottom bar gained controls: a right-aligned **3-stop zoom slider** in icon view (click a
  stop to resize the tiles; mirrors the per-folder `zoom` state), the `+`/`=`/`-`/`0` **zoom
  keyboard shortcuts** (zoom in / out / actual size), and a red **Stop pill** that appears while
  a background copy/search job runs and cancels it on click (the click twin of Esc). The
  `StatusBar` became interactive: a `disabled`-aware `Popup` was already there, so this added
  per-control hit rects routed through `FilesApp::dispatch_mouse`; `RPAD` keeps the controls
  clear of twm's bottom-right resize corner. New `[files] zoomrect` geometry canary. Verified on
  a boot (slider click + shortcuts drive `set_zoom`; the Stop pill shows mid-copy).
- **Terminal configurable scrollback ring size (2026-06-11).** The scrollback ring is now
  **heap-allocated at startup and sized from `term.scrollback`** in the registry (default 256,
  clamped up to at least one screenful), replacing the compiled-in 256-row static array — so the
  history depth scales with the setting instead of hitting a fixed wall. New seed key in
  `fs/etc/registry`; `sbch/sbfg/sbbg` became heap pointers + a runtime `sb_rows`; `[term]
  scrollback <n> rows` canary at startup. Disposable boot confirmed a non-default seed (1000)
  flows through.
- **Files lock badges + greyed actions on system-owned items #1 (2026-06-11).** Finishes the
  System-ownership UI: a gold **padlock badge** now sits at the lower-right of every
  system-owned item's icon in the list / details / icon / split views, and the context menu
  **greys Cut / Rename / Delete** on those items (a new `disabled` flag on the `Popup` rows).
  The keyboard/toolbar paths (`do_delete` / `copy_sel(cut)` / `start_rename`) short-circuit with
  a status-bar **deny-flash** (`[files] denied <name>`) instead of a silently-refused syscall.
  Owner now rides on `struct dirent` (filled by `readdir` from the tosfs entry), so the badge
  reads it **without a per-row `SYS_STAT`**; the Get Info pane's "Read only" badge was already
  there. New `G_LOCK` glyph + cached caller uid. Disposable boot + screenshots (`/Apps` bundles
  badged; the greyed Rename/Delete menu).
- **Filter/search bar click-away dismiss §5 (2026-06-11).** Clicking outside an open-but-empty
  filter/search bar dismisses it, like Esc — routed click-first so the click lands in the
  pre-close layout, then the chosen row is re-selected (close's `apply_filter` resets `sel`).
  A bar holding a live filter, or search results, is untouched by clicks; Esc remains the way
  out. Disposable boot `tests/repro_filterdismiss.py` + screenshots.
- **Background jobs + conflict prompts §12 and recursive search §5 (2026-06-11).** Cross-pane
  copy/move now runs as a **chunked job on the window tick** (`Window::on_tick` → 4 items/tick
  over a pre-collected tree list): the status bar shows "Copying k of n..." plus a 2px accent
  **permille band**, Esc cancels, and a colliding destination raises **Replace / Keep Both /
  Skip** (the shared `ConfirmDialog`; `[files] job conflict/start/done/skip/cancel`) before
  anything copies — moves carry tags and journal OP_MOVE for undo. **File ▸ Find** (^F) arms
  the **filter bar — which existed but was never wired or even add()ed to the window** — and
  Enter walks the tree from the current folder as the same kind of job (2 dirs/tick, dotfiles +
  `.app` skipped), streaming hits into the view with "in <dir>" status and double-click
  jump-to-hit (`[files] search start/done/open/close`). En route: the **`crumbend` e2e canary
  clamped inside the breadcrumb bar** (a long path pushed the click target past the bar's edge
  → dead clicks) and **`listrect` re-emitted when the filter bar opens/closes** (it shifts the
  list 29px). Verified: units + one disposable boot (`tests/repro_jobs.py`) + 4 screenshots
  (conflict card, frozen mid-copy band, results, jump); smoke 13/13.
- **Gallery view §1 (2026-06-10).** View ▸ as Gallery (item 7, appended; `[files] view
  gallery`): a full-size decoded preview (cached one image at a time) over a filmstrip of §11
  thumbnails — wheel pans, ←/→/Enter + double-click work, round-trips through §2 view memory;
  split view forces list. Disposable boot `tests/repro_gallery.py` + screenshots.
- **Thumbnails + Quick Look §11; lone-Esc toolkit fix (2026-06-10).** Real 96px previews for
  `.argb` images (pure `thumb.h` fit + box-average scale, unit `t_thumb`; eager per-folder RAM
  cache) in list rows, icon tiles and the gallery; **Space** toggles a Quick Look scrim+card
  (image fitted / text head / icon+summary), any click or Esc dismisses. Found en route: the
  toolkit **never delivered a bare Esc keypress** (the ANSI escape-sequence latch ate it);
  `ui::Window::run` now flushes a lone Esc after two idle drains as `UK_ESC` — fixes every
  in-app Esc binding OS-wide. Disposable boot `tests/repro_quicklook.py` + screenshots.
- **Tags / labels §10 + a scrolling sidebar (2026-06-10).** Finder-style colored tags on a
  `~/.tags` sidecar (no fs xattrs): pure codec `tagstore.h` (get/set/move; unit `t_tagstore`),
  carried across rename / move-to-trash / Put Back. Context-menu **"Tags..."** opens a
  stay-open picker (Popup grew per-item `checked` + colour `dot` + an `on_toggle` callback;
  Open With's shared toggle untouched) that writes through per flip (`[files] tags <path>
  <mask>`); details rows draw the dots overlapped, right-aligned in the Name column; the
  sidebar's **Tags section** (collapsed by default) filters the listing per color, click
  again to clear (`[files] tagfilter <name|off> <n>`, status "N of M shown"). Found via the
  verification boot: the expanded sections **overflowed past the pinned Trash row** and
  misrouted clicks to Trash — the sidebar now wheel-scrolls by whole rows, clips at the
  Trash divider, and `side_dump` reports only on-screen rows. Verified the NEW way: units +
  one disposable boot (`tests/repro_tags.py`) + screenshots; smoke tier 13/13, no new e2e.
- **Test-suite restructure: smoke tier + in-OS selftest (2026-06-10).** The suite had
  crept back to "boot full QEMU for every little thing" (56 boots per `make test`, re-run
  per increment). Now: **`make test` = a smoke tier** of 13 deliberate journeys
  (`SMOKE_BIOS`/`SMOKE_UEFI_LIST`); the full catalog moved behind **`make test-all`**
  (`--all`) as the release / cross-cutting-change gate. New **`selftest`** userspace
  program (`/System/bin/selftest`, shell command) runs **46 native fs / registry /
  fork-wait / statfs / clipboard assertions in one boot** — replacing a boot-per-area in
  the smoke tier — and prints `selftest: N/N OK` (failures name the expression). Policy
  sharpened in design/testing.md: features verify with **units + one disposable ad-hoc
  boot + screenshot**; new permanent e2e only for a critical journey or a pinned bug, and
  new in-OS checks go into `selftest` first.
- **Cursor hints + the press-vs-hover compositor bug; folder sizes + column resize (2026-06-10).**
  Root-caused a real twm input bug: on the press frame the hover block posted the hover-leave
  packet (0xfff,0xfff,0), which the toolkit reads as a **button-up** — so a widget grab (the
  header divider) was cancelled before the first `WEV_MOUSE_DRAG` arrived. Fix: hover posting is
  frozen while a button is held, and the release edge posts an explicit btn-0 packet to the
  `cdrag` owner (the leave/enter pair had been, by accident, the app's only up signal). Probe
  kept: `tests/repro_hdr_drag.py`. On top, **global cursor hints**: `SYS_WIN_SETCURSOR` (70) +
  per-window snapshot plumbing; twm shows the hint over that client (held live mid-drag); the
  toolkit's `Widget::cursor`/`cursor_at(x,y)` relays the hot widget's shape — every TextField
  OS-wide shows an I-beam (term's hardcoded title hack removed), the header divider shows **⇔**
  (the resize affordance; the hover highlight was dropped per user preference). And in Files:
  `load_dir` fills directory sizes via the recursive `dir_usage` walk (real Size cells + size
  sort for folders/.apps), and **View ▸ Info** (item 6, ^I) toggles the inspector. e2e
  `t_files_details` extended (divider drag widens Name ≥40 px; size-desc ranks a stuffed folder
  on top; screenshot with the ⇔ cursor).
- **Editable Places sidebar §7 (2026-06-10).** The 8 hardcoded sidebar rows became a sectioned
  list: **Favorites** (registry-backed editable pins — pure `places.h` codec, unit `t_places`;
  context-menu **Add to / Remove from Places**) + **Locations** (the volume row carries a statfs
  used-space bar), collapsible section headers, Trash pinned at the bottom. Shared `vrows`/`vrow`
  row model so draw + hit-test agree; rows dumped for e2e (`[files] siderow`, deduped). e2e
  `t_files_places` (pin → navigates → collapse/expand → survives in the on-disk registry →
  remove; screenshot). Still open: drag-reorder + pin rename (want DnD / inline-field plumbing).

- **Files undo / redo of file ops §12 (2026-06-10).** Edit ▸ Undo / Redo (Ctrl+Z/Y) invert the
  last file op via a pure journal (`undojournal.h`, unit `t_undojournal`; cap 24, a push
  truncates the redo tail) interpreting RENAME / MOVE / CREATE / COPY / TRASH with the existing
  fs helpers; the menu items grey via can-undo/can-redo. Menu caps `WINMENU_ITEMS`/`MENU_MAXI`
  8 → 12. e2e `t_files_undo`; canaries `[files] undo|redo <type> <path>`. Also fixed
  `_files_menu_open` clicking another app's same-named menu tile (take the *last* `[twm] appmenu`
  match, not the first).
- **Files details / column view §1 (2026-06-09).** List mode is a real details view: a
  Name | Kind | Size | Date Modified header — click sorts (▲/▼ caret, re-click flips), dividers
  drag-resize (Date fills the rest), widths remembered per folder; new `FSORT_DATE` off
  `dirent.mtime`. Pure width math `colfit.h` (unit `t_colfit`); `filesort.h`/`viewmem.h` extended
  compatibly. e2e `t_files_details`. Left: a Sort-menu Date item, grouping, column add/remove.
- **Files split / dual pane §4 (2026-06-09).** View ▸ Split View: a second lean pane (its own
  path/listing/selection), splitter + active-pane accent, **Copy / Move to Other Pane** (Edit +
  context menus, split-only) via `copy_tree` + dedupe. e2e `t_files_split`; canaries
  `[files] split / pane2 cd / copy-across / listrect2`. The shared `_files_nav` e2e helper now
  retries the whole open→type→Enter as a unit (was the top flake in nav-heavy runs).
- **Files tabs §4 (2026-06-09).** A tab strip of folder pills (each tab keeps its own folder +
  history + selection; hidden with one tab): New Tab ^T / the strip's + / Open in New Tab
  (context menu); the pill's × or ^W closes; relabels on navigate; growable heap store. `TabStrip`
  widget + pure `tabtitle.h` (unit `t_tabtitle`); e2e `t_files_tabs`; canaries
  `[files] tab new|sel|close` + `tabbar/tabpos`.
- **Files rich Get Info / Properties §8 (2026-06-09).** Selecting fills the Details pane with a
  folder's recursive size + item count (the `du`-style `dir_usage` walk), the Owner (System /
  You), a gold "Read only" lock badge (the `tos_may_write` rule), and Opens-with; Ctrl+I toggles
  the pane. Pure `fileinfo.h` (unit `t_fileinfo`); e2e `t_files_getinfo` (screenshots: a user
  folder + locked /Apps); canary `[files] sel <name> (ro|rw) owner=<uid> size=<n> [items=<n>]`.
- **statfs / free-space §6/§7 (2026-06-08).** New `SYS_STATFS` (69) off the tosfs sector bitmap;
  surfaced as the shell's `df` and a "<n> free" Details-pane footer. Pure formatter `humansize.h`
  (unit `t_humansize`); e2e `t_statfs`.
- **tosfs file timestamps (mtime) §8 (2026-06-08).** Every entry carries a packed FAT-style
  mtime (`kernel/fstime.h`, unit `t_fstime`), stamped from the CMOS RTC on create/write/mkdir
  (+ build time via mkfs), plumbed through `dirent`/`fstat` to the Details pane's Modified line.
- **Files Duplicate + New File §12 + tosfs 0-byte files (2026-06-08).** Duplicate clones
  Finder-style "X copy" (folders recursively via the new `copy_tree`); New File drops an empty
  `newfile.txt` and enters rename — which needed tosfs to persist real **0-byte entries**
  (`close_l` used to discard empty writes). Pure `dupname.h` (unit `t_dupname`); e2e
  `t_files_newdup`.
- **Userspace de-clutter — the big single files split into modules (2026-06-08).** The Makefile
  now compiles every `.c`/`.cpp` in an app's dir; twm (2325 lines) became `twm.c` + `twm.h` +
  `bar.c`/`dock.c`/`controlcenter.c`/`notify.c`/`switcher.c`/`menubar.c`; Files split out
  `fileswidgets.h` + `filesutil.{h,cpp}`; `TextField` moved to `ui_textfield.cpp`. No behaviour
  change (suites green).
- **Files Trash §9 — move / Put Back / Empty (2026-06-08).** Delete moves to `~/.Trash`
  (`rename_`, whole dirs too) with a `.trashinfo` origin sidecar (pure `trashinfo.h`, unit
  `t_trashinfo`); Put Back restores to the origin, Empty Trash / delete-inside-Trash remove for
  good; dotfiles hidden from listings + counts. e2e `t_files_trash`; the reusable
  `[files] listrect`/`ctxmenu` canaries + the `rightclick` harness helper landed here.
- **Exit-fullscreen no longer leaves black areas (2026-06-06).** `WEV_RESIZE` only set partial
  damage, so any same-drain hover made `redraw()` partial-paint the freshly resized surface; the
  resize handler now `invalidate()`s the whole surface (fixes every toolkit app's restore).
- **Picker hardening + modality (2026-06-06).** Pid-namespaced `/tmp/.picker-<pid>.req/.res`
  (new `SYS_GETPID`/`SYS_GETPPID` 67/68; naming centralised in `sys.c`) so concurrent pickers
  can't clobber each other; and `WIN_MODAL` — twm keeps the picker topmost + focused, dims
  everything behind a full-screen scrim, and swallows input outside it (no `wininfo.parent`
  ABI change needed). e2e `t_file_picker` + `t_launchers_exclusive`/`t_alt_tab` green.
- **Files per-folder view memory §2 (2026-06-06).** View mode + sort + zoom per folder in the
  registry (`view.<path>`, hashed past `REG_KEYMAX`; a stable `view.default` fallback); the
  on-navigate menu re-sync batched into one `win_setmenu` (the per-item republish raced menu
  clicks). Pure codec `viewmem.h` (unit `t_viewmem`); e2e `t_files_viewmem`.
- **Files in-place rename + path-bar/rename click-away (2026-06-06).** Inline rename over the selected
  tile in list or icon view (New Folder enters it Finder-style); Enter/click-away commit, Esc cancels.
  Clicking outside the editable path bar now reverts it like Esc. `dispatch_mouse` made virtual so
  `FilesApp` can hook click-away. e2e `t_files_rename` + extended `t_files_breadcrumb`.
- **Incremental directory flush — kills the notepad-autosave freeze (2026-06-06).** The real
  culprit behind "saving freezes the desktop": `flush_super()` rewrote the entire **189 KB**
  tosfs directory table through polled PIO on every file close (several times per autosave).
  New `flush_super_ent(slot)` writes only the 1–2 sectors holding the changed entry (~190× less
  metadata I/O), byte-identical on disk. Large *data* writes stay synchronous — see Known issues.

- **Preemptible syscalls — long disk ops no longer freeze the machine (2026-06-05).** Every
  syscall ran IF=0 (interrupt gate, no `sti`) with `irqsave` fs/ata locks, so a slow polled-PIO
  transfer starved the timer and froze the single-core desktop (the "typing in notepad freezes
  the OS" report). Fix: `sti` in the 0x80 dispatch; a new IF-preserving `spin_lock_preempt` for
  `fs_lock`/`ata_lock`; all disk I/O moved out of `sched_lock` (also fixing a multi-ms app-launch
  freeze and a 1-CPU deadlock); a `SYS_READ` lost-wakeup made check+block interrupt-atomic.
  Caveat: a write finishing inside one 10 ms tick still blocks — the full cure is async/DMA +
  a write-back cache (Known issues).
- **Damage-rect presents + notepad save/close fixes (2026-06-03).** New `win_present_rect` (66):
  a per-window damage union carried in `struct wmwin`; twm composites only that sub-rect, and the
  toolkit invalidates per-widget (typing/caret/hover no longer re-blit the whole client). Also
  fixed quit dropping a dirty background tab; close/save UX reworked (window close never prompts —
  the autosave draft holds everything; closing a *tab* guards); autosave debounced.
- **Notepad tabs + session autosave #5 (2026-06-03).** The tabbed editor + a new toolkit
  `Window::on_tick()` hook driving the session autosave to `~/.cache/notepad/`; a bare relaunch
  rebuilds the whole session, even never-saved notes (two-boot e2e `t_notepad_session`); the
  unsaved guard moved to tab/window close (`t_notepad_guard`).
- **Reusable file picker `ui::FileDialog` #4 (2026-06-03).** The in-process toolkit Open/Save
  modal (Favorites sidebar, Up + path bar, Save name field, ownership-greyed OK,
  Replace/Keep-Both/Cancel overwrite). *(Retired + deleted 2026-06-06 — replaced by
  Files-as-picker; the entry stays for the history.)*
- **Notepad default save location = Documents (2026-06-03).** Bare note names resolve to
  `~/Documents/<name>` instead of littering `$HOME`.
- **Notepad unsaved-changes guard + reusable `ui::ConfirmDialog` #5 (2026-06-03).** A modal
  sheet (dim scrim + up to 3 buttons, Enter = primary, swallows outside clicks) + a
  `Window::on_close()` veto hook; Notepad defers New/Quit on a dirty buffer until
  Save / Discard / Cancel answers. e2e `t_notepad_guard`; `[ui] dlgbtn` canary.
- **Terminal + Files ported onto the app-menu API #6 (2026-06-03).** Files declares File/Edit/Go
  via the toolkit; Terminal builds `struct winmenu` by hand (a raw-syscall app — proving the
  protocol isn't toolkit-only) with no Ctrl accelerators so ^C still interrupts the shell.
  e2e `t_files_menu` + `t_term_menu`.
- **TextField undo/redo — the global text contract (2026-06-03).** Bounded ring stacks of
  insert/delete span records with typing-run coalescing (pure `editlog.h`, unit `t_editlog`);
  every toolkit field inherits Ctrl+Z/Y; Notepad's Edit ▸ Undo enabled + Redo ^Y added.
  e2e `t_notepad_undo`.
- **User-program heap — confirmed already done (2026-06-03).** `user/lib/libc.c` already ships a
  full mmap-backed `malloc/free/realloc` (first-fit free list, coalescing, ≥1 MiB arenas); the
  stale "needs sbrk" bullet was removed.
- **LAPIC timer calibrated against the PIT (2026-06-03).** `lapic_timer_calibrate(hz)` measures
  the local timer over a gated PIT channel-2 one-shot at boot (no IRQs needed), replacing the
  magic QEMU-tuned count; implausible readings fall back to the old constant.
- **Live resize + reflow — verified already done (2026-06-03).** twm already streams `WEV_RESIZE`
  mid-drag and term/toolkit apps reflow live; the stale open bullet was removed.
- **App-menu accelerators + checkmarks + disabled items #6 (2026-06-03).** `winmenu` items gained
  `flags` (`WMI_DISABLED`/`WMI_CHECKED`) + an accel letter; twm greys/✓-marks rows, right-aligns
  `^X`, and fires `Ctrl+<letter>` as the matching `WEV_MENU` (opt-in per declared menu).
  `t_app_menu` extended.
- **App menus #6 (2026-06-02).** The app→WM menu protocol: `struct winmenu` via
  `SYS_WIN_SETMENU`/`SYS_WM_GETMENU`, `WEV_MENU` picks, the `ui::Window` menu API + `on_menu()`;
  twm draws a dropdown tile per menu. `t_app_menu`.
- **Maximize hides both bars + hover-reveal (2026-06-02).** Fullscreen fills the whole screen;
  the title bar + menu bar hide as one top group, revealed (and held) by a top-edge hover.
  Window geometry centralised in `is_fs`/`client_rect`/`outer_rect`/`in_client`. `t_fullscreen`.
- **System ownership #1 (2026-06-02).** tosfs v3: a per-entry `owner` uid; tasks carry a `uid`
  (init = system; the desktop session drops to user via `SYS_SETUID`); the mutating fs syscalls
  enforce `tos_may_write` (unit `t_perm`, e2e `t_system_ownership`); the shell prints
  `permission denied (system file)` and gains an `id` builtin.
- **Notification click routing (2026-06-02).** `notify_to(title, body, target)`: clicking a toast
  or a notification-center row focuses (or launches) the target app. `t_notif_click_routing`.
- **Dock pinned | running divider (2026-06-02).** `rebuild_dock()` records the boundary index
  (`dock_runsep`) after the last pinned tile; `draw_dock()` draws a faint 1px vertical separator
  in the gap before the first running-unpinned tile, shown only when one exists. `[twm] docksep`
  trace; asserted in `t_notepad_edit_save`.
- **Launchers mutually exclusive (2026-06-02).** `dismiss_launchers(except)` at every summon
  path; focus telemetry tracks the window *id* across slot reuse. `t_launchers_exclusive`.
- **UEFI boot fixed above 4 GiB (2026-06-02).** The loader identity-mapped only 0–4 GiB, so
  `MEM=8G` #PF'd on the CR3 switch; it now maps all of RAM from `GetMemoryMap`. `t_ram_scales`
  gained a 6G case.
- **Settings app uses Lucide glyphs (2026-06-02).** `ui::Button` gained optional `icon`/`value`;
  new `tools/genglyphs.py` → `user/lib/glyphs.h` bakes a reusable Lucide app-glyph set.
- **Multi-region frame pool across the 4 GiB hole (2026-06-02).** `vmm` reads the e820 map
  (`fw_cfg`), maps only real RAM + skips the PCI hole, uses RAM above 4 GiB; `MEM ?= 8G`; `memtest`.
- **Notification toast: shadow-smear fixed + Lucide `x` dismiss + global `ugfx_set_shadows`
  (`ui.shadows` key) (2026-06-02).**
- **Files Ctrl+C/X/V (files) over the clip ring + context menu (2026-06-02).**
- **Shift-select completed — Shift+click via `WEV_MOUSE_SHIFT` (2026-06-02).**
- **Terminal Shift-PgUp/PgDn scrollback paging (2026-06-02).**
- **Notification expand: Lucide chevron + per-row expand in the center (2026-06-02).**
- **Notification-center dirty-rect / shadow-halo fixes (2026-06-02).**
- **Crisp icons: premultiplied resampler + 128px masters + Lucide bar glyphs; tosfs 1→2 MiB
  (2026-06-01).**
- **Test suite rebuilt as a pyramid: 49 e2e → 19 e2e + 28 unit; `textutil.h` (2026-06-01).**
- **macOS-style Alt-Tab switcher overlay (#7, 2026-06-01).**
- **Terminal copy-path test coverage — `t_term_copy` (2026-06-01).**
- **Notification QoL: hover-pause, collapsible toast, slide-into-open-center, Clear (2026-06-01).**
- **Notifications / toasts: `notify()` → `SYS_NOTIFY` → toast + center + bell (2026-06-01).**
- **Status-bar cluster + registry-driven clock (2026-06-01).**
- **Single-sourced shadow-halo extents via `ugfx_elevation_extent` (2026-06-01).**
- **Global ScrollBar: `ugfx_scroll_thumb` + `ui::ScrollBar` (Files/Spotlight thumbs) (2026-06-01).**
- **Notepad / global text editing (#5): blink caret, word-select, word-jump/delete, Delete
  (2026-06-01).**
- **Rounded, bigger search bars (#14, 2026-06-01).**
- **Frosted-glass UI pass: `ugfx_frost` blur under bar/dock/CC/Launchpad (2026-05-31).**
- **Launchpad polish (#11): centred grid + type-to-filter (2026-05-31).**
- **Draggable scroll indicator (#12, 2026-05-31).**
- **Spotlight keyboard QoL (#13): arrows/Tab walk results (2026-05-31).**
- **Scroll-wheel support (#9): `WEV_SCROLL` (2026-05-30).**
- **Launchpad/Spotlight real icons (#2), dynamic dock (#3), Hello removed (#4) (2026-05-30).**
- **Window UX: single-instance `summon`, Super+V clipboard, Super+Tab switcher (2026-05-30).**
- **Hierarchical filesystem (tosfs v2, parent-indexed) + Dolphin-style Files app.**
- **Compositor desktop: `kernel/ipc.c` protocol; twm back-buffered compositor; `term`; fastfetch.**
- **C++ application SDK: freestanding crt + heap + libc + `ulib`/`sys` + `ui::` toolkit.**
- **UI-modernization pass: hover feedback, AA borders / state layers / elevation (2026-05-29).**
- **macOS-style system layout: `/System` `/Apps` `/Users`; registry; real cursors; fullscreen.**
- **Kernel foundations: delete+free-map, per-task fd tables, ELF loader, MBR, sleep, fork/exec +
  pids/wait/zombies, fine-grained SMP, RTC/PCI/speaker/reboot, VBE fb, AA system font, test suite.**

---

## Issue history (resolved / not blocking)

- **BIOS real-mode load envelope `#UD` — FIXED (2026-05-29).** The chunked disk-read loop ran
  `mov ah,0x42` before `push ax`, baking `0x42` into the sectors-remaining counter; one-line
  ordering fix in `boot/stage1.asm`. (Debug via `-d int,cpu_reset` + `pmemsave` dumps; gdb breaks
  in the boot sector are unreliable — the loop self-modifies the DAP page.)
- **Flaky UEFI tests under host load — hardened (2026-05-29).** Tight inject schedules can outrun
  OVMF+TCG; the harness now retries (`t_mouse` re-injects, `line_for` retypes). Environmental.
- **tmpfs scratch leak — FIXED.** `Tos.stop()` removes the per-run scratch disk / OVMF-vars / serial
  log it created (a caller-supplied scratch is left alone).
- **Synchronous disk I/O can still freeze the desktop on *large* writes — MOSTLY FIXED for the
  everyday case (2026-06-06).** The headline symptom (notepad autosave freezes the cursor) is fixed:
  it was dominated not by the file data but by `flush_super()` rewriting the whole **189 KB** directory
  table on every close — now an incremental per-entry flush of ≤1 KB (see Done). What *remains* is the
  residual architecture: writes are still **synchronous polled PIO**, and a syscall is only preempted
  if it spans a **10 ms** timer tick, so writing a genuinely large file's *data* (tens of KB+) still
  busy-waits the single core for its duration (every transferred word is a VM-exit under KVM; raising
  `TIMER_HZ` barely helps — a 3 ms write still freezes ~3 ms at 1 kHz). For typical notes this is now
  imperceptible; the full cure for big writes is still, in order of payoff: **(a)** an **async / DMA**
  block driver (virtio-blk or AHCI+DMA — see the Phase-4 driver item) so a transfer is one descriptor,
  not thousands of `inb`/`outb`; **(b)** a **write-back buffer cache** that returns immediately and
  flushes on idle/sync, so the app never blocks on the platter; **(c)** make the writer not block the
  UI thread (background the flush). Cheaper palliatives already in place: autosave is debounced to a
  real ~1.3 s pause and only drafts. (More CPU cores would let *other* tasks run during a writer's
  blocking write, but won't stop the *writer* itself from hitching — only async I/O does that.)
