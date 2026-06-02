#!/usr/bin/env python3
"""tOS integration tests: boot the OS under QEMU and assert on its behaviour.

Run with `make test` (or `python3 tests/run_tests.py [--uefi-only|--bios-only]`).
Exits non-zero if any test fails.
"""
import re
import sys
import time
from harness import Tos

PERSIST_IMG = "/tmp/tos_persist_fs.img"


# --- individual tests (raise AssertionError on failure) -------------------

def t_boot_and_ls(uefi):
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up cleanly"
        s = t.serial()
        assert "mounted tosfs from disk" in s, "FS was not mounted"
        assert "[init] pid 1 is up" in s, "init (pid 1) did not start"
        # the boot chain + shell-invoked tools live under /System/bin now.
        t.line("ls /System/bin")
        assert t.wait_for("shell\t", 5), "ls did not list system binaries"
        s = t.serial()
        for f in ("init\t", "shell\t", "ticker\t", "twm\t", "fastfetch\t"):
            assert f in s, f"ls /System/bin missing entry: {f!r}"
        # dock apps ship as /Apps/<Name>.app bundles.
        t.line("ls /Apps")
        assert t.wait_for("Terminal.app/", 5), "app bundles missing from /Apps"
        s = t.serial()
        for d in ("Terminal.app/", "Files.app/", "Notepad.app/"):
            assert d in s, f"ls /Apps missing bundle: {d!r}"
        # the shell starts in the user's home, which has the standard folders.
        t.line("ls /Users/user")
        assert t.wait_for("Documents/", 5), "home folders missing"


def t_fs_persist(uefi):
    # One reboot exercises create-, delete-, AND directory-flush persistence: a kept
    # file (and its contents) survives, a file deleted before the reboot stays gone,
    # and a directory tree + nested file persist. (Folds together t_persistence,
    # t_rm_persist, and t_dir_persist.)
    with Tos(uefi=uefi, scratch=PERSIST_IMG, reuse=False) as t:
        assert t.boot_ok(), "shell did not come up (boot 1)"
        assert t.line_for("write keep.txt", "enter a line"), "write prompt missing (keep)"
        t.line("survivesreboot")
        assert t.wait_for("saved keep.txt", 5), "write did not save (keep)"
        assert t.line_for("write gone.txt", "enter a line"), "write prompt missing (gone)"
        t.line("temporary")
        assert t.wait_for("saved gone.txt", 5), "write did not save (gone)"
        t.line("rm gone.txt")
        assert t.wait_for("removed gone.txt", 5), "rm did not report removal"
        # a directory tree + nested file must persist too (folds in t_dir_persist)
        t.line("mkdir keepdir"); t.line("cd keepdir")
        assert t.line_for("write nested.txt", "enter a line"), "nested write prompt missing"
        t.line("nesteddata"); assert t.wait_for("saved nested.txt", 5), "nested write did not save"
        t.line("cd ..")
        t.line("poweroff")
        assert t.wait_for("shutdown requested", 5), "did not shut down"
    # Second boot on the SAME disk image: prove the directory + data blocks were
    # flushed, without typing anything this time.
    with Tos(uefi=uefi, scratch=PERSIST_IMG, reuse=True) as t:
        assert t.boot_ok(), "shell did not come up (boot 2)"
        t.line("ls")
        assert t.wait_for("keep.txt\t", 5), "file did not persist across reboot"
        t.line("cat keep.txt")
        assert t.wait_for("survivesreboot", 5), "file contents did not persist"
        t.line("cat gone.txt")
        assert t.wait_for("cat: no such file: gone.txt", 5), "deleted file reappeared after reboot"
        t.line("ls"); assert t.wait_for("keepdir/", 5), "directory did not persist across reboot"
        t.line("cat keepdir/nested.txt"); assert t.wait_for("nesteddata", 5), "nested file did not persist"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_partition(uefi):
    # The FS must be found via the MBR partition table, not assumed at LBA 0.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        m = re.search(r"mounted tosfs from disk \(partition LBA (\d+)\)", t.serial())
        assert m, "kernel did not report the tosfs partition"
        base = int(m.group(1))
        assert base > 0, "FS not in a partition (fell back to LBA 0)"


def _count_at_least(t, needle, n, timeout=8):
    """Wait until `needle` appears at least `n` times in the serial log."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if t.serial().count(needle) >= n:
            return True
        time.sleep(0.1)
    return False


def t_alt_tab(uefi):
    # macOS-style Alt-Tab switcher overlay (#7). With Terminal + Files open (Files
    # focused), Alt+Tab opens a centred card of window tiles and advances the
    # selection to the next MRU window (Terminal) WITHOUT changing focus yet
    # ("[twm] altswitch open <n> <px> <py>" + "[twm] altswitch sel 1 Terminal").
    # The harness can't hold a modifier, so the card commits on its linger timeout
    # ("[twm] altswitch commit Terminal"), which focuses the selection. (On real
    # hardware it commits on Alt release; ESC cancels, Enter / a tile click commit.)
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files")
        assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        before = t.serial().count("[twm] focus Terminal")
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] altswitch open 2", 6), "Alt+Tab did not open the switcher card"
        assert t.wait_for("[twm] altswitch sel 1 Terminal", 6), \
            "the switcher did not advance the selection to the next window"
        t.screenshot("/tmp/tos_altswitch.ppm")          # card is up during the linger
        assert t.wait_for("[twm] altswitch commit Terminal", 6), \
            "the switcher did not commit the selection on the linger timeout"
        assert _count_at_least(t, "[twm] focus Terminal", before + 1, 6), \
            "committing the switch did not focus the selected window"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_notepad_edit_save(uefi):
    # Notepad is a real editor. It is no longer pinned to the dock (only Terminal
    # and Files are), so open it via Spotlight; a running unpinned app then shows up
    # as a transient dock tile. Type a note into the editor TextField and Ctrl+S
    # saves it to the user's home, read back through a terminal to prove it hit disk.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1)                # Super+Space -> Spotlight
        assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06)                    # filter down to Notepad
        t.key("ret", delay=0.1)                        # Enter launches it
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        # a running unpinned app appears as a transient dock tile (issue #3)
        assert t.icon_xy("Notepad"), "running Notepad did not appear in the dock"
        t.type("notepadworks", delay=0.06)            # into the focused editor
        t.key("ctrl-s", delay=0.1)                    # save
        assert t.wait_for("[notepad] saved /Users/user/untitled.txt (12 bytes)", 8), \
            "Notepad did not save the typed note"
        # prove it persisted: read it back in the terminal (shell cwd is ~)
        t.key("alt-tab", delay=0.1)                   # focus the terminal again
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("cat untitled.txt")
        assert t.wait_for("notepadworks", 6), "saved note not readable from the filesystem"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_spotlight(uefi):
    # Super+Space opens the Spotlight launcher (a popup). Typing filters the
    # installed apps; Enter launches the match (here: Notepad).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1)               # Super+Space
        assert t.wait_for("[spotlight] up", 8), "Super+Space did not open Spotlight"
        assert t.wait_for("[twm] focus Spotlight", 6), "Spotlight popup did not focus"
        t.type("note", delay=0.06)                   # filter down to Notepad
        t.key("ret", delay=0.1)                      # Enter launches the selection
        assert t.wait_for("[notepad] up", 10), "Spotlight did not launch the filtered app"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


REG_IMG = "/tmp/tos_reg_fs.img"


def t_registry(uefi):
    # The registry serves system defaults and stores per-user overrides that persist
    # across a reboot (written to /Users/user/.config/registry).
    with Tos(uefi=uefi, scratch=REG_IMG, reuse=False) as t:
        assert t.boot_ok(), "shell did not come up (boot 1)"
        t.line("reg get theme.accent")
        assert t.wait_for("#5AA0FC", 5), "system default not readable from the registry"
        t.line("reg set ui.test hello123")
        assert t.wait_for("set ui.test", 5), "reg set did not confirm"
        t.line("poweroff")
        assert t.wait_for("shutdown requested", 5), "did not shut down"
    with Tos(uefi=uefi, scratch=REG_IMG, reuse=True) as t:
        assert t.boot_ok(), "shell did not come up (boot 2)"
        t.line("reg get ui.test")               # only set on the previous boot
        assert t.wait_for("hello123", 5), "user setting did not persist across reboot"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_sleep(uefi):
    # SYS_SLEEP parks the task on the timer and returns once, then the shell
    # carries on -- proving the sleep wakes and doesn't wedge the scheduler.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("sleep 30")
        assert t.wait_for("slept 30 ticks", 6), "sleep did not return"
        t.line("echo awakeok")
        assert t.wait_for("awakeok", 5), "shell unresponsive after sleep"
        assert "[EXCEPTION]" not in t.serial(), "fault during sleep"


def t_smp(uefi):
    # SMP end to end on 4 CPUs in a single boot: the BSP brings every AP online,
    # and the batch of CPU-bound tasks the `smp` command forks runs to completion
    # spread across more than one core (real parallel scheduling + balancing) with
    # at least one task landing on an application processor (CPU index > 0).
    # (Folds together the old t_smp / t_smp_tasks / t_smp_balance.)
    with Tos(uefi=uefi, cpus=4) as t:
        assert t.boot_ok(), "shell did not come up under -smp 4"
        assert t.wait_for("[smp] 4 of 4 CPUs online", 8), "APs did not all come online"
        t.line("smp")
        assert t.wait_for("smp: all tasks done", 25), "smp tasks did not all finish/reap"
        seen = set(re.findall(r"running on CPU (\d)", t.serial()))
        assert any(c != "0" for c in seen), f"no task ran on an AP (CPUs seen: {sorted(seen)})"
        assert len(seen) >= 2, f"tasks were not balanced across CPUs (CPUs seen: {sorted(seen)})"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial(), "fault under SMP"


def t_fork(uefi):
    # fork() returns twice: the child runs its branch and exits, the parent reaps
    # it with wait() and gets the child's pid.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("fork")
        assert t.wait_for("[child] hello from the forked child", 5), "forked child did not run"
        assert t.wait_for("[parent] reaped child pid", 5), "parent did not reap the child"
        assert "[EXCEPTION]" not in t.serial(), "fault during fork/wait"


def t_orphan_reparent(uefi):
    # Spawn (fork+exec) a background ticker, then close the terminal while it is
    # still running. The shell, the terminal and the orphaned ticker all exit and
    # are reaped (the ticker is re-parented to init, runs to completion, and does
    # not leak a zombie or crash). The kernel logs every ring-3 task exit, so we
    # count those: shell + term + ticker = 3 after the close.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        before = t.serial().count("task exited (ran at CPL=3)")
        t.line("spawn")
        assert t.wait_for("spawned ticker as pid", 5), "spawn (fork+exec) failed"
        assert t.wait_for("[ticker] tick 1", 6), "ticker did not start"
        t.line("halt")                              # shell exits -> ticker orphaned -> init
        deadline = time.time() + 20
        while time.time() < deadline:
            if t.serial().count("task exited (ran at CPL=3)") >= before + 3:
                break
            time.sleep(0.2)
        assert t.serial().count("task exited (ran at CPL=3)") >= before + 3, \
            "orphaned ticker was not reparented/reaped"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_gui(uefi):
    # init launches twm (the compositor); both boot paths are graphical (UEFI via
    # GOP, BIOS via a VBE linear framebuffer), so the desktop comes up, the PS/2
    # mouse initialises, and double-clicking the Terminal icon launches the
    # terminal emulator, which runs the shell over a pty.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "desktop did not launch the terminal"
        s = t.serial()
        assert "[twm] desktop ready" in s, "compositor did not come up"
        assert "PS/2 mouse enabled on IRQ12" in s, "mouse did not init on the framebuffer boot"
        t.line("echo windowed")
        assert t.wait_for("windowed", 5), "shell in the terminal window is unresponsive"


def t_mouse(uefi):
    # The PS/2 mouse driver tracks movement: the `mouse` command prints the cursor
    # position via SYS_MOUSE, and injecting relative movement must change it.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("mouse")
        assert t.wait_for("mouse: x=", 5), "mouse command produced no report"
        before = re.findall(r"mouse: x=(\d+) y=(\d+)", t.serial())[-1]
        # Inject movement and re-read until the reported position actually changes.
        # Retrying the whole inject+report cycle (rather than trusting one fixed-sleep
        # burst) keeps this robust when the host is loaded and the guest drops a PS/2
        # packet: a dropped packet just means another lap, not a spurious failure.
        deadline = time.time() + 20
        after = before
        while time.time() < deadline:
            for _ in range(8):
                t.key("right"); t.mon.sendall(b"mouse_move 40 30\n"); time.sleep(0.04)
            time.sleep(0.3)
            t.line("mouse")
            if not t.wait_for("mouse: x=", 5):
                continue
            after = re.findall(r"mouse: x=(\d+) y=(\d+)", t.serial())[-1]
            if after != before:
                break
        assert after != before, f"mouse position did not change ({before} -> {after})"


def t_many_files(uefi):
    # The directory is multi-sector now, so the file count is bounded by disk
    # space, not the old 15-entry cap. Create well past 15 and confirm they list.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        n = 24
        for i in range(n):
            t.line("write m%d" % i)
            t.line("d")
        t.line("ls")
        time.sleep(1.0)
        s = t.serial()
        present = sum(1 for i in range(n) if ("m%d\t" % i) in s)
        assert present == n, f"only {present}/{n} files listed (old 15-file cap?)"
        assert "[EXCEPTION]" not in s and "PANIC" not in s


def t_ram_scales(uefi):
    # The frame pool is sized from the firmware e820 map, not a fixed cap: more RAM
    # yields a proportionally larger pool. The 6G case also pushes RAM ABOVE the 4 GiB
    # PCI hole, exercising the multi-region kernel map -- AND, on the UEFI path, the
    # loader's all-RAM transition map: OVMF loads the EFI app high (~5 GiB), so without
    # a full identity map the CR3 switch #PFs before the kernel ever runs. Regression
    # guard for the >4 GiB UEFI boot fix (uefi/uefi.c ram_top/build_tables).
    def pool(mem):
        with Tos(uefi=uefi, mem=mem) as t:
            assert t.boot_ok(), f"did not boot with -m {mem}"
            s = t.serial()
            m = re.search(r"frame pool: (\d+) frames", s)
            assert m, f"no frame pool report with -m {mem}"
            return int(m.group(1)), s
    small, _ = pool("64M")
    big, _   = pool("256M")
    assert big > small * 3, f"pool did not scale with RAM ({small} @64M vs {big} @256M)"
    huge, s = pool("6G")            # 6G straddles the 4 GiB hole -> RAM remapped high
    assert huge > big * 3, f"pool did not scale to 6G ({big} @256M vs {huge} @6G)"
    assert "regions across the 4 GiB hole" in s, \
        "6G boot did not report RAM across the 4 GiB hole (above-4G RAM unused?)"


def t_app_crash(uefi):
    # Fault-injection: `crash` forks a child that touches a wild address. The kernel
    # must kill ONLY that child (a ring-3 fault), the shell reaps it, and the OS
    # stays up -- one crashing process must not halt the whole machine.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("crash")
        assert t.wait_for("[EXCEPTION]", 6), "the crash did not trap"
        assert t.wait_for("killing faulting task", 6), "kernel did not kill the faulting task"
        assert t.wait_for("shell and OS still alive", 6), "shell did not survive the child crash"
        assert "system halted" not in t.serial(), "a user-mode crash halted the OS"
        assert "PANIC" not in t.serial(), "a user-mode crash panicked the kernel"
        # the shell must still be interactive after the crash
        t.line("uname")
        assert t.wait_for("a hobby OS", 5), "shell stopped responding after a crash"


def t_fs_crud(uefi):
    # The filesystem CRUD surface in one boot: read shipped content (incl. a file two
    # directories deep), the missing-file error, create + read-back, rewrite-in-place
    # (O_TRUNC), hierarchical mkdir/cd (a subdir file is invisible at the root), move
    # across directories, delete, and recursive remove with rmdir's non-empty guard.
    # (Replaces the per-op fs micro-tests; persistence across reboot is t_fs_persist.)
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("cat /System/etc/motd")
        assert t.wait_for("Welcome to tOS.", 5), "cat of a shipped file failed"
        t.line("cat /Apps/Terminal.app/manifest")
        assert t.wait_for("min_width", 5), "could not read a nested app manifest"
        t.line("cat nope")
        assert t.wait_for("cat: no such file: nope", 5), "missing-file error not reported"
        # create + read back from disk
        assert t.line_for("write note.txt", "enter a line"), "write prompt missing"
        t.line("uniqueaaa")
        assert t.wait_for("saved note.txt", 5), "write did not save"
        t.line("ls"); assert t.wait_for("note.txt\t", 5), "new file not listed"
        # rewrite in place (O_TRUNC): the old content is gone, the new is read back
        assert t.line_for("write note.txt", "enter a line"), "rewrite prompt missing"
        t.line("uniquebbb")
        assert _count_at_least(t, "saved note.txt", 2, 6), "rewrite did not save"
        t.line("cat note.txt"); assert t.wait_for("uniquebbb", 5), "rewritten content not read back"
        assert t.serial().count("uniqueaaa") == 1, "old content survived the rewrite"
        # hierarchical namespace: a subdir file is not visible at the root
        t.line("mkdir proj"); t.line("cd proj")
        assert t.line_for("write inside.txt", "enter a line"), "subdir write prompt missing"
        t.line("deepfile"); assert t.wait_for("saved inside.txt", 5), "write into subdir failed"
        t.line("ls"); assert t.wait_for("inside.txt\t", 5), "file not listed in the subdir"
        t.line("cd ..")
        t.line("cat inside.txt")
        assert t.wait_for("cat: no such file: inside.txt", 5), "file leaked into the parent namespace"
        # move across directories: the original is gone, the content follows
        t.line("mkdir box"); t.line("mv proj/inside.txt box/m.txt")
        assert t.wait_for("moved proj/inside.txt", 5), "mv did not report the move"
        t.line("cat box/m.txt")
        assert _count_at_least(t, "deepfile", 2, 6), "moved file content not at the destination"
        # delete a file, then a whole tree (rmdir refuses a non-empty dir)
        t.line("rm box/m.txt"); assert t.wait_for("removed box/m.txt", 5), "rm did not remove the file"
        t.line("mkdir tree"); t.line("cd tree"); t.line("mkdir sub"); t.line("cd ..")
        t.line("rmdir tree"); assert t.wait_for("rmdir: cannot remove tree", 5), "rmdir removed a non-empty dir"
        t.line("rm -r tree"); assert t.wait_for("removed tree", 5), "rm -r did not complete"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_window_mgmt(uefi):
    # Compositor window management in one journey: launch a second app from the dock,
    # the clipboard popup opens with Super+V and is single-instance (a second Super+V
    # summons, doesn't relaunch), Esc dismisses the popup, and Super+Q closes the
    # focused window. (Replaces t_files_app, t_clipboard_summon, t_clipboard_popup_esc,
    # t_super_q_close; the Alt-Tab switcher is t_alt_tab.)
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files did not launch from its icon"
        assert t.wait_for("[twm] focus Files", 8), "Files never took focus"
        # Super+V opens the clipboard popup; a second Super+V summons it (no relaunch)
        t.key("meta_l-v", delay=0.1)
        assert t.wait_for("[clipboard] up", 8), "Super+V did not open the clipboard"
        assert t.wait_for("[twm] focus Clipboard", 5), "clipboard popup did not focus"
        time.sleep(0.5); t.key("meta_l-v", delay=0.1); time.sleep(2.0)
        assert t.serial().count("[clipboard] up") == 1, "Super+V relaunched the clipboard instead of summoning it"
        # Esc dismisses the popup; focus returns to the window underneath (Files)
        before = t.serial().count("[twm] focus Files")
        t.key("esc", delay=0.1)
        assert _count_at_least(t, "[twm] focus Files", before + 1, 8), "Esc did not dismiss the clipboard popup"
        # Super+Q closes the focused window (Files); focus returns to the Terminal
        before = t.serial().count("[twm] focus Terminal")
        t.key("meta_l-q", delay=0.1)
        assert _count_at_least(t, "[twm] focus Terminal", before + 1, 8), "Super+Q did not close the focused window"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_drivers(uefi):
    # Hardware-driver smoke in one boot: the CMOS RTC prints a plausible timestamp,
    # PCI enumeration lists QEMU's i440fx devices, the PC speaker doesn't wedge the
    # shell, and `reboot` resets the machine via the 8042 (reboot runs last -- it ends
    # the session). (Replaces t_date, t_lspci, t_beep, t_reboot.)
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("date")
        m = re.search(r"(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})", t.serial())
        assert m, "date did not print a YYYY-MM-DD HH:MM:SS timestamp"
        yr, mo, dy, hh, mm, ss = (int(x) for x in m.groups())
        assert 2000 <= yr <= 2100 and 1 <= mo <= 12 and 1 <= dy <= 31, f"implausible date {m.group(0)}"
        assert hh < 24 and mm < 60 and ss < 60, f"implausible time {m.group(0)}"
        t.line("lspci")
        assert t.wait_for("8086:1237", 5), "PCI host bridge (8086:1237) not listed"
        assert "8086:7010" in t.serial(), "IDE controller (8086:7010) not listed"
        t.line("beep"); t.line("echo beepok")
        assert t.wait_for("beepok", 5), "shell unresponsive after beep"
        t.line("reboot")
        assert t.wait_for("reboot requested", 5), "reboot was not requested"
        assert "[EXCEPTION]" not in t.serial()


# The e2e smoke/journey set -- a tight set of canaries that prove the OS is alive
# (see design/testing.md). Pure logic lives in the host unit tests (make unit), so
# per-feature behaviours are NOT re-tested here by booting QEMU.
BIOS_TESTS = [
    # boot + filesystem
    t_boot_and_ls, t_partition, t_fs_crud, t_fs_persist, t_registry, t_many_files,
    # processes / scheduler
    t_sleep, t_fork, t_orphan_reparent, t_app_crash, t_smp,
    # compositor + GUI journeys
    t_gui, t_window_mgmt, t_alt_tab, t_notepad_edit_save, t_spotlight,
    # hardware
    t_mouse, t_ram_scales, t_drivers,
]
# A representative subset on UEFI to confirm both boot paths reach the same OS.
# t_ram_scales runs here too: its 6G case is the regression guard for the UEFI
# loader's >4 GiB transition map (the kernel-only path can't catch that bug).
UEFI_TESTS = [t_boot_and_ls, t_partition, t_fs_crud, t_fork, t_app_crash, t_smp,
              t_gui, t_window_mgmt, t_mouse, t_many_files, t_ram_scales]


def run(label, tests, uefi):
    passed = failed = 0
    for fn in tests:
        name = f"[{label}] {fn.__name__}"
        try:
            fn(uefi)
            print(f"PASS  {name}")
            passed += 1
        except AssertionError as e:
            print(f"FAIL  {name}: {e}")
            failed += 1
        except Exception as e:  # noqa: BLE001
            print(f"ERROR {name}: {e!r}")
            failed += 1
    return passed, failed


def main():
    do_bios = "--uefi-only" not in sys.argv
    do_uefi = "--bios-only" not in sys.argv
    total_pass = total_fail = 0
    if do_bios:
        p, f = run("bios", BIOS_TESTS, uefi=False)
        total_pass += p; total_fail += f
    if do_uefi:
        p, f = run("uefi", UEFI_TESTS, uefi=True)
        total_pass += p; total_fail += f
    print(f"\n{total_pass} passed, {total_fail} failed")
    sys.exit(1 if total_fail else 0)


if __name__ == "__main__":
    main()
