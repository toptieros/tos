#include "blockdev.h"
#include "console.h"

static struct bdev devs[BDEV_MAX];
static int         ndev = 0;

int bdev_register(const char *name, uint64_t sectors, bdev_read_fn rd, bdev_write_fn wr) {
    if (ndev >= BDEV_MAX || !rd || !wr) return -1;
    struct bdev *d = &devs[ndev];
    int i = 0; for (; name[i] && i < 15; i++) d->name[i] = name[i]; d->name[i] = 0;
    d->sectors = sectors; d->read = rd; d->write = wr;
    return ndev++;
}

int bdev_count(void) { return ndev; }

struct bdev *bdev_get(int i) { return (i >= 0 && i < ndev) ? &devs[i] : (struct bdev *)0; }

int bdev_find(const char *name) {
    for (int i = 0; i < ndev; i++) {
        const char *a = devs[i].name; int j = 0;
        while (a[j] && a[j] == name[j]) j++;
        if (!a[j] && !name[j]) return i;
    }
    return -1;
}

int bdev_read(int i, uint64_t lba, uint32_t count, void *buf) {
    struct bdev *d = bdev_get(i);
    return d ? d->read(lba, count, buf) : -1;
}
int bdev_write(int i, uint64_t lba, uint32_t count, const void *buf) {
    struct bdev *d = bdev_get(i);
    return d ? d->write(lba, count, buf) : -1;
}

void bdev_dump(void) {
    for (int i = 0; i < ndev; i++) {
        console_puts("[bdev] ");
        console_puts(devs[i].name);
        console_puts(" ");
        console_putdec(devs[i].sectors);
        console_puts(" sectors\r\n");
    }
}
