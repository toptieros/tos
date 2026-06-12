"""Repro for serial console *input* (headless operation, roadmap Phase 4 "real serial
console for input"). COM1 RX now raises IRQ4; the handler feeds bytes into the same
key ring the PS/2 keyboard uses (CR/CRLF -> Enter, DEL -> backspace), so the shell can
be driven entirely over the serial line.

Boots tOS with COM1 on a bidirectional socket, opens a terminal via the QEMU monitor
(to focus a shell -- serial input is a plain text stream, it can't click the dock),
then TYPES `ls /` over the serial line and checks the listing comes back. The output
contains filesystem entries we did NOT type, so it proves the command actually RAN,
not merely that keystrokes were echoed. Not part of make test (socket serial backend)."""
import sys, time
sys.path.insert(0, "tests")
from harness import Tos

with Tos(uefi=False, serial_socket=True) as t:
    assert t.open_terminal(), "terminal/shell did not come up"
    before = len(t.serial())
    t.serial_send("ls /\r")                       # type over COM1; CR = Enter to the shell

    ok = False
    deadline = time.time() + 15
    while time.time() < deadline:
        new = t.serial()[before:]
        if "System" in new and "Apps" in new and "Users" in new:
            ok = True
            break
        time.sleep(0.2)
    assert ok, f"serial-typed `ls /` did not run (new serial: {t.serial()[before:][:400]!r})"

    s = t.serial()
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during serial input"
    print("OK: typed `ls /` over the serial line and the shell listed the root -- "
          "headless serial input works")
