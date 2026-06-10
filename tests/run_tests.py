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
        t.key("ctrl-s", delay=0.1)                    # Save -> a never-saved note asks WHERE (the picker)
        _accept_save_picker(t)                        # accept the suggested ~/Documents/untitled.txt
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
        t.key("ctrl-s", delay=0.12)                   # first save -> picker (never saved); buffer is empty
        _accept_save_picker(t)                        # accept ~/Documents/untitled.txt
        assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (0 bytes)", 8), \
            "undo did not clear the typed run (expected a 0-byte save)"
        t.key("ctrl-y", delay=0.12)                   # redo -> Edit > Redo (menu 1, item 2)
        assert t.wait_for("[notepad] menu 1 2", 8), "Ctrl+Y did not fire Edit > Redo"
        t.key("ctrl-s", delay=0.12)                   # the tab now has a path -> saves directly (no picker)
        assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (8 bytes)", 8), \
            "redo did not restore the typed run (expected an 8-byte save)"
        # prove the restored text is exactly what was typed, read back off disk
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("cat Documents/untitled.txt")
        assert t.wait_for("undoredo", 6), "redo did not restore the original text"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_notepad_wordedit(uefi):
    # Regression: Ctrl+Backspace must word-delete in the editor, NOT fire Notepad's
    # "Close Tab ^W" accelerator. The kernel used to collapse Ctrl+Backspace to the bare
    # ^W control byte (0x17), which the compositor matched as the Close Tab accelerator
    # and closed the window; it now emits ESC[127~ (a CSI sequence forwarded to the app,
    # outside the 1..26 accel range) which the toolkit decodes to a word-delete. We also
    # confirm a real Ctrl+W still fires Close Tab so the accelerator path is intact.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1); assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        t.type("hello world", delay=0.05)
        t.key("ctrl-backspace", delay=0.2)
        assert t.wait_for("[ui] wdel", 6), "Ctrl+Backspace did not word-delete in the editor"
        assert "[twm] accel W" not in t.serial(), "Ctrl+Backspace wrongly matched the ^W accelerator"
        assert "[notepad] guard close" not in t.serial(), "Ctrl+Backspace wrongly raised the close guard"
        # a genuine Ctrl+W still fires Close Tab -- the dirty tab raises the guard
        t.key("ctrl-w", delay=0.2)
        assert t.wait_for("[notepad] guard close 0", 8), "Ctrl+W no longer fires the Close Tab accelerator"
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


def _accept_save_picker(t, start="/Users/user/Documents", name=None):
    """A never-saved note's Save (or close-with-Save) opens the Files app as a *picker
    process* (#11) -- a separate window. Wait for it to come up + take focus, optionally
    replace the pre-selected suggested name, then Enter (the name field has focus; Enter
    submits = Save)."""
    assert t.wait_for("[files] picker save " + start, 12), "Save did not open the Files picker"
    assert t.wait_for("[twm] focus Save As", 8), "the picker window did not take focus"
    if name is not None:
        t.type(name, delay=0.05)            # the suggested name opens selected -> typing replaces it
    t.key("ret", delay=0.15)                # Enter in the name field = Save


def t_notepad_guard(uefi):
    # Unsaved-changes guard (#5), now on tab/window CLOSE -- File > New just opens a
    # fresh tab (no data loss, no guard). Closing a dirty tab (File > Close Tab,
    # item 4) raises the modal ConfirmDialog (Save / Discard / Cancel). We open two
    # dirty tabs (so closing one leaves the window alive) and exercise both the
    # Discard (click) and Save (Enter) paths; Save on a never-saved tab opens the
    # picker to choose a location, then writes + closes once the pick is confirmed.
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
        _accept_save_picker(t)                            # never-saved tab -> picker asks where, then closes
        assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (6 bytes)", 8), \
            "Save path did not write the tab before closing"
        # the note persisted to disk (read it back from the terminal)
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("cat Documents/untitled.txt")
        assert t.wait_for("keepme", 6), "the Save-then-Close path did not persist the text"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_file_picker(uefi):
    # The system Open/Save dialog IS the Files app launched in a picker mode (#11): a
    # separate process that hands the chosen path back over /tmp/.picker-res (notepad's
    # on_tick polls sys_pick_poll). Notepad's File > Save As... launches it; we replace
    # the pre-selected suggested name, Save, and read the file back off disk. A same-name
    # Save As then exercises the overwrite -> Keep Both dedupe -- whose ConfirmDialog now
    # lives in the PICKER window. Finally File > Open... reopens the file through the
    # picker (open mode) to prove that path round-trips too.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1); assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        t.type("viapicker", delay=0.05)                  # 9 bytes into the editor
        # --- Save As "picked.txt" through the picker process ---
        _notepad_file_menu(t, 3)                          # File > Save As...
        assert t.wait_for("[notepad] menu 0 3", 6), "File > Save As was not delivered"
        assert t.wait_for("[files] picker save /Users/user/Documents", 12), \
            "Save As did not launch the Files picker in ~/Documents"
        assert t.wait_for("[twm] focus Save As", 8), "the picker window did not take focus"
        # modality (#11 step 5): the picker is a WIN_MODAL dialog -- a click on the window
        # behind it (the terminal, in the strip left of the centred dialog) is swallowed by
        # the compositor, so focus stays on the picker and nothing behind it activates.
        tr = t.win_rect("Terminal"); pr = t.win_rect("Save As")
        assert tr and pr, "window rects for the modality check were not reported"
        bx, by = tr[0] + 6, pr[1] + 40
        if bx < pr[0] - 8:                                # a real point on the terminal, left of the picker
            mark = len(t.serial())
            t.click(bx, by)
            time.sleep(0.4)
            assert "[twm] focus Terminal" not in t.serial()[mark:], \
                "a click behind the modal picker stole focus from it"
        t.type("picked.txt", delay=0.05)                  # replaces the pre-selected name
        t.key("ret", delay=0.15)                          # Enter in the name field = Save
        assert t.wait_for("[files] picked /Users/user/Documents/picked.txt", 8), \
            "the picker did not report the chosen path"
        assert t.wait_for("[notepad] saved /Users/user/Documents/picked.txt (9 bytes)", 8), \
            "Notepad did not save to the picked path"
        # --- Save As the same name -> overwrite warning -> Keep Both -> "picked (2).txt" ---
        _notepad_file_menu(t, 3)                          # File > Save As... again
        assert t.wait_for("[files] picker save /Users/user/Documents", 12), \
            "the second Save As did not relaunch the picker"
        assert t.wait_for("[twm] focus Save As", 8), "the picker did not take focus"
        pr = t.win_rect("Save As"); assert pr, "picker window rect not reported"
        pwx, pwy = pr[0], pr[1]
        t.key("ret", delay=0.15)                          # Enter on the pre-selected "picked.txt" -> it exists
        d = None
        for _ in range(40):
            d = re.search(r"\[ui\] dlgbtn 1 (\d+) (\d+)", t.serial())   # button 1 = Keep Both (in the picker window)
            if d:
                break
            time.sleep(0.1)
        assert d, "the overwrite warning's Keep Both button was not reported"
        t.click(pwx + int(d.group(1)), pwy + int(d.group(2)))
        assert t.wait_for("[ui] confirm 1", 6), "clicking Keep Both did not register"
        assert t.wait_for("[files] picked /Users/user/Documents/picked (2).txt", 8), \
            "Keep Both did not dedupe the colliding name"
        assert t.wait_for("[notepad] saved /Users/user/Documents/picked (2).txt (9 bytes)", 8), \
            "Notepad did not save the deduped file"
        # --- Open the saved file back through the picker (open mode) ---
        _notepad_file_menu(t, 1)                          # File > Open...
        assert t.wait_for("[files] picker open /Users/user/Documents", 12), \
            "File > Open did not launch the picker in open mode"
        assert t.wait_for("[twm] focus Open", 8), "the open picker did not take focus"
        t.key("down", delay=0.15)                         # row 0 = ".."
        t.key("down", delay=0.15)                         # row 1 = "picked (2).txt" (space sorts before '.')
        t.key("ret", delay=0.15)                          # Enter opens the selected file
        assert t.wait_for("[files] picked /Users/user/Documents/picked (2).txt", 8), \
            "the open picker did not report the chosen file"
        assert t.wait_for("[notepad] opened /Users/user/Documents/picked (2).txt", 8), \
            "Notepad did not open the file the picker returned"
        # prove both saved files persisted, read back through the terminal
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
        t.click(mx + 30, ry + 2 * row + row // 2)        # choose "Save" (File: New/Open/Save/Save As/Close Tab)
        assert t.wait_for("[notepad] menu 0 2", 6), "menu selection not delivered to the app (WEV_MENU)"
        _accept_save_picker(t)                            # an unsaved note's Save opens the picker
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


def _files_menu_open(t, name):
    """Open Files' menu-bar menu `name` (click its tile, not the Ctrl accelerator, which
    races the modifier release). Returns its dropdown geometry (ry, row_h, mx).
    Takes the LAST appmenu canary for the name -- another app (e.g. the Terminal, which
    declares its own Edit menu) may have printed the same tile name at a different x
    while it was focused; the most recent bar repaint is the focused app's. The open
    is confirmed count-based since the geometry line is a substring stale-match trap."""
    ms = None
    for _ in range(50):
        ms = re.findall(r"\[twm\] appmenu \d+ %s (\d+) (\d+)" % name, t.serial())
        if ms:
            break
        time.sleep(0.1)
    assert ms, "Files' %s menu tile was not shown" % name
    fx, fw = int(ms[-1][0]), int(ms[-1][1])
    opens = t.serial().count("[twm] menu app %s" % name)
    t.click(fx + fw // 2, 12)
    assert _count_at_least(t, "[twm] menu app %s" % name, opens + 1, 6), \
        "the %s menu did not open" % name
    gs = re.findall(r"\[twm\] menu app %s y (\d+) row (\d+) x (\d+)" % name, t.serial())
    assert gs, "%s menu geometry not reported" % name
    return int(gs[-1][0]), int(gs[-1][1]), int(gs[-1][2])


def _files_menu_click(t, name, idx):
    """Open Files' menu-bar menu `name` and click item `idx` (a click, not the Ctrl
    accelerator, which races the modifier release -- see the project notes)."""
    ry, row, mx = _files_menu_open(t, name)
    t.click(mx + 20, ry + idx * row + row // 2)


def t_files_breadcrumb(uefi):
    # The location bar (files-app §3): the static path label is now a clickable
    # breadcrumb plus an editable path field. We click the breadcrumb's empty area to
    # open the editable path (Files logs "[files] crumbend <x> <y>" for it + "[files]
    # pathedit" on entry), type a deep path + Enter to navigate, then click the
    # "/Users" ancestor crumb (logged as "[files] crumb <cx> <cy> <path>") to jump up.
    # All driven by click, not the Ctrl+L chord, to avoid the menu-accelerator race.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        assert t.wait_for("[files] cd /Users/user", 8), "Files did not open at home"
        wr = t.win_rect("Files"); assert wr, "Files window rect not reported"
        # --- editable mode: click the breadcrumb empty area, type a deep path, Enter ---
        ce = re.findall(r"\[files\] crumbend (\d+) (\d+)", t.serial())
        assert ce, "the breadcrumb empty-area click target was not reported"
        t.click(wr[0] + int(ce[-1][0]), wr[1] + int(ce[-1][1]))
        assert t.wait_for("[files] pathedit", 6), "clicking the breadcrumb empty area did not open the path editor"
        t.type("/Users/user/Documents", delay=0.03)
        t.key("ret", delay=0.2)
        assert t.wait_for("[files] cd /Users/user/Documents", 8), "the edited path did not navigate"
        # --- click the "/Users" ancestor crumb -> jump up to it ---
        cr = None
        for _ in range(40):
            ms = re.findall(r"\[files\] crumb (\d+) (\d+) /Users[\r\n]", t.serial())
            if ms:
                cr = ms[-1]; break
            time.sleep(0.1)
        assert cr, "the /Users breadcrumb crumb geometry was not reported"
        t.click(wr[0] + int(cr[0]), wr[1] + int(cr[1]))
        # match "/Users" at end-of-line so it doesn't alias the earlier "/Users/user" load
        jumped = False
        for _ in range(80):
            if re.search(r"\[files\] cd /Users[\r\n]", t.serial()):
                jumped = True; break
            time.sleep(0.1)
        assert jumped, "clicking the /Users crumb did not navigate up to /Users"
        # --- click-away dismisses the path editor too (#11), the same as Esc ---
        ce2 = None
        for _ in range(40):
            ms = re.findall(r"\[files\] crumbend (\d+) (\d+)", t.serial())
            if ms:
                ce2 = ms[-1]; break
            time.sleep(0.1)
        assert ce2, "the breadcrumb empty-area target was not re-reported after the jump"
        t.click(wr[0] + int(ce2[0]), wr[1] + int(ce2[1]))
        assert t.wait_for("[files] pathedit", 6), "could not reopen the path editor"
        n_leave = len(re.findall(r"\[files\] pathleave", t.serial()))
        t.click(wr[0] + 250, wr[1] + 250)               # click a list row, well outside the location bar
        left = False
        for _ in range(60):
            if len(re.findall(r"\[files\] pathleave", t.serial())) > n_leave:
                left = True; break
            time.sleep(0.1)
        assert left, "clicking away from the path editor did not dismiss it"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_sort(uefi):
    # Data-driven sort (files-app §2): the View menu drives Sort by Name/Kind/Size +
    # Reversed + Folders First. The comparator itself is unit-tested (t_filesort); here
    # we just confirm the View-menu picks re-sort (Files logs "[files] sort <key> <dir>
    # <ff>"). Driven by menu click, not the accelerator.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        _files_menu_click(t, "Sort", 2)                   # Sort by Size
        assert t.wait_for("[files] sort size asc 1", 8), "Sort > Sort by Size did not apply"
        _files_menu_click(t, "Sort", 3)                   # Reversed
        assert t.wait_for("[files] sort size desc 1", 8), "Sort > Reversed did not flip the direction"
        _files_menu_click(t, "Sort", 0)                   # Sort by Name
        assert t.wait_for("[files] sort name desc 1", 8), "Sort > Sort by Name did not switch the key"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_iconview(uefi):
    # Icon (grid) view + zoom (files-app §1): View ▸ as Icons swaps the list for a
    # wrapping icon grid; Zoom In/Out resize the tiles; as List swaps back. Files logs
    # "[files] view icons|list" and "[files] zoom <level>". Driven by menu click.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        _files_menu_click(t, "View", 0)                   # as Icons
        assert t.wait_for("[files] view icons", 8), "View > as Icons did not switch to the grid"
        _files_menu_click(t, "View", 2)                   # Zoom In
        assert t.wait_for("[files] zoom 2", 8), "View > Zoom In did not enlarge the tiles"
        _files_menu_click(t, "View", 4)                   # Actual Size
        assert t.wait_for("[files] zoom 1", 8), "View > Actual Size did not reset the zoom"
        _files_menu_click(t, "View", 1)                   # as List
        assert t.wait_for("[files] view list", 8), "View > as List did not switch back"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_rename(uefi):
    # In-place rename (files-app §foundation #10): File ▸ New Folder makes a folder and
    # drops straight into a rename field over the new tile (Finder behaviour) with the
    # name pre-selected. Files logs "[files] renaming <name>" on entry; typing replaces
    # the selection and Enter commits, logging "[files] rename <old> -> <new>" and doing
    # the rename_() on disk. We then confirm the new name from the terminal's ls. Driven
    # by menu click (not Ctrl+N) to dodge the accelerator race.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        _files_menu_click(t, "File", 0)                   # File > New Folder
        # New Folder lands on disk and Files drops into the rename field over it.
        nm = None
        for _ in range(60):
            m = re.search(r"\[files\] renaming (\S+)", t.serial())
            if m:
                nm = m.group(1); break
            time.sleep(0.1)
        assert nm, "New Folder did not enter the in-place rename"
        # the name is pre-selected (Ctrl+A); typing replaces it, Enter commits
        t.type("keepers", delay=0.04)
        t.key("ret", delay=0.2)
        assert t.wait_for("[files] rename %s -> keepers" % nm, 8), "the rename did not commit"
        # prove it hit disk: the renamed folder shows from the terminal, the old name is gone
        t.key("alt-tab", delay=0.1)
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("ls /Users/user")
        assert t.wait_for("keepers", 6), "the renamed folder is not on disk"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_viewmem(uefi):
    # Per-folder view memory (files-app §2): each folder remembers its view mode + zoom +
    # sort in the registry; an unvisited folder falls back to the default. Make Home an
    # icon view + Zoom In, navigate Up to /Users (never visited -> default list view), then
    # Back to Home and confirm the icon+zoom view is restored from the registry. Files logs
    # "[files] viewmem <path> <mode> zoom <z>" on every navigation. Driven by menu clicks.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        # Home opens with the built-in default (list view, actual size).
        assert t.wait_for("[files] viewmem /Users/user list zoom 1", 8), "Home did not open in the default list view"
        _files_menu_click(t, "View", 0)                   # as Icons  -> persisted for Home
        assert t.wait_for("[files] view icons", 8), "View > as Icons did not apply"
        _files_menu_click(t, "View", 2)                   # Zoom In    -> persisted for Home
        assert t.wait_for("[files] zoom 2", 8), "View > Zoom In did not apply"
        _files_menu_click(t, "Go", 0)                     # Up to /Users (never visited)
        assert t.wait_for("[files] viewmem /Users list zoom 1", 8), "an unvisited folder did not use the default view"
        _files_menu_click(t, "Go", 1)                     # Back to Home
        assert t.wait_for("[files] viewmem /Users/user icons zoom 2", 8), "Home did not restore its remembered icon+zoom view"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def _dock_focus(t, title):
    """Raise + focus a running app by clicking its (always-on-top) dock icon, then wait
    for a NEW "[twm] focus <title>". Reliable where alt-tab is not: an immediate click
    after alt-tab cancels the switcher before it commits, and a bare wait_for("focus
    <title>") is fooled by an earlier identical line. Caller must be switching FROM
    another window (clicking an already-focused app's icon logs nothing)."""
    xy = t.icon_xy(title); assert xy, "%s dock icon not reported" % title
    c0 = t.serial().count("[twm] focus %s" % title)
    t.click(*xy)
    assert _count_at_least(t, "[twm] focus %s" % title, c0 + 1, 6), "could not focus %s via the dock" % title


def _files_nav(t, wr, path):
    """Navigate Files to `path` via the breadcrumb path editor (the proven-robust route,
    vs. the Ctrl+L chord that races the modifier release). Counts NEW "[files] pathedit"
    / "[files] cd <path>" lines so a re-visit isn't satisfied by an earlier identical
    one (wait_for is a substring search over the whole log).

    The whole open->type->Enter is retried as a unit: under host load either the click
    that opens the editor or the Enter that commits can be dropped, so a single shot is
    flaky. Esc before each retry closes any half-open editor (harmless no-op when the
    editor isn't up), so the next crumbend click re-opens a FRESH field whose select-all
    discards any garbage a dropped keystroke left behind."""
    cdc = t.serial().count("[files] cd %s\r" % path)
    # the breadcrumb's geometry canary can lag the window's first paint under host load;
    # wait for it rather than asserting on the first glance (keeps the nav robust when busy)
    deadline = time.time() + 8
    while time.time() < deadline and not re.search(r"\[files\] crumbend \d+ \d+", t.serial()):
        time.sleep(0.1)
    for attempt in range(3):
        if attempt:
            t.key("esc", delay=0.2)                       # close a stuck editor before reopening
        pe = t.serial().count("[files] pathedit")
        opened = False
        for _ in range(3):
            ms = re.findall(r"\[files\] crumbend (\d+) (\d+)", t.serial())
            assert ms, "the breadcrumb empty-area target was not reported"
            t.click(wr[0] + int(ms[-1][0]), wr[1] + int(ms[-1][1]))
            if _count_at_least(t, "[files] pathedit", pe + 1, 4):
                opened = True
                break
        if not opened:
            continue
        t.type(path, delay=0.03)
        t.key("ret", delay=0.25)
        if _count_at_least(t, "[files] cd %s\r" % path, cdc + 1, 8):
            return
    assert False, "Files did not navigate to %s" % path


def _files_row_xy(t, wr, row):
    """Absolute (x, y) of list `row`'s centre, from the latest "[files] listrect x y w
    rowh" canary. Row 0 is the synthetic ".." up-entry when not at the volume root."""
    ms = re.findall(r"\[files\] listrect (\d+) (\d+) (\d+) (\d+)", t.serial())
    assert ms, "the Files list-rect canary was not reported"
    lx, ly, lw, rh = (int(v) for v in ms[-1])
    return wr[0] + lx + lw // 2, wr[1] + ly + row * rh + rh // 2


def _files_ctx_click(t, wr, idx, before):
    """After a right-click has opened the context menu (its canary count must exceed
    `before`), click item `idx`. The "[files] ctxmenu px py rowh n" canary reports the
    menu's placed (clamped) origin, so the click lands regardless of edge clamping. Only
    valid for items before any separator (true for Delete / Put Back here)."""
    assert _count_at_least(t, "[files] ctxmenu", before + 1, 6), "the context menu did not open"
    ms = re.findall(r"\[files\] ctxmenu (\d+) (\d+) (\d+) (\d+)", t.serial())
    px, py, rh, _n = (int(v) for v in ms[-1])
    t.click(wr[0] + px + 14, wr[1] + py + 5 + idx * rh + rh // 2)


def _files_pane2_row_xy(t, wr, row):
    """Absolute (x, y) of the second (split) pane's `row` centre, from the latest
    "[files] listrect2 x y w rowh" canary. Row 0 is its ".." up-entry."""
    ms = re.findall(r"\[files\] listrect2 (\d+) (\d+) (\d+) (\d+)", t.serial())
    assert ms, "the Files pane-2 list-rect canary was not reported"
    lx, ly, lw, rh = (int(v) for v in ms[-1])
    return wr[0] + lx + lw // 2, wr[1] + ly + row * rh + rh // 2


def _files_hdr(t):
    """Latest details-view column-header geometry, from "[files] hdr x y h x0 w0 x1 w1 x2
    w2 x3 w3" (client coords + per-column offset/width). Returns (hx, hy, hh, [(cx,cw)*4])."""
    ms = re.findall(r"\[files\] hdr (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)", t.serial())
    assert ms, "the Files column-header geometry canary was not reported"
    v = [int(x) for x in ms[-1]]
    cols = [(v[3 + 2 * c], v[4 + 2 * c]) for c in range(4)]
    return v[0], v[1], v[2], cols


def _files_hdr_click(t, wr, col):
    """Click the centre of details-view header column `col` (0=Name 1=Kind 2=Size 3=Date)
    -- well clear of the resize dividers at the column edges -- to sort by it."""
    hx, hy, hh, cols = _files_hdr(t)
    cx, cw = cols[col]
    t.click(wr[0] + hx + cx + cw // 2, wr[1] + hy + hh // 2)


def _files_tab_click(t, wr, i, close=False):
    """Click tab `i`'s pill body (or its × when close=True) on the Files tab strip, using
    the latest "[files] tabbar y h n cur" + "[files] tabpos i x w" geometry canaries. The
    px/w are window-relative; wr is the Files window rect."""
    bar = re.findall(r"\[files\] tabbar (\d+) (\d+) (\d+) (\d+)", t.serial())
    assert bar, "the tab strip geometry (tabbar) was not reported"
    by, bh = int(bar[-1][0]), int(bar[-1][1])
    pos = {int(a): (int(x), int(w)) for a, x, w in re.findall(r"\[files\] tabpos (\d+) (\d+) (\d+)", t.serial())}
    assert i in pos, "tab %d position not reported" % i
    px, pw = pos[i]
    cx = wr[0] + px + (pw - 8 if close else 12)
    t.click(cx, wr[1] + by + bh // 2)


def t_files_trash(uefi):
    # Trash (files-app §9): Delete in a normal folder MOVES the item to ~/.Trash (a
    # rename, recorded in a .trashinfo sidecar) instead of destroying it; "Put Back"
    # restores it to where it came from; "Empty Trash" removes for good. The sidecar
    # codec is unit-tested (t_trashinfo); here we drive the real round-trip through the
    # UI and confirm each move on disk from the terminal. Context-menu rows are clicked
    # via the "[files] ctxmenu" geometry canary, list rows via "[files] listrect".
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        # stage a deterministic single-item folder from the shell (trashme -> row 1)
        t.line("mkdir /Users/user/stage")
        t.line("mkdir /Users/user/stage/trashme")
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        wr = t.win_rect("Files"); assert wr, "Files window rect not reported"
        _files_nav(t, wr, "/Users/user/stage")
        # --- Delete -> move to Trash (right-click the trashme row, pick Delete) ---
        c_ctx = t.serial().count("[files] ctxmenu")
        rx, ry = _files_row_xy(t, wr, 1)
        t.rightclick(rx, ry)
        _files_ctx_click(t, wr, 4, c_ctx)                 # dir menu: Open(0) Open-in-New-Tab(1) Duplicate(2) Rename(3) Delete(4)
        assert t.wait_for("[files] trash trashme", 8), "Delete did not move trashme to the Trash"
        # on disk: it physically moved into ~/.Trash. (The "[files] trash" canary alone
        # can't prove this -- move_to_trash logs it even on the rmrf fallback -- so we
        # confirm from the shell, checking the ls output TAIL since "trashme" already
        # appears earlier in the serial via the canaries.)
        _dock_focus(t, "Terminal")
        mark = len(t.serial())
        t.line("ls /Users/user/.Trash"); t.line("echo TRASH-A")
        assert t.wait_for("TRASH-A", 6), "the trash listing did not complete"
        assert "trashme" in t.serial()[mark:], "trashme is not in ~/.Trash on disk"
        # --- Put Back -> restore it to where it came from ---
        _dock_focus(t, "Files")
        _files_nav(t, wr, "/Users/user/.Trash")
        c_ctx = t.serial().count("[files] ctxmenu")
        rx, ry = _files_row_xy(t, wr, 1)
        t.rightclick(rx, ry)
        _files_ctx_click(t, wr, 0, c_ctx)                 # trash menu: Put Back(0) Delete Immediately(1)
        assert t.wait_for("[files] untrash trashme", 8), "Put Back did not restore trashme"
        # on disk: it physically moved back into the stage folder
        _dock_focus(t, "Terminal")
        mark = len(t.serial())
        t.line("ls /Users/user/stage"); t.line("echo STAGE-A")
        assert t.wait_for("STAGE-A", 6), "the stage listing did not complete"
        assert "trashme" in t.serial()[mark:], "trashme was not restored to its origin on disk"
        # --- trash it again, then Empty Trash (File menu) clears it for good ---
        _dock_focus(t, "Files")
        _files_nav(t, wr, "/Users/user/stage")
        c_trash = t.serial().count("[files] trash trashme")
        c_ctx = t.serial().count("[files] ctxmenu")
        rx, ry = _files_row_xy(t, wr, 1)
        t.rightclick(rx, ry)
        _files_ctx_click(t, wr, 4, c_ctx)                 # Delete again (Open0 Open-in-New-Tab1 Duplicate2 Rename3 Delete4)
        assert _count_at_least(t, "[files] trash trashme", c_trash + 1, 8), "re-Delete did not re-trash trashme"
        _files_menu_click(t, "File", 3)                   # File > Empty Trash (after New Folder/New File/Refresh)
        assert t.wait_for("[files] trash empty", 8), "Empty Trash did not run"
        # on disk: the Trash is now empty
        _dock_focus(t, "Terminal")
        et_mark = len(t.serial())
        t.line("ls /Users/user/.Trash")
        t.line("echo TRASH-CHECKED")
        assert t.wait_for("TRASH-CHECKED", 6), "the trash listing did not complete"
        assert "trashme" not in t.serial()[et_mark:], "Empty Trash left trashme on disk"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_newdup(uefi):
    # New File + Duplicate (files-app §12). New File drops an empty "newfile.txt" here and
    # enters rename (like New Folder); Duplicate clones the selected item beside itself as
    # "<name> copy" -- files copy their bytes, folders copy recursively (copy_tree). The
    # "X copy" name math is unit-tested (t_dupname); here we drive the real ops through the
    # UI and confirm each one on disk from the terminal. Deterministic single-item staged
    # folders make the target row 1; the shell's `ls <path>` keeps the whole rest of the
    # line as the path, so a duplicated "box copy" with a space is checkable directly.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        # stage two empty folders + a nested tree (box/sub) for the recursion check
        t.line("mkdir /Users/user/dup")
        t.line("mkdir /Users/user/dup2")
        t.line("mkdir /Users/user/dup2/box")
        t.line("mkdir /Users/user/dup2/box/sub")
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        wr = t.win_rect("Files"); assert wr, "Files window rect not reported"
        # --- New File: File > New File creates newfile.txt + enters rename; Esc keeps it ---
        _files_nav(t, wr, "/Users/user/dup")
        _files_menu_click(t, "File", 1)                   # File > New File
        assert t.wait_for("[files] renaming newfile.txt", 8), "New File did not create + name newfile.txt"
        t.key("esc", delay=0.2)                           # keep the default name
        # on disk: the empty file is really there
        _dock_focus(t, "Terminal")
        mark = len(t.serial())
        t.line("ls /Users/user/dup"); t.line("echo NEWFILE-A")
        assert t.wait_for("NEWFILE-A", 6), "the dup listing did not complete"
        assert "newfile.txt" in t.serial()[mark:], "New File did not land on disk"
        # --- Duplicate a file: right-click newfile.txt (row 1), pick Duplicate ---
        _dock_focus(t, "Files")
        c_ctx = t.serial().count("[files] ctxmenu")
        rx, ry = _files_row_xy(t, wr, 1)
        t.rightclick(rx, ry)
        _files_ctx_click(t, wr, 4, c_ctx)                 # file menu: Open(0) OpenWith(1) Copy(2) Cut(3) Duplicate(4)
        assert t.wait_for("[files] duplicate newfile copy.txt", 8), "Duplicate did not clone the file as 'newfile copy.txt'"
        # on disk: both the original and the copy exist
        _dock_focus(t, "Terminal")
        mark = len(t.serial())
        t.line("ls /Users/user/dup"); t.line("echo NEWFILE-B")
        assert t.wait_for("NEWFILE-B", 6), "the post-duplicate listing did not complete"
        tail = t.serial()[mark:]
        assert "newfile copy.txt" in tail, "the duplicated file is not on disk"
        # --- Duplicate a folder: copy_tree recurses (box/sub -> 'box copy'/sub) ---
        _dock_focus(t, "Files")
        _files_nav(t, wr, "/Users/user/dup2")
        c_ctx = t.serial().count("[files] ctxmenu")
        rx, ry = _files_row_xy(t, wr, 1)
        t.rightclick(rx, ry)
        _files_ctx_click(t, wr, 2, c_ctx)                 # folder menu: Open(0) Open-in-New-Tab(1) Duplicate(2)
        assert t.wait_for("[files] duplicate box copy", 8), "Duplicate did not clone the folder as 'box copy'"
        # on disk: the recursive copy reproduced the child directory inside 'box copy'
        _dock_focus(t, "Terminal")
        mark = len(t.serial())
        t.line("ls /Users/user/dup2/box copy"); t.line("echo BOXCOPY")
        assert t.wait_for("BOXCOPY", 6), "the box-copy listing did not complete"
        assert "sub" in t.serial()[mark:], "copy_tree did not recurse into the folder's child"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_getinfo(uefi):
    # Rich Get Info / Properties (§8): selecting an item fills the Details pane with a
    # folder's *recursive* size + item count (a du-style walk), the owning identity, a
    # read-only lock for system-owned items, and the default app for a file's type.
    # select_row emits "[files] sel <name> (ro|rw) owner=<uid> size=<n> [items=<n>]";
    # a folder pick walks the tree, a system item reads owner=0/ro. A screenshot captures
    # the rendered pane (Size "N, 3 items", Owner: You, Where).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        # stage a deterministic tree under a fresh parent: gi/tree/{a.txt, sub/b.txt}
        t.line("mkdir /Users/user/gi")
        t.line("mkdir /Users/user/gi/tree")
        t.line("mkdir /Users/user/gi/tree/sub")
        t.line("write /Users/user/gi/tree/a.txt"); t.line("hello get info")
        assert t.wait_for("saved /Users/user/gi/tree/a.txt", 6), "could not stage a.txt"
        t.line("write /Users/user/gi/tree/sub/b.txt"); t.line("a nested file body")
        assert t.wait_for("saved /Users/user/gi/tree/sub/b.txt", 6), "could not stage b.txt"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        wr = t.win_rect("Files"); assert wr, "Files window rect not reported"
        # --- folder Get Info: recursive size + 3 descendants, owned by the user ---
        _files_nav(t, wr, "/Users/user/gi")
        before = t.serial().count("[files] sel ")
        rx, ry = _files_row_xy(t, wr, 1)                  # row 0 = "..", row 1 = "tree"
        t.click(rx, ry)
        assert _count_at_least(t, "[files] sel ", before + 1, 6), "selecting the folder reported no Get Info"
        m = None
        for _ in range(40):
            m = re.search(r"\[files\] sel tree rw owner=1 size=(\d+) items=(\d+)", t.serial())
            if m:
                break
            time.sleep(0.1)
        assert m, "the folder's recursive Get Info (rw owner=1 size/items) was not reported"
        assert int(m.group(2)) == 3, "recursive item count should be 3 (a.txt, sub, sub/b.txt), got %s" % m.group(2)
        assert int(m.group(1)) > 0, "recursive folder size should be non-zero"
        t.screenshot("/tmp/tos_getinfo.ppm")              # the pane shows Size "N, 3 items" + Owner
        # --- a system-owned item reads read-only (owner 0 -> the lock badge) ---
        _files_nav(t, wr, "/")
        rx, ry = _files_row_xy(t, wr, 0)                  # volume root has no ".."; row 0 = first entry (Apps)
        t.click(rx, ry)
        m = None
        for _ in range(40):
            m = re.search(r"\[files\] sel \S+ ro owner=0 ", t.serial())
            if m:
                break
            time.sleep(0.1)
        assert m, "a system-owned item did not report read-only / owner=System"
        t.screenshot("/tmp/tos_getinfo_locked.ppm")       # the "Read only" lock badge under the name
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_tabs(uefi):
    # Tabs (§4): one window, several folders. Each tab keeps its own folder + history +
    # selection; New Tab (File menu) adds one, clicking a pill switches (restoring that
    # tab's folder), "Open in New Tab" (folder context menu) spawns one at that folder,
    # and a pill's × closes it. Canaries: "[files] tab new/sel/close ..." + "[files]
    # tabbar y h n cur". The strip is hidden with a single tab. Screenshot the strip.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        t.line("mkdir /Users/user/ta")
        t.line("mkdir /Users/user/tb")
        t.line("mkdir /Users/user/tp")
        t.line("mkdir /Users/user/tp/child")
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        wr = t.win_rect("Files"); assert wr, "Files window rect not reported"
        assert "[files] tabbar" not in t.serial(), "the tab strip showed with a single tab"
        # tab 0 -> /Users/user/ta
        _files_nav(t, wr, "/Users/user/ta")
        # New Tab (File menu item 4) opens a second tab (at the current folder)
        nc = t.serial().count("[files] tab new")
        _files_menu_click(t, "File", 4)
        assert _count_at_least(t, "[files] tab new", nc + 1, 6), "New Tab did not open a second tab"
        assert t.wait_for("[files] tabbar", 6), "the tab strip did not appear with 2 tabs"
        # drive the 2nd (now-active) tab to a different folder
        _files_nav(t, wr, "/Users/user/tb")
        # switch back to tab 0 -> it must restore /Users/user/ta (its own folder)
        cd_a = t.serial().count("[files] cd /Users/user/ta\r")
        _files_tab_click(t, wr, 0)
        assert t.wait_for("[files] tab sel 0", 4), "tab 0 selection was not reported"
        assert _count_at_least(t, "[files] cd /Users/user/ta\r", cd_a + 1, 6), \
            "switching to tab 0 did not restore its folder"
        t.screenshot("/tmp/tos_tabs.ppm")                 # two pills under the location bar
        # switch to tab 1 -> /Users/user/tb
        cd_b = t.serial().count("[files] cd /Users/user/tb\r")
        _files_tab_click(t, wr, 1)
        assert _count_at_least(t, "[files] cd /Users/user/tb\r", cd_b + 1, 6), \
            "switching to tab 1 did not restore its folder"
        # Open in New Tab: drive tab 1 to /Users/user/tp (only "child" inside), right-click it
        _files_nav(t, wr, "/Users/user/tp")
        c_ctx = t.serial().count("[files] ctxmenu")
        rx, ry = _files_row_xy(t, wr, 1)                  # row 0 = "..", row 1 = "child"
        t.rightclick(rx, ry)
        _files_ctx_click(t, wr, 1, c_ctx)                 # folder menu: Open(0) Open-in-New-Tab(1)
        assert t.wait_for("[files] tab new /Users/user/tp/child", 6), \
            "Open in New Tab did not spawn a tab at the folder"
        bar = re.findall(r"\[files\] tabbar \d+ \d+ (\d+) \d+", t.serial())
        assert bar and int(bar[-1]) == 3, "the third tab did not register (n=%s)" % (bar[-1] if bar else "none")
        # close tab 1 via its × -> back to 2 tabs
        cl = t.serial().count("[files] tab close")
        _files_tab_click(t, wr, 1, close=True)
        assert _count_at_least(t, "[files] tab close", cl + 1, 6), "the pill × did not close the tab"
        bar = re.findall(r"\[files\] tabbar \d+ \d+ (\d+) \d+", t.serial())
        assert bar and int(bar[-1]) == 2, "closing a tab did not drop the count to 2"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_split(uefi):
    # Split / dual-pane view (§4): View ▸ Split View shows a second pane beside the first,
    # each with its own folder. The second pane is navigable (.. + double-click a folder);
    # "Copy to Other Pane" sends the active pane's selection into the other pane's folder.
    # Canaries: "[files] split 1", "[files] pane2 cd <path>", "[files] copy-across <name> ->
    # <dstdir>". Screenshot the two panes.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        # a deterministic parent sp/{src,dst}; src holds the file we'll copy across
        t.line("mkdir /Users/user/sp")
        t.line("mkdir /Users/user/sp/src")
        t.line("mkdir /Users/user/sp/dst")
        t.line("write /Users/user/sp/src/movefile.txt"); t.line("payload across panes")
        assert t.wait_for("saved /Users/user/sp/src/movefile.txt", 6), "could not stage movefile.txt"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        wr = t.win_rect("Files"); assert wr, "Files window rect not reported"
        # primary pane -> sp/src
        _files_nav(t, wr, "/Users/user/sp/src")
        # turn on Split View (View menu item 5); pane 2 opens at the same folder
        _files_menu_click(t, "View", 5)
        assert t.wait_for("[files] split 1", 6), "Split View did not turn on"
        assert t.wait_for("[files] pane2 cd /Users/user/sp/src", 6), "pane 2 did not open at the current folder"
        # drive pane 2 to sp/dst: double-click ".." (row 0) up to sp, then "dst" (row 1)
        p2 = t.serial().count("[files] pane2 cd /Users/user/sp\r")
        rx, ry = _files_pane2_row_xy(t, wr, 0)
        t.doubleclick(rx, ry)
        assert _count_at_least(t, "[files] pane2 cd /Users/user/sp\r", p2 + 1, 6), "pane 2 did not go up to sp"
        rx, ry = _files_pane2_row_xy(t, wr, 1)            # sp: row0=".." row1="dst" (dst<src)
        t.doubleclick(rx, ry)
        assert t.wait_for("[files] pane2 cd /Users/user/sp/dst", 6), "pane 2 did not enter dst"
        t.screenshot("/tmp/tos_split.ppm")               # two panes, primary=src active, pane2=dst
        # right-click the file in the PRIMARY pane, Copy to Other Pane (sends it into dst)
        c_ctx = t.serial().count("[files] ctxmenu")
        rx, ry = _files_row_xy(t, wr, 1)                 # primary src: row0=".." row1="movefile.txt"
        t.rightclick(rx, ry)
        # file menu in split: Open0 OpenWith1 Copy2 Cut3 Duplicate4 Rename5 Delete6 Copy-to-Other-Pane7
        _files_ctx_click(t, wr, 7, c_ctx)
        assert t.wait_for("[files] copy-across movefile.txt -> /Users/user/sp/dst", 8), \
            "Copy to Other Pane did not copy the file across"
        # confirm on disk
        _dock_focus(t, "Terminal")
        mark = len(t.serial())
        t.line("ls /Users/user/sp/dst"); t.line("echo SPLITDONE")
        assert t.wait_for("SPLITDONE", 6), "the dst listing did not complete"
        assert "movefile.txt" in t.serial()[mark:], "the copied file did not land in the other pane's folder"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_details(uefi):
    # Details / column view (§1): the list mode grows a header row -- Name | Kind | Size |
    # Date Modified -- whose cells sort on click (toggling asc/desc, with a caret) and are
    # resizable. We stage a folder with files of differing kind/size, open Files (list mode is
    # the default, so the header is up), screenshot the aligned columns, then drive sorting by
    # clicking the Size / Date / Name headers. Canaries: "[files] hdr ...", "[files] sort ...".
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        t.line("mkdir /Users/user/dv")
        t.line("write /Users/user/dv/alpha.txt"); t.line("a")
        assert t.wait_for("saved /Users/user/dv/alpha.txt", 6), "could not stage alpha.txt"
        t.line("write /Users/user/dv/beta.md"); t.line("a much longer line of bytes here")
        assert t.wait_for("saved /Users/user/dv/beta.md", 6), "could not stage beta.md"
        t.line("mkdir /Users/user/dv/zsub")
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        wr = t.win_rect("Files"); assert wr, "Files window rect not reported"
        _files_nav(t, wr, "/Users/user/dv")
        # the details header is up in list mode -> its geometry canary must report four columns
        hx, hy, hh, cols = _files_hdr(t)
        assert cols[0][1] > cols[1][1], "Name should be the widest column by default"
        assert all(cw > 0 for _, cw in cols), "every column has a positive width"
        t.screenshot("/tmp/tos_details.ppm")             # Name/Kind/Size/Date columns + a caret
        # click the Size header -> sort by size ascending
        s = t.serial().count("[files] sort size asc")
        _files_hdr_click(t, wr, 2)
        assert _count_at_least(t, "[files] sort size asc", s + 1, 6), "Size header did not sort by size asc"
        # click it again -> the active column toggles to descending
        s = t.serial().count("[files] sort size desc")
        _files_hdr_click(t, wr, 2)
        assert _count_at_least(t, "[files] sort size desc", s + 1, 6), "re-clicking Size did not flip to desc"
        # Date header -> sort by date (a fresh key resets to ascending)
        s = t.serial().count("[files] sort date asc")
        _files_hdr_click(t, wr, 3)
        assert _count_at_least(t, "[files] sort date asc", s + 1, 6), "Date header did not sort by date"
        # Name header -> back to name ascending
        s = t.serial().count("[files] sort name asc")
        _files_hdr_click(t, wr, 0)
        assert _count_at_least(t, "[files] sort name asc", s + 1, 6), "Name header did not sort by name"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_files_undo(uefi):
    # Undo / redo of file ops (§12): a journal records each reversible op; Edit ▸ Undo
    # (item 8 / Ctrl+Z) inverts the last one, Edit ▸ Redo (item 9 / Ctrl+Y) re-applies it.
    # We drive Duplicate (undo deletes the dup, redo re-copies) and Trash (undo puts it
    # back), confirming each on disk. OP types in the canaries: RENAME0 MOVE1 CREATE2 COPY3
    # TRASH4. Canaries: "[files] duplicate/trash ...", "[files] undo/redo <type> ...".
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        t.line("mkdir /Users/user/ud")
        t.line("write /Users/user/ud/doc.txt"); t.line("hello")
        assert t.wait_for("saved /Users/user/ud/doc.txt", 6), "could not stage doc.txt"
        xy = t.icon_xy("Files"); assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        wr = t.win_rect("Files"); assert wr, "Files window rect not reported"
        _files_nav(t, wr, "/Users/user/ud")
        # --- Duplicate doc.txt, then undo (remove the dup), then redo (re-create it) ---
        rx, ry = _files_row_xy(t, wr, 1)                  # row0=".." row1="doc.txt"
        t.click(rx, ry)
        d = t.serial().count("[files] duplicate")
        _files_menu_click(t, "Edit", 3)                   # Edit ▸ Duplicate
        assert _count_at_least(t, "[files] duplicate", d + 1, 6), "Duplicate did not run"
        # the Edit menu now shows Undo enabled -- screenshot it open (visible proof of §12)
        ry0, row, mx = _files_menu_open(t, "Edit")
        t.screenshot("/tmp/tos_undo.ppm")
        u = t.serial().count("[files] undo 3")
        t.click(mx + 20, ry0 + 8 * row + row // 2)        # click Undo (item 8) on the open menu
        assert _count_at_least(t, "[files] undo 3", u + 1, 6), "Undo of Duplicate (OP_COPY) did not fire"
        rdo = t.serial().count("[files] redo 3")
        _files_menu_click(t, "Edit", 9)                   # Edit ▸ Redo -> re-copy the dup
        assert _count_at_least(t, "[files] redo 3", rdo + 1, 6), "Redo of Duplicate did not fire"
        # --- Trash the dup, then undo (put it back) ---
        rx, ry = _files_row_xy(t, wr, 1)                  # row1 = "doc copy.txt" (space < '.', sorts first)
        t.click(rx, ry)
        tr = t.serial().count("[files] trash")
        _files_menu_click(t, "Edit", 4)                   # Edit ▸ Delete -> move to Trash
        assert _count_at_least(t, "[files] trash", tr + 1, 6), "Delete did not move to Trash"
        un = t.serial().count("[files] undo 4")
        _files_menu_click(t, "Edit", 8)                   # Edit ▸ Undo -> un-trash
        assert _count_at_least(t, "[files] undo 4", un + 1, 6), "Undo of Trash (OP_TRASH) did not fire"
        # confirm on disk: both doc.txt and the (undeleted) dup are back in the folder
        _dock_focus(t, "Terminal")
        mark = len(t.serial())
        t.line("ls /Users/user/ud"); t.line("echo UNDODONE")
        assert t.wait_for("UNDODONE", 6), "the listing did not complete"
        out = t.serial()[mark:]
        assert "doc.txt" in out and "copy" in out, "the original + undeleted duplicate are not both present"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_statfs(uefi):
    # Free-space query (files-app §6/§7): SYS_STATFS reports the mounted volume's data
    # capacity + free bytes from the sector bitmap. The shell's `df` surfaces it (and the
    # Files status bar shows "<n> free"); the human_bytes formatter is unit-tested
    # (t_humansize). Here we confirm the syscall returns sane figures end-to-end, and that
    # free shrinks after we write a file.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        mark = len(t.serial())
        t.line("df"); t.line("echo DF-A")
        assert t.wait_for("DF-A", 6), "df did not complete"
        out = t.serial()[mark:]
        assert "tosfs" in out, "df did not name the filesystem"
        m = re.search(r"Size: ([0-9.]+) (KB|MB|GB)\s+Used: ([0-9.]+) (KB|MB|GB)\s+Free: ([0-9.]+) (KB|MB|GB)", out)
        assert m, "df did not report Size/Used/Free figures (%r)" % out[-200:]
        size, free = float(m.group(1)), float(m.group(5))
        assert size > 0 and free > 0, "df reported non-positive size/free"
        # write a file, then free must not be larger than before (it dropped or held)
        free_before = m.group(5) + " " + m.group(6)
        t.line("write /Users/user/blob")              # prompts, then saves the next line
        t.line("the quick brown fox jumps over the lazy dog over and over again")
        assert t.wait_for("saved /Users/user/blob", 6), "write did not save the test file"
        mark2 = len(t.serial())
        t.line("df"); t.line("echo DF-B")
        assert t.wait_for("DF-B", 6), "second df did not complete"
        out2 = t.serial()[mark2:]
        m2 = re.search(r"Free: ([0-9.]+) (KB|MB|GB)", out2)
        assert m2, "second df did not report Free"
        # a sector got consumed, so free is <= the earlier figure (same or one sector less)
        assert (m2.group(2) != m.group(6)) or (float(m2.group(1)) <= free + 0.001), \
            "free space grew after writing a file (was %s, now %s %s)" % (free_before, m2.group(1), m2.group(2))
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
    t_system_ownership, t_statfs,
    # processes / scheduler
    t_sleep, t_fork, t_orphan_reparent, t_app_crash, t_smp,
    # compositor + GUI journeys
    t_gui, t_window_mgmt, t_launchers_exclusive, t_notif_click_routing, t_fullscreen,
    t_app_menu, t_files_menu, t_files_breadcrumb, t_files_sort, t_files_iconview, t_files_rename, t_files_viewmem, t_files_trash, t_files_newdup, t_files_getinfo, t_files_tabs, t_files_split, t_files_details, t_files_undo, t_term_menu, t_alt_tab, t_notepad_edit_save, t_notepad_undo,
    t_notepad_guard, t_file_picker, t_notepad_wordedit, t_notepad_session, t_spotlight,
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
    # Optional positional filter: any bare args name the tests to run (substring
    # match against the function name), e.g. `run_tests.py t_file_picker`. Without
    # a filter the full suite runs as before.
    names = [a for a in sys.argv[1:] if not a.startswith("-")]
    def _sel(tests):
        return [t for t in tests if any(n in t.__name__ for n in names)] if names else tests
    bios_tests, uefi_tests = _sel(BIOS_TESTS), _sel(UEFI_TESTS)
    total_pass = total_fail = 0
    if do_bios and bios_tests:
        p, f = run("bios", bios_tests, uefi=False)
        total_pass += p; total_fail += f
    if do_uefi and uefi_tests:
        p, f = run("uefi", uefi_tests, uefi=True)
        total_pass += p; total_fail += f
    print(f"\n{total_pass} passed, {total_fail} failed")
    sys.exit(1 if total_fail else 0)


if __name__ == "__main__":
    main()
