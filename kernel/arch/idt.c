#include "idt.h"
#include "gdt.h"
#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];

/* from cpu.asm */
extern uint64_t isr_stub_table[32];   /* exception handler entry points */
extern void isr_syscall(void);        /* int 0x80 handler               */
extern void isr_irq0(void);           /* IRQ0 timer handler (PIT, BSP)  */
extern void isr_irq1(void);           /* IRQ1 keyboard handler          */
extern void isr_irq12(void);          /* IRQ12 PS/2 mouse handler       */
extern void isr_lapic_timer(void);    /* LAPIC timer (vector 0x22)      */

static void set_gate(int n, uint64_t handler, uint8_t type_attr) {
    idt[n].offset_low  = handler & 0xffff;
    idt[n].selector    = SEL_KCODE;
    idt[n].ist         = 0;
    idt[n].type_attr   = type_attr;
    idt[n].offset_mid  = (handler >> 16) & 0xffff;
    idt[n].offset_high = (handler >> 32) & 0xffffffff;
    idt[n].zero        = 0;
}

void idt_init(void) {
    for (int i = 0; i < 32; i++)
        set_gate(i, isr_stub_table[i], 0x8e);            /* ring-0 interrupt gate */

    set_gate(0x20, (uint64_t)isr_irq0, 0x8e);            /* IRQ0 timer (ring 0)    */
    set_gate(0x21, (uint64_t)isr_irq1, 0x8e);            /* IRQ1 keyboard (ring 0) */
    set_gate(0x2C, (uint64_t)isr_irq12, 0x8e);           /* IRQ12 PS/2 mouse       */
    set_gate(0x22, (uint64_t)isr_lapic_timer, 0x8e);     /* LAPIC timer (per-CPU)  */
    set_gate(0x80, (uint64_t)isr_syscall, 0xee);         /* DPL=3 so ring 3 can call */

    idt_load();
}

/* Load the shared IDT on the current CPU (the APs call this). */
void idt_load(void) {
    struct idtr idtr = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
