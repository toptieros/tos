#include "speaker.h"
#include "cpu.h"
#include <stdint.h>

#define PIT_CH2  0x42
#define PIT_CMD  0x43
#define SPK_PORT 0x61     /* bit0 = timer-2 gate, bit1 = speaker data enable */

void speaker_set(uint32_t freq) {
    if (freq == 0) {
        outb(SPK_PORT, inb(SPK_PORT) & ~3);          /* gate + speaker off */
        return;
    }
    uint32_t div = 1193182u / freq;                  /* PIT base frequency */
    outb(PIT_CMD, 0xB6);                             /* channel 2, mode 3, lo/hi byte */
    outb(PIT_CH2, (uint8_t)(div & 0xFF));
    outb(PIT_CH2, (uint8_t)((div >> 8) & 0xFF));
    outb(SPK_PORT, inb(SPK_PORT) | 3);               /* gate + speaker on */
}
