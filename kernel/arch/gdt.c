#include "gdt.h"
#include "cpu.h"
#include "smp.h"
#include <stdint.h>

/* 64-bit Task State Segment. Only rsp0 matters to us: it is the stack the CPU
 * switches to when an interrupt/syscall arrives while running in ring 3. Each
 * CPU has its own TSS (so two CPUs in ring 3 don't share one ring-0 stack). */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* One shared GDT: null, kcode, kdata, ucode, udata, then a 16-byte TSS
 * descriptor per CPU. Each CPU loads the same GDT and `ltr`s its own TSS. */
#define NGDT (5 + 2 * MAX_CPUS)
static uint64_t gdt[NGDT];
static struct tss tss[MAX_CPUS];

static void set_tss_desc(int slot, struct tss *t) {
    uint64_t base = (uint64_t)t, limit = sizeof(struct tss) - 1;
    gdt[slot] = (limit & 0xffff)
              | ((base & 0xffffff) << 16)
              | ((uint64_t)0x89 << 40)                 /* present, 64-bit TSS */
              | (((limit >> 16) & 0xf) << 48)
              | (((base >> 24) & 0xff) << 56);
    gdt[slot + 1] = (base >> 32) & 0xffffffff;
}

/* Build the shared GDT (call once, on the BSP) and load it on the BSP. */
void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffff;                         /* kernel code (ring 0) */
    gdt[2] = 0x00af92000000ffff;                         /* kernel data (ring 0) */
    gdt[3] = 0x00affa000000ffff;                         /* user code   (ring 3) */
    gdt[4] = 0x00aff2000000ffff;                         /* user data   (ring 3) */
    for (int c = 0; c < MAX_CPUS; c++) {
        set_tss_desc(5 + 2 * c, &tss[c]);
        tss[c].iopb_offset = sizeof(struct tss);
    }
    gdt_load_cpu(0);
}

/* Load the shared GDT on this CPU and install its own TSS (selector 0x28 + 16*cpu). */
void gdt_load_cpu(int cpu) {
    struct gdtr gdtr = { sizeof(gdt) - 1, (uint64_t)gdt };
    gdt_flush(&gdtr);
    uint16_t tss_sel = SEL_TSS + (uint16_t)(cpu * 16);
    __asm__ volatile("ltr %0" : : "r"(tss_sel));
}

/* The scheduler points a CPU's TSS at the incoming task's kernel stack before
 * each switch: the stack the CPU loads on a ring-3 -> ring-0 transition. */
void tss_set_rsp0(int cpu, uint64_t rsp0) {
    tss[cpu].rsp0 = rsp0;
}
