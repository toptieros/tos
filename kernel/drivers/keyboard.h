#pragma once

void kbd_init(void);          /* remap the PIC and drain the controller */
int  kbd_getc(void);          /* dequeue a translated char, or -1 if empty */
void keyboard_irq(void);      /* IRQ1 handler body (called from isr_dispatch) */
