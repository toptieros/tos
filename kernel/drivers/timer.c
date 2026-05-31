/* 8254 PIT, channel 0, used as the preemption tick on IRQ0. */
#include "timer.h"
#include "cpu.h"

#define PIT_HZ   1193182u            /* PIT input frequency       */
#define TICK_HZ  100u                /* preemptions per second    */

static volatile uint64_t ticks = 0;

void timer_init(void) {
    uint32_t divisor = PIT_HZ / TICK_HZ;
    outb(0x43, 0x36);                /* channel 0, lo/hi byte, mode 3 (square) */
    outb(0x40, (uint8_t)(divisor & 0xff));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xff));
}

void     timer_tick(void)  { ticks++; }
uint64_t timer_ticks(void) { return ticks; }
