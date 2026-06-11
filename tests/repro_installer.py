"""End-to-end repro for the live->disk installer (#11), built on virtio-blk + the
block-device layer. Two phases:

  Phase 1  Boot from the IDE disk with a BLANK virtio-blk disk attached. In the
           shell, run `install`: the kernel clones the boot disk (bdev0/ata0) onto
           the virtio target (bdev1/virtio0) over DMA, then verifies the MBR read
           back. Expect "[install] done, verify OK" + "installed 6144 sectors".

  Phase 2  Boot a fresh VM using the NOW-WRITTEN virtio image as the IDE boot disk.
           If the installer wrote a complete, valid tOS disk, it mounts tosfs and
           brings the desktop up -- proving the clone is independently bootable.

(Booting the clone as IDE, not virtio, sidesteps mounting root from virtio-blk --
that needs fs-on-bdev, a separate step -- while still proving the install WROTE a
correct bootable disk over virtio DMA.) Not part of make test."""
import sys, os, time, shutil, socket, subprocess
sys.path.insert(0, "tests")
from harness import Tos, _free_port

TARGET = "/tmp/tos_installer_target.img"
TARGET_V = "/tmp/tos_installer_target_virtio.img"


def boot_virtio_only(image, want, timeout=30):
    """Boot a VM with `image` as the ONLY disk, attached as virtio-blk -- so the
    firmware boots from virtio and the kernel must mount root from virtio0 too
    (fs-on-bdev). Returns the serial text once every string in `want` appears, or
    raises on timeout. A direct QEMU run: the Tos harness always wires an IDE disk."""
    log = f"/tmp/tos_virtioboot_{_free_port()}.log"
    if os.path.exists(log):
        os.remove(log)
    proc = subprocess.Popen(
        ["qemu-system-x86_64",
         "-drive", f"format=raw,file={image},if=virtio",
         "-smp", "1", "-no-reboot", "-display", "none",
         "-serial", f"file:{log}", "-monitor", "none"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                s = open(log, "rb").read().decode("latin1")
            except FileNotFoundError:
                s = ""
            if all(w in s for w in want):
                return s
            if "PANIC" in s or "[EXCEPTION]" in s:
                raise AssertionError("the virtio-booted install faulted:\n" + s[-800:])
            time.sleep(0.2)
        raise AssertionError("virtio boot did not reach %r; tail:\n%s" % (want, s[-800:]))
    finally:
        proc.terminate()
        try: proc.wait(timeout=3)
        except Exception: proc.kill()
        try: os.remove(log)
        except OSError: pass

# A blank 16 MiB virtio target (>= the 6144-sector / 3 MiB boot disk).
with open(TARGET, "wb") as f:
    f.truncate(16 * 1024 * 1024)

# ---- Phase 1: install onto the virtio disk ----
with Tos(uefi=False, extra_args=["-drive", f"format=raw,file={TARGET},if=virtio"]) as t:
    assert t.open_terminal(), "desktop/terminal did not come up"
    assert t.wait_for("[bdev] virtio0 32768 sectors", 10), "virtio target not registered"
    t.line("install")
    # the clone is ~6144 PIO-read sectors -> generous timeout
    assert t.wait_for("[install] cloning ata0 -> virtio0", 15), "installer did not start"
    assert t.wait_for("[install] done, verify OK", 90), "install did not finish / verify failed"
    assert t.wait_for("installed 6144 sectors", 10), "shell did not report the sector count"
    s = t.serial()
    assert "[install] done, verify FAIL" not in s and "[EXCEPTION]" not in s and "PANIC" not in s
print("Phase 1 OK: cloned the boot disk onto virtio-blk (verify OK)")
shutil.copy(TARGET, TARGET_V)        # an untouched copy for the virtio-boot phase

# ---- Phase 2: boot the written image as IDE -> the clone is a valid bootable disk ----
with Tos(uefi=False, scratch=TARGET, reuse=True) as t:   # reuse=True -> boot TARGET as-is
    assert t.wait_for("[kernel] mounted tosfs from disk", 20), "the installed disk did not mount tosfs"
    assert t.wait_for("[twm] desktop ready", 25), "the installed disk did not reach the desktop"
    s = t.serial()
    assert "[EXCEPTION]" not in s and "PANIC" not in s, "the installed disk faulted on boot"
print("Phase 2 OK: the installed disk booted tOS as IDE (tosfs mounted, desktop up)")

# ---- Phase 3: boot the written image AS VIRTIO -> the kernel mounts root from virtio0 ----
# This is the true install boot: firmware boots virtio-blk, and fs-on-bdev finds the
# tosfs partition on virtio0 (there is no IDE disk at all).
s = boot_virtio_only(TARGET_V,
                     ["[kernel] mounted tosfs from disk", "[twm] desktop ready"],
                     timeout=35)
assert "[bdev] virtio0" in s, "virtio0 was not the disk the kernel saw"
print("Phase 3 OK: booted the install directly off virtio-blk (root mounted from virtio0)")

for p in (TARGET, TARGET_V):
    try: os.remove(p)
    except OSError: pass
print("OK: live -> disk install verified end-to-end (clone + boot-as-IDE + boot-as-virtio)")
