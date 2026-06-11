#pragma once
#include <stdint.h>

/* Read/write `count` (1..255) 512-byte sectors starting at LBA `lba` on the IDE
 * primary-slave disk. Return 0 on success, -1 on error/timeout. */
int ata_read(uint32_t lba, uint8_t count, void *buf);
int ata_write(uint32_t lba, uint8_t count, const void *buf);

/* Total addressable sectors on the master (boot) drive via IDENTIFY; 0 if unknown. */
uint64_t ata_sectors(void);

/* Generic block-device adapters (64-bit LBA / 32-bit count, chunked into the
 * ATA 8-bit-count PIO transfers). Registered with the block layer as "ata0". */
int ata_bdev_read(uint64_t lba, uint32_t count, void *buf);
int ata_bdev_write(uint64_t lba, uint32_t count, const void *buf);
