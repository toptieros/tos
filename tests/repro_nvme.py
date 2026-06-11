"""One-off repro for the NVMe driver (roadmap Phase 4 #3). Boots BIOS with an NVMe
controller (`-device nvme`) and a blank namespace attached, and checks the kernel's
boot-time self-test: it finds the controller on PCI (class 01.08), maps the register
block (MMIO, in the PCI hole -> vmm_map_mmio), disables/re-enables the controller,
sets up the admin queue pair + one I/O queue pair (Create I/O CQ/SQ), IDENTIFYs
namespace 1 for capacity + LBA size, and round-trips a sector over DMA (NVM Read/Write
with PRPs) through the generic block layer (save -> write a pattern -> read back +
compare -> restore). A passing run prints "[nvme] up: <sectors> sectors" and
"[nvme] selftest OK", and the IDE boot disk still mounts (no regression). Kernel-level
self-test; not part of make test (the smoke tier boots without an NVMe disk)."""
import sys, os, re
sys.path.insert(0, "tests")
from harness import Tos

SCRATCH = "/tmp/tos_nvme_scratch.img"

# 16 MiB blank namespace -> 32768 512-byte LBAs. The self-test is non-destructive.
with open(SCRATCH, "wb") as f:
    f.truncate(16 * 1024 * 1024)

nvme = [
    "-drive",  f"if=none,id=nvm,format=raw,file={SCRATCH}",
    "-device", "nvme,drive=nvm,serial=tos0nvme",
]

with Tos(uefi=False, extra_args=nvme) as t:
    assert t.wait_for("[nvme] up:", 25), "NVMe controller was not brought up"
    assert "32768 sectors" in t.serial(), "unexpected reported capacity (IDENTIFY)"
    # the self-test runs THROUGH the generic block layer (bdev_find/read/write)
    assert t.wait_for("[nvme] selftest OK", 10), "NVMe DMA self-test failed"
    # both disks register in the block-device layer (ATA boot disk + the NVMe namespace)
    assert t.wait_for("[bdev] nvme0 32768 sectors", 10), "nvme0 did not register in the block layer"
    assert re.search(r"\[bdev\] ata0 \d+ sectors", t.serial()), "ata0 did not register (IDENTIFY)"
    # the normal IDE boot path is untouched
    assert t.wait_for("[kernel] mounted tosfs from disk", 10), "IDE boot disk did not mount"
    s = t.serial()
    assert "[nvme] selftest FAIL" not in s, "self-test reported a failure"
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during NVMe bring-up"
    print("OK: NVMe up (32768 sectors), DMA round-trip self-test passed, IDE boot intact")

os.remove(SCRATCH)
