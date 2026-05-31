/* Minimal ATA (IDE) PIO driver -- enough to read the filesystem disk.
 *
 * We talk to the legacy primary channel (ports 0x1F0-0x1F7, control 0x3F6) and
 * the *master* drive on it -- the boot disk, which now also carries the tosfs
 * filesystem in its own MBR partition (the kernel finds the partition's base LBA
 * by reading the partition table; see fs.c). LBA28, polled -- no interrupts --
 * so it works the same during early kernel init and inside a syscall, on both
 * the BIOS and (post-ExitBootServices) UEFI paths.
 */
#include "ata.h"
#include "cpu.h"
#include "spinlock.h"

/* The IDE channel is shared hardware -- one transfer at a time across all CPUs. */
static spinlock_t ata_lock = SPINLOCK_INIT;

#define ATA_DATA    0x1F0
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_CMD     0x1F7        /* write: command   */
#define ATA_STATUS  0x1F7        /* read:  status    */
#define ATA_CTRL    0x3F6        /* alternate status */

#define ST_BSY  0x80
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define DRIVE_MASTER_LBA 0xE0    /* LBA mode, master (the boot disk) */

static inline void io_wait_400ns(void) {
    for (int i = 0; i < 4; i++) inb(ATA_CTRL);   /* ~100ns each */
}

/* Spin until BSY clears; returns -1 on timeout or error. */
static int wait_ready(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY)) return 0;
    }
    return -1;
}

/* Spin until DRQ is set (data ready); returns -1 on timeout or error. */
static int wait_drq(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return 0;
    }
    return -1;
}

static int ata_read_unlocked(uint32_t lba, uint8_t count, void *buf) {
    if (count == 0) return -1;
    if (wait_ready() < 0) return -1;

    outb(ATA_DRIVE, DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F));
    io_wait_400ns();
    outb(ATA_SECCNT, count);
    outb(ATA_LBA0, (uint8_t)(lba         & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, 0x20);                          /* READ SECTORS (PIO) */

    uint16_t *out = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq() < 0) return -1;
        uint32_t words = 256;                     /* 512 bytes / sector */
        __asm__ volatile("rep insw"
                         : "+D"(out), "+c"(words)
                         : "d"((uint16_t)ATA_DATA)
                         : "memory");
    }
    return 0;
}

static int ata_write_unlocked(uint32_t lba, uint8_t count, const void *buf) {
    if (count == 0) return -1;
    if (wait_ready() < 0) return -1;

    outb(ATA_DRIVE, DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F));
    io_wait_400ns();
    outb(ATA_SECCNT, count);
    outb(ATA_LBA0, (uint8_t)(lba         & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, 0x30);                          /* WRITE SECTORS (PIO) */

    const uint16_t *in = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq() < 0) return -1;
        uint32_t words = 256;
        __asm__ volatile("rep outsw"
                         : "+S"(in), "+c"(words)
                         : "d"((uint16_t)ATA_DATA));
    }
    outb(ATA_CMD, 0xE7);                           /* FLUSH CACHE */
    return wait_ready();
}

int ata_read(uint32_t lba, uint8_t count, void *buf) {
    uint64_t f = spin_lock_irqsave(&ata_lock);
    int r = ata_read_unlocked(lba, count, buf);
    spin_unlock_irqrestore(&ata_lock, f);
    return r;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
    uint64_t f = spin_lock_irqsave(&ata_lock);
    int r = ata_write_unlocked(lba, count, buf);
    spin_unlock_irqrestore(&ata_lock, f);
    return r;
}
