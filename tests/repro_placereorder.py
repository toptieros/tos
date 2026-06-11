"""One-off repro for #7b: drag-reorder the Favorites (Places) in the Files sidebar.
Boots BIOS, opens Files (default pins: Home, Desktop, Documents, Downloads,
Pictures = indices 0..4), then drags "Downloads" up above "Desktop". That exercises
the intra-app reorder built on the DnD keystone:
  press   FilesWin::on_press notes the pressed Favorites row    (place_src)
  arm     FilesWin::on_drag -> begin_drag(DRAG_PLACE, label, path) ([files] place drag)
  session twm draws the ghost chip + posts WEV_DROP
  drop    FilesWin::on_drop -> places_move + save/sync          ([files] places reorder 3 -> 1)
Verifies the new on-screen order (Downloads now sits above Desktop) and screenshots
the ghost chip + the accent insertion line mid-drag. Not part of make test."""
import sys, re, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import _files_siderow

GHOST = "/tmp/tos_placereorder_ghost.ppm"
DONE = "/tmp/tos_placereorder_done.ppm"

with Tos(uefi=False) as t:
    assert t.open_terminal(), "desktop/terminal did not come up"
    xy = t.icon_xy("Files"); t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12), "Files did not launch"
    assert t.wait_for("[twm] focus Files", 8), "Files did not take focus"
    wr = t.win_rect("Files"); assert wr, "no Files window rect"
    assert t.wait_for("[files] cd /Users/user", 8), "Files did not land at home"

    # The default Favorites, top to bottom: Home(0) Desktop(1) Documents(2) Downloads(3) Pictures(4).
    src = _files_siderow(t, "Downloads"); assert src, "Downloads pin not dumped"
    dst = _files_siderow(t, "Desktop");   assert dst, "Desktop pin not dumped"
    sx, sy = wr[0] + src[0], wr[1] + src[1]
    # Drop just ABOVE Desktop's row midline -> insertion gap 1 (before Desktop).
    dx, dy = wr[0] + dst[0], wr[1] + dst[1] - 6

    # Press the favourite (this also navigates to it -- expected), then crawl up to the
    # drop in small relative steps so the held-button bit stays set.
    t.mouse_to(sx, sy)
    t.mon.sendall(b"mouse_button 1\n"); time.sleep(0.10)
    ddx, ddy, steps = dx - sx, dy - sy, 10
    for i in range(steps):
        mx = ddx // steps + (1 if i < (ddx % steps) else 0)
        my = ddy // steps + (1 if i < (ddy % steps) else 0)
        t.mon.sendall(f"mouse_move {mx} {my}\n".encode()); time.sleep(0.05)
    time.sleep(0.2)

    assert t.wait_for("[files] place drag Downloads", 6), "dragging a pin did not arm a DRAG_PLACE"
    assert t.wait_for("[twm] drag begin", 6), "twm did not start the drag session"
    t.screenshot(GHOST)                       # ghost chip + accent insertion line
    mark = len(t.serial())
    t.mon.sendall(b"mouse_button 0\n"); time.sleep(0.15)   # drop
    assert t.wait_for("[files] places reorder 3 -> 1", 6), "drop did not reorder Places"
    t.screenshot(DONE)

    # Prove the new on-screen order: Downloads now sits ABOVE Desktop.
    nd = _files_siderow(t, "Downloads", since=mark); assert nd, "no fresh dump after reorder"
    ne = _files_siderow(t, "Desktop",   since=mark); assert ne, "Desktop missing after reorder"
    assert nd[1] < ne[1], "Downloads did not move above Desktop (Downloads y=%d, Desktop y=%d)" % (nd[1], ne[1])

    s = t.serial()
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during place reorder"
    print("OK: dragged Downloads above Desktop (reorder 3 -> 1); new order verified")
    print("ghost screenshot: " + GHOST)
    print("done screenshot:  " + DONE)
