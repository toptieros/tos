"""Boot tOS under QEMU headless and drive it, for integration tests.

Each Tos() boots the OS with two IDE disks (the boot medium + a scratch copy of
the FS image), exposes the QEMU monitor to inject keystrokes, and captures the
serial console to a file so tests can assert on output. Keep one instance alive
at a time; call stop() when done (use it as a context manager).
"""
import os
import re
import shutil
import socket
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

OVMF_CODE = "/usr/share/edk2/x64/OVMF_CODE.4m.fd"
OVMF_VARS = "/usr/share/edk2/x64/OVMF_VARS.4m.fd"

# sendkey names for non-alphanumeric characters
_KEYMAP = {
    " ": "spc", "\n": "ret", "\t": "tab",
    ".": "dot", "-": "minus", "/": "slash", ",": "comma",
    ";": "semicolon", "=": "equal",
}


def _qmp_path(name):
    return os.path.join("/tmp", name)


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Tos:
    def __init__(self, uefi=False, scratch=None, reuse=False, port=None, cpus=1, mem=None):
        self.uefi = uefi
        self.cpus = cpus
        self.mem = mem
        self.port = port if port is not None else _free_port()
        self.serial_path = _qmp_path(f"tos_test_serial_{self.port}.log")
        if os.path.exists(self.serial_path):
            os.remove(self.serial_path)

        # One disk: a scratch copy of the boot disk (which carries the tosfs
        # partition). Writes land in the copy, so the build image is untouched
        # and a reused scratch persists across reboots.
        boot_src = os.path.join(ROOT, "build", "uefi.img" if uefi else "tOS.img")
        self._auto_scratch = scratch is None    # auto temp files are ours to delete on stop()
        if scratch is None:
            scratch = _qmp_path(f"tos_test_disk_{self.port}.img")
        self.scratch = scratch
        self.vars = None
        if not (reuse and os.path.exists(scratch)):
            shutil.copy(boot_src, scratch)

        common = [
            "-drive", f"format=raw,file={scratch},if=ide,index=0",
            "-smp", str(self.cpus),
            "-no-reboot", "-display", "none",
            "-serial", f"file:{self.serial_path}",
            "-monitor", f"telnet:127.0.0.1:{self.port},server,nowait",
        ]
        if self.mem is not None:
            common += ["-m", str(self.mem)]
        if uefi:
            self.vars = _qmp_path(f"tos_test_OVMF_VARS_{self.port}.fd")
            shutil.copy(OVMF_VARS, self.vars)
            cmd = ["qemu-system-x86_64",
                   "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
                   "-drive", f"if=pflash,format=raw,file={self.vars}"] + common
        else:
            cmd = ["qemu-system-x86_64"] + common

        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self._connect_monitor()

    def _connect_monitor(self):
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                self.mon = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                self.mon.settimeout(2)
                time.sleep(0.2)
                try:
                    self.mon.recv(4096)   # banner
                except Exception:
                    pass
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("could not connect to QEMU monitor")

    def serial(self):
        try:
            with open(self.serial_path, "rb") as f:
                return f.read().decode("latin1")
        except FileNotFoundError:
            return ""

    def wait_for(self, text, timeout=10):
        """Block until `text` appears in the serial log; return True/False."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if text in self.serial():
                return True
            time.sleep(0.1)
        return False

    def key(self, name, delay=0.05):
        self.mon.sendall(f"sendkey {name}\n".encode())
        time.sleep(delay)

    def type(self, s, delay=0.05):
        for c in s:
            if c.isupper():
                self.key(f"shift-{c.lower()}", delay)
            else:
                self.key(_KEYMAP.get(c, c), delay)

    def line(self, s):
        """Type a command and press Enter."""
        self.type(s)
        self.key("ret")

    def line_for(self, s, text, attempts=3, timeout=5):
        """Type command `s`, then wait for `text` in the serial log; if it doesn't
        appear, clear any partial line (a bare Enter) and retype, up to `attempts`.
        Robust against a keystroke dropped when the host is heavily loaded (which
        would otherwise garble the command and hang the test). Returns True/False."""
        for _ in range(attempts):
            self.line(s)
            if self.wait_for(text, timeout):
                return True
            self.key("ret")    # discard a possibly half-typed line -> fresh prompt
        return False

    def mouse_to(self, x, y):
        """Move the (relative PS/2) cursor to an absolute position. PS/2 packets
        cap a delta at +-255, so move in small steps: pin to the top-left corner,
        then step out to (x, y)."""
        for _ in range(14):
            self.mon.sendall(b"mouse_move -120 -120\n"); time.sleep(0.008)
        time.sleep(0.05)
        sx = x
        while sx > 0:
            d = min(sx, 120); self.mon.sendall(f"mouse_move {d} 0\n".encode()); sx -= d; time.sleep(0.008)
        sy = y
        while sy > 0:
            d = min(sy, 120); self.mon.sendall(f"mouse_move 0 {d}\n".encode()); sy -= d; time.sleep(0.008)
        time.sleep(0.05)

    def doubleclick(self, x, y):
        self.mouse_to(x, y)
        for _ in range(2):
            self.mon.sendall(b"mouse_button 1\n"); time.sleep(0.05)
            self.mon.sendall(b"mouse_button 0\n"); time.sleep(0.05)

    def wheel(self, dz, n=1):
        """Spin the scroll wheel: dz>0 = up, dz<0 = down (QEMU mouse_move's 3rd arg)."""
        for _ in range(n):
            self.mon.sendall(f"mouse_move 0 0 {dz}\n".encode()); time.sleep(0.03)

    def click(self, x, y):
        self.mouse_to(x, y)
        self.mon.sendall(b"mouse_button 1\n"); time.sleep(0.05)
        self.mon.sendall(b"mouse_button 0\n"); time.sleep(0.05)

    def rightclick(self, x, y):
        """Right-click (QEMU button bit 1 = the PS/2 right button), which the toolkit
        delivers as on_context -> the app's context menu."""
        self.mouse_to(x, y)
        self.mon.sendall(b"mouse_button 2\n"); time.sleep(0.05)
        self.mon.sendall(b"mouse_button 0\n"); time.sleep(0.05)

    def screenshot(self, path):
        """Dump the framebuffer to a PPM via the QEMU monitor (for visual checks)."""
        self.mon.sendall(f"screendump {path}\n".encode())
        time.sleep(0.4)
        return path

    def win_rect(self, title, timeout=12):
        """twm prints each window's placed on-screen rect ("[twm] win <title> <wx>
        <wy> <w> <h>"), so tests can drive a window by its real geometry. Returns
        (wx, wy, w, h) or None."""
        pat = re.compile(r"\[twm\] win %s (\d+) (\d+) (\d+) (\d+)" % re.escape(title))
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = pat.search(self.serial())
            if m:
                return tuple(int(g) for g in m.groups())
            time.sleep(0.1)
        return None

    def drag(self, x0, y0, x1, y1, steps=8):
        """Press at (x0,y0), then move to (x1,y1) with the button held via small
        RELATIVE deltas (so the compositor emits WEV_MOUSE_DRAG packets that keep
        the button bit set), then release. mouse_to establishes the absolute start
        with the button up; we must not re-pin during the drag."""
        self.mouse_to(x0, y0)
        self.mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        dx, dy = x1 - x0, y1 - y0
        for i in range(steps):
            sx = dx // steps + (1 if i < dx % steps else 0)
            sy = dy // steps + (1 if i < dy % steps else 0)
            self.mon.sendall(f"mouse_move {sx} {sy}\n".encode()); time.sleep(0.04)
        self.mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)

    def icon_xy(self, label, timeout=15):
        """twm prints each dock launcher's centre to serial ("[twm] icon <label>
        <x> <y>"), so tests drive the dock by the actual layout/resolution rather
        than hardcoded pixels."""
        pat = re.compile(r"\[twm\] icon %s (\d+) (\d+)" % re.escape(label))
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = pat.search(self.serial())
            if m:
                return int(m.group(1)), int(m.group(2))
            time.sleep(0.1)
        return None

    def open_terminal(self):
        """Double-click the Terminal launcher in the dock to open it, then wait
        for its shell. The dock coordinates come from twm's serial output."""
        # Generous waits: under full-suite load the host is running back-to-back
        # QEMU boots, so the desktop/shell can take noticeably longer to come up
        # than in isolation. wait_for returns the instant the line appears, so the
        # headroom is free on a fast boot and only spent on a contended one.
        if not self.wait_for("[twm] desktop ready", timeout=25):
            return False
        xy = self.icon_xy("Terminal")
        if not xy:
            return False
        self.doubleclick(*xy)
        return self.wait_for("Welcome to the tOS shell", timeout=20)

    def stop(self):
        try:
            self.mon.close()
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()
        # Clean up the per-run temp files we created so they don't pile up in
        # /tmp (which is tmpfs/RAM-backed) across many runs. A caller-supplied
        # scratch path (e.g. a persistence test) is left for the caller to manage.
        for p in (self.serial_path, self.vars,
                  self.scratch if self._auto_scratch else None):
            if p:
                try: os.remove(p)
                except OSError: pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.stop()

    def boot_ok(self):
        """Bring the desktop up and open a terminal; True if its shell came up
        cleanly. (The terminal is launched from a desktop icon now.)"""
        ok = self.open_terminal()
        s = self.serial()
        return ok and "[EXCEPTION]" not in s and "PANIC" not in s
