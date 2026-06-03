# tOS — architecture & internals

tOS is an interactive, preemptively-multitasked, SMP x86-64 teaching OS. Two boot
front-ends (BIOS and UEFI) load the *same* higher-half kernel, which mounts a
filesystem off an IDE disk, starts `init` (PID 1), and brings up a graphical
desktop with a terminal running a real ring-3 shell.

This document explains how the whole system fits together. `README.md` is the
quick intro + build/run; `NEXT_STEPS.md` is the forward-looking checklist.

---

## Repository layout

```
boot/                 BIOS bootloader (stage1.asm) + AP startup (trampoline.asm)
uefi/                 UEFI loader (uefi.c) — embeds and boots the same kernel
kernel/
  kmain.c             kernel entry; brings subsystems up in order
  sched.c sched.h     SMP scheduler, fork/exec/wait, dynamic kernel stacks
  percpu.h            per-CPU state (struct cpu, run queues)
  syscall.h           the int 0x80 ABI (numbers + shared structs), used by kernel+user
  bootinfo.h          boot → kernel handoff struct (framebuffer geometry)
  string.c            freestanding mem* the compiler emits
  sysfont.h           system font: JetBrains Mono, anti-aliased (console + ugfx)
  linker.ld           higher-half kernel link script
  arch/               x86 low-level: cpu.asm/.h, gdt, idt, traps (ISR+syscall
                      dispatch), apic, smp, spinlock
  mm/                 vmm.c/.h — paging, frame pool, ELF loader, fork/destroy
  drivers/            console, keyboard, mouse, rtc, timer, ata
  fs/                 fs.c (tosfs, hierarchical), tosfs.h on-disk format
user/                 ring-3 programs, each its own app dir + a shared lib:
  lib/                ulib (syscall wrappers) + ugfx (graphics, surface-aware) + user.ld
  init/ shell/        init (PID 1) and the interactive shell (a pty client)
  twm/                the window manager / compositor (twm/logo.h via genlogo.py)
  term/               the terminal emulator (grid + ANSI parser, runs the shell)
  files/              the graphical file manager (Dolphin-style)
  fastfetch/ ticker/  the fetch banner + a demo background task
tools/                mkfs.c (packs the FS tree), genfont.py + genlogo.py (asset bakers)
fs/shortcuts          desktop icon list ("Label|program"), read by twm
tests/                harness.py (drives QEMU) + run_tests.py (the suite)
fs/                   files packed into the image (readme, motd; home/, docs/ trees)
```

The kernel build uses a flat include style (`#include "console.h"`); the Makefile
adds `-I` for each `kernel/` subfolder (`KINCS`) so headers resolve regardless of
where a file lives. `KSRC` globs `kernel/*.c` and `kernel/*/*.c`. Userspace mirrors
this: each app is `user/<app>/<app>.c`, the shared runtime lives in `user/lib`
(`-I`'d as `user/lib`), and each program links its own object plus `ulib.o` +
`ugfx.o`. Both the kernel console and ugfx draw text from the same `sysfont.h`.

---

## Boot

Both paths leave the CPU in 64-bit long mode, on a CR3 that maps the higher-half
kernel + a framebuffer (if present), and jump to `kmain(struct boot_info *)` with
`rdi` = the handoff struct.

**BIOS (`boot/stage1.asm`)** — boot sector does LBA disk reads (stage 2 + the
kernel) and jumps to `stage2_rm` (still real mode). There it:
1. zeroes a `boot_info` at phys `0x7000` (defaults to `console = VGA`);
2. `setup_vbe`: VBE `int 0x10` — get controller info, walk the mode list, pick the
   first 32bpp linear-framebuffer graphics mode with `800 ≤ width ≤ 1280`, set it
   (function `0x4F02` with the LFB bit), and fill `boot_info`
   (console=FB, width/height/pitch=BytesPerScanLine/4, fb_phys);
3. enables A20, builds page tables (low identity + higher-half kernel, and — if a
   framebuffer was set — maps it at `FB_VBASE` so `console_init` can touch it
   before `vmm_init`), enters long mode, jumps with `rdi = 0x7000`.

If no VBE mode is found, `console` stays 0 and the OS runs the text shell.

**UEFI (`uefi/uefi.c`)** — embeds `kernel.bin` as a blob, allocates its load
pages, queries GOP for the framebuffer, builds identity + higher-half + `FB_VBASE`
page tables, exits boot services, and jumps with `rdi = &boot_info`.

**AP startup (`boot/trampoline.asm`)** — a flat blob the BSP copies to phys
`0x8000`; each application processor walks real→long mode and calls `ap_main`.

---

## Memory model (`kernel/mm/vmm.c`)

- **Higher half**: the kernel is linked at `0xFFFFFFFF80000000` (phys `0x200000`),
  mapped by one 2 MiB page — so the whole kernel image (incl. `.bss`) must fit in
  2 MiB. A *shared* PDPT (`shared_pdpt_high`) holds the kernel, the framebuffer
  (`FB_VBASE`), and the LAPIC MMIO (`LAPIC_VBASE`); it is spliced into every
  address space's PML4[511], so those mappings survive CR3 switches.
- **Identity map = all RAM**: RAM size is read from QEMU `fw_cfg` (key
  `0x0003`); the low half identity-maps *all* of it with 2 MiB pages
  (`build_low_map`, one page directory per GiB). The frame pool is that whole
  span minus the kernel image and the per-process user-window slot — so it scales
  with the machine (≈32k frames @128 MiB, ≈523k @2 GiB) instead of a fixed cap.
- **Frame allocator**: a bump pointer over the pool plus a free list of reclaimed
  frames; `frame_alloc_contig(n)` bumps `n` physically-contiguous frames (for
  kernel stacks). Out-of-memory panics rather than corrupting the kernel. Frames
  are identity-mapped, so `virt == phys` for the kernel.
- **Per-process address space**: each task gets its own PML4. Low half = the
  identity map (`build_low_map`) with `pd_low[2]` overridden to a private user
  page table (`upt`) covering the 2 MiB user window `[0x400000, 0x600000)`:
  - code/data/bss at the bottom (up to `USER_SEG_LIMIT`, ~1.9 MiB),
  - a 64 KiB stack just below the data page at the top (grows down),
  - the data/role page at the very top (`USER_DATA_VADDR`, seeded with argv0).
- **ELF loader** (`vmm_create_user`): reads the ELF header + program headers,
  validates every `PT_LOAD` *before* allocating (so a bad ELF leaks nothing),
  then streams each segment from disk straight to its pages (`load_bytes`, up to
  8 sectors per read, page-at-a-time copy) — program size is bounded by the
  window, not a fixed buffer.
- **fork/destroy**: `vmm_fork` deep-copies present user pages into a fresh space;
  `vmm_destroy_user` reclaims user frames, identity PDs, an on-demand framebuffer
  PD if any, and the paging frames.
- **User framebuffer** (`vmm_map_user_fb` / `SYS_FBINFO`): maps the framebuffer
  US=1 into the caller in its own PDPT slot just past RAM (`fb_pdpt_slot`), and
  returns geometry. Used by twm.

---

## Scheduler & SMP (`kernel/sched.c`, `arch/smp.c`, `arch/apic.c`, `percpu.h`)

- **Unified interrupt path** (`arch/cpu.asm` → `arch/traps.c`): every vector (32
  exceptions, IRQ0 timer, IRQ1 keyboard, IRQ12 mouse, LAPIC timer 0x22, int 0x80
  syscall) saves a `struct regs` and calls `isr_dispatch`, which returns the
  frame to resume — returning a *different* task's frame is a context switch.
- **Per-CPU scheduling**: `cpus[]` each have their own `current`, idle context,
  per-CPU TSS (one shared GDT with a TSS descriptor per CPU), and a RUNNABLE run
  queue behind their own `rq_lock`. Preemption (PIT on the BSP, LAPIC timer on
  APs) and `yield` touch only that `rq_lock`; `sched_lock` guards just the task
  table (create/fork/exec/exit/wait + waking). New tasks go to the least-loaded
  CPU; an idle CPU steals from the busiest queue. `TASK_RUNNING` keeps a task on
  one CPU. APs stay interrupts-off until `sched_start` (avoids a bring-up race).
- **Tasks**: `struct task` has `cr3`, a pool-allocated kernel stack
  (`vmm_alloc_kstack`, freed on reap — so the task count is RAM-bounded, not a
  static array; `MAX_TASKS` 256 is just the table size), pid, parent, state.
- **Lifecycle**: `fork` clones (child returns 0), `exec` replaces the image in
  place, `exit` → ZOMBIE (slot kept until reaped), `wait`/`trywait` reap a child
  (orphans re-parent to init). `sleep` parks off the PIT tick. init exiting is
  fatal.
- **Locks**: order `sched > fs > ata`, `fs > ata`, console a leaf. `make
  SCHED_DEBUG=1` compiles in run-queue invariant checks.

---

## Syscalls (`kernel/syscall.h`, dispatched in `arch/traps.c`)

`int 0x80`, number in `rax`, args in `rdi/rsi/rdx`, return in `rax`. Userspace
wraps them in `user/ulib.c`.

```
0  EXIT      7  SHUTDOWN   14 SLEEP        21 TRYWAIT (non-blocking reap)
1  WRITE     8  LS         15 FORK         22 MOUSE   (struct mousestate*)
2  PUTC      9  OPEN       16 EXEC         23 TIME    (struct rtctime*)
3  READ      10 READ_FD    17 GETCPU       24 LSPCI   (print PCI devices)
4  YIELD     11 WRITE_FD   18 CPAINT       25 BEEP    (PC speaker, 0 = off)
5  SPAWN     12 CLOSE      19 FBINFO       26 REBOOT  (8042 reset)
6  WAIT      13 UNLINK     20 CON_WINDOW

27-43  compositor / windowing / pty (GETKEY, WIN_*, WM_*, PTY_*, SYSINFO, ISATTY)
44 MKDIR   46 CHDIR    48 READDIR (path, dirent*, max)   50 STAT (path, fstat*)
45 RMDIR   47 GETCWD   49 RENAME  (oldpath, newpath)
```
(18 CPAINT = block cursor; 19 FBINFO = map framebuffer → struct fbinfo;
20 CON_WINDOW = confine the text console to a pixel rect; 44-50 = the
hierarchical filesystem.) SHUTDOWN does an ACPI poweroff (QEMU ports
0x604/0xB004) before halting.

---

## Drivers (`kernel/drivers/`)

- **console** — VGA text (BIOS fallback) or a GOP/VBE framebuffer rendering the
  anti-aliased system font (`sysfont.h`, a 9x19 cell; glyph coverage is blended
  between fg/bg), plus a COM1 serial mirror (what the tests read). The cell size
  is taken from `SYSFONT_W/H`, so the column/row count, scrolling, and windowing
  all follow the font. The framebuffer console can be confined to a *window*
  (cell rect) via `console_set_window` so twm runs the shell inside a terminal
  window; it also paints the shell's inverse block cursor
  (`console_paint_cursor`, screen-only).
- **keyboard** — PS/2 set-1 scancodes → ASCII (+ arrows as control bytes) into a
  ring buffer on IRQ1; blocking `READ` parks the caller until a key arrives.
- **mouse** — PS/2 aux device on IRQ12; decodes 3-byte packets into an absolute
  position (clamped to the screen) + buttons; `SYS_MOUSE` reports it. Enabled
  only on framebuffer boots.
- **rtc** — CMOS real-time clock (ports 0x70/0x71); stable (no-straddle) read,
  BCD/12-hour aware; `SYS_TIME` backs the shell `date` command.
- **timer** — 8254 PIT @ 100 Hz drives preemption + sleeps on the BSP; each AP runs
  its LAPIC timer at the same 100 Hz, its initial count measured at boot by
  `lapic_timer_calibrate` (PIT channel-2 one-shot reference) instead of a fixed constant.
- **ata** — minimal ATA PIO (LBA28) driver for the primary master (the boot
  disk); polled, so it works identically during early init and inside syscalls.
- **pci** — PCI config-space access (legacy 0xCF8/0xCFC) + a bus scan; `SYS_LSPCI`
  / the `lspci` command list devices. A foundation for real device drivers later.
- **speaker** — PC speaker via PIT channel 2; `SYS_BEEP` / the `beep` command.
- system control: `SYS_REBOOT` resets via the 8042; `SYS_SHUTDOWN` ACPI-poweroff.

---

## Filesystem (`kernel/fs/`, `tools/mkfs.c`)

`tosfs` (v2) is a **hierarchical** FS in its own MBR partition (type `0x7f`) on the
boot disk; the kernel finds its base LBA by scanning the partition table at mount.

- **Layout**: sectors `0..D-1` are a flat **slot table** (`struct tosfs_super`:
  magic + `struct tosfs_ent[]`), then file data sector-aligned. Each entry is
  `{name[32], start_lba, size, parent, type}` where `type` is FREE / FILE / DIR
  and `parent` is the *slot index* of the containing directory (`TOSFS_ROOT = -1`
  for top level). The tree is therefore encoded without nesting the layout: a
  directory is just an entry that others point at. **Entries are never relocated**,
  so a parent index is stable for the life of the FS — delete only flips an
  entry's `type` to FREE (and frees a file's sectors). The table spans
  `TOSFS_DIR_SECTORS` sectors, sized so the disk runs out of *data* sectors before
  *slots* — the entry count is bounded by free space, not a fixed cap.
- **Paths & cwd**: each task has a working directory (`fs_chdir`/`fs_getcwd`,
  inherited across `fork`, reset on exit). `resolve()` walks a path component by
  component (`.`/`..`, absolute or cwd-relative). Programs are always loaded by
  their root-level name, so `exec("shell")` works from any cwd.
- **Operations**: `mkdir`/`rmdir` (empty-only), `chdir`/`getcwd`, `rename`
  (move/rename, with a cycle check so a dir can't move inside itself), `readdir`
  (fills `struct dirent[]`), `stat`, plus the file API. Per-task open-file tables
  (`oft[task][fd]`) keep fds private; a single shared write buffer means one writer
  system-wide. Reads stream from disk; writes first-fit a free sector, grow
  contiguously, and commit the entry on close; metadata changes flush the whole
  table. `unlink` frees sectors back to an in-memory bitmap rebuilt at mount.
- `mkfs` (host gcc) builds a **tree**: `dest=hostfile` packs a file (auto-creating
  parent dirs), a bare `dest` makes an empty directory; it pads to the disk size.
  The shipped image seeds `home/notes.txt` and `docs/guide` to exercise nesting.
- **Shell**: `ls [dir]`, `cd`, `pwd`, `mkdir`, `rmdir`, `rm [-r]`, `cp`, `mv`,
  `tree`; the prompt shows the cwd. **`files`** is a graphical file manager (below).

---

## The desktop: a compositor + separate apps

init launches `twm`. With a framebuffer the desktop is built from **three layers
that don't know about each other**, glued by two kernel mechanisms (window
surfaces + ptys, both in `kernel/ipc.c`):

**Window surfaces (shared memory).** An app calls `win_create(w,h,title)`
(`SYS_WIN_CREATE`); the kernel allocates a run of contiguous frames and maps them
(US=1, 4 KiB pages, in the surface PDPT slot — see vmm.c) into **both** the app
and the compositor at the same per-id vaddr. The app draws into its surface and
calls `SYS_WIN_PRESENT` (bumps a sequence); the compositor reads the live window
set with `SYS_WM_WINDOWS` and blits each surface. To avoid re-blitting a whole
window for a tiny change (a hover layer, a blinking caret, a typed glyph), an app
can instead call `SYS_WIN_PRESENT_RECT` with a damage rect: the kernel unions
partial presents into a per-window rect (carried in `struct wmwin`, reset each
snapshot) and the compositor composites only that sub-rect. `SYS_WM_POST` delivers events
back to a window — key bytes (`WEV_KEY`), `WEV_CLOSE`, `WEV_RESIZE`, and a
client-area click (`WEV_MOUSE`, the press packed as x/y/buttons by `WEV_MOUSE_PACK`)
— which the app drains with `SYS_WIN_POLL`. `SYS_WIN_RESIZE` reallocates a surface
in place. A window owned by an exiting task is torn down automatically (sched hook
→ reclaimed by the compositor's next snapshot, where it isn't mid-blit).

**ptys (`kernel/ipc.c`).** A pty is two byte rings. A task bound to one (its
`tty`) reads/writes it on `SYS_READ`/`SYS_WRITE` instead of the keyboard/console;
output is also mirrored to COM1 so the serial-log tests still see it. The
keyboard goes to the compositor (`SYS_GETKEY`, non-blocking), which routes it to
the focused window. Kernel text stops painting the framebuffer once the
compositor registers (`SYS_WM_REGISTER` → `console_set_gui`), so it can't corrupt
the desktop; it still goes to serial.

**`twm` (compositor / window manager).** Owns the framebuffer; does *only* window
management. Draws the desktop + top bar (logo, focused-app title, live clock) and
**desktop icons** read from the `shortcuts` file (Terminal, Files); **double-
clicking** an icon `fork`+`exec`s that program. It composites window surfaces,
draws chrome, and handles **focus, dragging (title bar), close (the `x` button →
`WEV_CLOSE`), resize (the corner grip → `WEV_RESIZE`), and forwarding client-area
clicks (`WEV_MOUSE`) to the focused window**. An app may also declare a **menu bar**
(`SYS_WIN_SETMENU` → a `struct winmenu` of ≤5 menus × ≤8 items, each item carrying
`WMI_DISABLED`/`WMI_CHECKED` flags + a Ctrl-accelerator letter); the compositor draws a
tile per menu after the app name, opens a dropdown (greyed/checked rows, right-aligned
`^X` hints), posts the pick back as `WEV_MENU`, and intercepts `Ctrl+<letter>` for the
focused window to fire the matching item directly. Dragging draws the window at its new
spot and then erases only the *trailing* sliver (never blanks-then-redraws), so
there's no flicker. The arrow cursor is composited on top each frame
(`ugfx_cursor_hide` before redrawing, redraw, then re-draw the cursor), so the
two-writers smear is gone — twm is the only thing that touches the framebuffer.
The desktop is persistent: closing the last window returns to it. Two **global
keyboard shortcuts** the compositor intercepts before forwarding keys (the keyboard
driver emits them as the single bytes `KEY_SUPER_V` / `KEY_SUPER_TAB`): **Super+V**
*summons* the clipboard manager (single-instance — focuses/restores the existing
window rather than forking a duplicate, via `summon()`), and **Super+Tab** is an
MRU **window switcher** that single-steps focus through the open windows (a switch
session snapshots the z-order, which doubles as the MRU since every focus raises).

**`term` (terminal emulator).** A normal app: creates a window, runs the **shell**
as a piped child (`pty_open` + `fork` + `pty_attach` + `exec`), maintains a
character grid (fixed stride, so resize preserves content), parses the shell's
byte stream — newline/CR/backspace/tab plus ANSI **CSI/SGR** escapes for colour —
renders changed cells into its surface, and forwards the keys twm routes to it
into the pty. It draws its own block cursor. Though it's a raw-syscall app (not the
`ui::` toolkit), it still declares a menu bar by hand (`win_setmenu` with a literal
`struct winmenu`) and handles `WEV_MENU`: **Edit [Copy, Paste, Clear]**, with no Ctrl
accelerators so a plain Ctrl+C still reaches the shell as an interrupt.

**`shell`** is an ordinary pty client: line editing with history, and it parses
**ANSI escape sequences** for the arrow/Home/End/Delete keys (the keyboard emits
them). On a terminal it runs **`fastfetch`** at startup (a colour system-info
banner over SGR). `halt` closes the terminal (returns to the desktop);
`poweroff` powers the machine off. With no framebuffer, `twm` just `exec`s the
shell, so a text boot is a plain Linux-style TTY (the shell falls back to the
kernel console + keyboard, no pty).

**`files` (graphical file manager).** A Dolphin-style window app launched from its
desktop icon. It draws a toolbar (**Up / New Folder / Delete / Refresh**), a path
bar, and an icon list of the current directory (folders first, then files with
sizes), and drives the filesystem through the FS syscalls. Navigation is mouse-
driven via the forwarded `WEV_MOUSE` clicks: single-click selects, double-click
opens a folder (or views a text file, with a Back button); `..` goes up. It is a
plain FS client — no kernel knowledge of "a file manager", just `readdir`/`mkdir`/
`rmdir`/`stat`/`rename`/`unlink` + the file API into its window surface. It declares a
**File / Edit / Go** menu bar (New Folder, Copy/Cut/Paste/Delete, Up/Back/Forward) whose
accelerators (^N/^C/^X/^V) the compositor routes back as `WEV_MENU` picks.

---

## Testing (`tests/`)

`make test` boots the OS headless under QEMU (`harness.py` drives keys via the
monitor and reads the COM1 serial log; `Tos(uefi=, cpus=, mem=)`) and asserts on
behaviour (`run_tests.py`). The suite (49 tests across BIOS + a UEFI subset)
covers boot/ls/cat/write+readback/persistence, delete/rewrite, **directories
(mkdir/cd/hierarchical isolation, mkfs-seeded trees, mv, rm -r, dir persistence),
the Files app launch**, sleep, fork/exec/orphan-reaping/shutdown, SMP (incl.
`-smp 4` task placement + balancing), the GUI desktop + mouse tracking, the
disk-bounded file count, the RTC, the RAM-scaled frame pool, PCI enumeration, the
speaker, and reboot. `make SCHED_DEBUG=1` adds scheduler invariant checks.

---

## Notable design choices & limits

- The kernel must fit in one 2 MiB page; pool-allocated kernel stacks and the
  streamed ELF loader keep `.bss` small enough for a large `MAX_TASKS`.
- The FS is hierarchical now, but: one writer at a time; files are stored
  contiguously (no fragmentation handling beyond first-fit); the FS image is a
  fixed-size partition set at build time. A metadata change rewrites the whole
  slot table.
- OOM panics (no swap / reclaim).
- Targets QEMU (fw_cfg for RAM/CPU count, ACPI poweroff ports, std VBE).
