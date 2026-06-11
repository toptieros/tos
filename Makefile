# tOS build. Outputs land in build/.
#
#   make                 build the BIOS disk image and the UEFI ESP image
#   make run-bios        build + boot the BIOS image in a QEMU window
#   make run-uefi        build + boot the UEFI image in a QEMU window (OVMF)
#   make run-bios QEMU="-display none -serial stdio"   headless / scripted
#   make clean
#
BUILD := build
BOOT  := boot
KDIR  := kernel
UDIR  := user
EDIR  := uefi

OVMF_CODE     := /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS_SRC := /usr/share/edk2/x64/OVMF_VARS.4m.fd

CC     := gcc
# Kernel sources live in subsystem subfolders (arch/ mm/ drivers/ fs/) plus the
# core/shared files at the kernel root; -I each so the flat #include "x.h" works.
KINCS  := -I$(KDIR) -I$(KDIR)/arch -I$(KDIR)/mm -I$(KDIR)/drivers -I$(KDIR)/fs
# -MMD -MP emits a .d file of header dependencies next to each .o (and phony
# targets for the headers), so editing a header recompiles every object that
# includes it. Without this, a struct that changed in a header could leave stale
# objects with a mismatched layout linked together -- a real bug we hit once.
CFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -O2 \
          -fno-tree-loop-distribute-patterns -Wall -Wextra -MMD -MP $(KINCS)
# the kernel runs in the top 2 GiB, so it needs the kernel code model.
# -I$(BUILD) lets smp.c pick up the generated trampoline blob.
KCFLAGS := $(CFLAGS) -mcmodel=kernel -I$(BUILD)
# `make SCHED_DEBUG=1 ...` compiles in the scheduler's run-queue invariant checks
# (catches a task scheduled on two CPUs etc.). Run `make clean` first so the
# kernel objects rebuild. Off by default -- zero cost in normal builds.
ifdef SCHED_DEBUG
KCFLAGS += -DSCHED_DEBUG
endif
# user programs live at 0x400000 and use the default (small) model. Each program
# is its own app (user/<app>/<app>.c); the shared runtime + graphics lib live in
# user/lib (ulib, ugfx), so -I it for the flat #include "ulib.h"/"ugfx.h".
UCFLAGS := $(CFLAGS) -fno-builtin -I$(UDIR)/lib
# C++ apps (the UI toolkit + the apps built on it). Freestanding, no exceptions /
# RTTI / thread-safe-static guards, same SSE-free integer-only ABI as the C side
# (the kernel saves no FP state). The C++ runtime (user/lib/crt.cpp) provides the
# entry _ustart, runs global ctors, and backs operator new with the libc heap.
CXX      := g++
CXXFLAGS := $(CFLAGS) -fno-builtin -I$(UDIR)/lib -std=c++17 \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-asynchronous-unwind-tables
LD      := ld
LDFLAGS := -nostdlib -static -no-pie -T $(KDIR)/linker.ld
# user programs: static ELF64, 4 KiB segment alignment, symbols stripped so the
# packed image stays small (the kernel parses the ELF and loads it by segment).
ULDFLAGS := -nostdlib -static -no-pie -s -z max-page-size=0x1000 -T $(UDIR)/lib/user.ld
NASM    := nasm

DISPLAY_OPT := -display gtk    # open a real window; override with QEMU=... below
QEMU :=                        # extra flags, e.g. QEMU="-display none -serial stdio"

# Hardware acceleration + RAM for the interactive run targets (the test harness
# builds its own QEMU command and stays on software TCG for deterministic SMP
# timing). accel=kvm:tcg uses KVM when /dev/kvm is present and falls back to TCG
# otherwise. The kernel now parses the firmware e820 map and builds a MULTI-REGION
# frame pool that spans the sub-4G PCI hole (RAM remapped above 4G is used too), so
# the old 3G safety cap is gone -- give the VM as much RAM as you like. Override:
# make run-bios MEM=2G.   (`memtest` in the shell stress-checks the pool.)
ACCEL := -machine accel=kvm:tcg
MEM  ?= 8G

# --- user programs (each its own app dir + ELF executable, stored on the FS) -
UPROGS   := init shell ticker twm term memtest selftest fastfetch
CXXPROGS := files notepad clipboard spotlight launchpad settings
UELFS    := $(patsubst %,$(BUILD)/%.elf,$(UPROGS) $(CXXPROGS))
ULIBOBJ  := $(BUILD)/$(UDIR)/lib/ulib.o $(BUILD)/$(UDIR)/lib/ugfx.o $(BUILD)/$(UDIR)/lib/libc.o $(BUILD)/$(UDIR)/lib/sys.o $(BUILD)/$(UDIR)/lib/registry.o
CXXRT    := $(BUILD)/$(UDIR)/lib/crt.o
# the C++ widget toolkit, linked into every C++ app. Split across ui*.cpp (ui.cpp +
# ui_textfield.cpp + ...); the wildcard picks up each piece automatically.
CXXLIB   := $(patsubst $(UDIR)/lib/%.cpp,$(BUILD)/$(UDIR)/lib/%.o,$(wildcard $(UDIR)/lib/ui*.cpp))

# Every object an app links: one per .c/.cpp in its source dir (user/<app>/). This
# lets an app outgrow a single file -- drop another .c/.cpp into its dir and it is
# compiled + linked automatically (the per-file compile rules below match nested
# paths). $(1) = app name. Used by the link rules and by UOBJ (dep tracking).
app_objs = $(patsubst $(UDIR)/%.c,$(BUILD)/$(UDIR)/%.o,$(wildcard $(UDIR)/$(1)/*.c)) \
           $(patsubst $(UDIR)/%.cpp,$(BUILD)/$(UDIR)/%.o,$(wildcard $(UDIR)/$(1)/*.cpp))

KSRC := $(wildcard $(KDIR)/*.c) $(wildcard $(KDIR)/*/*.c)
KOBJ := $(patsubst %.c,$(BUILD)/%.o,$(KSRC)) $(BUILD)/$(KDIR)/arch/cpu.o

# All compiled objects (C/C++), used to pull in the generated header-dependency
# files. cpu.o is from NASM (no .d); -include ignores the missing entry.
UOBJ := $(ULIBOBJ) $(CXXRT) $(CXXLIB) \
        $(foreach p,$(UPROGS),$(call app_objs,$(p))) \
        $(foreach p,$(CXXPROGS),$(call app_objs,$(p)))
DEPS := $(KOBJ:.o=.d) $(UOBJ:.o=.d)
# NOTE: the actual `-include $(DEPS)` is at the very bottom of this file -- if it
# ran here (before the `all` rule) the first dep rule would become the default goal.

IMG     := $(BUILD)/tOS.img
UEFIIMG := $(BUILD)/uefi.img
ESPPART := $(BUILD)/esp.part
EFIAPP  := $(BUILD)/BOOTX64.EFI
MKFS    := $(BUILD)/mkfs
FSIMG   := $(BUILD)/fs.img

.PHONY: all bios uefi run-bios run-uefi test test-all test-bios unit check clean
all: $(IMG) $(UEFIIMG)
bios: $(IMG)
uefi: $(UEFIIMG)

# --- kernel ---------------------------------------------------------------
$(BUILD)/$(KDIR)/%.o: $(KDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/$(KDIR)/arch/cpu.o: $(KDIR)/arch/cpu.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

# AP startup trampoline: a flat binary the BSP copies to 0x8000, exposed to the
# kernel as a C array. smp.c includes the generated header.
$(BUILD)/trampoline.bin: $(BOOT)/trampoline.asm
	@mkdir -p $(BUILD)
	$(NASM) -f bin $< -o $@
$(BUILD)/trampoline_blob.h: $(BUILD)/trampoline.bin
	cd $(BUILD) && xxd -i trampoline.bin > trampoline_blob.h
$(BUILD)/$(KDIR)/arch/smp.o: $(BUILD)/trampoline_blob.h

$(BUILD)/kernel.elf: $(KOBJ) $(KDIR)/linker.ld
	$(LD) $(LDFLAGS) $(KOBJ) -o $@

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	objcopy -O binary $< $@

# --- user programs: user/<app>/<app>.c + the shared lib -> static ELF -------
# The compile rule's % matches nested paths, so it covers both user/lib/*.c and
# user/<app>/<app>.c. Each app links its own main object plus the shared lib.
$(BUILD)/$(UDIR)/%.o: $(UDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/$(UDIR)/%.o: $(UDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

define UPROG_RULE
$(BUILD)/$(1).elf: $(call app_objs,$(1)) $(ULIBOBJ) $(UDIR)/lib/user.ld
	$(LD) $(ULDFLAGS) $(call app_objs,$(1)) $(ULIBOBJ) -o $$@
endef
$(foreach p,$(UPROGS),$(eval $(call UPROG_RULE,$(p))))

# C++ apps additionally link the C++ runtime (crt: entry + ctors + new/delete).
define CXXPROG_RULE
$(BUILD)/$(1).elf: $(call app_objs,$(1)) $(CXXRT) $(CXXLIB) $(ULIBOBJ) $(UDIR)/lib/user.ld
	$(LD) $(ULDFLAGS) $(call app_objs,$(1)) $(CXXRT) $(CXXLIB) $(ULIBOBJ) -o $$@
endef
$(foreach p,$(CXXPROGS),$(eval $(call CXXPROG_RULE,$(p))))

# --- filesystem image: programs + text files, packed by the host mkfs tool -
$(MKFS): tools/mkfs.c $(KDIR)/fs/tosfs.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -o $@ tools/mkfs.c

# The seed image is laid out like a real system (see design/filesystem-layout.md):
# the boot chain + system tools under /System/bin, system config under /System/etc,
# installed apps under /Apps (populated as .app bundles -- see app-package-format.md),
# and per-user data under /Users/<user>. mkfs auto-creates parent dirs for dest=host
# entries; bare args (no '=') are empty directories.
# Apps (Terminal, Files, Notepad) ship as /Apps/<Name>.app bundles: a committed
# manifest + icon.argb (fs/apps/) plus the built ELF under the bundle's bin/. The
# boot chain + shell-invoked tools stay in /System/bin.
APP_BUNDLES := fs/apps/Terminal.app/manifest fs/apps/Terminal.app/icon.argb \
               fs/apps/Files.app/manifest    fs/apps/Files.app/icon.argb \
               fs/apps/Notepad.app/manifest  fs/apps/Notepad.app/icon.argb \
               fs/apps/Settings.app/manifest fs/apps/Settings.app/icon.argb
$(FSIMG): $(MKFS) $(UELFS) fs/motd fs/etc/registry $(APP_BUNDLES)
	$(MKFS) $@ \
	    /System/bin/init=$(BUILD)/init.elf /System/bin/shell=$(BUILD)/shell.elf \
	    /System/bin/ticker=$(BUILD)/ticker.elf /System/bin/twm=$(BUILD)/twm.elf \
	    /System/bin/fastfetch=$(BUILD)/fastfetch.elf /System/bin/memtest=$(BUILD)/memtest.elf \
	    /System/bin/selftest=$(BUILD)/selftest.elf \
	    /System/bin/clipboard=$(BUILD)/clipboard.elf \
	    /System/bin/spotlight=$(BUILD)/spotlight.elf \
	    /System/bin/launchpad=$(BUILD)/launchpad.elf \
	    /System/etc/motd=fs/motd /System/etc/registry=fs/etc/registry \
	    /System/lib /tmp \
	    /Apps/Terminal.app/manifest=fs/apps/Terminal.app/manifest \
	    /Apps/Terminal.app/icon.argb=fs/apps/Terminal.app/icon.argb \
	    /Apps/Terminal.app/bin/term=$(BUILD)/term.elf \
	    /Apps/Files.app/manifest=fs/apps/Files.app/manifest \
	    /Apps/Files.app/icon.argb=fs/apps/Files.app/icon.argb \
	    /Apps/Files.app/bin/files=$(BUILD)/files.elf \
	    /Apps/Notepad.app/manifest=fs/apps/Notepad.app/manifest \
	    /Apps/Notepad.app/icon.argb=fs/apps/Notepad.app/icon.argb \
	    /Apps/Notepad.app/bin/notepad=$(BUILD)/notepad.elf \
	    /Apps/Settings.app/manifest=fs/apps/Settings.app/manifest \
	    /Apps/Settings.app/icon.argb=fs/apps/Settings.app/icon.argb \
	    /Apps/Settings.app/bin/settings=$(BUILD)/settings.elf \
	    /Users/user/Documents /Users/user/Desktop /Users/user/Downloads /Users/user/Pictures /Users/user/.config

# --- BIOS disk image: MBR+boot | kernel | ... | tosfs partition @ LBA 2048 --
# One disk now: the boot sector's partition table points the kernel at the
# tosfs partition that follows the kernel (FS_PART_LBA in stage1.asm = 2048).
FS_PART_LBA := 2048
$(IMG): $(BUILD)/kernel.bin $(FSIMG) $(BOOT)/stage1.asm
	@ksect=$$(( ( $$(stat -c%s $(BUILD)/kernel.bin) + 511 ) / 512 )); \
	$(NASM) -f bin -DKERNEL_SECTORS=$$ksect $(BOOT)/stage1.asm -o $(BUILD)/boot.bin; \
	cp $(BUILD)/kernel.bin $(BUILD)/kernel.pad; truncate -s $$(( ksect * 512 )) $(BUILD)/kernel.pad; \
	cat $(BUILD)/boot.bin $(BUILD)/kernel.pad > $@; \
	truncate -s $$(( $(FS_PART_LBA) * 512 )) $@; \
	cat $(FSIMG) >> $@; \
	rm -f $(BUILD)/kernel.pad; \
	echo "built $@ (kernel $$ksect sectors, tosfs partition @ LBA $(FS_PART_LBA))"

# --- UEFI: a loader that embeds the kernel image as a blob -----------------
$(BUILD)/kernel_blob.h: $(BUILD)/kernel.bin
	cd $(BUILD) && xxd -i kernel.bin > kernel_blob.h

$(EFIAPP): $(EDIR)/uefi.c $(BUILD)/kernel_blob.h
	clang -target x86_64-unknown-windows -ffreestanding -fno-stack-protector \
	      -fshort-wchar -mno-red-zone -Wall -Wextra -I$(BUILD) -I$(KDIR) -c $(EDIR)/uefi.c -o $(BUILD)/uefi.o
	lld-link -subsystem:efi_application -nodefaultlib -entry:efi_main \
	      $(BUILD)/uefi.o -out:$@

# UEFI disk: one MBR-partitioned disk, like the BIOS one but with a FAT ESP
# partition (holding BOOTX64.EFI, which OVMF boots) in place of the boot sector,
# followed by the same tosfs partition the kernel mounts.
# ESP starts at 1 MiB (2048); 16 MiB FAT ESP; tosfs follows at ESP_LBA+ESP_SECTORS.
ESP_LBA     := 2048
ESP_SECTORS := 32768
UFS_LBA     := 34816
UFS_SECTORS := 4096          # tosfs partition size in sectors (== TOSFS_DISK_SECTORS / FS_PART_CNT)
$(UEFIIMG): $(EFIAPP) $(FSIMG)
	truncate -s $$(( $(ESP_SECTORS) * 512 )) $(ESPPART)
	mformat -i $(ESPPART) ::
	mmd   -i $(ESPPART) ::/EFI ::/EFI/BOOT
	mcopy -i $(ESPPART) $(EFIAPP) ::/EFI/BOOT/BOOTX64.EFI
	truncate -s $$(( ($(UFS_LBA) + $(UFS_SECTORS)) * 512 )) $@
	printf 'label: dos\nstart=$(ESP_LBA), size=$(ESP_SECTORS), type=ef, bootable\nstart=$(UFS_LBA), size=$(UFS_SECTORS), type=7f\n' | sfdisk $@ >/dev/null
	dd if=$(ESPPART) of=$@ bs=512 seek=$(ESP_LBA) conv=notrunc status=none
	dd if=$(FSIMG)   of=$@ bs=512 seek=$(UFS_LBA) conv=notrunc status=none
	@echo "built $@ (ESP @ LBA $(ESP_LBA), tosfs @ LBA $(UFS_LBA))"

# --- run ------------------------------------------------------------------
# One disk now (IDE primary master, index 0): it carries the boot sector / ESP,
# the kernel, and the tosfs partition, which the kernel's ATA driver reads.
run-bios: $(IMG)
	qemu-system-x86_64 $(ACCEL) -m $(MEM) \
	  -drive format=raw,file=$(IMG),if=ide,index=0 \
	  -no-reboot $(DISPLAY_OPT) $(QEMU)

run-uefi: $(UEFIIMG)
	cp -f $(OVMF_VARS_SRC) $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 $(ACCEL) -m $(MEM) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive format=raw,file=$(UEFIIMG),if=ide,index=0 \
	  -no-reboot $(DISPLAY_OPT) $(QEMU)

# --- tests ----------------------------------------------------------------
# Tiers (design/testing.md): `make test` boots the SMOKE set -- a dozen
# deliberate end-to-end journeys (incl. the in-OS `selftest` batch). The full
# catalog (`make test-all`) is the release gate: run it for cross-cutting
# kernel/compositor/toolkit changes, not per feature increment.
test: all
	cd tests && python3 run_tests.py

test-all: all
	cd tests && python3 run_tests.py --all

test-bios: $(IMG) $(FSIMG)
	cd tests && python3 run_tests.py --bios-only

# Host unit tests for pure logic (parsers, index/geometry math). Compiled with the
# HOST cc and run in milliseconds -- no QEMU. See design/testing.md.
HOSTCC ?= cc
UNIT_SRCS := $(wildcard tests/unit/t_*.c)
unit:
	@mkdir -p $(BUILD)/unit; fail=0; \
	for f in $(UNIT_SRCS); do \
	  bin=$(BUILD)/unit/$$(basename $$f .c); \
	  $(HOSTCC) -std=c11 -Wall -Wextra -O0 -g -o $$bin $$f || exit 1; \
	  $$bin || fail=1; \
	done; exit $$fail

# The day-to-day gate: fast unit tests first, then the smoke journeys.
check: unit test

clean:
	rm -rf $(BUILD)

# Header-dependency files (generated by -MMD -MP). Included last so the rules
# they carry never override the default goal (`all`). Missing entries (e.g. the
# NASM-built cpu.o) are ignored.
-include $(DEPS)
