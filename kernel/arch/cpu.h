/* Low-level CPU helpers shared across the kernel. */
#pragma once
#include <stdint.h>

#define COM1 0x3f8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t r;
    __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* The complete register frame saved by isr_common (cpu.asm). The field order
 * mirrors the push sequence there, so a `struct regs *` overlays the frame the
 * interrupt left on the kernel stack. C handlers receive a pointer to one of
 * these and may modify it (e.g. set rax for a syscall return) before it is
 * restored, or hand back a different task's frame to perform a context switch. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err;                       /* pushed by the stubs            */
    uint64_t rip, cs, rflags, rsp, ss;       /* pushed by the CPU on interrupt */
};

/* implemented in cpu.asm */
void gdt_flush(void *gdtr);
void sched_run_first(uint64_t krsp) __attribute__((noreturn));
