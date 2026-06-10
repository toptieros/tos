"""One-off verification boot for the filter/search bar's click-away dismiss
(files-app.md S5). DISPOSABLE per design/testing.md. An OPEN-BUT-EMPTY bar is
dismissed by clicking anywhere outside it (like Esc); with text typed (a live
filter) a click acts normally and the bar stays; Esc still closes it. The signal
is the listrect canary close_filter/open_filter re-emit on the 29px layout shift.
Not part of make test."""
import sys, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import _files_row_xy, _files_menu_click, _count_at_least

def _main(t):
    assert t.open_terminal(), "desktop/terminal did not come up"
    assert t.wait_for("[twm] focus Terminal", 8)
    xy = t.icon_xy("Files"); assert xy
    t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12)
    assert t.wait_for("[twm] focus Files", 8)
    wr = t.win_rect("Files"); assert wr

    # --- empty bar: a click on a row dismisses it (and still selects the row) ---
    lr = t.serial().count("[files] listrect")
    _files_menu_click(t, "File", 6)                  # Find -> the armed, empty bar
    assert t.wait_for("[files] searchbar", 6), "Find did not open the bar"
    assert _count_at_least(t, "[files] listrect", lr + 1, 4), "opening the bar did not re-aim listrect"
    lr = t.serial().count("[files] listrect")
    n_sel = t.serial().count("[files] sel ")
    t.click(*_files_row_xy(t, wr, 1))                # click away (a row) -> dismiss
    assert _count_at_least(t, "[files] listrect", lr + 1, 4), "clicking away did not dismiss the empty bar"
    assert _count_at_least(t, "[files] sel ", n_sel + 1, 4), "the dismissing click lost its row selection"
    time.sleep(0.4)
    t.screenshot("/tmp/tos_filterdismiss.ppm")       # bar gone, row selected

    # --- bar with text: the click acts normally, the bar STAYS ------------------
    _files_menu_click(t, "File", 6)
    assert _count_at_least(t, "[files] searchbar", 2, 6)
    t.type("doc")                                    # a live filter (Documents matches)
    time.sleep(0.3)
    lr = t.serial().count("[files] listrect")
    t.click(*_files_row_xy(t, wr, 0))                # filtered view has no "..": row 0 = a hit
    time.sleep(0.5)
    assert t.serial().count("[files] listrect") == lr, "a click closed a bar that holds a live filter"
    time.sleep(0.2)
    t.screenshot("/tmp/tos_filterstays.ppm")         # bar still up with "doc" in it

    # --- Esc still closes it ----------------------------------------------------
    t.key("esc", delay=0.3)
    assert _count_at_least(t, "[files] listrect", lr + 1, 4), "Esc did not close the bar"

    s = t.serial()
    assert "EXCEPTION" not in s and "PANIC" not in s
    print("OK: click-away dismisses an empty bar, keeps a live filter; Esc still closes")

with Tos(uefi=False) as t:
    try:
        _main(t)
    except Exception:
        with open("/tmp/repro_filterdismiss_serial.txt", "w") as f:
            f.write(t.serial())
        print("---- serial saved to /tmp/repro_filterdismiss_serial.txt ----")
        raise
