# Design guideline — virtual NIC (virtio-net)

> Status: **design / roadmap.** Today tOS has **no networking at all** — no NIC, no
> stack, no sockets ([roadmap.md](roadmap.md): "can't talk to the world"). This doc is
> the plan for the **first network driver**, **virtio-net**: the simplest, best-
> documented NIC, and QEMU's default. It's the bottom layer of [roadmap.md](roadmap.md)
> Phase 4 ("Connectivity"); the TCP/IP stack and sockets API above it are a separate,
> larger effort sketched at the end.

## Why virtio-net first

A real NIC (e1000, Realtek, Intel iwlwifi…) is a pile of vendor-specific quirks;
**virtio-net** is a clean, standardised paravirtual device that QEMU/KVM expose by
default. It shares its whole transport with [virtio-gpu.md](virtio-gpu.md), so building
one buys most of the other. **e1000** is the natural *second* driver — it's the
fallback for VMs/firmware that don't offer virtio and a step toward real hardware — but
virtio-net is the right first target.

The layers, kept honest (this doc is only the bottom one):

| Layer | What it is | This doc? |
|---|---|---|
| **NIC driver** | virtio-net: move Ethernet frames in/out of the device | **yes** |
| **Protocol stack** | ARP, IPv4, ICMP, UDP, DHCP, TCP | no — see below |
| **Sockets API** | a `SYS_SOCKET`/`bind`/`connect`/`send`/`recv` syscall layer | no — later |
| **Apps** | resolver, an app-store fetch, `ping`, … | no |

## The virtio foundation (shared with [virtio-gpu.md](virtio-gpu.md))

Identical transport to virtio-gpu — **build it once** under `kernel/drivers/` and reuse:

- **PCI discovery + setup.** Find vendor `0x1AF4`, device `0x1041` (modern virtio-net).
  Needs the same PCI expansion tOS lacks today (`kernel/drivers/pci.c` has only
  `pci_read32` + a print scan): add `pci_write32`, find-by-id, **BAR decode**, and
  **bus-master + MMIO enable** in the command register.
- **virtio-pci transport (modern, 1.x).** Common-config / notify / ISR / device-config
  via PCI capabilities; negotiate features, drive the status bits to `DRIVER_OK`.
- **Virtqueues (split ring) in DMA memory.** The kernel **identity-maps all RAM
  (virt == phys)** and `vmm_alloc_surface(nframes)` returns **physically-contiguous**
  pages, so a buffer's pointer *is* its DMA address — no IOMMU/bounce buffers in the VM.
- **Completion: poll → legacy INTx.** tOS IRQs are statically wired in
  `kernel/arch/idt.c` over the legacy 8259 PIC (no IOAPIC/MSI-X yet), so reap the used
  ring by **polling** first, then add a wired interrupt, then MSI-X later.

## How virtio-net moves packets

virtio-net uses (at least) a **receiveq** and a **transmitq** (plus an optional
controlq for offload/MAC/filters). Every frame is prefixed by a small
**`virtio_net_hdr`** (10 or 12 bytes) carrying checksum/segmentation-offload flags —
**start with all offloads off** (don't negotiate them), so the header is zeroed and we
deal in plain Ethernet frames.

- **Device config** gives us the **MAC address** (if the `MAC` feature is set) and link
  status.
- **RX:** pre-post a pool of empty buffers (`virtio_net_hdr` + 1514-byte frame) onto the
  receiveq. The device fills them and marks them used; we reap, strip the header, and
  hand the Ethernet frame up to the stack. Re-post the buffer.
- **TX:** prepend a zeroed `virtio_net_hdr` to the frame, put it on the transmitq, kick
  the notify register, then reclaim the descriptor from the used ring when the device is
  done with it.

That is the entire driver contract: **frames in, frames out, plus a MAC.** Everything
protocol-shaped lives above it.

## Above the driver: the stack (separate effort)

A NIC alone does nothing visible — it needs a protocol stack. Two realistic paths:

- **Port a small, permissively-licensed stack.** **lwIP** (BSD-licensed) is the
  standard choice for exactly this situation: small, freestanding-friendly, and it
  expects a tiny driver shim (`netif` `output`/`input`) that our virtio-net driver
  fills. Fastest route to working TCP/UDP; license is compatible (BSD, not GPL — see the
  reuse note in [roadmap.md](roadmap.md) Phase 4).
- **Grow a native minimal stack.** ARP → IPv4 → ICMP (ping) → UDP → DHCP → TCP, in that
  order. More work, but fully ours and a great teaching artifact.

Either way, expose it to userspace through a **sockets syscall layer**
(`SYS_SOCKET`/`bind`/`connect`/`send`/`recv`/`close`) so apps get a familiar API, gated
by the capability model (see [app-runtime.md](app-runtime.md) — net access is a cap).

## What has to change to build it

- **PCI expansion** (`pci_write32`, find-by-id, BAR decode, bus-master) — shared with
  virtio-gpu and every device driver.
- **The shared `virtio` transport module** (features, status, virtqueue alloc/kick,
  used-ring reaping).
- **DMA buffers** — covered by `vmm_alloc_surface` + the identity map; a thin
  `dma_alloc(nbytes) -> {virt, phys}` helper.
- **Interrupt or poll** plumbing (poll first).
- **A stack** (port lwIP *or* grow native) and a **sockets syscall + capability** layer.

## Phasing (each keeps `make test` green)

1. **Transport + device bring-up.** PCI find, feature negotiation, queues, `DRIVER_OK`,
   read the MAC. A test asserts the kernel logs the device + MAC.
2. **TX a raw frame.** Send one hand-built Ethernet/ARP frame; verify it on the host
   (QEMU `-netdev` + a `tcpdump`/pcap in the test harness).
3. **RX + ARP.** Post RX buffers, reply to ARP — the guest becomes pingable at L2/L3
   bring-up. Test: host `arping`/`ping` gets a reply.
4. **Minimal IP/UDP + DHCP.** Lease an address, answer/issue UDP — a test does a DHCP
   handshake and a UDP echo against the host.
5. **TCP** (native or via lwIP) + the **sockets syscall layer**; an app opens a
   connection.
6. **e1000** as the second driver for non-virtio targets (shares the stack + sockets).

## Out of scope (for now)

Real-hardware NICs beyond e1000, **WiFi** (enormous — firmware blobs, mac80211-class
infrastructure), IPv6, routing/NAT, TLS (a userspace library later), and any
performance work (GRO/checksum/segmentation offload, multiqueue, MSI-X). The goal of
this doc is the **driver contract** — frames in, frames out — that the stack builds on.
