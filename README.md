# tOS

A small x86-64 operating system, written from scratch. One higher-half kernel
boots from BIOS or UEFI, runs ring-3 processes with per-process address spaces
and a preemptive SMP scheduler, mounts a writable on-disk filesystem, and brings
up a graphical desktop: a compositing window manager with draggable windows, a
terminal, and a file manager, each running as a separate process.

## Build & run

```
make            build the BIOS image, the UEFI image, and the filesystem image
make run-bios   boot in a QEMU window
make run-uefi   boot via OVMF firmware
make test       boot under QEMU and assert behaviour
make clean
```

Requires `qemu-system-x86_64`, `nasm`, `gcc`, `ld`, and (for UEFI) OVMF, `clang`,
`lld-link`, `mtools`, and `sfdisk`.

## More

- **[PROJECT.md](PROJECT.md)** — architecture and internals.
- **[NEXT_STEPS.md](NEXT_STEPS.md)** — what's planned next.
