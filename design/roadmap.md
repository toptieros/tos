# tOS — roadmap to a fully functional OS

A high-level plan for taking tOS from where it is now (a capable teaching OS with a
real graphical desktop) to a system that can install onto real hardware, run
third-party software, and stand on its own. This is the *strategic* view; the
concrete, checkable tasks live in [NEXT_STEPS.md](../NEXT_STEPS.md), and the
subsystem designs in the other `design/` docs. Phases are roughly ordered by
leverage — earlier phases unblock later ones — but they can overlap.

## Where we are (honest assessment: early-to-mid development)

tOS already has a surprising amount of a real OS:

- **Kernel:** boots from BIOS *and* UEFI; higher-half; **SMP, preemptive** scheduler
  with per-CPU run queues; per-process address spaces; `fork`/`exec`, real pids,
  `wait`, zombie reaping; `init` as PID 1; an ELF loader; limits that scale with RAM.
- **Storage:** a **hierarchical, writable** on-disk filesystem (tosfs v2) on an MBR
  partition, with a `/System` `/Apps` `/Users/user` layout and a settings registry.
- **Desktop:** a real **compositor** (`twm`) with the window manager, terminal, and
  file manager as **separate processes** glued by a kernel IPC protocol (shared
  surfaces + ptys); draggable/resizable windows; a dock, Launchpad, Spotlight,
  Control Center; **frosted-glass** UI in a consistent slate palette.
- **SDK:** a freestanding **C++ widget toolkit** over a software rasterizer, a small
  libc, and a syscall layer; several apps built on it.

What makes it still "early-to-mid", not "fully functional":

- It is a **closed world** — it only runs *its own* hand-written apps. There is no
  way to bring in outside software.
- Userspace is **freestanding** — no POSIX, no C++ runtime (exceptions/RTTI/STL), no
  dynamic linking, no heap growth beyond a static image.
- It is effectively **single-user and fully trusted** — no enforced ownership,
  permissions, or sandboxing.
- It **can't talk to the world or to most hardware** — no networking, no USB, no
  modern storage (AHCI/NVMe), PIO-only disk.
- It can't yet be **installed** — it runs from the build image; there's no live →
  disk install, and the filesystem is fixed-size.

## Phase 1 — Foundation hardening *(make it a real installable system)*

The base must use the real machine and survive real use before anything else matters.

- **Use all of RAM** — parse an e820/UEFI memory map and build a multi-region frame
  pool; drop the 3 GB cap. *(NEXT_STEPS: "Use all of RAM".)*
- **Growable, robust filesystem** — extents/indirect blocks (no contiguous
  requirement), a runtime-sized partition, and journaling the slot table.
- **Installer (live → disk)** — treat the running system as live media and install
  onto a target disk (partition, mkfs, copy `/System`+`/Apps`, seed `/Users`).

Exit criteria: you can install tOS onto a disk of any size and reboot into it.

## Phase 2 — The userspace runtime *(the substrate that unblocks everything)*

This is the single highest-leverage investment: it turns "rewrite for tOS" into
"recompile for tOS". (Detailed in NEXT_STEPS "A real userspace runtime + SDK
sysroot".)

1. A reusable **sysroot** + `tos-cc`/`tos-c++`/`tos-link`.
2. A **hosted C++ runtime** — `libstdc++`/`libc++` with exceptions, RTTI, STL, unwind.
3. **`libposix`** — pthreads, file descriptors, `mmap`, signals, `dlopen`.
4. A **heap/`sbrk`** for user programs and a **dynamic linker**.
5. A **platform/QPA-style** framebuffer + input shim for graphical software.

Exit criteria: a non-trivial third-party C/C++ program compiles and runs unmodified
(or nearly so). Note: this is also the *only* realistic path to ever hosting a big
framework like Qt/SDL — porting one without this substrate is not feasible.

## Phase 3 — Security & multi-user *(stop being one trusted blob)*

- **System ownership** — a hidden `system` user owns `/System` + system apps; normal
  users can run but not modify/delete them. *(in-OS todo #1.)*
- **Capability enforcement / app sandbox** — wire the manifest `caps` to a per-task
  capability set the kernel checks at the syscall boundary (fs jails, spawn/window/
  notify gating). *(design/app-runtime.md.)*
- **Real users + permissions** — login, per-user home/registry, file ownership.

Exit criteria: an untrusted app cannot read another user's files or delete the system.

## Phase 4 — Connectivity & modern hardware *(talk to the world)*

tOS today drives **PCI enumeration, an ATA PIO disk, PS/2 input, and the firmware
framebuffer (VBE/GOP)**. Reaching real hardware (and a real install target) means a
batch of new drivers — but **fewer than it first looks**, because liveboot leans on
firmware: UEFI/BIOS loads the kernel + an initial RAM image from the boot medium for
you, so a RAM-resident live system needs **no** storage/USB driver to *run* — only to
*install onto the target disk*. That splits the work into "boot the live env" (firmware
does the I/O) and "write the install" (needs a real block driver).

Prioritized, VM-first so the installer is testable in QEMU before metal:

1. **virtio-blk** — ✅ **landed 2026-06-11/12** (`kernel/drivers/virtio_blk.c`): legacy virtio-pci,
   polled split-virtqueue DMA (bounce-buffered), self-test. Now wired through a **block-device layer**
   (`blockdev.c`: ata0 + virtio0), a **live→disk installer** (`install` clones the boot disk onto it),
   and **fs-on-bdev** so tOS boots straight off the installed virtio disk (root mounted from virtio0).
2. **AHCI/SATA + DMA** — ✅ **landed 2026-06-12** (`kernel/drivers/ahci.c`): AHCI 1.x over the MMIO
   ABAR (BAR5), polled command list + PRDT DMA (bounce-buffered), READ/WRITE DMA EXT + IDENTIFY,
   registered as "ahci0". Added `vmm_map_mmio()` (a higher-half MMIO window, since device BARs live in
   the PCI hole outside the RAM identity map). Verified: kernel self-test, *and tOS installs onto and
   boots straight off a SATA disk* through the same fs-on-bdev path. The realistic desktop install
   target; replaces PIO.
3. **NVMe** — ✅ **landed 2026-06-12** (`kernel/drivers/nvme.c`): MMIO controller registers
   (BAR0/1, via `vmm_map_mmio`), an admin + one I/O queue pair, polled DMA with PRP lists,
   `IDENTIFY` namespace for capacity, registered as "nvme0". Verified: kernel self-test, *and tOS
   boots straight off an NVMe namespace* (`-device nvme`) through the same fs-on-bdev path. How modern
   laptops/desktops boot. *Next: GPT + ESP(FAT) writer.*
4. **GPT + ESP(FAT) writer** — the installer must lay down a bootable layout (see
   [installation.md](installation.md)).
5. **USB: xHCI + USB core + HID + mass-storage** — the big bare-metal tax (modern
   machines have no PS/2; live-USB media). Large, multi-part, unavoidable beyond VMs.
6. **ACPI** — ✅ **landed 2026-06-12** (`kernel/acpi.c`): RSDP scan → RSDT/XSDT walk → **MADT**
   (real CPU/APIC topology, replacing the QEMU-only `fw_cfg` count; SMP now discovers CPUs from it) +
   **FADT** (PM1a control + the `_S5` byte scan → ACPI poweroff; RESET_REG → ACPI reset), with the old
   magic ports kept as a fallback. No AML interpreter — just table walking + the standard tiny `_S5`
   scan. Verified: MADT reports the right CPU count (`-smp 4` → 4, SMP 4/4 from the MADT) and ACPI S5
   poweroff works *on its own* (QEMU exits with the fallbacks removed). Tables reached via
   `vmm_map_mmio` (any e820 type). **UEFI too ✅ (2026-06-13):** the loader walks the EFI config tables
   for the RSDP (ACPI 2.0 GUID, 1.0 fallback) and passes its physical address via `boot_info.acpi_rsdp`,
   which `acpi_init()` validates+uses before the legacy scan — so UEFI now does `[acpi] (UEFI handoff)
   rev 2 (XSDT), 4 CPU(s) via MADT` + SMP 4/4 (was `[acpi] no RSDP` → `fw_cfg`); BIOS unchanged. Full
   uACPI/LAI is still the long-term path for AML/runtime ACPI.
7. **Networking** — *NIC + a full native stack through TCP, now exposed to userspace* ✅
   **2026-06-12**. NIC: `kernel/drivers/virtio_net.c` — legacy virtio-pci, RX + TX virtqueues,
   MAC, raw Ethernet frames. Stack (`kernel/net/`): **ARP (resolver+cache) → IPv4 → ICMP → UDP →
   DHCP → TCP**. The guest **leases its address** via DHCP, **pings** the gateway, and completes a
   real **TCP** conversation (3-way handshake + push/echo with the pseudo-header checksum + FIN
   close), all verified by boot self-tests (ICMP/DHCP against SLIRP; TCP against a host echo server
   through 10.0.2.2). **Userspace networking landed too** ✅: `SYS_NET_*` (78-82) wrap the TCP
   client (`net_ping`/`net_connect`/`net_send`/`net_recv`/`net_close` in `ulib`), **each gated on
   `CAP_NET`** so only an app whose manifest declares `net` can touch the wire (the Terminal does).
   The shell drives it with `ping <ip>` and `get <ip> <port> <path>` (an HTTP/1.0 `GET`), so
   **tOS fetches a file over the network** — the Phase 4 exit criterion, met. **Second NIC landed**
   ✅ **2026-06-13**: `kernel/drivers/e1000.c` (Intel 8254x / QEMU `-device e1000`) behind a new
   NIC-agnostic **`netif`** layer (`net/netif.{c,h}`) the stack drives instead of naming a driver —
   first NIC up wins, so an e1000-only box leases/pings/fetches through the identical path (verified
   `[net] NIC e1000` + DHCP + ICMP; virtio unchanged). **TCP server landed** ✅ **2026-06-13**: passive
   open (`net_tcp_listen`/`accept`) + CAP_NET-gated `SYS_NET_LISTEN`/`ACCEPT` + a shell `serve <port>`
   one-shot HTTP server, so **tOS serves a page over the network** (the host fetches it through a SLIRP
   host-forward) — one connection at a time. **Still to do:** a fuller **sockets layer** — multi-connection
   state (a TCB table) + `bind` so clients/servers coexist; TCP retransmit/windowing for lossy links.
   Unlocks downloads, an app store, remote anything. Design: [virtio-net.md](virtio-net.md).

Also fold in a **real serial console** for input (✅ **2026-06-13**: COM1 RX on IRQ4 feeds the key
ring, so the shell runs headless over the serial line) and **LAPIC timer calibration** (done).

> **Drivers don't port from Linux.** Linux has no stable in-kernel ABI; its drivers are
> welded to one kernel version's internals, and it's **GPLv2** — copying code makes tOS a
> derivative work. Use the Linux tree (and NVIDIA's "open" GPU modules) as a *datasheet
> you can grep*, then clean-room reimplement from the **public hardware specs** (PCIe,
> AHCI, NVMe, xHCI are all freely downloadable), the **OSDev wiki**, and **BSD-licensed**
> drivers you can actually borrow.

Exit criteria: tOS can fetch a file over the network *(✅ met — shell `get` does an HTTP/1.0
GET over TCP from userspace)* and boot on hardware that lacks PS/2 / legacy IDE.

## Phase 5 — Desktop maturity & an app ecosystem

- Finish the **desktop UI roadmap**: app menus + the OS-logo dropdown (#6/#8), a
  status bar + notifications/toasts, a global **selection model** + desktop-as-folder
  (#10), a **drag-and-drop** protocol, the Pocket Dimension shelf.
- A **shared ScrollBar** so every scroll surface has the draggable thumb (recorded in
  NEXT_STEPS) — small but representative of "the toolkit owns the contract".
- A real **package/app format** with install/update, and ported/native apps that make
  the system useful day to day (editor, viewer, settings, a browser engine via the
  Phase 2 runtime).

## Phase 6 — The stretch goals *(self-hosting & beyond)*

- A **self-hosting toolchain** — compile tOS *on* tOS.
- **Audio**, multi-monitor, power management.
- **Graphics acceleration — VM-only, realistically.** On **bare metal**, tOS stays on
  **software rendering** over the firmware framebuffer (VBE/GOP): real GPU drivers are
  per-vendor, mostly undocumented (NVIDIA) or enormous (AMD/Intel), and the useful
  userspace stacks (CUDA, GL/Vulkan) are closed — out of reach for a hobby OS. In a **VM**
  it's tractable, because the "GPU" is one standardized, documented device: a
  **virtio-gpu** driver gets host-side scanout, and with **virgl / Venus** the host
  forwards GL/Vulkan to its real GPU — a modest driver yields real acceleration because
  the host does the hard part (design: [virtio-gpu.md](virtio-gpu.md)). Software
  rendering on metal is *not* a dead end for a 2D
  compositor like tOS — it's memory-bandwidth bound, not compute, so damage tracking
  (already done) + SIMD blits go a long way.

## How to use this

When picking up roadmap work, prefer the **earliest unfinished phase** — foundation
and runtime before features. Within a phase, NEXT_STEPS lists the concrete tasks; keep
`make test` green (BIOS + UEFI) as each lands. The near-term bottleneck is **Phase 1
(installable foundation)** and **Phase 2 (the userspace runtime)** — almost everything
desirable downstream (third-party apps, a browser, real toolkits) is gated on Phase 2.
