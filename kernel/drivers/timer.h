#pragma once
#include <stdint.h>

void     timer_init(void);     /* program the PIT for periodic IRQ0 ticks */
void     timer_tick(void);     /* called from the IRQ0 handler            */
uint64_t timer_ticks(void);    /* monotonic tick count since boot         */
