/* A throwaway background task: tick a few times with busy-work in between, then
 * exit. Because the timer preempts every 10 ms, the shell stays responsive the
 * whole time this is looping -- proving real preemptive multitasking. */
#include "ulib.h"

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    print("\r\n[ticker] background task started; ticking 5 times\r\n");
    for (int i = 1; i <= 5; i++) {
        print("[ticker] tick ");
        printc('0' + i);
        print("\r\n");
        for (volatile unsigned long d = 0; d < 60000000UL; d++) { }   /* busy work */
    }
    print("[ticker] background task done\r\n");
    proc_exit();
    for (;;) { }
}
