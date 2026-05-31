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

- **Networking** — a NIC driver (e.g. virtio-net / e1000), a TCP/IP stack, and a
  sockets API. Unlocks downloads, an app store, remote anything.
- **Modern storage** — AHCI/NVMe instead of legacy PIO.
- **USB** — HID (keyboard/mouse) and mass storage.
- **Real serial console** for input, and **LAPIC timer calibration**.

Exit criteria: tOS can fetch a file over the network and boot on hardware that lacks
PS/2 / legacy IDE.

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
- **Audio**, multi-monitor, GPU acceleration, power management.

## How to use this

When picking up roadmap work, prefer the **earliest unfinished phase** — foundation
and runtime before features. Within a phase, NEXT_STEPS lists the concrete tasks; keep
`make test` green (BIOS + UEFI) as each lands. The near-term bottleneck is **Phase 1
(installable foundation)** and **Phase 2 (the userspace runtime)** — almost everything
desirable downstream (third-party apps, a browser, real toolkits) is gated on Phase 2.
