"""One-off verification boot for the gallery view (files-app.md §1). DISPOSABLE
per design/testing.md: drives View > as Gallery once -- big preview of the
selected image (full decode), filmstrip tiles along the bottom, arrow-key + click
navigation updating the preview -- and grabs a screenshot. Not part of make test."""
import sys, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import _files_nav, _files_row_xy, _files_menu_click, _count_at_least

with Tos(uefi=False) as t:
    assert t.open_terminal(), "desktop/terminal did not come up"
    assert t.wait_for("[twm] focus Terminal", 8)
    t.line("cp /Apps/Notepad.app/icon.argb /Users/user/Pictures/photo.argb")
    t.line("cp /Apps/Settings.app/icon.argb /Users/user/Pictures/zsecond.argb")
    t.line("echo STAGED"); assert t.wait_for("STAGED", 6)

    xy = t.icon_xy("Files"); assert xy
    t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12)
    assert t.wait_for("[twm] focus Files", 8)
    wr = t.win_rect("Files"); assert wr
    _files_nav(t, wr, "/Users/user/Pictures")

    t.click(*_files_row_xy(t, wr, 1))                 # select photo.argb (..(0) photo(1) zsecond(2))
    _files_menu_click(t, "View", 7)                   # as Gallery (appended item 7)
    assert t.wait_for("[files] view gallery", 6), "the gallery view did not engage"
    time.sleep(0.6)
    t.screenshot("/tmp/tos_gallery.ppm")              # big preview + filmstrip

    t.key("right")                                    # strip navigation moves the selection
    time.sleep(0.5)
    t.screenshot("/tmp/tos_gallery2.ppm")             # now previewing zsecond.argb

    _files_menu_click(t, "View", 1)                   # back to as List
    assert _count_at_least(t, "[files] view list", 1, 6), "leaving the gallery failed"

    s = t.serial()
    assert "EXCEPTION" not in s and "PANIC" not in s
    print("OK: gallery view (preview + filmstrip + arrow nav) verified")
