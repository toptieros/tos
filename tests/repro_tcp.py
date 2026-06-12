"""One-off repro for the TCP client (roadmap Phase 4 #7, top of the net stack). Starts
a host TCP echo server on port 7777, then boots BIOS with QEMU user-mode networking.
SLIRP aliases the host at 10.0.2.2, so the guest's boot self-test connects to
10.0.2.2:7777, completes the 3-way handshake, sends "tOS-tcp", reads the echo back,
and closes (FIN) -- exercising the mandatory pseudo-header checksum and seq/ack
arithmetic. A passing run prints "[net] TCP echo OK". Proves a real TCP conversation
end-to-end. Not part of make test (needs the external echo server)."""
import sys, socket, threading, re
sys.path.insert(0, "tests")
from harness import Tos

stop = threading.Event()
got = []

def echo_server(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    srv.settimeout(0.5)
    while not stop.is_set():
        try:
            c, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        with c:
            c.settimeout(3)
            try:
                data = c.recv(64)
                if data:
                    got.append(data)
                    c.sendall(data)          # echo it back
            except OSError:
                pass
    srv.close()

th = threading.Thread(target=echo_server, args=(7777,), daemon=True)
th.start()

net = ["-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"]
try:
    with Tos(uefi=False, extra_args=net) as t:
        assert t.wait_for("[net] DHCP lease", 25), "net stack did not come up"
        ok = t.wait_for("[net] TCP echo OK", 20)
        s = t.serial()
        if not ok:
            skipped = "[net] TCP: no echo server (skipped)" in s
            failed  = "[net] TCP echo FAIL" in s
            raise AssertionError(
                "TCP echo did not succeed; "
                + ("connect never reached the host server (SLIRP/routing)" if skipped
                   else "echo mismatch" if failed else "no TCP result line")
                + f"  [server saw: {got}]")
        assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during TCP test"
        assert got and got[0] == b"tOS-tcp", f"server did not receive the expected bytes: {got}"
        print(f"OK: TCP handshake + echo round-trip works (server received {got[0]!r})")
finally:
    stop.set()
    th.join(timeout=2)
