"""One-off verification boot for tags/labels (files-app §10). DISPOSABLE per
design/testing.md: the pure ~/.tags codec is unit-tested (tests/unit/t_tagstore.c);
this drives the GUI wiring once -- context "Tags..." picker toggles write the store
and draw row dots, the sidebar's Tags section filters the listing -- and grabs
screenshots. Not part of make test.

Journey: stage /Users/user/tagdir with two files; tag tagme.txt Blue + Green from
the context picker (stay-open toggles, "[files] tags <path> <mask>" canaries);
expand the sidebar Tags section and click Blue ("[files] tagfilter Blue 1": only
the tagged file shows); click Blue again to clear ("tagfilter off 2")."""
import sys, re, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import (_files_nav, _files_row_xy, _files_ctx_click,
                       _files_siderow, _count_at_least)

with Tos(uefi=False) as t:
    assert t.open_terminal(), "desktop/terminal did not come up"
    assert t.wait_for("[twm] focus Terminal", 8)
    t.line("mkdir /Users/user/tagdir")
    t.line("write /Users/user/tagdir/tagme.txt")
    assert t.wait_for("enter a line", 6); t.line("hello")
    assert t.wait_for("saved /Users/user/tagdir/tagme.txt", 6)
    t.line("write /Users/user/tagdir/plain.txt")
    assert _count_at_least(t, "enter a line", 2, 6); t.line("untagged")
    assert t.wait_for("saved /Users/user/tagdir/plain.txt", 6)

    xy = t.icon_xy("Files"); assert xy
    t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12)
    assert t.wait_for("[twm] focus Files", 8)
    wr = t.win_rect("Files"); assert wr
    _files_nav(t, wr, "/Users/user/tagdir")

    # --- tag tagme.txt (row2: ..(0) plain(1) tagme(2)) Blue + Green ---
    c_ctx = t.serial().count("[files] ctxmenu")
    t.rightclick(*_files_row_xy(t, wr, 2))
    _files_ctx_click(t, wr, 7, c_ctx)        # file ctx: ...Rename(5) Delete(6) Tags...(7)
    # the picker is its own popup -> a second ctxmenu canary (7 colour rows)
    assert _count_at_least(t, "[files] ctxmenu", c_ctx + 2, 6), "the Tags... picker did not open"
    _files_ctx_click(t, wr, 4, c_ctx + 1)    # Blue (bit 4) -- stays open
    assert t.wait_for("[files] tags /Users/user/tagdir/tagme.txt 16", 6), \
        "the Blue toggle did not write the store"
    _files_ctx_click(t, wr, 3, c_ctx + 1)    # Green (bit 3) on top
    assert t.wait_for("[files] tags /Users/user/tagdir/tagme.txt 24", 6), \
        "the Green toggle did not stack onto the mask"
    t.screenshot("/tmp/tos_tags_picker.ppm") # the stay-open picker: dots + two checks
    t.click(wr[0] + 30, wr[1] + 60)          # click-away (toolbar area) dismisses
    time.sleep(0.6)

    # --- filter by Blue from the sidebar's Tags section ---
    hdr = _files_siderow(t, "#2"); assert hdr, "no Tags section header in the side dump"
    mark = len(t.serial())
    t.click(wr[0] + hdr[0], wr[1] + hdr[1])  # expand (Tags defaults collapsed)
    assert _count_at_least(t, "[files] sidesect 2 0", 1, 6), "the Tags header did not expand"
    # 18 rows overflow the panel, so the later tags sit below the Trash clip --
    # wheel the sidebar down until a FRESH dump shows the Blue row on screen
    row = _files_siderow(t, "Blue", since=mark, timeout=2)
    for _ in range(8):
        if row: break
        mark = len(t.serial())
        t.mouse_to(wr[0] + hdr[0], wr[1] + 300)  # over the sidebar body
        t.wheel(-1)
        row = _files_siderow(t, "Blue", since=mark, timeout=2)
    assert row, "no on-screen Blue row after expanding + scrolling"
    t.click(wr[0] + row[0], wr[1] + row[1])
    if not t.wait_for("[files] tagfilter Blue 1", 6):
        for ln in t.serial().splitlines():
            if re.search(r"\[files\] (tag|sidesect|siderow|cd )", ln):
                print(ln)
        print("clicked Blue at", wr[0] + row[0], wr[1] + row[1], "wr=", wr)
        t.screenshot("/tmp/tos_tags_fail.ppm")
        raise AssertionError("the Blue filter did not reduce the listing to the tagged file")
    time.sleep(0.4)
    t.screenshot("/tmp/tos_tags.ppm")        # Blue lit in the sidebar; only tagme.txt + dots
    t.click(wr[0] + row[0], wr[1] + row[1])  # click again = off
    assert t.wait_for("[files] tagfilter off 2", 6), "re-clicking Blue did not clear the filter"

    s = t.serial()
    assert "EXCEPTION" not in s and "PANIC" not in s
    print("OK: tags picker + store + sidebar filter all verified")
