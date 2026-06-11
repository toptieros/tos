/* A tiny block-device registry. Each storage driver (ATA, virtio-blk, future
 * AHCI/NVMe) registers a named device with 512-byte read/write callbacks, so the
 * installer (and later the fs) can target any disk by index instead of calling a
 * specific driver. 64-bit LBA + 32-bit sector counts -- the common denominator. */
#pragma once
#include <stdint.h>

#define BDEV_MAX 8

typedef int (*bdev_read_fn)(uint64_t lba, uint32_t count, void *buf);
typedef int (*bdev_write_fn)(uint64_t lba, uint32_t count, const void *buf);

struct bdev {
    char          name[16];
    uint64_t      sectors;        /* capacity in 512-byte sectors (0 = unknown) */
    bdev_read_fn  read;
    bdev_write_fn write;
};

int          bdev_register(const char *name, uint64_t sectors, bdev_read_fn rd, bdev_write_fn wr);
int          bdev_count(void);
struct bdev *bdev_get(int i);                  /* NULL if out of range */
int          bdev_find(const char *name);      /* index, or -1 */
int          bdev_read(int i, uint64_t lba, uint32_t count, void *buf);
int          bdev_write(int i, uint64_t lba, uint32_t count, const void *buf);
void         bdev_dump(void);                  /* print each registered device */
