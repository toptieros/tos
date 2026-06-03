/* Local APIC register access + the IPIs that start the application processors. */
#include "apic.h"
#include "cpu.h"               /* inb/outb for the PIT-based timer calibration */

#define LAPIC_ID   0x020       /* local APIC id (bits 24-31)        */
#define LAPIC_EOI  0x0B0       /* end of interrupt                  */
#define LAPIC_SVR  0x0F0       /* spurious interrupt vector reg     */
#define LAPIC_ICRL 0x300       /* interrupt command, low  (write triggers) */
#define LAPIC_ICRH 0x310       /* interrupt command, high (destination)    */
#define LAPIC_LVT_TIMER 0x320  /* local vector table: timer                */
#define LAPIC_TICR 0x380       /* timer initial count                      */
#define LAPIC_TCCR 0x390       /* timer current count (read-only)          */
#define LAPIC_TDCR 0x3E0       /* timer divide configuration               */
#define LVT_PERIODIC 0x20000u  /* timer mode bit 17                        */
#define LVT_MASKED   0x10000u  /* mask bit 16 (timer still counts, no IRQ) */

#define ICR_INIT     0x00000500u   /* delivery mode: INIT    */
#define ICR_STARTUP  0x00000600u   /* delivery mode: STARTUP */
#define ICR_ASSERT   0x00004000u   /* bit 14: level = assert */
#define ICR_DELIVS   0x00001000u   /* bit 12: delivery status (busy) */

static inline volatile uint32_t *reg(uint32_t off) {
    return (volatile uint32_t *)(LAPIC_VBASE + off);
}
static inline uint32_t rd(uint32_t off) { return *reg(off); }
static inline void     wr(uint32_t off, uint32_t v) { *reg(off) = v; }

void lapic_enable(void) {
    wr(LAPIC_SVR, rd(LAPIC_SVR) | 0x100 | 0xFF);   /* APIC enable + spurious vec 0xFF */
}

uint32_t lapic_id(void) { return rd(LAPIC_ID) >> 24; }

void lapic_eoi(void) { wr(LAPIC_EOI, 0); }

static void icr_send(uint32_t apic_id, uint32_t cmd) {
    wr(LAPIC_ICRH, apic_id << 24);
    wr(LAPIC_ICRL, cmd);
    while (rd(LAPIC_ICRL) & ICR_DELIVS) __asm__ volatile("pause");  /* wait for delivery */
}

void lapic_send_init(uint32_t apic_id) {
    icr_send(apic_id, ICR_INIT | ICR_ASSERT);
}

void lapic_send_sipi(uint32_t apic_id, uint8_t vector) {
    icr_send(apic_id, ICR_STARTUP | ICR_ASSERT | vector);
}

void lapic_timer_init(uint8_t vector, uint32_t count) {
    wr(LAPIC_TDCR, 0x3);                       /* divide by 16 */
    wr(LAPIC_LVT_TIMER, LVT_PERIODIC | vector);
    wr(LAPIC_TICR, count);                      /* writing the count starts it */
}

/* Measure the LAPIC timer's real tick rate against the PIT, then return the
 * initial count for a periodic preempt at `hz`. We run the local timer divide-by-16
 * (the same divisor lapic_timer_init uses) and see how far it counts during a known
 * PIT-timed interval -- so the count maps to a defined frequency instead of a magic
 * QEMU-tuned constant. The PIT reference uses channel 2 in one-shot mode, gated and
 * polled via port 0x61 (bit5 = its OUT), so it needs no interrupts (we calibrate at
 * boot with IRQs still off). On anything where the reading looks implausible -- or a
 * watchdog trips because OUT never asserts -- we fall back to a safe fixed count, so
 * the worst case is exactly today's behaviour. */
uint32_t lapic_timer_calibrate(uint32_t hz) {
    if (hz == 0) hz = 100;
    wr(LAPIC_TDCR, 0x3);                        /* divide by 16 (must match run-time) */
    wr(LAPIC_LVT_TIMER, LVT_MASKED);            /* masked one-shot: counts, raises nothing */

    const uint32_t ms = 10;
    uint32_t div = (1193182u * ms) / 1000u;     /* ~11931 PIT ticks ~= 10 ms (< 65536) */
    uint8_t p = inb(0x61);
    outb(0x61, (uint8_t)((p & ~0x02u) | 0x01u));/* ch2 gate on (bit0), speaker data off (bit1) */
    outb(0x43, 0xB0);                           /* ch2, lo/hi byte, mode 0 (int on terminal count) */
    outb(0x42, (uint8_t)(div & 0xff));
    wr(LAPIC_TICR, 0xFFFFFFFFu);                /* start the local timer counting down from the top */
    outb(0x42, (uint8_t)(div >> 8));            /* high byte completes the load -> PIT starts counting */

    int timed_out = 0;
    uint64_t guard = 0;
    while (!(inb(0x61) & 0x20)) {               /* spin until ch2 OUT goes high (terminal count) */
        if (++guard > 400000000ull) { timed_out = 1; break; }
    }
    uint32_t after = rd(LAPIC_TCCR);
    wr(LAPIC_LVT_TIMER, LVT_MASKED);            /* stop the local timer */
    outb(0x61, (uint8_t)(p & ~0x03u));          /* ch2 gate + speaker off, other bits restored */

    if (timed_out) return 1000000u;             /* PIT never signalled -> safe fallback */
    uint32_t elapsed = 0xFFFFFFFFu - after;     /* LAPIC ticks during the ~10 ms window */
    uint64_t per_sec = ((uint64_t)elapsed * 1000u) / ms;
    uint32_t count   = per_sec ? (uint32_t)(per_sec / hz) : 0;
    if (count < 2000u) return 1000000u;         /* implausibly small -> safe fallback */
    return count;
}
