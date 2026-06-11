#include "install.h"
#include "blockdev.h"
#include "console.h"
#include "vmm.h"

#define CHUNK 64                  /* sectors per copy step (32 KiB buffer) */

/* Clone bdev 0 (boot disk) -> bdev `target`. Copies min(src, dst) sectors in
 * CHUNK-sized transfers through the block layer, then reads the MBR back off the
 * target and compares it to the source as a sanity check. */
int64_t install_run(int target) {
    if (target <= 0 || target >= bdev_count()) { console_puts("[install] no target disk\r\n"); return -1; }
    struct bdev *src = bdev_get(0), *dst = bdev_get(target);
    if (!src || !dst) return -1;

    uint64_t nsrc = src->sectors, ndst = dst->sectors;
    if (nsrc == 0) { console_puts("[install] unknown source size\r\n"); return -1; }
    uint64_t n = (nsrc < ndst || ndst == 0) ? nsrc : ndst;
    if (ndst && ndst < nsrc) { console_puts("[install] target too small\r\n"); return -1; }

    uint64_t buf = vmm_alloc_surface((CHUNK * 512) / 4096);
    uint64_t cmp = vmm_alloc_surface(1);
    if (!buf || !cmp) { console_puts("[install] no DMA buffer\r\n"); return -1; }
    void *b = (void *)(uintptr_t)buf;

    console_puts("[install] cloning ");
    console_puts(src->name); console_puts(" -> "); console_puts(dst->name);
    console_puts(" (");  console_putdec(n); console_puts(" sectors)\r\n");

    for (uint64_t lba = 0; lba < n; lba += CHUNK) {
        uint32_t c = (n - lba) < CHUNK ? (uint32_t)(n - lba) : CHUNK;
        if (bdev_read(0, lba, c, b) < 0)      { console_puts("[install] read error\r\n");  return -1; }
        if (bdev_write(target, lba, c, b) < 0){ console_puts("[install] write error\r\n"); return -1; }
    }

    /* verify: the MBR (sector 0) must read back identical from the target */
    uint8_t *s0 = (uint8_t *)(uintptr_t)buf, *t0 = (uint8_t *)(uintptr_t)cmp;
    int ok = bdev_read(0, 0, 1, s0) == 0 && bdev_read(target, 0, 1, t0) == 0;
    if (ok) for (int i = 0; i < 512; i++) if (s0[i] != t0[i]) { ok = 0; break; }

    console_puts(ok ? "[install] done, verify OK\r\n" : "[install] done, verify FAIL\r\n");
    return ok ? (int64_t)n : -1;
}
