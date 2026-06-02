/* memtest: stress the multi-region physical frame allocator. mmap most of RAM
 * (the kernel allocates every page eagerly), which on a >3 GiB machine forces the
 * bump allocator across the sub-4 GiB PCI hole into RAM remapped above 4 GiB. Then
 * write a unique address-keyed value to every page and read it all back: a handed-
 * out hole/MMIO frame, an aliased frame, or an unmapped page would make the
 * readback mismatch -- so a clean pass over >3 GiB proves the pool spans the hole.
 * Run it from the shell (`memtest`); harmless no-op on a small machine. */
#include "ulib.h"

#define PAGE   4096UL
#define KEY    0x0123456789abcdefUL

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    struct sysinfo si; sysinfo(&si);
    unsigned long ram = si.ram_bytes;
    print("[memtest] RAM "); printu((unsigned)(ram >> 20)); print(" MiB; ");

    unsigned long margin = 384UL << 20;                /* leave room for the kernel + apps */
    unsigned long want   = ram > margin ? ram - margin : 0;
    if (want < (64UL << 20)) { print("too small, skipping\r\n"); proc_exit(); for (;;) {} }

    unsigned char *p = (unsigned char *)mmap_(want);
    if (!p) { print("mmap FAILED\r\n"); proc_exit(); for (;;) {} }
    unsigned long pages = want / PAGE;
    print("mapped "); printu((unsigned)(want >> 20)); print(" MiB ("); printu((unsigned)pages);
    print(" pages), writing...\r\n");

    for (unsigned long i = 0; i < pages; i++)           /* unique value per page */
        *(volatile unsigned long *)(p + i * PAGE) = i ^ KEY;

    unsigned long bad = 0, hi = 0;
    for (unsigned long i = 0; i < pages; i++) {          /* read it all back */
        unsigned long v = *(volatile unsigned long *)(p + i * PAGE);
        if (v != (i ^ KEY)) bad++; else hi = i;
    }

    print("[memtest] verified "); printu((unsigned)((hi + 1) * PAGE >> 20));
    print(" MiB, mismatches="); printu((unsigned)bad);
    print(bad ? " -- FAIL\r\n" : " -- PASS\r\n");
    proc_exit();
    for (;;) { }
}
