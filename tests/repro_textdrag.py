"""One-off repro for cross-app text drag (DRAG_TEXT) + MOVE semantics + the X11-style
PRIMARY selection (middle-click paste). Boots BIOS, opens Notepad, types a line,
keyboard-selects the word "world", then PRESSES inside the selection and drags it
left to column 0. That exercises the whole text-DnD path:
  source  TextField::drag_to -> begin_drag(DRAG_TEXT)        ([ui] textdrag 5)
  session twm reads drag_state, draws the ghost, posts WEV_DROP ([twm] drag begin)
  target  Window::on_drop -> TextField::accept_text_drop ins ([ui] textdrop 5 move)
A drop back into the SAME field MOVES the text (deletes the source), so "hello world"
becomes "worldhello " (11 B). The moved text stays selected -> it is now the PRIMARY
selection, which a middle-click pastes at the click point. The protocol is app-agnostic
(payload lives in the kernel, twm routes WEV_DROP to the window under the cursor), so
within-Notepad proves the same path a cross-app drop takes. Screenshots the translucent
text ghost mid-drag. Not part of make test."""
import sys, time
sys.path.insert(0, "tests")
from harness import Tos
from run_tests import _accept_save_picker

GHOST = "/tmp/tos_textdrag_ghost.ppm"
DROPPED = "/tmp/tos_textdrag_dropped.ppm"

with Tos(uefi=False) as t:
    assert t.open_terminal(), "desktop/terminal did not come up"
    # Launch Notepad via Spotlight (it is no longer dock-pinned).
    t.key("meta_l-spc", delay=0.1)
    assert t.wait_for("[spotlight] up", 8), "Spotlight did not open"
    t.type("note", delay=0.06)
    t.key("ret", delay=0.1)
    assert t.wait_for("[notepad] up", 12), "Notepad did not launch"
    assert t.wait_for("[twm] focus Notepad", 8), "Notepad did not take focus"
    wr = t.win_rect("Notepad"); assert wr, "no Notepad window rect"
    wx, wy = wr[0], wr[1]

    # Type a known line; caret ends at column 11 (end of "world").
    t.type("hello world", delay=0.06)
    # Select "world" (cols 6..10) with 5x Shift+Left from the end of the line.
    for _ in range(5):
        t.key("shift-left", delay=0.06)
    assert t.wait_for("[ui] shsel 6 11", 6), "keyboard selection of 'world' failed"

    # Editor geometry: row 0, char i -> screen (wx + TF_PAD + i*fw, wy + TBH + TF_PAD)
    # with TF_PAD=6, fw=9 (SYSFONT_W), TBH=fh+16=35 (fh=19). Mid-glyph y = +9.
    def cx(i): return wx + 6 + 9 * i
    cy = wy + 41 + 9
    sx, drop_x = cx(8), cx(0)            # press inside the selection, drop at column 0

    # Press inside the selection, then crawl left in small relative steps so the
    # held-button bit stays set (mouse_to pins to a corner with the button UP, so
    # we must not re-pin during the drag).
    t.mouse_to(sx, cy)
    t.mon.sendall(b"mouse_button 1\n"); time.sleep(0.10)
    ddx, steps = drop_x - sx, 10
    for i in range(steps):
        s = ddx // steps + (1 if i < (ddx % steps) else 0)   # floor-div sums to ddx (ddx<0)
        t.mon.sendall(f"mouse_move {s} 0\n".encode()); time.sleep(0.05)
    time.sleep(0.2)

    assert t.wait_for("[twm] drag begin", 6), "twm did not start a drag session"
    assert t.wait_for("[ui] textdrag 5", 6), "TextField did not arm a 5-char DRAG_TEXT payload"
    t.screenshot(GHOST)                  # capture the translucent text ghost mid-drag
    t.mon.sendall(b"mouse_button 0\n"); time.sleep(0.15)     # drop
    assert t.wait_for("[ui] textdrop 5 move", 6), "same-field drop did not MOVE the text"
    t.screenshot(DROPPED)

    # End-to-end proof: MOVE semantics make "hello world" -> "worldhello " (11 B).
    t.key("ctrl-s", delay=0.12)
    _accept_save_picker(t)
    assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (11 bytes)", 8), \
        "moved text did not land in the buffer (expected an 11-byte save)"

    # PRIMARY selection: the moved "world" stayed selected, so a middle-click at the
    # end of the line pastes it there ("worldhello " -> "worldhello world", 16 B).
    t.middleclick(cx(11), cy)            # end of "worldhello " is column 11
    assert t.wait_for("[ui] primary paste 5", 6), "middle-click did not paste the primary selection"
    t.key("ctrl-s", delay=0.12)
    assert t.wait_for("[notepad] saved /Users/user/Documents/untitled.txt (16 bytes)", 8), \
        "primary paste did not extend the buffer (expected a 16-byte save)"

    s = t.serial()
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during text drag"
    print("OK: text drag armed/ghosted/dropped (MOVE) -> 11 B; middle-click primary paste -> 16 B")
    print("ghost screenshot:   " + GHOST)
    print("dropped screenshot: " + DROPPED)
