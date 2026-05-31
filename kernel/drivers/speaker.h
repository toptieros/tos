/* PC speaker via PIT channel 2. speaker_set(freq) plays a square wave at `freq`
 * Hz; speaker_set(0) silences it. Asynchronous -- the caller controls duration
 * (e.g. sleep, then set 0), so it doesn't block the kernel. */
#pragma once
#include <stdint.h>

void speaker_set(uint32_t freq);
