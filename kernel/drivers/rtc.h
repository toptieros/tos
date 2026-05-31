/* CMOS real-time clock: reads the wall-clock date/time (ports 0x70/0x71).
 * Polled, no interrupts -- safe to call from a syscall. */
#pragma once

struct rtctime;
void rtc_now(struct rtctime *out);
