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
SESSION_IMG = "/tmp/tos_session_fs.img"


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
        # ...and the dock draws a pinned|running divider once an unpinned app runs
        assert t.wait_for("[twm] docksep", 6), "dock did not record a pinned|running divider"
        t.type("notepadworks", delay=0.06)            # into the focused editor
        t.key("ctrl-s", delay=0.1)                    # save
        assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (12 bytes)", 8), \
            "Notepad did not save the typed note"
        # prove it persisted: read it back in the terminal (shell cwd is ~, saves land in Documents)
        t.key("alt-tab", delay=0.1)                   # focus the terminal again
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("cat Documents/untitled.txt")
        assert t.wait_for("notepadworks", 6), "saved note not readable from the filesystem"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_notepad_undo(uefi):
    # Undo/redo is a global TextField contract: type a word into Notepad's editor,
    # Ctrl+Z drops the whole typed run in one step (the chars coalesce), and Ctrl+Y
    # puts it back. Notepad surfaces these as Edit > Undo/Redo (accelerators ^Z/^Y),
    # so the compositor routes them as menu picks (menu 1, items 1/2). We prove the
    # buffer actually changed by saving after each and reading the byte count.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1)                # Super+Space -> Spotlight
        assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06)
        t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        t.type("undoredo", delay=0.06)                # one coalesced typing run
        t.key("ctrl-z", delay=0.12)                   # undo -> Edit > Undo (menu 1, item 1)
        assert t.wait_for("[notepad] menu 1 1", 8), "Ctrl+Z did not fire Edit > Undo"
        t.key("ctrl-s", delay=0.12)                   # buffer is now empty
        assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (0 bytes)", 8), \
            "undo did not clear the typed run (expected a 0-byte save)"
        t.key("ctrl-y", delay=0.12)                   # redo -> Edit > Redo (menu 1, item 2)
        assert t.wait_for("[notepad] menu 1 2", 8), "Ctrl+Y did not fire Edit > Redo"
        t.key("ctrl-s", delay=0.12)
        assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (8 bytes)", 8), \
            "redo did not restore the typed run (expected an 8-byte save)"
        # prove the restored text is exactly what was typed, read back off disk
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("cat Documents/untitled.txt")
        assert t.wait_for("undoredo", 6), "redo did not restore the original text"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def _notepad_file_menu(t, idx):
    """Open Notepad's File menu and click item `idx` (0=New, 1=Open, 2=Save,
    3=Save As). Deterministic (a click, not the Ctrl accelerator, which races the
    modifier release at launch)."""
    m = None
    for _ in range(50):
        m = re.search(r"\[twm\] appmenu 0 File (\d+) (\d+)", t.serial())
        if m:
            break
        time.sleep(0.1)
    assert m, "Notepad's File menu tile was not shown"
    fx, fw = int(m.group(1)), int(m.group(2))
    t.click(fx + fw // 2, 12)
    assert t.wait_for("[twm] menu app File", 6), "the File menu did not open"
    g = re.search(r"\[twm\] menu app File y (\d+) row (\d+) x (\d+)", t.serial())
    assert g, "File menu geometry not reported"
    ry, row, mx = int(g.group(1)), int(g.group(2)), int(g.group(3))
    t.click(mx + 20, ry + idx * row + row // 2)


def t_notepad_guard(uefi):
    # Unsaved-changes guard (#5), now on tab/window CLOSE -- File > New just opens a
    # fresh tab (no data loss, no guard). Closing a dirty tab (File > Close Tab,
    # item 4) raises the modal ConfirmDialog (Save / Discard / Cancel). We open two
    # dirty tabs (so closing one leaves the window alive) and exercise both the
    # Discard (click) and Save (Enter) paths; the Save path writes before closing.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1); assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        wr = t.win_rect("Notepad"); assert wr, "Notepad window rect not reported"
        wx, wy = wr[0], wr[1]
        # tab 0 = "keepme"; File > New opens a second tab (no guard) ...
        t.type("keepme", delay=0.05)
        _notepad_file_menu(t, 0)                          # File > New -> a fresh tab
        assert t.wait_for("[notepad] newtab 1/2", 8), "File > New did not open a second tab"
        t.type("savedtext", delay=0.05)                   # tab 1 = "savedtext" (also dirty)
        # --- Discard path: Close Tab on the dirty active tab 1, click Discard ---
        _notepad_file_menu(t, 4)                          # File > Close Tab
        assert t.wait_for("[notepad] guard close 1", 8), "closing a dirty tab did not raise the guard"
        d = None
        for _ in range(40):
            d = re.search(r"\[ui\] dlgbtn 1 (\d+) (\d+)", t.serial())   # button 1 = Discard
            if d:
                break
            time.sleep(0.1)
        assert d, "the guard's Discard button position was not reported"
        t.click(wx + int(d.group(1)), wy + int(d.group(2)))
        assert t.wait_for("[ui] confirm 1", 6), "clicking Discard did not register"
        assert t.wait_for("[notepad] closetab 0/1", 6), "Discard did not close the tab (tab 0 should remain)"
        # --- Save path: Close Tab on the remaining dirty tab 0, Enter = Save ---
        _notepad_file_menu(t, 4)                          # File > Close Tab (the last tab)
        assert t.wait_for("[notepad] guard close 0", 8), "closing the last dirty tab did not raise the guard"
        t.key("ret", delay=0.12)                          # Enter = the primary (Save) button
        assert t.wait_for("[ui] confirm 0", 6), "Enter did not choose the primary (Save) button"
        assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (6 bytes)", 8), \
            "Save path did not write the tab before closing"
        # the note persisted to disk (read it back from the terminal)
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("cat Documents/untitled.txt")
        assert t.wait_for("keepme", 6), "the Save-then-Close path did not persist the text"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_file_dialog(uefi):
    # Reusable file picker (#4): Notepad's File > Save As... opens a modal ui::FileDialog
    # (a Favorites sidebar + an Up button + the directory list + a name field). It starts
    # in ~/Documents; we replace the suggested name (it opens select-all'd) by typing a new
    # one, Enter confirms, the dialog reports the chosen path, Notepad saves there, and we
    # read it back off disk through the terminal to prove it persisted.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1); assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        wr = t.win_rect("Notepad"); assert wr, "Notepad window rect not reported"
        wx, wy = wr[0], wr[1]
        t.type("viapicker", delay=0.05)                  # 9 bytes into the editor
        _notepad_file_menu(t, 3)                          # File > Save As...
        assert t.wait_for("[notepad] menu 0 3", 6), "File > Save As was not delivered"
        assert t.wait_for("[filedialog] open save /Users/user/Documents", 8), \
            "Save As did not open the picker in ~/Documents"
        # the name field opens pre-filled + select-all'd, so typing replaces it
        t.type("picked.txt", delay=0.05)
        t.key("ret", delay=0.12)                          # Enter = the primary (Save) button
        assert t.wait_for("[filedialog] pick /Users/user/Documents/picked.txt", 8), \
            "the picker did not report the chosen path"
        assert t.wait_for("[notepad] saved /Users/user/Documents/picked.txt (9 bytes)", 8), \
            "Notepad did not save to the picked path"
        # --- overwrite -> Keep Both: Save As the same name, then dedupe to "picked (2).txt" ---
        _notepad_file_menu(t, 3)                          # File > Save As... again
        time.sleep(0.6)                                   # let the picker open + grab focus (same name pre-filled)
        t.key("ret", delay=0.15)                          # Enter on "picked.txt" -> the file exists
        d = None
        for _ in range(40):
            d = re.search(r"\[ui\] dlgbtn 1 (\d+) (\d+)", t.serial())   # button 1 = Keep Both
            if d:
                break
            time.sleep(0.1)
        assert d, "the overwrite warning's Keep Both button was not reported"
        t.click(wx + int(d.group(1)), wy + int(d.group(2)))
        assert t.wait_for("[ui] confirm 1", 6), "clicking Keep Both did not register"
        assert t.wait_for("[filedialog] pick /Users/user/Documents/picked (2).txt", 8), \
            "Keep Both did not dedupe the colliding name"
        assert t.wait_for("[notepad] saved /Users/user/Documents/picked (2).txt (9 bytes)", 8), \
            "Notepad did not save the deduped file"
        # prove both persisted: read picked.txt back + list the deduped sibling
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("cat Documents/picked.txt")
        assert t.wait_for("viapicker", 6), "the picker-saved note was not readable from the filesystem"
        t.line("ls Documents")
        assert t.wait_for("picked (2).txt", 6), "the Keep Both dedupe did not land on disk"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_notepad_session(uefi):
    # Session autosave + restore (#5): Notepad periodically drafts every open tab +
    # the session layout to ~/.cache/notepad, so a relaunch restores the whole
    # session -- even notes never explicitly saved. Boot 1 opens two unsaved tabs and
    # waits for a draft; boot 2 (same disk) relaunches Notepad and must rebuild both
    # tabs from the drafts. The restore markers carry each tab's loaded byte count, so
    # they prove the content (not just the layout) came back.
    with Tos(uefi=uefi, scratch=SESSION_IMG, reuse=False) as t:
        assert t.open_terminal(), "desktop/terminal did not come up (boot 1)"
        t.key("meta_l-spc", delay=0.1); assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch (boot 1)"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        t.type("restoreme", delay=0.05)                   # tab 0 (9 bytes), never saved
        _notepad_file_menu(t, 0)                           # File > New -> tab 1
        assert t.wait_for("[notepad] newtab 1/2", 8), "second tab did not open"
        t.type("second tab", delay=0.04)                  # tab 1 (10 bytes), never saved
        # wait for a draft written AFTER the last edit (count must strictly increase)
        c0 = t.serial().count("[notepad] autosave")
        assert _count_at_least(t, "[notepad] autosave", c0 + 1, timeout=10), \
            "Notepad did not autosave a draft of the session"
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("poweroff"); assert t.wait_for("shutdown requested", 5), "did not shut down (boot 1)"
    # Boot 2 on the SAME disk: relaunch Notepad; it must restore both tabs from drafts.
    with Tos(uefi=uefi, scratch=SESSION_IMG, reuse=True) as t:
        assert t.open_terminal(), "desktop/terminal did not come up (boot 2)"
        t.key("meta_l-spc", delay=0.1); assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch (boot 2)"
        assert t.wait_for("[notepad] restored 2 tabs active 1", 10), "Notepad did not restore the session"
        assert t.wait_for("[notepad] restored tab 0 9 untitled.txt", 4), "tab 0 content was not restored"
        assert t.wait_for("[notepad] restored tab 1 10 untitled 2.txt", 4), "tab 1 content was not restored"
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


def t_system_ownership(uefi):
    # tosfs carries a per-entry owner and the desktop session runs as the user
    # (uid 1), so the system-owned tree can't be modified: the shell's rm reports a
    # permission denial and the file survives, while a user-owned file still deletes
    # (we didn't over-lock). (NEXT_STEPS: system ownership #1; design/system-ownership.md.)
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        # deleting a system file is refused...
        t.line("rm /System/etc/motd")
        assert t.wait_for("permission denied (system file)", 6), "rm of a system file was not refused"
        # ...and the file is still there
        t.line("ls /System/etc")
        assert t.wait_for("motd", 6), "the system file did not survive the refused delete"
        # a user-owned file in the home dir still creates and deletes normally
        assert t.line_for("write own.txt", "enter a line"), "write prompt missing"
        t.line("mineonly"); assert t.wait_for("saved own.txt", 6), "user file did not save"
        t.line("rm own.txt"); assert t.wait_for("removed own.txt", 6), "a user-owned file failed to delete"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_app_menu(uefi):
    # An app declares its own menu bar (SYS_WIN_SETMENU); the compositor shows the
    # File/Edit tiles next to the app name, and choosing an item delivers WEV_MENU
    # back to the app. Here Notepad's File > Save saves the document. (NEXT_STEPS:
    # app menus #6; design/ui.md.)
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        t.key("meta_l-spc", delay=0.1); assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        m = None
        for _ in range(50):                            # the bar reports the app's File tile once it's fetched
            m = re.search(r"\[twm\] appmenu 0 File (\d+) (\d+)", t.serial())
            if m:
                break
            time.sleep(0.1)
        assert m, "Notepad's declared File menu tile was not shown in the bar"
        fx, fw = int(m.group(1)), int(m.group(2))
        t.click(fx + fw // 2, 12)                       # open the File menu (bar row)
        assert t.wait_for("[twm] menu app File", 6), "the File menu did not open"
        g = re.search(r"\[twm\] menu app File y (\d+) row (\d+) x (\d+)", t.serial())
        assert g, "File menu geometry not reported"
        ry, row, mx = int(g.group(1)), int(g.group(2)), int(g.group(3))
        t.click(mx + 30, ry + 2 * row + row // 2)        # choose "Save" (File is New/Open/Save/Save As)
        assert t.wait_for("[notepad] menu 0 2", 6), "menu selection not delivered to the app (WEV_MENU)"
        assert t.wait_for("[notepad] saved", 8), "File > Save did not save the document"
        # Keyboard accelerator (#6): Ctrl+N fires File > New (item 0) via a WEV_MENU,
        # the same path a click takes -- the compositor intercepts the chord and never
        # forwards the control byte.
        t.key("ctrl-n", delay=0.1)
        assert t.wait_for("[twm] accel N 0 0", 6), "Ctrl+N accelerator was not recognised by the compositor"
        assert t.wait_for("[notepad] menu 0 0", 6), "the accelerator did not deliver WEV_MENU(File,New) to the app"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_menu(uefi):
    # Files is now a toolkit app with a real File/Edit/Go menu bar (app menus #6).
    # The compositor shows the three tiles next to the app name, and the Edit/File
    # accelerators (^C/^X/^V/^N) route to the same actions the toolbar + right-click
    # run. Here Ctrl+N fires File > New Folder via a WEV_MENU and a folder lands on
    # disk in the user's home (where Files opens).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files")
        assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        # the compositor draws all three declared menu tiles in the bar
        for tile in ("appmenu 0 File", "appmenu 1 Edit", "appmenu 2 Go"):
            assert t.wait_for("[twm] " + tile, 8), f"Files menu bar missing the {tile} tile"
        # Ctrl+N is intercepted for the focused Files window and delivered as a menu pick
        t.key("ctrl-n", delay=0.12)
        assert t.wait_for("[twm] accel N 0 0", 6), "Ctrl+N accelerator not recognised for Files"
        assert t.wait_for("[files] menu 0 0", 6), "the accelerator did not deliver WEV_MENU(File,New Folder)"
        # prove the action ran: the new folder is on disk, visible from the terminal
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("ls /Users/user")
        assert t.wait_for("newfolder", 6), "File > New Folder did not create the folder on disk"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_term_menu(uefi):
    # The terminal is a raw-syscall app (not the ui:: toolkit), but it declares a
    # menu the same way via SYS_WIN_SETMENU: an Edit menu with Copy/Paste/Clear (no
    # Ctrl accelerators, so the shell keeps ^C). Open the menu and pick Clear; the
    # pick arrives as a WEV_MENU the terminal handles ("[term] menu 2").
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        m = None
        for _ in range(50):
            m = re.search(r"\[twm\] appmenu 0 Edit (\d+) (\d+)", t.serial())
            if m:
                break
            time.sleep(0.1)
        assert m, "the terminal's Edit menu tile was not shown in the bar"
        ex, ew = int(m.group(1)), int(m.group(2))
        t.click(ex + ew // 2, 12)                        # open the Edit menu
        assert t.wait_for("[twm] menu app Edit", 6), "the Edit menu did not open"
        g = re.search(r"\[twm\] menu app Edit y (\d+) row (\d+) x (\d+)", t.serial())
        assert g, "Edit menu geometry not reported"
        ry, row, mx = int(g.group(1)), int(g.group(2)), int(g.group(3))
        t.click(mx + 20, ry + row * 2 + row // 2)        # choose "Clear" (item index 2)
        assert t.wait_for("[term] menu 2", 6), "Edit > Clear was not delivered to the terminal (WEV_MENU)"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_fullscreen(uefi):
    # Fullscreen (Super+F) fills the whole screen and auto-hides BOTH the menu bar
    # and the window's own title bar; a top-edge hover reveals them together as one
    # group, and diving into the content retracts them. Super+F again restores the
    # floating window. (NEXT_STEPS: maximize hides both bars + hover-reveal; design/ui.md.)
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        t.key("meta_l-f", delay=0.1)                   # Super+F -> fullscreen
        assert t.wait_for("[twm] fullscreen Terminal 1", 6), "Super+F did not fullscreen"
        assert t.wait_for("[twm] topbar hidden", 6), "the top group did not auto-hide in fullscreen"
        t.mouse_to(400, 1)                             # top-edge hover -> reveal both bars together
        assert t.wait_for("[twm] topbar shown", 6), "top-edge hover did not reveal the top group"
        t.mouse_to(400, 320)                           # dive into the content -> retract
        assert _count_at_least(t, "[twm] topbar hidden", 2, 6), "the top group did not retract on dive into content"
        t.key("meta_l-f", delay=0.1)                   # Super+F again -> restore
        assert t.wait_for("[twm] fullscreen Terminal 0", 6), "Super+F did not restore the window"
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


def t_launchers_exclusive(uefi):
    # The transient launchers (Spotlight / Launchpad / Clipboard) are a single-
    # instance group: summoning one dismisses the others, so you can never stack
    # Spotlight over the clipboard. (NEXT_STEPS: launchers mutually exclusive.)
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-v", delay=0.1)                  # Super+V -> Clipboard
        assert t.wait_for("[clipboard] up", 8), "Super+V did not open the clipboard"
        assert t.wait_for("[twm] focus Clipboard", 6), "clipboard popup did not focus"
        t.key("meta_l-spc", delay=0.1)               # Super+Space -> Spotlight
        assert t.wait_for("[spotlight] up", 8), "Super+Space did not open Spotlight"
        # summoning Spotlight closed the still-open clipboard (mutual exclusivity)
        assert t.wait_for("[twm] unmap Clipboard", 6), "Spotlight did not dismiss the open clipboard"
        assert t.wait_for("[twm] focus Spotlight", 6), "Spotlight did not take focus"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_notif_click_routing(uefi):
    # A notification declares a target app (notify_to); clicking the toast opens or
    # focuses that app. Here the shell posts a notification routed to the not-yet-
    # running Files app; clicking the toast launches it. (NEXT_STEPS: notification
    # click routing.)
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        t.line("notify Files open the file manager")   # toast titled "Files", routed to Files
        assert t.wait_for("[twm] toast at", 8), "notify did not raise a toast"
        m = re.search(r"\[twm\] toast at (\d+) (\d+) (\d+) (\d+)", t.serial())
        assert m, "toast coordinates not reported"
        x, y, w, h = (int(g) for g in m.groups())
        t.click(x + 30, y + h // 2)                    # the toast body, left of the X/chevron
        assert t.wait_for("[twm] notif open Files", 6), "toast click did not route to its sender"
        assert t.wait_for("[files] file manager up", 12), "routing did not launch the target app"
        assert t.wait_for("[twm] focus Files", 8), "target app did not take focus"
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
    t_system_ownership,
    # processes / scheduler
    t_sleep, t_fork, t_orphan_reparent, t_app_crash, t_smp,
    # compositor + GUI journeys
    t_gui, t_window_mgmt, t_launchers_exclusive, t_notif_click_routing, t_fullscreen,
    t_app_menu, t_files_menu, t_term_menu, t_alt_tab, t_notepad_edit_save, t_notepad_undo,
    t_notepad_guard, t_file_dialog, t_notepad_session, t_spotlight,
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
