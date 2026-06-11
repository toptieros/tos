/* The live -> disk installer (roadmap Phase 4 / installation.md). Clones the
 * running boot disk onto a target block device (e.g. virtio-blk) so the target
 * becomes an independent, bootable copy. Built on the block-device layer. */
#pragma once
#include <stdint.h>

/* Clone block device 0 (the boot disk) onto block device `target` (>0), sector
 * for sector, then verify-read the MBR. Returns sectors written, or -1 on error
 * / bad target. Long-running and polled -- a deliberate one-shot operation. */
int64_t install_run(int target);
