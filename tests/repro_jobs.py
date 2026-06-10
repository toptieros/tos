"""One-off verification boot for background jobs + conflict prompts (files-app.md
S12) and recursive search (S5). DISPOSABLE per design/testing.md. Drives a split-pane
folder copy whose destination collides -- Replace / Keep Both / Skip dialog -- with the
chunked on_tick job emitting "[files] job *" canaries and the status-bar progress band;
then File > Find streams recursive results into the list and a double-click jumps to
the hit's folder. Not part of make test."""
import re, sys, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import (_files_nav, _files_row_xy, _files_menu_click, _files_ctx_click,
                       _files_pane2_row_xy, _count_at_least, _dock_focus)

def _dlg_click(t, wr, idx):
    """Click button `idx` of the open ConfirmDialog via its dlgbtn canary."""
    d = None
    for _ in range(40):
        ms = re.findall(r"\[ui\] dlgbtn %d (\d+) (\d+)" % idx, t.serial())
        if ms: d = ms[-1]; break
        time.sleep(0.1)
    assert d, "dialog button %d was not reported" % idx
    t.click(wr[0] + int(d[0]), wr[1] + int(d[1]))

def _main(t):
    assert t.open_terminal(), "desktop/terminal did not come up"
    assert t.wait_for("[twm] focus Terminal", 8)
    # stage sp2/{src,dst}; src/pack holds 12 files; dst/pack collides
    t.line("mkdir /Users/user/sp2")
    t.line("mkdir /Users/user/sp2/src")
    t.line("mkdir /Users/user/sp2/dst")
    t.line("mkdir /Users/user/sp2/src/pack")
    t.line("mkdir /Users/user/sp2/dst/pack")
    for i in range(1, 41):                           # enough items that the job spans many ticks
        t.line("write /Users/user/sp2/src/pack/f%02d.txt" % i); t.line("p%d" % i)
    assert t.wait_for("saved /Users/user/sp2/src/pack/f40.txt", 10), "staging failed"

    xy = t.icon_xy("Files"); assert xy
    t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12)
    assert t.wait_for("[twm] focus Files", 8)
    wr = t.win_rect("Files"); assert wr
    _files_nav(t, wr, "/Users/user/sp2/src")

    # split view; drive pane 2 to sp2/dst (.. then dst)
    _files_menu_click(t, "View", 5)
    assert t.wait_for("[files] split 1", 6)
    assert t.wait_for("[files] pane2 cd /Users/user/sp2/src", 6)
    p2 = t.serial().count("[files] pane2 cd /Users/user/sp2\r")
    t.doubleclick(*_files_pane2_row_xy(t, wr, 0))
    assert _count_at_least(t, "[files] pane2 cd /Users/user/sp2\r", p2 + 1, 6)
    t.doubleclick(*_files_pane2_row_xy(t, wr, 1))    # sp2: ..(0) dst(1) src(2)
    assert t.wait_for("[files] pane2 cd /Users/user/sp2/dst", 6)

    # --- copy "pack" across: collision -> conflict dialog -> Keep Both ---------
    c_ctx = t.serial().count("[files] ctxmenu")
    t.rightclick(*_files_row_xy(t, wr, 1))           # src: ..(0) pack(1)
    _files_ctx_click(t, wr, 6, c_ctx)                # folder ctx: Copy to Other Pane = 6
    assert t.wait_for("[files] job conflict pack", 8), "no conflict prompt for the colliding name"
    time.sleep(0.5)
    t.screenshot("/tmp/tos_jobs_conflict.ppm")       # Replace / Keep Both / Skip card
    _dlg_click(t, wr, 1)                             # Keep Both
    assert t.wait_for("[files] job start copy 40", 6), "the copy job did not start"
    t.screenshot("/tmp/tos_jobs_progress.ppm")       # "Copying k of 40..." + the accent band
    assert t.wait_for("[files] copy-across pack -> /Users/user/sp2/dst", 8)
    assert t.wait_for("[files] job done copy", 6)

    # --- again -> Replace (rmrf the old dst/pack, then copy) -------------------
    c_ctx = t.serial().count("[files] ctxmenu")
    t.rightclick(*_files_row_xy(t, wr, 1))
    _files_ctx_click(t, wr, 6, c_ctx)
    assert _count_at_least(t, "[files] job conflict pack", 2, 8)
    _dlg_click(t, wr, 0)                             # Replace
    assert _count_at_least(t, "[files] job done copy", 2, 10), "the Replace copy did not finish"

    # on disk: dst has pack (replaced, 12 files) + pack (2) (Keep Both)
    _dock_focus(t, "Terminal")
    mark = len(t.serial())
    t.line("ls /Users/user/sp2/dst/pack"); t.line("echo JOBSDONE")
    assert t.wait_for("JOBSDONE", 6)
    seg = t.serial()[mark:]
    assert "f01.txt" in seg and "f40.txt" in seg, "Replace did not copy pack's contents"
    mark = len(t.serial())
    t.line("ls /Users/user/sp2/dst"); t.line("echo LSDONE")
    assert t.wait_for("LSDONE", 6)
    assert "pack (2)" in t.serial()[mark:], "Keep Both did not dedupe the folder name"

    # --- recursive search (S5): File > Find, query "pack", jump to a hit -------
    _dock_focus(t, "Files")
    _files_menu_click(t, "View", 5)                  # split off (search uses one pane)
    assert t.wait_for("[files] split 0", 6)
    _files_nav(t, wr, "/Users/user/sp2")
    _files_menu_click(t, "File", 6)                  # Find (appended item 6)
    assert t.wait_for("[files] searchbar", 6), "Find did not arm the search bar"
    t.type("pack"); t.key("ret")
    assert t.wait_for("[files] search start pack", 6), "Enter did not start the search"
    assert t.wait_for("[files] search done 3", 8), "expected 3 hits (src/pack, dst/pack, dst/pack (2))"
    time.sleep(0.5)
    t.screenshot("/tmp/tos_jobs_search.ppm")         # results + "3 results" status
    mark = len(t.serial())
    t.doubleclick(*_files_row_xy(t, wr, 0))          # open a hit = jump to its folder
    assert t.wait_for("[files] search open pack", 6), "double-clicking a result did not open it"
    assert t.wait_for("[files] search close", 6)
    seg = ""
    for _ in range(40):
        seg = t.serial()[mark:]
        if "[files] cd /Users/user/sp2/" in seg: break
        time.sleep(0.1)
    assert "[files] cd /Users/user/sp2/" in seg, "the jump did not navigate to the hit's parent"
    time.sleep(0.4)
    t.screenshot("/tmp/tos_jobs_jump.ppm")           # landed in the parent, hit selected

    s = t.serial()
    assert "EXCEPTION" not in s and "PANIC" not in s
    print("OK: copy job (conflict Replace/Keep Both, progress, canaries) + recursive search verified")

with Tos(uefi=False) as t:
    try:
        _main(t)
    except Exception:
        with open("/tmp/repro_jobs_serial.txt", "w") as f:
            f.write(t.serial())
        print("---- serial saved to /tmp/repro_jobs_serial.txt ----")
        raise
