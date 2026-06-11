"""One-off repro for the AHCI/SATA driver (roadmap Phase 4 #2). Boots BIOS with an
AHCI HBA (`-device ich9-ahci`) and a blank SATA disk attached to it, and checks the
kernel's boot-time self-test: it finds the HBA on PCI (class 01.06), maps the ABAR
(MMIO, in the PCI hole -> vmm_map_mmio), brings up the first port with a SATA disk
(command list + received-FIS + a slot-0 command table), IDENTIFYs it for capacity,
and round-trips a sector over DMA (READ/WRITE DMA EXT) through the generic block
layer (save -> write a pattern -> read back + compare -> restore). A passing run
prints "[ahci] up: port <n>, <sectors> sectors" and "[ahci] selftest OK", and the
IDE boot disk still mounts (no regression). Kernel-level self-test, driven from
outside; not part of make test (the smoke tier boots without an AHCI disk)."""
import sys, os, re
sys.path.insert(0, "tests")
from harness import Tos

SCRATCH = "/tmp/tos_ahci_scratch.img"

# 16 MiB blank SATA disk -> 32768 sectors. The self-test is non-destructive (it
# restores the sector it touches), so a fresh blank image is just for clarity.
with open(SCRATCH, "wb") as f:
    f.truncate(16 * 1024 * 1024)

ahci = [
    "-device", "ich9-ahci,id=ahci",
    "-drive",  f"if=none,id=sata0,format=raw,file={SCRATCH}",
    "-device", "ide-hd,drive=sata0,bus=ahci.0",
]

with Tos(uefi=False, extra_args=ahci) as t:
    assert t.wait_for("[ahci] up:", 25), "AHCI port was not brought up"
    assert "32768 sectors" in t.serial(), "unexpected reported capacity (IDENTIFY)"
    # the self-test runs THROUGH the generic block layer (bdev_find/read/write)
    assert t.wait_for("[ahci] selftest OK", 10), "AHCI DMA self-test failed"
    # both disks register in the block-device layer (ATA boot disk + the SATA target)
    assert t.wait_for("[bdev] ahci0 32768 sectors", 10), "ahci0 did not register in the block layer"
    assert re.search(r"\[bdev\] ata0 \d+ sectors", t.serial()), "ata0 did not register (IDENTIFY)"
    # the normal IDE boot path is untouched
    assert t.wait_for("[kernel] mounted tosfs from disk", 10), "IDE boot disk did not mount"
    s = t.serial()
    assert "[ahci] selftest FAIL" not in s, "self-test reported a failure"
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during AHCI bring-up"
    print("OK: AHCI up (32768 sectors), DMA round-trip self-test passed, IDE boot intact")

os.remove(SCRATCH)
