# Design guideline — tOS live environment & installation

> Status: **design / roadmap.** Today tOS boots as a **live environment**: the
> disk image *is* the system, built by the host and run directly. This is the plan
> to treat that running system as a live medium and **install** tOS onto a target
> disk — partition it, lay down the tree, seed per-user data, and make it bootable
> — the way a live USB installs a desktop OS.

## Live vs installed

| | **Live (today)** | **Installed (target)** |
|---|---|---|
| Origin | host-built `tOS.img` / `uefi.img`, booted as-is | written by the installer onto a real/second disk |
| `/System`, `/Apps` | shipped in the image | copied from the live medium |
| `/Users/user` | seeded skeleton on the image | created fresh on the target |
| Writes | land in the booted disk (or a scratch copy under test) | persist on the target |
| Purpose | try it / run the installer | the system you boot into normally |

The live environment already self-heals its tree (`init`'s `ensure_tree`, see
[filesystem-layout.md](filesystem-layout.md)), so a "live session" and a "fresh
install" converge on the same layout — the installer's job is to reproduce that
layout on another disk and make it boot.

## What the installer does

A guided flow (a GUI `Installer.app` or a shell `install` command), running in the
live session:

1. **Pick a target disk.** Enumerate attached disks (today only the IDE primary
   master is wired up — see *What has to change*), show size/model, let the user
   choose. Refuse the live disk unless explicitly allowed.
2. **Partition it.** Write an MBR matching the build:
   - BIOS target: boot sector + kernel region, then a tosfs partition (type `0x7f`)
     at the same `FS_PART_LBA` the kernel expects.
   - UEFI target: a FAT **ESP** (with `BOOTX64.EFI`) + the tosfs partition.
   (The build already does exactly this with `sfdisk`/`mformat`/`dd`; the installer
   does the in-OS equivalent.)
3. **Write the bootloader.** Copy the boot sector / stage code (BIOS) or the ESP +
   `BOOTX64.EFI` (UEFI) to the target.
4. **Make a filesystem.** Initialize an empty tosfs (v2 superblock + slot table) on
   the target partition — the in-OS equivalent of `tools/mkfs.c`.
5. **Copy the OS.** Recursively copy `/System` and `/Apps` from the live tree to
   the target (the kernel already reads these; the installer streams file data
   sector-by-sector). This is a general directory-copy primitive (also useful as
   `cp -r` and for app install).
6. **Create the user.** Make `/Users/<name>/{Desktop,Documents,Downloads,Pictures,
   .config}` and `/tmp` on the target (the `ensure_tree` set), prompting for the
   user name (defaults to `user`).
7. **Seed settings.** Copy `/System/etc/registry` defaults; leave the user override
   empty (see [settings.md](settings.md)). Optionally record hostname / first-boot
   flags.
8. **Finish.** Flush, then offer reboot. The target now boots a standalone tOS.

## What has to change to build it

- **Multi-disk support.** The ATA driver targets the primary master only; add the
  primary **slave** (and ideally the secondary channel) so a second disk can be the
  install target while the live disk stays mounted. A `SYS`/`fs` call to address a
  *specific* disk for raw read/write.
- **In-OS partitioning + mkfs.** Port the logic in `tools/mkfs.c` and the Makefile's
  partition steps into the kernel/an installer lib: write an MBR, write a tosfs
  superblock + slot table, mark free space. (`mkfs.c` is already structured around
  `tosfs.h`, so the on-disk format is shared.)
- **Recursive directory copy.** A `copytree(src, dst)` over the FS syscalls
  (readdir + create + stream) — shared with `cp -r` and `.app` install.
- **Raw block write to a chosen disk + LBA**, gated so only the installer (a
  capable/trusted task — see [app-runtime.md](app-runtime.md)) may do it.
- **The installer UI.** A `ui::` wizard (target → user → confirm → progress) or a
  scripted `install` shell command for the first cut.

## Phasing (each keeps `make test` green)

1. **Second disk + raw addressing.** Wire the ATA slave; a test boots with two
   disks and reads/writes the second. (No install yet.)
2. **In-OS mkfs + partition.** Format the second disk into a valid empty tosfs +
   MBR; mount it; verify with `ls`.
3. **`copytree` + bootloader copy.** Copy `/System` + `/Apps` to the target and
   write the boot sector / ESP.
4. **User + registry seed; reboot into the target.** End-to-end: install to the
   second disk, detach the first, boot the installed system under QEMU in a test.
5. **Installer.app** (GUI wizard) on top of the working pipeline.

## Out of scope (for now)

Resizing/repartitioning an existing disk non-destructively, dual-boot/coexistence
with other OSes, networked installs, and multi-user provisioning (one `user` for
now, but the layout under `/Users/<name>` is ready for more). Disk selection
assumes whole-disk install.
