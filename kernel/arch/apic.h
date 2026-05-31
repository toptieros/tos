/* Local APIC: the per-CPU interrupt controller. We talk to it through the MMIO
 * page that vmm_init maps into the shared higher half at LAPIC_VBASE, so the
 * same virtual address reaches whichever CPU's LAPIC is executing the code. */
#pragma once
#include <stdint.h>

#define LAPIC_VBASE 0xFFFFFFFF40000000ULL   /* PDPT_high[509]; maps phys 0xFEE00000 */

void     lapic_enable(void);     /* set the spurious-vector enable bit on this CPU */
uint32_t lapic_id(void);         /* this CPU's local APIC id                       */
void     lapic_eoi(void);        /* end-of-interrupt                               */

/* Inter-processor interrupts used to start the other CPUs. */
void lapic_send_init(uint32_t apic_id);
void lapic_send_sipi(uint32_t apic_id, uint8_t vector);

/* Start this CPU's local timer firing `vector` periodically (for preemption). */
void lapic_timer_init(uint8_t vector, uint32_t count);
