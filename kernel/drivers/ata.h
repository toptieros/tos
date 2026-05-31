#pragma once
#include <stdint.h>

/* Read/write `count` (1..255) 512-byte sectors starting at LBA `lba` on the IDE
 * primary-slave disk. Return 0 on success, -1 on error/timeout. */
int ata_read(uint32_t lba, uint8_t count, void *buf);
int ata_write(uint32_t lba, uint8_t count, const void *buf);
