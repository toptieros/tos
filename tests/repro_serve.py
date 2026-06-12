"""Repro for the TCP *server* path (roadmap Phase 4 #7): tOS doesn't just fetch over
the network, it can serve. Adds passive open -- SYS_NET_LISTEN / SYS_NET_ACCEPT
(CAP_NET-gated) wrapping a one-connection TCP server -- and a shell `serve <port>`
command (listen, accept, read the request, send a small HTML page, close).

Boots tOS with QEMU user-mode networking and a host-forward (host 127.0.0.1:18080 ->
guest :80), drives the shell `serve 80`, then makes a real HTTP request FROM THE HOST
to 127.0.0.1:18080 and checks the page tOS served comes back. Purely local (SLIRP
hostfwd), no internet needed. Not part of make test (external client)."""
import sys, time, urllib.request
sys.path.insert(0, "tests")
from harness import Tos

HOST_PORT = 18080
net = [
    "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{HOST_PORT}-:80",
    "-device", "virtio-net-pci,netdev=n0",
]

with Tos(uefi=False, extra_args=net) as t:
    assert t.boot_ok(), "shell did not come up"
    assert t.wait_for("[net] DHCP lease", 20), "net stack did not initialize"

    t.line("serve 80")                                  # blocks the shell in accept()
    assert t.wait_for("[serve] listening on port 80", 12), "server did not start listening"

    # fetch from the HOST through the SLIRP host-forward -> the guest's listener
    body = b""
    last = None
    for _ in range(20):                                 # the listener is up; connect promptly
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{HOST_PORT}/", timeout=8) as r:
                body = r.read()
            break
        except Exception as e:
            last = e
            time.sleep(0.5)
    assert b"Hello from tOS" in body, f"host did not receive the page tOS served (body={body!r}, last err={last})"

    assert t.wait_for("[serve] served 1 request", 12), "server did not report serving the request"
    s = t.serial()
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during the TCP server path"
    print(f"OK: tOS served an HTTP page over TCP -- host got {len(body)} bytes "
          "('Hello from tOS'); tOS can serve, not just fetch")
