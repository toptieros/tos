"""One-off repro for icon-view drag SOURCES (the DnD follow-on: list view could
already drag a file onto a folder; icon + gallery views now can too, via cur_sel()
as the source and view_row_at() as the drop hit-test). Boots BIOS, stages a folder
'dest' + a file 'doc.txt' in a fresh dir, opens Files, switches to ICON view, and
press-drags the doc.txt TILE onto the dest folder TILE:
  source  Files::on_drag -> begin_drag(DRAG_FILES) from the icon tile  ([files] drag doc.txt)
  target  Files::on_drop  -> view_row_at() finds the folder tile, moves ([files] dropped doc.txt into dest)
Default icon zoom = level 1 => 100x92 tiles; the grid origin is the listrect x/y.
Entries sort Folders-First: row 0 '..', row 1 'dest', row 2 'doc.txt'. Not part of
make test (the offset math is unit-tested; this drives the GUI wiring once)."""
import sys, time, re
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import _files_nav, _files_menu_click, _count_at_least

with Tos(uefi=False) as t:
    assert t.open_terminal(), "desktop/terminal did not come up"
    assert t.wait_for("[twm] focus Terminal", 8)
    t.line("mkdir /Users/user/dndtest")
    t.line("mkdir /Users/user/dndtest/dest")
    t.line("cp /Apps/Notepad.app/icon.argb /Users/user/dndtest/doc.txt")
    t.line("echo STAGED"); assert t.wait_for("STAGED", 6)

    xy = t.icon_xy("Files"); assert xy
    t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12)
    assert t.wait_for("[twm] focus Files", 8)
    wr = t.win_rect("Files"); assert wr
    _files_nav(t, wr, "/Users/user/dndtest")

    _files_menu_click(t, "View", 0)                  # as Icons
    assert t.wait_for("[files] view icons", 6), "icon view did not engage"
    time.sleep(0.4)

    # tile centres from the listrect canary (grid origin == list origin; tiles 100x92)
    ms = re.findall(r"\[files\] listrect (\d+) (\d+) (\d+) (\d+)", t.serial())
    assert ms, "no listrect canary"
    lx, ly, lw, _rh = (int(v) for v in ms[-1])
    TW, TH = 100, 92
    cols = max(1, lw // TW)
    def tile(i): return (wr[0] + lx + (i % cols) * TW + TW // 2,
                         wr[1] + ly + (i // cols) * TH + TH // 2)
    src = tile(2)      # doc.txt
    dst = tile(1)      # dest folder

    dragc = t.serial().count("[files] drag doc.txt\r")
    dropc = t.serial().count("[files] dropped doc.txt into dest\r")

    # press on the doc.txt tile (selects it), then crawl to the dest tile in small
    # relative steps so the held-button bit stays set (mouse_to re-pins with button UP).
    t.mouse_to(*src)
    t.mon.sendall(b"mouse_button 1\n"); time.sleep(0.10)
    ddx, ddy, steps = dst[0] - src[0], dst[1] - src[1], 12
    for i in range(steps):
        sx = ddx // steps + (1 if i < (abs(ddx) % steps) and ddx >= 0 else (-1 if i < (abs(ddx) % steps) else 0))
        sy = ddy // steps + (1 if i < (abs(ddy) % steps) and ddy >= 0 else (-1 if i < (abs(ddy) % steps) else 0))
        t.mon.sendall(f"mouse_move {sx} {sy}\n".encode()); time.sleep(0.05)
    time.sleep(0.2)
    assert _count_at_least(t, "[files] drag doc.txt", dragc + 1, 6), \
        "icon-view drag did not arm a DRAG_FILES source"
    t.screenshot("/tmp/tos_icondrag_ghost.ppm")
    t.mon.sendall(b"mouse_button 0\n"); time.sleep(0.2)   # drop on the dest tile
    assert _count_at_least(t, "[files] dropped doc.txt into dest", dropc + 1, 6), \
        "icon-view drop onto the folder tile did not move the file"
    t.screenshot("/tmp/tos_icondrag_done.ppm")

    # prove it hit disk: doc.txt is now inside dest/
    t.key("alt-tab", delay=0.1)
    assert t.wait_for("[twm] focus Terminal", 6)
    t.line("ls /Users/user/dndtest/dest")
    assert t.wait_for("doc.txt", 6), "the moved file is not in dest/ on disk"

    s = t.serial()
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during icon drag"
    print("OK: icon-view drag source armed + dropped onto folder tile -> file moved on disk")
