#!/usr/bin/env python3
"""Investigation scaffold for the reported "cross-pane click-drag in Files split view
freezes the whole desktop" bug (NEXT_STEPS.md ▸ Known issues, 2026-06-09). NOT part of
`make test` (run_tests.py only auto-discovers t_* functions inside itself).

Drives Files into split, drags from pane 1 across the splitter into pane 2, dumps the
serial the drag alone produced (so far: only "[files] sel ..." -- the app side is benign),
then probes whether input is still serviced. CAVEAT: the harness drag() uses *relative*
mouse_move packets that can themselves desync QEMU's emulated PS/2 pointer/button, so a
post-drag "input stall" here is not by itself proof of an OS bug -- a real mouse self-heals.
Run: `python3 tests/repro_split_drag.py [--uefi]`."""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_tests import (Tos, _files_nav, _files_menu_click, _files_row_xy,
                       _files_pane2_row_xy, _count_at_least, _dock_focus)

def main():
    uefi = "--uefi" in sys.argv
    with Tos(uefi=uefi) as t:
        assert t.open_terminal(), "terminal did not come up"
        assert t.wait_for("[twm] focus Terminal", 8)
        t.line("mkdir /Users/user/sp")
        t.line("mkdir /Users/user/sp/src")
        t.line("write /Users/user/sp/src/a.txt"); t.line("hello")
        assert t.wait_for("saved /Users/user/sp/src/a.txt", 6)
        xy = t.icon_xy("Files"); assert xy
        t.doubleclick(*xy)
        assert t.wait_for("[files] file manager up", 12)
        assert t.wait_for("[twm] focus Files", 8)
        wr = t.win_rect("Files"); assert wr
        _files_nav(t, wr, "/Users/user/sp/src")
        _files_menu_click(t, "View", 5)
        assert t.wait_for("[files] split 1", 6)
        assert t.wait_for("[files] pane2 cd /Users/user/sp/src", 6)

        x1, y1 = _files_row_xy(t, wr, 1)
        x2, y2 = _files_pane2_row_xy(t, wr, 0)
        mark = len(t.serial())
        print(">>> cross-pane drag pane1(%d,%d) -> pane2(%d,%d)" % (x1, y1, x2, y2))
        t.drag(x1, y1, x2, y2, steps=12)
        time.sleep(1.0)
        print("============ SERIAL PRODUCED BY THE DRAG ALONE ============")
        print(t.serial()[mark:])
        print("===========================================================")

        # The harness drag() uses RELATIVE mouse_move packets, which can desync QEMU's
        # absolute pointer. Re-sync with an absolute move + a click on empty desktop so
        # the later dock click can't miss -- this separates a harness pointer-desync
        # artifact from a genuine OS input stall.
        print(">>> probe twm liveness: move mouse, expect hover/icon serial")
        live_mark = len(t.serial())
        t.mouse_to(30, 300); time.sleep(0.3)
        t.click(30, 300)            # empty desktop click to reset button state
        time.sleep(0.4)
        print(">>> twm produced serial after post-drag mouse activity: %s"
              % (len(t.serial()) > live_mark))
        print(">>> refocusing terminal via _dock_focus, then echo")
        _dock_focus(t, "Terminal")
        t.line("echo STILL_ALIVE_XYZZY")
        responded = t.wait_for("STILL_ALIVE_XYZZY", 8)
        print(">>> terminal echoed after drag: %s" % bool(responded))
        print(">>> EXCEPTION/PANIC: %s" %
              ("[EXCEPTION]" in t.serial() or "PANIC" in t.serial()))

if __name__ == "__main__":
    main()
