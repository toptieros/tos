"""One-off repro for the virtio-blk driver (roadmap Phase 4 #1). Boots BIOS with an
extra blank virtio-blk disk attached (`if=virtio`), and checks the kernel's boot-time
self-test: it probes PCI, negotiates the legacy virtio-pci device, sets up the request
virtqueue, and round-trips a sector over DMA (save -> write a pattern -> read back +
compare -> restore). A passing run prints "[virtio-blk] up: <n> sectors ..." and
"[virtio-blk] selftest OK", and the IDE boot disk still mounts (no regression).
This is a kernel-level self-test (the Linux-style in-kernel check), driven from outside.
Not part of make test (the smoke tier boots without a virtio disk)."""
import sys, os, re, time
sys.path.insert(0, "tests")
from harness import Tos

SCRATCH = "/tmp/tos_virtioblk_scratch.img"

# A 16 MiB blank disk -> 32768 sectors. The driver's self-test is non-destructive
# (it restores the sector it touches), so a fresh blank image is just for clarity.
with open(SCRATCH, "wb") as f:
    f.truncate(16 * 1024 * 1024)

with Tos(uefi=False, extra_args=["-drive", f"format=raw,file={SCRATCH},if=virtio"]) as t:
    assert t.wait_for("[virtio-blk] up:", 25), "virtio-blk device was not brought up"
    # the legacy device reports 32768 512-byte sectors for a 16 MiB disk
    assert "[virtio-blk] up: 32768 sectors" in t.serial(), "unexpected reported capacity"
    # the self-test runs THROUGH the generic block layer (bdev_find/read/write)
    assert t.wait_for("[virtio-blk] selftest OK", 10), "virtio-blk DMA self-test failed"
    # both disks register in the block-device layer (ATA boot disk + the virtio target)
    assert t.wait_for("[bdev] virtio0 32768 sectors", 10), "virtio0 did not register in the block layer"
    assert re.search(r"\[bdev\] ata0 \d+ sectors", t.serial()), "ata0 did not register (IDENTIFY)"
    # the normal IDE boot path is untouched
    assert t.wait_for("[kernel] mounted tosfs from disk", 10), "IDE boot disk did not mount"
    s = t.serial()
    assert "[virtio-blk] selftest FAIL" not in s, "self-test reported a failure"
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "kernel fault during virtio-blk bring-up"
    print("OK: virtio-blk up (32768 sectors), DMA round-trip self-test passed, IDE boot intact")

os.remove(SCRATCH)
