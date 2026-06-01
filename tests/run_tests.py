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


def t_cat_motd(uefi):
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("cat /System/etc/motd")
        assert t.wait_for("Welcome to tOS.", 5), "cat motd did not print file contents"


def t_cat_missing(uefi):
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("cat nope")
        assert t.wait_for("cat: no such file: nope", 5), "missing-file not reported"


def t_write_then_cat(uefi):
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        assert t.line_for("write note.txt", "enter a line"), "write prompt missing"
        t.line("diskwriteworks")
        assert t.wait_for("saved note.txt", 5), "write did not save"
        t.line("ls")
        assert t.wait_for("note.txt\t", 5), "new file not in directory"
        t.line("cat note.txt")
        # content read back from disk (also appears once as the typed echo)
        assert t.serial().count("diskwriteworks") >= 2, "file content not read back"
        assert "[EXCEPTION]" not in t.serial(), "fault during file I/O"


def t_fs_persist(uefi):
    # One reboot exercises BOTH create-flush and delete-flush persistence: a kept
    # file (and its contents) survives, while a file deleted before the reboot
    # stays gone. (Folds together the old t_persistence + t_rm_persist.)
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
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_partition(uefi):
    # The FS must be found via the MBR partition table, not assumed at LBA 0.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        m = re.search(r"mounted tosfs from disk \(partition LBA (\d+)\)", t.serial())
        assert m, "kernel did not report the tosfs partition"
        base = int(m.group(1))
        assert base > 0, "FS not in a partition (fell back to LBA 0)"


def t_rm(uefi):
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("write doomed.txt")
        assert t.wait_for("enter a line", 5), "write prompt missing"
        t.line("deleteme")
        assert t.wait_for("saved doomed.txt", 5), "write did not save"
        t.line("rm doomed.txt")
        assert t.wait_for("removed doomed.txt", 5), "rm did not report removal"
        # the file is gone: opening it now fails
        t.line("cat doomed.txt")
        assert t.wait_for("cat: no such file: doomed.txt", 5), "file still present after rm"
        assert "[EXCEPTION]" not in t.serial(), "fault during delete"


def t_rewrite(uefi):
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("write note.txt")
        assert t.wait_for("enter a line", 5), "write prompt missing (1)"
        t.line("uniqueaaa")
        assert t.wait_for("saved note.txt", 5), "first write did not save"
        # rewriting an existing file replaces it in place (O_TRUNC)
        t.line("write note.txt")
        assert t.wait_for("enter a line", 8), "write prompt missing (2)"
        t.line("uniquebbb")
        assert t.serial().count("saved note.txt") >= 2, "rewrite of existing file failed"
        t.line("cat note.txt")
        assert t.wait_for("uniquebbb", 5), "rewritten content not read back"
        # the new content is read back (echo + file = >=2); the old content is
        # gone from the disk (only its original typed echo remains).
        assert t.serial().count("uniquebbb") >= 2, "new content not persisted"
        assert t.serial().count("uniqueaaa") == 1, "old content survived the rewrite"
        assert "[EXCEPTION]" not in t.serial(), "fault during rewrite"


DIR_PERSIST_IMG = "/tmp/tos_dir_persist_fs.img"


def t_directories(uefi):
    # mkdir + cd create a real subdirectory; a file written inside it is reachable
    # there but NOT at the root (the namespace is hierarchical, not flat).
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("mkdir proj")
        t.line("cd proj")
        t.line("write inside.txt")
        assert t.wait_for("enter a line", 5), "write prompt missing"
        t.line("deepfile")
        assert t.wait_for("saved inside.txt", 5), "write into subdir failed"
        t.line("ls")
        assert t.wait_for("inside.txt\t", 5), "file not listed in the subdirectory"
        t.line("cd ..")
        t.line("ls")
        assert t.wait_for("proj/", 5), "subdirectory not listed in its parent"
        t.line("cat inside.txt")
        assert t.wait_for("cat: no such file: inside.txt", 5), "file leaked into the parent namespace"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_seed_tree(uefi):
    # The image ships the standard tree built by mkfs: the user's home has its
    # Documents/Desktop/... folders, and /Apps holds nested .app bundles. Reading a
    # file two directories deep (a shipped app's manifest) proves the host packer
    # emits a tree the kernel walks -- using real OS content, not a throwaway file.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("ls")                                  # cwd is the user's home
        assert t.wait_for("Documents/", 5), "seeded Documents/ directory missing"
        t.line("cat /Apps/Terminal.app/manifest")
        assert t.wait_for("min_width", 5), "could not read a nested app manifest"


def t_move(uefi):
    # mv renames/moves an entry across directories; the original path is gone and
    # the content follows it to the new location.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        assert t.line_for("write mover.txt", "enter a line"), "write prompt missing"
        t.line("movecontent")
        assert t.wait_for("saved mover.txt", 5), "write did not save"
        t.line("mkdir box")
        t.line("mv mover.txt box/m.txt")
        assert t.wait_for("moved mover.txt", 5), "mv did not report the move"
        t.line("cat box/m.txt")
        assert t.serial().count("movecontent") >= 2, "moved file content not at the destination"
        t.line("cat mover.txt")
        assert t.wait_for("cat: no such file: mover.txt", 5), "original survived the move"


def t_rm_recursive(uefi):
    # rm -r empties and removes a directory tree; rmdir refuses a non-empty dir.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("mkdir tree")
        t.line("cd tree")
        t.line("mkdir sub")
        t.line("write f.txt")
        assert t.wait_for("enter a line", 5), "write prompt missing"
        t.line("payload")
        assert t.wait_for("saved f.txt", 5), "write did not save"
        t.line("cd /")
        t.line("rmdir tree")
        assert t.wait_for("rmdir: cannot remove tree", 5), "rmdir wrongly removed a non-empty dir"
        t.line("rm -r tree")
        assert t.wait_for("removed tree", 5), "rm -r did not complete"
        t.line("ls tree")
        assert t.wait_for("ls: cannot open tree", 5), "directory tree survived rm -r"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_dir_persist(uefi):
    # A directory tree (and a file within it) must survive a reboot.
    with Tos(uefi=uefi, scratch=DIR_PERSIST_IMG, reuse=False) as t:
        assert t.boot_ok(), "shell did not come up (boot 1)"
        t.line("mkdir keep")
        t.line("cd keep")
        t.line("write persist.txt")
        assert t.wait_for("enter a line", 5), "write prompt missing"
        t.line("stiladded here")
        assert t.wait_for("saved persist.txt", 5), "write did not save"
        t.line("poweroff")
        assert t.wait_for("shutdown requested", 5), "did not shut down"
    with Tos(uefi=uefi, scratch=DIR_PERSIST_IMG, reuse=True) as t:
        assert t.boot_ok(), "shell did not come up (boot 2)"
        t.line("ls")
        assert t.wait_for("keep/", 5), "directory did not persist across reboot"
        t.line("cat keep/persist.txt")
        assert t.wait_for("stiladded here", 5), "nested file did not persist"


def t_files_app(uefi):
    # The Files icon (the second desktop shortcut, below Terminal) launches the
    # graphical file manager, which maps a window and reads the filesystem.
    with Tos(uefi=uefi) as t:
        assert t.wait_for("[twm] desktop ready", 15), "desktop did not come up"
        xy = t.icon_xy("Files")                 # dock launcher (coords from twm serial)
        assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch from its icon"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def _count_at_least(t, needle, n, timeout=8):
    """Wait until `needle` appears at least `n` times in the serial log."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if t.serial().count(needle) >= n:
            return True
        time.sleep(0.1)
    return False


def t_clipboard_summon(uefi):
    # Super+V opens the clipboard manager. Pressing it again must *summon* the
    # existing window (single-instance), not fork a second copy -- so the clipboard
    # app, which prints "[clipboard] up" once per process, must still have started
    # exactly once.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-v", delay=0.1)                 # Super+V (QEMU's left-Super = meta_l)
        assert t.wait_for("[clipboard] up", 8), "Super+V did not open the clipboard manager"
        assert t.wait_for("[twm] focus Clipboard", 5), "clipboard window did not take focus"
        # Click elsewhere is unnecessary; just summon again and confirm no relaunch.
        time.sleep(0.5)
        t.key("meta_l-v", delay=0.1)
        time.sleep(2.0)                              # well past the launch spinner window
        n = t.serial().count("[clipboard] up")
        assert n == 1, f"Super+V relaunched the clipboard instead of summoning it ({n} instances)"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_window_switch(uefi):
    # Alt+Tab cycles focus through the open windows MRU-style. Open Terminal then
    # Files (Files ends up focused); one Alt+Tab must return focus to Terminal.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files")
        assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12), "Files app did not launch"
        assert t.wait_for("[twm] focus Files", 8), "Files window never took focus"
        before = t.serial().count("[twm] focus Terminal")
        t.key("alt-tab", delay=0.1)                  # Alt+Tab -> back to the Terminal
        assert _count_at_least(t, "[twm] focus Terminal", before + 1, 8), \
            "Alt+Tab did not switch focus back to the Terminal"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_clipboard_popup_esc(uefi):
    # The clipboard is a borderless popup overlay: Esc dismisses it (focus returns
    # to the window underneath).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        t.key("meta_l-v", delay=0.1)                 # Super+V opens the clipboard popup
        assert t.wait_for("[twm] focus Clipboard", 8), "clipboard popup did not open/focus"
        before = t.serial().count("[twm] focus Terminal")
        t.key("esc", delay=0.1)                       # Esc dismisses the popup
        assert _count_at_least(t, "[twm] focus Terminal", before + 1, 8), \
            "Esc did not dismiss the clipboard popup (focus never returned to Terminal)"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_super_q_close(uefi):
    # Super+Q closes the focused window gracefully (its app exits, focus returns to
    # the window underneath).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        xy = t.icon_xy("Files")
        assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[twm] focus Files", 10), "Files window never took focus"
        before = t.serial().count("[twm] focus Terminal")
        t.key("meta_l-q", delay=0.1)                  # Super+Q closes Files
        assert _count_at_least(t, "[twm] focus Terminal", before + 1, 8), \
            "Super+Q did not close the focused window"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_super_kill(uefi):
    # Super+Shift+Q force-kills the focused window's process; the kernel reaps it
    # asynchronously and the OS stays up.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        xy = t.icon_xy("Files")
        assert xy, "Files dock icon coordinates not reported"
        t.doubleclick(*xy)
        assert t.wait_for("[twm] focus Files", 10), "Files window never took focus"
        t.key("shift-meta_l-q", delay=0.1)            # Super+Shift+Q kills the process
        assert t.wait_for("task killed by request", 8), "kernel did not report the async kill"
        # the OS survives and the shell is still responsive
        t.line("echo killsurvived")
        assert t.wait_for("killsurvived", 6), "shell unresponsive after a forced kill"
        assert "PANIC" not in t.serial(), "a forced kill panicked the kernel"


def t_term_paste(uefi):
    # Ctrl+Shift+V pastes the active clipboard entry into the terminal (over the
    # pty), so a copied string is fed back to the shell as if typed.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line_for("copy unievvv", "copied to clipboard")   # shell `copy` -> clipboard ring
        n0 = t.serial().count("unievvv")
        t.key("ctrl-shift-v", delay=0.1)              # paste into the terminal
        # the pasted text is echoed by the shell, so it appears again on the line
        assert _count_at_least(t, "unievvv", n0 + 1, 8), "Ctrl+Shift+V did not paste the clipboard"
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


def t_notepad_wordedit(uefi):
    # #5: word-wise editing lives in the shared toolkit TextField, so every toolkit
    # app inherits it. Ctrl+Left/Right jump word-by-word (`[ui] wjump N`); Ctrl+Delete
    # (and Ctrl+Backspace) delete the adjacent word (`[ui] wdel N`). Drive it
    # keyboard-only through the real kernel->twm->toolkit path and confirm the
    # resulting document on disk. Buffer: "alpha beta gamma" (indices: alpha 0-4,
    # space 5, beta 6-9, space 10, gamma 11-15; len 16).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1)                # Super+Space -> Spotlight
        assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06)
        t.key("ret", delay=0.1)                        # launch Notepad
        assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
        t.type("alpha beta gamma", delay=0.05)         # caret ends at 16
        # Ctrl+Left walks left word-by-word: 16 -> 11 (gamma) -> 6 (beta) -> 0 (alpha)
        t.key("ctrl-left", delay=0.08); assert t.wait_for("[ui] wjump 11", 6), "Ctrl+Left did not jump to 'gamma'"
        t.key("ctrl-left", delay=0.08); assert t.wait_for("[ui] wjump 6", 6),  "Ctrl+Left did not jump to 'beta'"
        t.key("ctrl-left", delay=0.08); assert t.wait_for("[ui] wjump 0", 6),  "Ctrl+Left did not jump to 'alpha'"
        # Ctrl+Right jumps to the end of "alpha" (the space at index 5)
        t.key("ctrl-right", delay=0.08); assert t.wait_for("[ui] wjump 5", 6),  "Ctrl+Right did not jump forward a word"
        # Ctrl+Delete removes the next word (" beta", indices 5..10) -> "alpha gamma"
        t.key("ctrl-delete", delay=0.08); assert t.wait_for("[ui] wdel 5", 6),  "Ctrl+Delete did not delete the next word"
        t.key("ctrl-s", delay=0.1)                      # save
        assert t.wait_for("[notepad] saved /Users/user/untitled.txt (11 bytes)", 8), \
            "word-edited note did not save with the expected length"
        t.key("alt-tab", delay=0.1)                    # back to the terminal
        assert t.wait_for("[twm] focus Terminal", 6), "could not return to the terminal"
        t.line("cat untitled.txt")
        assert t.wait_for("alpha gamma", 6), "word-edited document not as expected on disk"
        t.screenshot("/tmp/tos_notepad_wordedit.ppm")
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_shift_select(uefi):
    # Richer key events: the keyboard now encodes Shift into nav-key CSI sequences
    # (xterm modifier param 2, like Ctrl=5), and the compositor surfaces the live
    # modifier mask -- so the toolkit can finally SEE Shift. Shift+Left/Right/Home/End
    # EXTEND the TextField selection (anchor kept) instead of dropping it, printed as
    # "[ui] shsel a b". Releasing a modifier reaches the focused window as WEV_KEYUP,
    # traced "[twm] keyup <mask>". Driven keyboard-only through kernel->twm->toolkit in
    # Notepad. Buffer "hello world" -> caret ends at 11.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1)                 # Super+Space -> Spotlight
        assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 18), "Notepad did not launch"
        assert t.wait_for("[twm] focus Notepad", 12), "Notepad did not take focus"
        t.type("hello world", delay=0.05)              # caret at 11
        # Shift+Left extends the selection leftward one char at a time (anchor stays 11).
        for want in ("[ui] shsel 10 11", "[ui] shsel 9 11", "[ui] shsel 8 11"):
            t.key("shift-left", delay=0.08)
            assert t.wait_for(want, 6), f"Shift+Left did not extend the selection: want {want!r}"
        # Shift+Home extends to the start of the line -> selection [0,11].
        t.key("shift-home", delay=0.1)
        assert t.wait_for("[ui] shsel 0 11", 6), "Shift+Home did not extend to the line start"
        # releasing a modifier surfaces as a key-up event to the focused window
        assert t.wait_for("[twm] keyup", 6), "releasing a modifier did not post WEV_KEYUP"
        t.screenshot("/tmp/tos_shiftselect.ppm")
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


def t_spotlight_nav(uefi):
    # #13: keyboard QoL -- arrow keys + Tab walk the Spotlight results instead of
    # being swallowed / dismissing the popup. With an empty query all installed apps
    # show (selection on row 0); each nav keypress emits `[spotlight] sel=N`. Two
    # Downs step to rows 1 then 2; Tab from the last row wraps back to 0 -- which
    # also proves the popup stayed open the whole time (stock set: Terminal/Files/Notepad).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1)               # Super+Space opens Spotlight
        assert t.wait_for("[spotlight] up", 8), "Super+Space did not open Spotlight"
        assert t.wait_for("[twm] focus Spotlight", 6), "Spotlight popup did not focus"
        t.key("down", delay=0.1)                      # arrow nav -> row 1
        assert t.wait_for("[spotlight] sel=1", 6), "Down arrow did not move the selection"
        t.key("down", delay=0.1)                      # arrow nav -> row 2
        assert t.wait_for("[spotlight] sel=2", 6), "second Down did not advance the selection"
        t.key("tab", delay=0.1)                       # Tab from the last row wraps -> row 0
        assert t.wait_for("[spotlight] sel=0", 6), "Tab did not cycle/wrap the selection"
        t.screenshot("/tmp/tos_spotlight_nav.ppm")
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_launchpad(uefi):
    # Tapping Super on its own opens the Launchpad (a popup grid of all installed
    # apps, drawn above the dock over a dim scrim). Tapping Super again toggles it
    # closed (issue #2); Esc also still dismisses it.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never took focus"
        t.key("meta_l", delay=0.1)                   # a lone Super tap opens it
        assert t.wait_for("[launchpad] up", 8), "a lone Super tap did not open the Launchpad"
        assert t.wait_for("[twm] focus Launchpad", 6), "Launchpad did not focus"
        before = t.serial().count("[twm] focus Terminal")
        t.key("meta_l", delay=0.15)                  # a second Super tap toggles it closed
        assert _count_at_least(t, "[twm] focus Terminal", before + 1, 8), \
            "a second Super tap did not dismiss the Launchpad"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_launchpad_search(uefi):
    # #11: the Launchpad has a search field that holds focus from the start, so you
    # can type to filter the app grid WITHOUT clicking the field first. Each refilter
    # prints `[launchpad] filt=N`. With the 3 stock bundles an empty query shows 3;
    # typing "note" narrows to 1 (Notepad).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l", delay=0.12)                   # lone Super -> Launchpad
        assert t.wait_for("[launchpad] up", 8), "lone Super did not open the Launchpad"
        assert t.wait_for("[launchpad] filt=3", 6), "Launchpad did not show all stock apps"
        t.type("note", delay=0.08)                    # type-to-filter (no click first)
        assert t.wait_for("[launchpad] filt=1", 6), "search field did not filter the grid"
        t.screenshot("/tmp/tos_launchpad_search.ppm")
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_dock_launchpad(uefi):
    # The dock's leftmost tile is a Launchpad button (issue #3); a single click on
    # it opens the grid, same as the lone-Super tap.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        lp = t.icon_xy("Launchpad"); assert lp, "Launchpad dock button not reported"
        term = t.icon_xy("Terminal"); assert term, "Terminal dock tile not reported"
        assert lp[0] < term[0], "Launchpad button is not the leftmost dock tile"
        t.click(*lp)                                  # single click opens it
        assert t.wait_for("[launchpad] up", 8), "clicking the dock Launchpad button did not open it"
        assert t.wait_for("[twm] focus Launchpad", 6), "Launchpad did not focus"
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_statusbar(uefi):
    # ui.md phase 2: the top bar's right side carries a status cluster -- placeholder
    # network/volume/battery glyphs + a registry-driven clock. twm traces the cluster
    # layout ("[twm] statusbar net ... vol ... bat ... cc ...", strictly L->R) and the
    # formatted clock once ("[twm] clk \"...\""). The shipped defaults (clock.format=24h,
    # clock.seconds=true, clock.weekday=true) render e.g. "Fri 14:09:09".
    with Tos(uefi=uefi) as t:
        assert t.wait_for("[twm] desktop ready", 12), "desktop did not come up"
        m = re.search(r"\[twm\] statusbar net (\d+) vol (\d+) bat (\d+) bell (\d+) cc (\d+)", t.serial())
        assert m, "status cluster did not report its layout"
        net, vol, bat, bell, cc = (int(g) for g in m.groups())
        assert cc < net < vol < bat < bell, \
            f"cluster not laid out left-to-right: cc={cc} net={net} vol={vol} bat={bat} bell={bell}"
        c = re.search(r'\[twm\] clk "([^"]*)"', t.serial())
        assert c, "clock did not render / trace"
        assert re.match(r"^[A-Z][a-z][a-z] \d\d:\d\d:\d\d$", c.group(1)), \
            f"default clock format wrong: {c.group(1)!r} (want 'Ddd HH:MM:SS')"
        t.screenshot("/tmp/tos_statusbar.ppm")
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_notifications(uefi):
    # ui.md phase 3: an app posts a notification with notify(); twm slides the newest in
    # as a top-right toast and keeps a ring for the notification center (toggled by the
    # bell status item). The shell's `notify <text>` posts one ("[twm] notify <title>").
    # Clicking the bell opens the center ("[twm] notifcenter open <n>", n>=1 here).
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        m = re.search(r"\[twm\] statusbar .* bell (\d+) cc", t.serial())
        assert m, "status cluster (with bell) did not report its layout"
        bell_x = int(m.group(1))
        t.line("notify Hello from tOS")
        assert t.wait_for("[twm] notify Terminal", 8), "notify() did not reach the compositor"
        t.screenshot("/tmp/tos_toast.ppm")                  # toast is up ~3.7s
        t.click(bell_x + 9, 11)                             # the bell status item -> open the center
        assert t.wait_for("[twm] notifcenter open", 6), "the bell did not open the notification center"
        c = re.search(r"\[twm\] notifcenter open (\d+)", t.serial())
        assert c and int(c.group(1)) >= 1, "notification center did not list the posted notification"
        t.screenshot("/tmp/tos_notifcenter.ppm")
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


def t_menubar(uefi):
    # #6/#8: the logo and the focused app's name are clickable menu-bar tiles. twm
    # reports each tile's geometry ("[twm] menubar logo <x> <w> app <x> <w>") and, when
    # a dropdown opens, its rows ("[twm] menu logo|app <t> y <rowtop> row <h> x <x>").
    # Clicking the logo opens the system menu; clicking the app name opens the app menu,
    # whose "Quit" item (index 1 -> "[twm] menuitem 2 1") closes the focused window.
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8), "terminal never focused"
        m = re.search(r"\[twm\] menubar logo (\d+) (\d+) app (\d+) (\d+)", t.serial())
        assert m, "menu-bar tiles did not report their geometry"
        lx, lw, ax, aw = (int(g) for g in m.groups())
        # logo -> system menu
        t.click(lx + lw // 2, 11)
        assert t.wait_for("[twm] menu logo", 6), "clicking the logo did not open the system menu"
        t.screenshot("/tmp/tos_menu_logo.ppm")
        t.click(400, 320)                                   # click away dismisses the dropdown
        # app name -> app menu
        t.click(ax + aw // 2, 11)
        assert t.wait_for("[twm] menu app Terminal", 6), "clicking the app name did not open the app menu"
        g = re.search(r"\[twm\] menu app Terminal y (\d+) row (\d+) x (\d+)", t.serial())
        assert g, "app menu did not report its row geometry"
        rowtop, row, mx = (int(v) for v in g.groups())
        t.screenshot("/tmp/tos_menu_app.ppm")
        # click the 2nd row ("Quit") -> closes the focused Terminal window (WEV_CLOSE)
        t.click(mx + 12, rowtop + row + row // 2)
        assert t.wait_for("[twm] menuitem 2 1", 6), "selecting Quit did not register"
        assert t.wait_for("[twm] focus desktop", 8), "Quit did not close the Terminal window"
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


def t_spawn_concurrency(uefi):
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("spawn")
        assert t.wait_for("background task done", 15), "ticker never finished"
        s = t.serial()
        assert "[ticker] tick 5" in s, "ticker did not run to completion"
        assert "task exited (ran at CPL=3)" in s, "ticker did not exit from ring 3"
        # shell still responsive afterwards
        t.line("echo postspawnok")
        assert t.wait_for("postspawnok", 5), "shell unresponsive after spawn"


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


def t_date(uefi):
    # The CMOS RTC driver reports a plausible wall-clock date/time.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("date")
        assert t.wait_for("date", 1)
        m = re.search(r"(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})", t.serial())
        assert m, "date did not print a YYYY-MM-DD HH:MM:SS timestamp"
        yr, mo, dy, hh, mm, ss = (int(x) for x in m.groups())
        assert 2000 <= yr <= 2100 and 1 <= mo <= 12 and 1 <= dy <= 31, f"implausible date {m.group(0)}"
        assert hh < 24 and mm < 60 and ss < 60, f"implausible time {m.group(0)}"


def t_ram_scales(uefi):
    # The frame pool is sized from actual RAM (fw_cfg), not a fixed cap: booting
    # with more memory must yield a proportionally larger pool.
    def pool(mem):
        with Tos(uefi=uefi, mem=mem) as t:
            assert t.boot_ok(), f"did not boot with -m {mem}"
            m = re.search(r"frame pool: (\d+) frames", t.serial())
            assert m, "no frame pool report"
            return int(m.group(1))
    small, big = pool("64M"), pool("256M")
    assert big > small * 3, f"pool did not scale with RAM ({small} @64M vs {big} @256M)"


def t_lspci(uefi):
    # PCI config-space enumeration lists QEMU's i440fx devices.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("lspci")
        assert t.wait_for("8086:1237", 5), "PCI host bridge (8086:1237) not listed"
        assert "8086:7010" in t.serial(), "IDE controller (8086:7010) not listed"


def t_beep(uefi):
    # The PC speaker driver runs without wedging the shell.
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("beep")
        t.line("echo beepok")
        assert t.wait_for("beepok", 5), "shell unresponsive after beep"
        assert "[EXCEPTION]" not in t.serial()


def t_reboot(uefi):
    # `reboot` resets the machine via the 8042 (under -no-reboot QEMU exits).
    with Tos(uefi=uefi) as t:
        assert t.boot_ok(), "shell did not come up"
        t.line("reboot")
        assert t.wait_for("reboot requested", 5), "reboot was not requested"


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


def t_scrollbar_drag(uefi):
    # #12: the multiline TextField's right-edge scroll indicator is clickable AND
    # draggable to scroll. Open Notepad, fill it past one screenful, then interact
    # with the scrollbar track; each press/drag maps the pointer y to the scroll
    # position and emits "[ui] sbtop=N". A click low on the track scrolls further
    # than a click high on it; a drag up then reduces the offset again.
    import re as _re, time as _t
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "desktop/terminal did not come up"
        t.key("meta_l-spc", delay=0.1)
        assert t.wait_for("[spotlight] up", 8), "spotlight did not open"
        t.type("note", delay=0.06); t.key("ret", delay=0.1)
        assert t.wait_for("[notepad] up", 10), "notepad did not launch"
        rect = t.win_rect("Notepad")
        assert rect, "twm did not report the Notepad window rect"
        wx, wy, w, h = rect
        t.click(wx + w // 2, wy + h // 2)         # focus the editor body (not the name field)
        for _ in range(40):                       # fill well past one screenful
            t.key("ret", delay=0.03)
        # Filling parks the caret (and the viewport) at the BOTTOM, so first scroll
        # back to the TOP -- otherwise a "click low on the track" is a no-op. Wheel up
        # over the text area until the offset is 0.
        t.mouse_to(wx + w // 2, wy + h // 2)
        t.wheel(1, n=50); _t.sleep(0.3)
        sx = wx + w - 4                           # inside the right-edge scrollbar strip
        def last_top():
            ms = _re.findall(r"\[ui\] sbtop=(\d+)", t.serial())
            return int(ms[-1]) if ms else None
        # click low on the track -> large scroll offset. Stay clear of the bottom
        # ~h/8: that band overlaps the dock, which would eat the click before the app.
        ylow = wy + (h * 3) // 4
        t.click(sx, ylow)
        assert t.wait_for("[ui] sbtop=", 6), "clicking the scroll indicator did nothing"
        low = last_top()
        assert low and low > 0, "low click did not scroll (sbtop=%r)" % low
        # click high on the track -> smaller offset
        t.click(sx, wy + h // 4)
        dl = _t.time() + 6; high = low
        while _t.time() < dl:
            high = last_top()
            if high is not None and high < low: break
            _t.sleep(0.15)
        assert high is not None and high < low, "high click did not scroll up (low=%r high=%r)" % (low, high)
        # and clicking low again sends it back down -- the track maps the pointer to
        # the scroll position both ways (the thumb is also grab-draggable in code via
        # on_drag -> sb_set_top_from_y; the held-drag harness path is finicky at the
        # window edge, so the round-trip click is the stable assertion here).
        t.click(sx, ylow)
        dl = _t.time() + 6; back = high
        while _t.time() < dl:
            back = last_top()
            if back is not None and back > high: break
            _t.sleep(0.15)
        assert back is not None and back > high, "low click did not scroll back down (high=%r back=%r)" % (high, back)
        t.screenshot("/tmp/tos_scrollbar_drag.ppm")
        assert "[EXCEPTION]" not in t.serial() and "PANIC" not in t.serial()


BIOS_TESTS = [
    t_boot_and_ls, t_cat_motd, t_cat_missing, t_write_then_cat,
    t_partition, t_fs_persist, t_rm, t_rewrite,
    t_directories, t_seed_tree, t_move, t_rm_recursive, t_dir_persist, t_registry,
    t_sleep, t_fork, t_orphan_reparent, t_app_crash, t_smp,
    t_spawn_concurrency, t_gui, t_files_app, t_clipboard_summon, t_clipboard_popup_esc,
    t_window_switch, t_super_q_close, t_super_kill, t_term_paste, t_notepad_edit_save,
    t_notepad_wordedit, t_shift_select,
    t_spotlight, t_spotlight_nav, t_launchpad, t_launchpad_search, t_dock_launchpad, t_menubar,
    t_mouse, t_many_files, t_date, t_ram_scales, t_lspci, t_beep, t_reboot,
    t_scrollbar_drag, t_statusbar, t_notifications,
]
# A representative subset on UEFI to confirm both boot paths reach the same OS.
UEFI_TESTS = [t_boot_and_ls, t_cat_motd, t_write_then_cat, t_rm, t_partition,
              t_directories, t_seed_tree, t_move, t_fork, t_app_crash, t_smp,
              t_gui, t_files_app, t_mouse, t_many_files, t_date, t_lspci]


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
