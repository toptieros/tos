/* Per-CPU scheduler state. Indexed by a small CPU index (0 = BSP); the running
 * CPU finds its own index with this_cpu() (a lookup on the local APIC id).
 *
 * Each CPU owns a run queue of RUNNABLE task slots, guarded by its own rq_lock.
 * The preemption tick and voluntary yield touch only this lock, so they never
 * contend on the global sched_lock; that lock now covers just the task table
 * (creation, exit, wait, and waking blocked/sleeping tasks). A task is in
 * exactly one CPU's queue iff it is RUNNABLE; idle CPUs steal from the busiest
 * queue. Lock order when both are needed: sched_lock first, then an rq_lock. */
#pragma once
#include <stdint.h>
#include "smp.h"
#include "sched.h"          /* MAX_TASKS */
#include "spinlock.h"

struct cpu {
    uint32_t apic_id;
    int      index;
    volatile int started;
    int      current;        /* task slot running here, or -1 when idle */
    uint64_t idle_krsp;      /* this CPU's idle context (saved frame)    */
    uint64_t idle_cr3;       /* kernel address space used while idle     */

    /* This CPU's run queue: a ring of RUNNABLE task slots. */
    spinlock_t rq_lock;
    int        rq[MAX_TASKS];
    int        rq_head, rq_tail, rq_count;
};

extern struct cpu cpus[MAX_CPUS];
extern int ncpu;             /* number of CPUs that came online (>=1)    */

int this_cpu(void);          /* index of the CPU running this code       */
