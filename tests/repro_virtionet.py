"""One-off repro for the virtio-net driver (roadmap Phase 4 #7, the first NIC). Boots
BIOS with a virtio-net NIC on QEMU's user-mode network (`-netdev user`), and checks the
kernel's boot self-test: it finds the device on PCI, negotiates the legacy virtio-pci
NIC (VIRTIO_NET_F_MAC only, offloads off), sets up the RX + TX virtqueues, reads the
MAC, then TXes an ARP "who-has 10.0.2.2" (the SLIRP gateway) and polls RX for the
reply. A passing run prints "[virtio-net] up: MAC ..." and "[virtio-net] ARP reply:
10.0.2.2 is at <mac>" / "[virtio-net] selftest OK" -- proving the full driver contract
(a frame OUT and a frame IN) end-to-end. The IDE boot disk still mounts (no regression).
Kernel-level self-test; not part of make test (smoke boots without a NIC)."""
import sys, re
sys.path.insert(0, "tests")
from harness import Tos

net = [
    "-netdev", "user,id=n0",
    "-device", "virtio-net-pci,netdev=n0",
]

with Tos(uefi=False, extra_args=net) as t:
    assert t.wait_for("[virtio-net] up: MAC", 25), "virtio-net device was not brought up"
    s = t.serial()
    assert re.search(r"\[virtio-net\] up: MAC ([0-9a-f]{2}:){5}[0-9a-f]{2}", s), \
        "MAC not read from device config"
    # the round-trip: a frame went OUT (ARP request) and a frame came IN (ARP reply)
    assert t.wait_for("[virtio-net] ARP reply: 10.0.2.2 is at", 15), \
        "no ARP reply from the SLIRP gateway (RX or TX broken)"
    assert t.wait_for("[virtio-net] selftest OK", 5), "virtio-net self-test failed"
    # UDP + DHCP: lease an address dynamically (DORA) from the SLIRP DHCP server
    assert t.wait_for("[net] DHCP lease 10.0.2.15", 20), "DHCP did not lease an address"
    # the L3 stack (net.c): an ICMP echo to the gateway returns a reply (ARP -> IPv4 -> ICMP)
    assert t.wait_for("[net] ping 10.0.2.2: reply", 15), "ICMP ping to the gateway got no reply"
    assert t.wait_for("[net] selftest OK", 5), "net (IP/ICMP) self-test failed"
    # the normal IDE boot path is untouched
    assert t.wait_for("[kernel] mounted tosfs from disk", 10), "IDE boot disk did not mount"
    s = t.serial()
    assert "[virtio-net] selftest FAIL" not in s and "[net] selftest FAIL" not in s, "a self-test failed"
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during virtio-net bring-up"
    gw = re.search(r"10\.0\.2\.2 is at (([0-9a-f]{2}:){5}[0-9a-f]{2})", s)
    print(f"OK: virtio-net up, ARP round-trip OK (gateway MAC {gw.group(1)}), "
          "ICMP ping to gateway OK, IDE boot intact")
