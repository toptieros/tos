# Design guideline — virtual GPU (virtio-gpu)

> Status: **design / roadmap.** Today tOS presents pixels through the **firmware
> framebuffer** — a fixed linear surface handed over by VBE (BIOS) or GOP (UEFI),
> captured in `boot_info` (`kernel/bootinfo.h`) and mapped into the compositor by
> `SYS_FBINFO`. `twm` software-rasterises into it with damage tracking. This doc is
> the plan to add a **virtio-gpu** driver: first as a better *presentation* path
> (real modesetting + a hardware cursor), and later as the **only realistic route to
> GPU acceleration** — which, per [roadmap.md](roadmap.md) Phase 6, is **VM-only**.

## Why virtio-gpu (and why only in a VM)

On bare metal a real GPU driver is a multi-year, per-vendor effort against mostly
undocumented silicon, with a closed userspace stack — out of reach for tOS, so metal
stays on **software rendering over the firmware framebuffer** (and that's fine: a 2D
compositor is memory-bandwidth bound, not compute bound). In a VM the "GPU" is **one
standardised, documented device**, and the host's real driver does the hard part. That
asymmetry is the whole reason this is worth building.

Two levels, shippable independently:

| Level | What it gives | Effort | Where the work happens |
|---|---|---|---|
| **A — 2D presentation** | Real modesetting (pick resolution), a host-composited **hardware cursor**, a clean scanout that isn't the firmware's fixed mode | Modest | guest issues 2D blits; host scans out |
| **B — 3D acceleration** | GL/Vulkan via **virgl** / **Venus** — the guest emits API command streams, the host runs them on its real GPU | Large | host GPU + Mesa |

Level A is the near-term target. Level B is a stretch goal that reuses the same
transport but adds a command-stream protocol; it does **not** block A.

## Where it fits in today's pipeline

```
 today:   twm  --software raster-->  SYS_FBINFO fb (firmware VBE/GOP linear)  --> screen
 level A: twm  --software raster-->  guest backing buffer (RAM)
                                     --TRANSFER_TO_HOST_2D + RESOURCE_FLUSH (per damage rect)--> host --> screen
```

Crucially, **the rasteriser does not change** for Level A. `twm` keeps drawing into a
linear buffer exactly as now; virtio-gpu only changes how that buffer reaches the
screen. The compositor already computes **damage rectangles** (see the back-buffer /
`add_dirty` logic in `user/twm/twm.c`), and those map one-to-one onto the
`TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` rects — so we flush only what changed, same as
today's dirty-rect repaint.

## The virtio foundation (shared with [virtio-net.md](virtio-net.md))

Both virtio drivers sit on the same transport; **build it once** and reuse it.

- **PCI discovery + setup.** Find the device by vendor `0x1AF4`, device `0x1050`
  (modern virtio-gpu) on the bus. This needs PCI work tOS doesn't have yet —
  `kernel/drivers/pci.c` today only exposes `pci_read32` + a print scan. Add
  `pci_write32`, a `pci_find(class/vendor)`, **BAR decode** (read/size the BARs), and
  **enable bus-mastering + MMIO** in the command register.
- **virtio-pci transport.** Target **modern virtio (1.x)**: the device exposes its
  common-config / notify / ISR / device-config structures via **PCI capabilities** in
  config space (BARs + offsets). Negotiate features, set `DRIVER`/`DRIVER_OK` status
  bits, read/write the device-config region. (A legacy I/O-BAR transport is simpler but
  must be explicitly enabled in QEMU; prefer modern.)
- **Virtqueues (split ring).** Each queue is three physically-contiguous regions
  (descriptor table, avail ring, used ring). tOS makes this easy: the kernel
  **identity-maps all RAM (virt == phys)**, and `vmm_alloc_surface(nframes)` already
  returns a **physically-contiguous** base — so a DMA buffer's pointer *is* its DMA
  address. No IOMMU/bounce buffers needed in the VM.
- **Notify + completion.** Kick the device by writing the queue index to its notify
  register; learn about completions either by **polling the used ring** (simplest first
  cut) or via the device's interrupt. tOS interrupts are statically wired in
  `kernel/arch/idt.c` over the **legacy 8259 PIC** (no IOAPIC/MSI-X yet), so the
  pragmatic order is **poll → legacy INTx**, with MSI-X as a later upgrade.

## Command flow (Level A, 2D)

virtio-gpu uses two queues: **controlq** (resource/scanout commands) and **cursorq**
(cursor position/image). The bring-up sequence:

1. `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` → preferred resolution(s) for each scanout.
2. `RESOURCE_CREATE_2D` → create a host-side `B8G8R8X8` resource at our chosen size.
3. Allocate a guest **backing buffer** (`vmm_alloc_surface`, `width*height*4`), and
   `RESOURCE_ATTACH_BACKING` to point the resource at those physical pages.
4. `SET_SCANOUT` → bind the resource to scanout 0 (the display).
5. **Per frame / per damage rect:** the compositor renders into the backing buffer,
   then `TRANSFER_TO_HOST_2D(rect)` copies the dirty region host-side, and
   `RESOURCE_FLUSH(rect)` presents it.
6. **Hardware cursor (bonus):** upload the cursor image once, then drive position via
   the cursorq — offloading the cursor `twm` currently composites by hand (and removing
   the shadow-halo/cursor-smear bookkeeping for it).

## What has to change to build it

- **PCI expansion** (`pci_write32`, find-by-id, BAR decode, bus-master enable) — shared
  with every future device driver.
- **A `virtio` transport module** (feature negotiation, status, virtqueue alloc/kick,
  used-ring reaping) under `kernel/drivers/` — shared with virtio-net.
- **DMA buffers** — already covered by `vmm_alloc_surface` + the identity map; maybe a
  thin `dma_alloc(nbytes)` wrapper that returns `{virt, phys}` (equal today).
- **Presentation hook.** Today `SYS_FBINFO` hands the compositor a raw firmware fb.
  Add a path where, if a virtio-gpu scanout exists, the kernel hands `twm` a
  **virtio-gpu-backed** surface plus a `SYS_FB_PRESENT(rect)` (or implicit-on-unmap)
  call that issues the transfer+flush. The compositor's damage rects feed it directly.
- **Modesetting.** Honour `GET_DISPLAY_INFO` so the desktop can run at a chosen
  resolution instead of the firmware's fixed mode (also unlocks VM window resize).
- **Interrupt or poll** plumbing as above.

## Phasing (each keeps `make test` green)

1. **PCI + virtio transport bring-up.** Find the device, negotiate features, set up one
   virtqueue, reach `DRIVER_OK`. A test asserts the kernel logs the device + reaches
   ready (no display change yet).
2. **Static present.** Create a 2D resource, attach backing, set scanout, and push one
   full-screen image — prove a known pattern appears (screenshot-verify).
3. **Damage-driven present.** Wire the compositor's damage rects to
   `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`; confirm normal desktop use only flushes
   dirty regions.
4. **Modesetting + hardware cursor.** Switch resolution from `GET_DISPLAY_INFO`; move
   the cursor to the cursorq.
5. *(Stretch, Level B)* **virgl / Venus** — a 3D command-stream context for GL/Vulkan
   acceleration. Separate, large, optional.

## Out of scope (for now)

3D acceleration (Level B / virgl / Venus) at first; multi-display; display hotplug;
EDID; and **bare-metal GPU drivers** of any kind — metal stays on the software
compositor over the firmware framebuffer. This driver is a **VM convenience + the accel
on-ramp**, not a path to accelerating real hardware.
