/* Bring up the application processors (APs).
 *
 * We run this from kmain, before the scheduler starts, on a kernel address space
 * (so the LAPIC MMIO mapping is reachable). CPUs are discovered from the legacy
 * MP table (which SeaBIOS leaves in low memory, reachable through the identity
 * map). Each AP is started with INIT-SIPI-SIPI pointing at the trampoline page at
 * physical 0x8000; the AP walks up to long mode and calls ap_main here. */
#include "smp.h"
#include "percpu.h"
#include "apic.h"
#include "vmm.h"
#include "sched.h"
#include "gdt.h"
#include "idt.h"
#include "console.h"
#include "cpu.h"
#include "trampoline_blob.h"
#include <stdint.h>

#define TRAMPO       0x8000u
#define TR_AP_CR3    (TRAMPO + 0xFE0)
#define TR_AP_ENTRY  (TRAMPO + 0xFE8)
#define TR_AP_STACK  (TRAMPO + 0xFF0)
#define AP_STACK_SZ  8192
#define LAPIC_PREEMPT_HZ  100u                 /* AP preempt rate; matches the BSP's PIT tick */

struct cpu cpus[MAX_CPUS];
int ncpu = 1;

/* Per-CPU LAPIC timer initial count, measured once on the BSP (lapic_timer_calibrate)
 * so the APs preempt at LAPIC_PREEMPT_HZ on whatever this machine's bus rate is,
 * rather than a magic constant. Stays at the calibration fallback until smp_init runs. */
static uint32_t lapic_count = 1000000u;

static uint8_t ap_stacks[MAX_CPUS][AP_STACK_SZ] __attribute__((aligned(16)));
static volatile int cpu_online = 1;            /* the BSP counts as online */

int smp_cpu_count(void) { return cpu_online; }

/* Which CPU is running this code? Map the local APIC id to our small index. */
int this_cpu(void) {
    uint32_t id = lapic_id();
    for (int i = 0; i < ncpu; i++)
        if (cpus[i].apic_id == id) return i;
    return 0;
}

/* crude busy delay -- exact timing doesn't matter to QEMU, only ordering does */
static void delay(volatile uint32_t loops) { while (loops--) (void)inb(0x80); }

/* The 64-bit entry every AP lands on (higher half). It installs its own GDT/TSS
 * + the shared IDT, enables its LAPIC and a periodic timer, reports online, then
 * joins the scheduler. sched_ap_enter keeps interrupts off until the BSP starts
 * scheduling (so a tick can't fire mid-bring-up), then idles; the timer preempts
 * it into tasks from there. */
__attribute__((used)) static void ap_main(void) {
    int idx = this_cpu();
    gdt_load_cpu(idx);
    idt_load();
    lapic_enable();
    lapic_timer_init(0x22, lapic_count);        /* BSP-calibrated count (LAPIC_PREEMPT_HZ) */
    __sync_fetch_and_add(&cpu_online, 1);
    sched_ap_enter(idx);                        /* never returns */
}

/* --- CPU discovery via QEMU's fw_cfg port interface ----------------------- */
/* SeaBIOS no longer ships a legacy MP table, so we ask QEMU directly: select a
 * key on port 0x510 and stream its bytes from 0x511. FW_CFG_NB_CPUS (0x0005)
 * gives the CPU count; with a plain `-smp N` the APIC ids are 0..N-1. */
#define FW_CFG_SELECT   0x510
#define FW_CFG_DATA     0x511
#define FW_CFG_NB_CPUS  0x0005

static int find_cpus(uint8_t *ids, int max) {
    outw(FW_CFG_SELECT, FW_CFG_NB_CPUS);
    uint32_t count = (uint32_t)inb(FW_CFG_DATA);
    count |= (uint32_t)inb(FW_CFG_DATA) << 8;          /* little-endian uint16 */
    if (count == 0 || count > (uint32_t)max) count = (count > (uint32_t)max) ? (uint32_t)max : 1;
    for (uint32_t i = 0; i < count; i++) ids[i] = (uint8_t)i;
    return (int)count;
}

void smp_init(void) {
    /* Run on the shared kernel address space so LAPIC_VBASE (and the higher half)
     * is mapped; this same PML4 is each AP's initial CR3 and its idle space. */
    uint64_t kpml4 = sched_kernel_cr3();
    __asm__ volatile("mov %0, %%cr3" : : "r"(kpml4) : "memory");
    lapic_enable();

    /* Measure the local timer once (IRQs are still off here) so the APs preempt at a
     * defined LAPIC_PREEMPT_HZ instead of a magic count. The BSP itself preempts on
     * the PIT, so this only sets the AP cadence. */
    lapic_count = lapic_timer_calibrate(LAPIC_PREEMPT_HZ);
    console_puts("[smp] lapic timer calibrated: count ");
    console_putdec(lapic_count);
    console_puts(" (~");
    console_putdec(LAPIC_PREEMPT_HZ);
    console_puts(" hz preempt)\r\n");

    cpus[0].apic_id = lapic_id();              /* the BSP */
    cpus[0].index   = 0;
    cpus[0].started = 1;
    ncpu = 1;

    uint8_t ids[MAX_CPUS];
    int n = find_cpus(ids, MAX_CPUS);
    if (n <= 1) { console_puts("[smp] single CPU\r\n"); return; }

    for (int i = 1; i < n; i++) { cpus[i].apic_id = ids[i]; cpus[i].index = i; }
    ncpu = n;

    for (unsigned i = 0; i < trampoline_bin_len; i++)
        ((volatile uint8_t *)(uintptr_t)TRAMPO)[i] = trampoline_bin[i];
    *(volatile uint64_t *)(uintptr_t)TR_AP_CR3   = kpml4;
    *(volatile uint64_t *)(uintptr_t)TR_AP_ENTRY = (uint64_t)ap_main;

    uint32_t bsp = cpus[0].apic_id;
    for (int i = 0; i < n; i++) {
        if (ids[i] == bsp) continue;
        *(volatile uint64_t *)(uintptr_t)TR_AP_STACK =
            (uint64_t)(ap_stacks[i] + AP_STACK_SZ);
        int before = cpu_online;
        lapic_send_init(ids[i]);
        delay(100000);
        lapic_send_sipi(ids[i], TRAMPO >> 12);
        delay(20000);
        lapic_send_sipi(ids[i], TRAMPO >> 12);
        for (int t = 0; t < 1000 && cpu_online == before; t++) delay(20000);
    }

    console_puts("[smp] ");
    console_putdec((uint32_t)cpu_online);
    console_puts(" of ");
    console_putdec((uint32_t)n);
    console_puts(" CPUs online\r\n");
}
