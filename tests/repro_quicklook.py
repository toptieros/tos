"""One-off verification boot for thumbnails + Quick Look (files-app.md §11).
DISPOSABLE per design/testing.md: the pure fit/box-average math is unit-tested
(tests/unit/t_thumb.c); this drives the GUI wiring once -- an .argb file shows
its own pixels as the row icon + icon-view tile, Space opens the Quick Look
overlay (image / text / folder summary), Esc / Space / click dismisses -- and
grabs screenshots. Not part of make test."""
import sys, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import _files_nav, _files_row_xy, _files_menu_click, _count_at_least

with Tos(uefi=False) as t:
    assert t.open_terminal(), "desktop/terminal did not come up"
    assert t.wait_for("[twm] focus Terminal", 8)
    t.line("cp /Apps/Notepad.app/icon.argb /Users/user/Pictures/photo.argb")
    t.line("write /Users/user/Pictures/notes.txt")
    assert t.wait_for("enter a line", 6); t.line("quick look text preview line")
    assert t.wait_for("saved /Users/user/Pictures/notes.txt", 6)
    t.line("echo STAGED"); assert t.wait_for("STAGED", 6)

    xy = t.icon_xy("Files"); assert xy
    t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12)
    assert t.wait_for("[twm] focus Files", 8)
    wr = t.win_rect("Files"); assert wr
    _files_nav(t, wr, "/Users/user/Pictures")

    # rows: ..(0) notes.txt(1) photo.argb(2)   (files sort by name, Folders First)
    t.click(*_files_row_xy(t, wr, 2))                 # select the image
    time.sleep(0.4)
    t.screenshot("/tmp/tos_thumb_rows.ppm")           # row icon = the image's own pixels
    t.key("spc")
    assert t.wait_for("[files] quicklook image photo.argb", 6), "Space did not open image Quick Look"
    time.sleep(0.4)
    t.screenshot("/tmp/tos_quicklook_img.ppm")        # scrim + centred card + scaled image
    t.key("esc")
    assert t.wait_for("[files] quicklook close", 6), "Esc did not dismiss Quick Look"

    t.click(*_files_row_xy(t, wr, 1))                 # the text file
    t.key("spc")
    assert t.wait_for("[files] quicklook text notes.txt", 6), "no text Quick Look"
    time.sleep(0.4)
    t.screenshot("/tmp/tos_quicklook_txt.ppm")
    t.key("spc")                                      # Space toggles closed too
    assert _count_at_least(t, "[files] quicklook close", 2, 6), "Space did not toggle Quick Look off"

    _files_menu_click(t, "View", 0)                   # as Icons: thumbnail tiles
    assert t.wait_for("[files] view icons", 6), "icon view did not engage"
    time.sleep(0.5)
    t.screenshot("/tmp/tos_thumb_tiles.ppm")

    s = t.serial()
    assert "EXCEPTION" not in s and "PANIC" not in s
    print("OK: thumbnails (rows + tiles) and Quick Look (image/text, esc/space) verified")
