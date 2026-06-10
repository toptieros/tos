"""One-off repro: does a header-divider drag reach ColumnHeader::on_drag?
Boots BIOS, opens Files at home, drags the Name|Kind divider +60px, then dumps
the colw/hdr canaries. A working drag commits "[files] colw 290 96 96" (230+60)
on release. Kept as the regression probe for the twm hover-vs-press bug: the
compositor used to post a hover-leave packet on the press frame, which the
toolkit read as a button-up, cancelling the grab before any drag arrived.
Not part of make test."""
import sys, re, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import _files_hdr, _count_at_least

with Tos(uefi=False) as t:
    assert t.open_terminal()
    xy = t.icon_xy("Files"); t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12)
    assert t.wait_for("[twm] focus Files", 8)
    wr = t.win_rect("Files")
    assert t.wait_for("[files] cd /Users/user", 8)
    hx, hy, hh, cols = _files_hdr(t)
    dvx, dvy = hx + cols[0][0] + cols[0][1], hy + hh // 2
    print("hdr: x=%d y=%d h=%d cols=%s -> divider at client (%d,%d) screen (%d,%d)"
          % (hx, hy, hh, cols, dvx, dvy, wr[0] + dvx, wr[1] + dvy))
    n = t.serial().count("[files] colw")
    t.drag(wr[0] + dvx, wr[1] + dvy, wr[0] + dvx + 60, wr[1] + dvy)
    time.sleep(1.0)
    for ln in t.serial().splitlines():
        if re.search(r"\[files\] (colw|hdr )|\[twm\] focus", ln):
            print(ln)
    ms = re.findall(r"\[files\] colw (\d+) (\d+) (\d+)", t.serial())
    assert len(ms) > n and int(ms[-1][0]) >= 270, \
        "drag did not commit a wider Name (colw lines: %s)" % ms
    print("OK: divider drag committed cw[0]=%s" % ms[-1][0])
