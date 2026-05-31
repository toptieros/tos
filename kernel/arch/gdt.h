#pragma once
#include <stdint.h>

/* Segment selectors laid out by gdt_init(). */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18    /* | 3 in ring-3 frames -> 0x1b */
#define SEL_UDATA 0x20    /* | 3 in ring-3 frames -> 0x23 */
#define SEL_TSS   0x28

void gdt_init(void);                /* build the shared GDT + load it on the BSP  */
void gdt_load_cpu(int cpu);         /* load the GDT + this CPU's TSS (APs)        */
void tss_set_rsp0(int cpu, uint64_t rsp0);   /* ring-0 stack for cpu's next switch */
