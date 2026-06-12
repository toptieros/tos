"""One-off repro for the Intel e1000 NIC (roadmap Phase 4 #7, the second NIC). Boots
BIOS with an `-device e1000` (82540EM) on QEMU's user-mode network and NO virtio NIC,
so the e1000 becomes the active netif. Proves two things at once: the e1000 driver
itself (MMIO BAR0, legacy RX/TX descriptor rings, MAC from RAL/RAH) does a full ARP
round-trip -- a frame OUT and a frame IN -- and the NIC-agnostic stack (net/netif.h)
drives it: `[net] NIC e1000`, a DHCP lease, and an ICMP ping reply, exactly the same
code path virtio-net uses. The IDE boot disk still mounts (no regression).
Kernel-level self-test; not part of make test (smoke boots without a NIC)."""
import sys, re
sys.path.insert(0, "tests")
from harness import Tos

net = [
    "-netdev", "user,id=n0",
    "-device", "e1000,netdev=n0",
]

with Tos(uefi=False, extra_args=net) as t:
    assert t.wait_for("[virtio-net] none", 25), "virtio-net should be absent in this probe"
    assert t.wait_for("[e1000] up: MAC", 10), "e1000 device was not brought up"
    s = t.serial()
    assert re.search(r"\[e1000\] up: MAC ([0-9a-f]{2}:){5}[0-9a-f]{2}", s), \
        "MAC not read from the e1000 receive-address / EEPROM"
    # driver contract: a frame went OUT (ARP request) and a frame came IN (ARP reply)
    assert t.wait_for("[e1000] ARP reply: 10.0.2.2 is at", 15), \
        "no ARP reply from the SLIRP gateway (e1000 RX or TX broken)"
    assert t.wait_for("[e1000] selftest OK", 5), "e1000 self-test failed"
    # the stack picked the e1000 as the active netif (no virtio present)
    assert t.wait_for("[net] NIC e1000", 5), "stack did not select the e1000 netif"
    # UDP + DHCP over e1000: lease an address dynamically from the SLIRP DHCP server
    assert t.wait_for("[net] DHCP lease 10.0.2.15", 20), "DHCP did not lease over e1000"
    # L3 over e1000: an ICMP echo to the gateway returns a reply (ARP -> IPv4 -> ICMP)
    assert t.wait_for("[net] ping 10.0.2.2: reply", 15), "ICMP ping over e1000 got no reply"
    assert t.wait_for("[net] selftest OK", 5), "net (IP/ICMP) self-test failed over e1000"
    # the normal IDE boot path is untouched
    assert t.wait_for("[kernel] mounted tosfs from disk", 10), "IDE boot disk did not mount"
    s = t.serial()
    assert "[e1000] selftest FAIL" not in s and "[net] selftest FAIL" not in s, "a self-test failed"
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during e1000 bring-up"
    gw = re.search(r"10\.0\.2\.2 is at (([0-9a-f]{2}:){5}[0-9a-f]{2})", s)
    print(f"OK: e1000 up, ARP round-trip OK (gateway MAC {gw.group(1)}), "
          "DHCP + ICMP ping over e1000, IDE boot intact")
