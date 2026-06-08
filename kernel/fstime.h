/* Packed file-modification time for tosfs (files-app §8). A wall-clock time squeezed
 * into 32 bits, FAT-style, at minute resolution -- plenty for a file manager's
 * "Modified" column, and it avoids any epoch <-> calendar arithmetic on either side
 * (the kernel packs from the CMOS RTC; the Files app unpacks for display).
 *
 *   bits 31..20  year  (12)   absolute, 0..4095
 *   bits 19..16  month ( 4)   1..12
 *   bits 15..11  day   ( 5)   1..31
 *   bits 10.. 6  hour  ( 5)   0..23
 *   bits  5.. 0  min   ( 6)   0..59
 *
 * A packed value of 0 means "unknown" (e.g. a pre-timestamp entry). Pure + standalone
 * so it unit-tests on the host (tests/unit/t_fstime.c) and compiles into both sides. */
#pragma once
#include <stdint.h>

static inline uint32_t fstime_pack(int year, int month, int day, int hour, int min) {
    return ((uint32_t)(year  & 0xFFF) << 20) | ((uint32_t)(month & 0x00F) << 16) |
           ((uint32_t)(day   & 0x01F) << 11) | ((uint32_t)(hour  & 0x01F) <<  6) |
            (uint32_t)(min   & 0x03F);
}
static inline void fstime_unpack(uint32_t t, int *year, int *month, int *day,
                                 int *hour, int *min) {
    if (year)  *year  = (int)((t >> 20) & 0xFFF);
    if (month) *month = (int)((t >> 16) & 0x00F);
    if (day)   *day   = (int)((t >> 11) & 0x01F);
    if (hour)  *hour  = (int)((t >>  6) & 0x01F);
    if (min)   *min   = (int)( t        & 0x03F);
}
