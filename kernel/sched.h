#pragma once
#include <stdint.h>
#include "cpu.h"

#define MAX_TASKS 256               /* task-table slots (1.., 0 unused); kernel stacks
                                     * are pool-allocated, so this scales with RAM   */

/* Preemptive round-robin scheduler, SMP-aware: each CPU has its own `current`
 * task, idle context, and run queue of RUNNABLE tasks (guarded by its own
 * rq_lock). The hot paths (timer preemption, yield) touch only an rq_lock, so
 * they never contend on the global sched_lock, which now covers just the task
 * table (create/exit/wait and waking blocked tasks). New tasks are placed on the
 * least-loaded CPU; an idle CPU steals from the busiest queue. Each task has its
 * own address space and kernel stack; switching means saving the running CPU's
 * register frame and resuming another task's. The timer (PIT on the BSP, LAPIC
 * timer on the APs) drives preemption; tasks may also yield, block, sleep, or
 * exit. The switch helpers take the interrupted frame and return the frame to
 * resume (see isr_dispatch in traps.c). */

void sched_init(void);                      /* set up the BSP's idle + kernel CR3 */
uint64_t sched_kernel_cr3(void);            /* the shared kernel/idle address space */
void sched_ap_enter(int cpu) __attribute__((noreturn));  /* an AP joins scheduling */
int  sched_spawn(const char *role);         /* new child task -> its pid (or -1) */
int  sched_current(void);                   /* slot id of the task running here  */
int  sched_uid(void);                       /* owner identity of the running task (perm.h) */
int  sched_setuid(int uid);                 /* SYS_SETUID: set/drop the caller's uid; 0/-1 */
struct regs *sched_fork(struct regs *r);    /* clone caller -> child pid / 0   */
struct regs *sched_exec(struct regs *r, const char *prog);  /* replace image   */
void sched_start(void) __attribute__((noreturn));   /* BSP runs the first user task */

struct regs *sched_tick(struct regs *r);        /* timer preemption          */
struct regs *sched_yield(struct regs *r);       /* voluntary yield           */
struct regs *sched_block_read(struct regs *r);  /* block current on keyboard */
struct regs *sched_sleep(struct regs *r, uint64_t nticks);  /* park N timer ticks */
struct regs *sched_wait(struct regs *r);        /* block until children exit */
struct regs *sched_trywait(struct regs *r);     /* non-blocking reap: pid / 0 / -1 */
struct regs *sched_exit(struct regs *r);        /* current task exits        */
struct regs *sched_kill(struct regs *r);        /* current task faulted -> kill it, keep OS up */
int  sched_request_kill(int slot);              /* async kill: flag a task to die (Super+Shift+Q) */
struct regs *sched_reap_killed(struct regs *r); /* kernel-entry hook: reap a flagged current task */
int  sched_pid(int slot);                       /* a task's process id (for diagnostics) */
void          sched_wake_readers(void);         /* a key arrived: unblock     */

int  sched_get_tty(int slot);                   /* pty bound to a task's stdio, or -1 */
void sched_set_tty(int slot, int tty);
uint64_t sched_task_cr3(int slot);              /* a task's PML4 phys                 */
int  sched_task_count(void);                    /* live task count (for sysinfo)      */
uint64_t sched_mmap(uint64_t nbytes);           /* map private anon RAM -> base vaddr, or 0 */
