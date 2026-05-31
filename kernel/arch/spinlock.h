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
