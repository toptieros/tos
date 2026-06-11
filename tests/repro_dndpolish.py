"""One-off repro for the DnD polish follow-ons: Esc-to-cancel a drag, and copy-on-Ctrl.
Boots BIOS, builds ~/dnd/{file.txt, zone}, opens Files there, then:
  1. starts dragging file.txt and presses Esc mid-flight -> "[twm] drag cancel",
     no drop fires (the file stays put);
  2. drags file.txt onto the `zone` folder with Ctrl HELD (sendkey hold-time) ->
     "[files] dropped-copy ... into zone"; the SOURCE survives (copy, not move).
Screenshots the Ctrl-drop ghost. Not part of make test."""
import sys, re, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import _files_row_xy, _count_at_least

COPY = "/tmp/tos_dndpolish_copy.ppm"

def row_named(t, wr, name, maxrows=6):
    """List row index whose selection canary reports `name` (sort-order agnostic)."""
    for r in range(1, maxrows):
        mark = len(t.serial())
        t.click(*_files_row_xy(t, wr, r))
        m = re.search(r"\[files\] sel (\S+)", t.serial()[mark:])
        if m and m.group(1) == name:
            return r
    return None

with Tos(uefi=False) as t:
    assert t.open_terminal(), "desktop/terminal did not come up"
    # A tiny deterministic folder: ~/dnd/{file.txt, zone}.
    t.line("mkdir dnd"); t.line("mkdir dnd/zone")
    t.line("write dnd/file.txt"); assert t.wait_for("enter a line", 6), "write did not prompt"
    t.line("hello"); assert t.wait_for("saved dnd/file.txt", 6), "write did not save"

    xy = t.icon_xy("Files"); t.doubleclick(*xy)
    assert t.wait_for("[files] file manager up", 12), "Files did not launch"
    assert t.wait_for("[twm] focus Files", 8), "Files did not take focus"
    wr = t.win_rect("Files"); assert wr, "no Files window rect"
    assert t.wait_for("[files] cd /Users/user", 8), "Files did not land at home"

    drow = row_named(t, wr, "dnd"); assert drow, "dnd folder not listed"
    t.doubleclick(*_files_row_xy(t, wr, drow))
    assert t.wait_for("[files] cd /Users/user/dnd", 8), "did not enter ~/dnd"
    frow = row_named(t, wr, "file.txt"); assert frow, "file.txt not listed"
    zrow = row_named(t, wr, "zone");     assert zrow, "zone folder not listed"
    fxy, zxy = _files_row_xy(t, wr, frow), _files_row_xy(t, wr, zrow)

    # --- (1) Esc cancels an armed drag; no drop fires ---
    t.click(*fxy); assert t.wait_for("[files] sel file.txt", 6), "could not select file.txt"
    mark = len(t.serial())
    begins = t.serial().count("[twm] drag begin")   # count-based: "drag begin" recurs, substring is stale
    t.mouse_to(*fxy)
    t.mon.sendall(b"mouse_button 1\n"); time.sleep(0.1)
    ddx, ddy = zxy[0] - fxy[0], zxy[1] - fxy[1]
    for i in range(6):                              # partial move: arm + hover the folder
        t.mon.sendall(f"mouse_move {ddx // 8} {ddy // 8}\n".encode()); time.sleep(0.05)
    assert _count_at_least(t, "[twm] drag begin", begins + 1, 6), "the file drag did not arm"
    t.key("esc")
    assert t.wait_for("[twm] drag cancel", 6), "Esc did not cancel the drag"
    t.mon.sendall(b"mouse_button 0\n"); time.sleep(0.3)    # release: must NOT become a drop
    assert "[files] dropped" not in t.serial()[mark:], "a drop fired despite the Esc cancel"

    # --- (2) Ctrl+drop copies (the source survives) ---
    t.click(*fxy); assert t.wait_for("[files] sel file.txt", 6), "could not re-select file.txt"
    begins = t.serial().count("[twm] drag begin")
    t.mon.sendall(b"sendkey ctrl 6000\n"); time.sleep(0.05)   # hold Ctrl through the whole drop
    t.mouse_to(*fxy)
    t.mon.sendall(b"mouse_button 1\n"); time.sleep(0.1)
    steps = 8
    for i in range(steps):
        mx = ddx // steps + (1 if i < ddx % steps else 0)
        my = ddy // steps + (1 if i < ddy % steps else 0)
        t.mon.sendall(f"mouse_move {mx} {my}\n".encode()); time.sleep(0.05)
    time.sleep(0.1)
    assert _count_at_least(t, "[twm] drag begin", begins + 1, 6), "the copy drag did not arm"
    t.screenshot(COPY)
    t.mon.sendall(b"mouse_button 0\n"); time.sleep(0.2)       # drop, Ctrl still held
    assert t.wait_for("[files] dropped-copy file.txt into zone", 6), "Ctrl+drop did not copy"
    assert row_named(t, wr, "file.txt"), "source file.txt vanished -- that was a move, not a copy"

    s = t.serial()
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during DnD polish"
    print("OK: Esc cancelled an armed drag (no drop); Ctrl+drop copied (source survived)")
    print("copy-drag screenshot: " + COPY)
