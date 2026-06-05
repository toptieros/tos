/* A tiny test-and-set spinlock for SMP. Acquiring also disables interrupts on
 * the current CPU and returns the previous RFLAGS, so a lock taken in ordinary
 * kernel context cannot deadlock against an interrupt handler on the *same* CPU
 * that wants the same lock; the matching release restores the saved IF. On a
 * single CPU this degenerates to a cli/sti pair around the critical section. */
#pragma once
#include <stdint.h>

typedef struct { volatile uint32_t locked; } spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&l->locked, 1))
        while (l->locked) __asm__ volatile("pause");
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    __sync_lock_release(&l->locked);
    if (flags & (1ull << 9)) __asm__ volatile("sti");   /* IF was set before */
}

/* A mutual-exclusion lock that LEAVES the interrupt flag untouched, so the section
 * it guards stays PREEMPTIBLE -- the PIT keeps ticking and the scheduler can run
 * other tasks (e.g. the compositor) while it is held. Use this instead of the
 * irqsave pair for a lock that (a) is never taken from an interrupt handler and
 * (b) guards slow, blocking-ish work like a polled PIO disk transfer; holding such
 * a lock with interrupts off froze the whole machine for the duration of the I/O
 * (on one CPU the timer never fired, so nothing else ran). On contention we spin
 * with `pause`; because the holder runs with interrupts on, the timer preempts the
 * spinner and lets the holder finish, so this still makes progress on a single CPU.
 *
 * The signature mirrors the irqsave pair (returns/takes a flags word, here unused)
 * so a call site can switch locks by changing only the function name. The caller's
 * IF is whatever it already was: a syscall body runs with IF=1 (see the 0x80 case
 * in traps.c), and the early-boot mount path runs single-threaded with IF=0 and so
 * never actually spins. */
static inline uint64_t spin_lock_preempt(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1))
        while (l->locked) __asm__ volatile("pause");
    return 0;
}

static inline void spin_unlock_preempt(spinlock_t *l, uint64_t flags) {
    (void)flags;
    __sync_lock_release(&l->locked);
}
