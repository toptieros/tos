/* Local APIC register access + the IPIs that start the application processors. */
#include "apic.h"

#define LAPIC_ID   0x020       /* local APIC id (bits 24-31)        */
#define LAPIC_EOI  0x0B0       /* end of interrupt                  */
#define LAPIC_SVR  0x0F0       /* spurious interrupt vector reg     */
#define LAPIC_ICRL 0x300       /* interrupt command, low  (write triggers) */
#define LAPIC_ICRH 0x310       /* interrupt command, high (destination)    */
#define LAPIC_LVT_TIMER 0x320  /* local vector table: timer                */
#define LAPIC_TICR 0x380       /* timer initial count                      */
#define LAPIC_TDCR 0x3E0       /* timer divide configuration               */
#define LVT_PERIODIC 0x20000u  /* timer mode bit 17                        */

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
