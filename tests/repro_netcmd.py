"""Capstone repro: userspace networking (roadmap Phase 4 exit criterion -- "fetch a
file over the network"). Exposes the kernel TCP/IP stack to apps via CAP_NET-gated
syscalls (SYS_NET_PING/CONNECT/SEND/RECV/CLOSE) and shell `ping`/`get` commands.

Starts a host HTTP server, boots tOS with QEMU user-mode networking, drives the shell:
  - `ping 10.0.2.2`               -> ICMP echo to the SLIRP gateway, expect "reply"
  - `get 10.0.2.2 8000 /hello`    -> an HTTP/1.0 GET over the TCP client; the response
                                     body (a known marker) must appear in the output.
SLIRP aliases the host at 10.0.2.2, so the guest reaches the host server. Proves the
whole stack end-to-end from a userspace program. Not part of make test (external server)."""
import sys, threading
from http.server import BaseHTTPRequestHandler, HTTPServer
sys.path.insert(0, "tests")
from harness import Tos

BODY = b"TOS-NET-FETCH-OK\n"

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)
    def log_message(self, *a):  # quiet
        pass

srv = HTTPServer(("0.0.0.0", 8000), H)
th = threading.Thread(target=srv.serve_forever, daemon=True)
th.start()

net = ["-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"]
try:
    with Tos(uefi=False, extra_args=net) as t:
        assert t.boot_ok(), "shell did not come up"
        assert t.wait_for("[net] DHCP lease", 20), "net stack did not initialize"

        t.line("ping 10.0.2.2")
        assert t.wait_for("reply received", 12), "ping got no reply from the gateway"

        t.line("get 10.0.2.2 8000 /hello")
        assert t.wait_for("TOS-NET-FETCH-OK", 15), "HTTP GET did not return the file body"
        assert t.wait_for("[get] received", 5), "get command did not finish"

        s = t.serial()
        assert "200" in s, "HTTP status line not seen in the response"
        assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during userspace networking"
        print("OK: userspace ping + HTTP GET over TCP work -- tOS fetched a file over the network")
finally:
    srv.shutdown()
    th.join(timeout=2)
