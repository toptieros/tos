#include "rtc.h"
#include "syscall.h"
#include "cpu.h"
#include <stdint.h>

#define CMOS_IDX  0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_IDX, reg);
    return inb(CMOS_DATA);
}

static int  update_in_progress(void) { return cmos_read(0x0A) & 0x80; }
static uint8_t bcd2bin(uint8_t v)     { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

/* Read a consistent snapshot: wait out any update-in-progress, then re-read
 * until two consecutive reads agree (so we never straddle a tick). Converts BCD
 * and 12-hour encodings per status register B. */
void rtc_now(struct rtctime *out) {
    uint8_t s, mi, h, d, mo, y, last_s = 0xff;
    for (int tries = 0; tries < 16; tries++) {
        while (update_in_progress()) { }
        s  = cmos_read(0x00); mi = cmos_read(0x02); h = cmos_read(0x04);
        d  = cmos_read(0x07); mo = cmos_read(0x08); y = cmos_read(0x09);
        if (s == last_s) break;                 /* stable read */
        last_s = s;
    }

    uint8_t b = cmos_read(0x0B);
    if (!(b & 0x04)) {                           /* BCD -> binary */
        s = bcd2bin(s); mi = bcd2bin(mi);
        h = (uint8_t)(bcd2bin(h & 0x7F) | (h & 0x80));   /* keep the PM flag */
        d = bcd2bin(d); mo = bcd2bin(mo); y = bcd2bin(y);
    }
    if (!(b & 0x02) && (h & 0x80))               /* 12-hour PM -> 24-hour */
        h = (uint8_t)(((h & 0x7F) + 12) % 24);

    out->sec = s; out->min = mi; out->hour = h;
    out->day = d; out->month = mo;
    out->year = (uint16_t)(2000 + y);
}
