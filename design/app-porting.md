# Design guideline — porting apps to tOS

> Status: **design / roadmap.** How software written elsewhere becomes a tOS
> `.app`. Covers the target ABI, a tiered porting strategy (recompile → shim →
> emulate), the SDK/toolchain an external project builds against, and the tooling
> that packages the result.

## The tOS target ABI

An external app must end up as what our own apps already are:

- **Static ELF64**, position-dependent, one RWX `LOAD` segment, entry `_ustart`
  (linked with `user/lib/user.ld`); symbols stripped. Loaded by the kernel ELF
  loader straight from the bundle.
- **Freestanding, integer-only.** Compiled `-ffreestanding -mno-sse -mno-mmx`
  (the kernel saves no FP state), no host libc.
- **Syscalls via `int 0x80`** (`rax` = number, args in `rdi/rsi/rdx`). The full set
  is in `kernel/syscall.h`; the C wrappers are `user/lib/ulib`.
- **I/O is brokered:** there are no raw devices. A GUI app draws into a
  compositor surface (`win_create` + `ugfx` / the `ui::` toolkit) and reads its
  event queue; a console app talks over its pty (stdin/stdout). Files go through
  the FS syscalls.

So "porting" is fundamentally *retargeting the app's bottom edge* (its libc and its
display/input) onto the tOS SDK. The closer the app already is to "plain C + a
framebuffer," the cheaper the port.

## Tiers (cheapest first)

1. **Native rewrite/recompile (today).** The app is (re)written against the tOS
   SDK directly: `libc` (malloc/printf/str/mem), the system API (`ulib`/`sys`),
   and the `ui::` widget toolkit. This is how Files/Notepad are built. Best result,
   most work for a large existing app.

2. **Recompile C apps over a POSIX shim (`libposix`).** A new SDK library that
   implements a *subset* of POSIX on top of tOS syscalls so portable C compiles
   with few changes:
   - `open/read/write/close/lseek/stat` → `fopen`/`fread_`/`fwrite_`/`fclose_`/
     `stat_` (path-based; map fds in a small table).
   - `malloc/free/printf/string.h` → already in tOS `libc`.
   - `getenv/time/usleep` → registry / `SYS_TIME` / `sleep_ms`.
   - stub or `-ENOSYS` the rest (signals, threads, fork unless `spawn` cap).
   A program that only uses this subset rebuilds with the tOS toolchain + `-lposix`.

3. **Graphical apps over a framebuffer/SDL shim.** A `libtosfb` (and a tiny
   `SDL`-shaped shim) that maps a "give me a w×h framebuffer + key/mouse events"
   API onto `win_create` + a surface + `win_poll`. Lets simple SDL1-style or
   raw-framebuffer games/demos run by linking the shim instead of SDL. Audio →
   the PC speaker / future driver (mostly stubbed).

4. **Foreign binaries (other OS ELF/PE, dynamic).** Out of scope: would need a
   syscall-translation layer or a usermode emulator (Linux `int 0x80`/`syscall`
   ABI translation, or a tiny user-mode runtime). Not worth it for a teaching OS;
   the answer is "recompile from source" via tiers 1–3.

## SDK / toolchain (the "sysroot")

To let an external project build for tOS without our repo layout, factor the SDK
into a small sysroot:

```
tos-sdk/
  include/      ulib.h ugfx.h ui.h libc.h sys.h syscall.h theme.h manifest.h ...
  lib/          crt.o ulib.o ugfx.o libc.o sys.o ui.o  (+ libposix.o, libtosfb.o later)
  user.ld
  tos-cc        wrapper: gcc -ffreestanding -mno-sse ... -I include
  tos-c++       wrapper: g++ -std=c++17 -fno-exceptions -fno-rtti ... -I include
  tos-link      ld -T user.ld <objs> crt.o ulib.o ... -o app.elf
```

These are exactly the flags in our `Makefile` (`UCFLAGS`/`CXXFLAGS`/`ULDFLAGS`)
packaged as reusable wrappers. An app author: `tos-c++ -c app.cpp && tos-link app.o
-o bin/app`, then `mkapp.py` to bundle.

## Packaging & format translation

- **`tools/mkapp.py`** assembles the bundle (manifest + `bin/<exe>` + `icon.argb`)
  — already exists.
- **`tools/mkicon.py`** (planned): PNG → `icon.argb` (the binary header
  `tools/genicons.py --app-icons` emits), so authors don't hand-roll pixels.
- **Manifest translation** (planned): a converter from common app-descriptor
  formats to a tOS `manifest`, e.g. a freedesktop `.desktop` file
  (`Name=`/`Exec=`/`Icon=`) → `name`/`exec`/`icon`, or an Android-style
  `label`/`package`. Pure metadata mapping; the binary still needs tiers 1–3.

## Porting checklist

1. Does it build freestanding? Remove host-libc assumptions; build with `tos-cc`.
2. Replace its libc bottom edge with tOS `libc` (or link `libposix`).
3. Replace display/input with `ui::`/`ugfx` (or link `libtosfb`); audio mostly
   stubbed.
4. Declare needed capabilities in the manifest (`caps`, see
   [app-runtime.md](app-runtime.md)) and respect least privilege.
5. `mkapp.py` → drop the bundle in `/Apps`.

## Phasing

1. Extract the SDK sysroot + `tos-cc`/`tos-c++`/`tos-link` wrappers from the Makefile.
2. `libposix` (file + mem + time subset) → port one non-GUI C utility as a proof.
3. `libtosfb` / SDL shim → port one simple framebuffer app.
4. `mkicon.py` + the `.desktop`→manifest converter.

## Ties

- Bundle format + manifest: [app-package-format.md](app-package-format.md).
- Capabilities the port must declare: [app-runtime.md](app-runtime.md).
- The toolkit the SDK exposes: `user/lib/ui.{h,cpp}`.
