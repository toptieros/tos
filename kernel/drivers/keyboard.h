#pragma once

void kbd_init(void);          /* remap the PIC and drain the controller */
int  kbd_getc(void);          /* dequeue a translated char, or -1 if empty */
unsigned kbd_mods(void);      /* live keyboard modifier bitmask (KMOD_*) */
void keyboard_irq(void);      /* IRQ1 handler body (called from isr_dispatch) */
void serial_irq(void);        /* IRQ4 COM1 RX body: feeds serial input into the key ring */
