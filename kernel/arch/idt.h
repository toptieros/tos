#pragma once
void idt_init(void);    /* build + load the IDT (BSP) */
void idt_load(void);    /* load the shared IDT on this CPU (APs) */
