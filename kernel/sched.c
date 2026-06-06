/* Preemptive round-robin scheduler, SMP-aware, with per-CPU run queues.
 *
 * Every CPU runs its own copy of the switch logic: it has its own `current` task
 * (cpus[].current, -1 when idle), its own idle context, and its own run queue of
 * RUNNABLE task slots (cpus[].rq, guarded by cpus[].rq_lock). A task is in
 * exactly one CPU's queue while RUNNABLE, and TASK_RUNNING (off every queue)
 * while a CPU executes it, so two CPUs never run the same task.
 *
 * Two locks, in this order when both are held:
 *   sched_lock  -- the shared task table: slot allocation, pids, state changes
 *                  driven by syscalls (fork/exec/exit/wait) and the wakers.
 *   rq_lock[c]  -- one CPU's run queue ring.
 * The hot paths (timer preemption on the APs, voluntary yield) take only an
 * rq_lock, so they never serialise on sched_lock. State writes they make are to
 * the task this CPU is running or just dequeued -- a single writer per CPU --
 * and cross-CPU hand-off is published through the rq_lock acquire/release.
 *
 * Load balancing: a new task is placed on the least-loaded CPU; a CPU whose own
 * queue is empty steals a task from the busiest queue rather than idling.
 *
 * Each task keeps its own kernel stack so a half-finished interrupt frame can be
 * parked there while another task runs. A switch is: save the outgoing frame,
 * pick a runnable task, load its CR3 and this CPU's TSS.rsp0, and return its
 * saved frame for the assembly tail to restore.
 */
#include "sched.h"
#include "fs/perm.h"      /* TOS_UID_* identities for per-task ownership */
#include "vmm.h"
#include "gdt.h"
#include "console.h"
#include "fs.h"
#include "timer.h"
#include "percpu.h"
#include "spinlock.h"
#include "ipc.h"
#include <stdint.h>

/* --- run-queue invariant checks (SCHED_DEBUG builds only) ----------------- */
/* The lock-free hot path rests on one invariant: a task pulled from a run queue
 * is RUNNABLE, and a RUNNING task belongs to exactly one CPU. The cheapest way
 * to catch a violation (a task scheduled on two CPUs, left queued after it
 * blocked, or double-enqueued) is to check the state at the points where we
 * hold the relevant rq_lock -- a single aligned read, race-tolerant, no
 * cross-CPU sweep that would false-positive against the lock-free path.
 * sched_panic writes straight to COM1 (no console_lock) so it reports even from
 * a wedged state, then halts this CPU. */
#ifdef SCHED_DEBUG
static void dbg_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}
static void dbg_str(const char *s) { for (; *s; s++) dbg_putc(*s); }
static void dbg_dec(int v) {
    char t[12]; int n = 0; unsigned u = (unsigned)v;
    if (!u) { dbg_putc('0'); return; }
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    while (n--) dbg_putc(t[n]);
}
/* No this_cpu() here: the panic must work even when it fires from the earliest
 * rq_push (sched_spawn of init, before smp_init maps the LAPIC). The two ints
 * are labelled by the message; each call documents what it passes. */
static __attribute__((noreturn)) void sched_panic(const char *msg, int a, int b) {
    dbg_str("\r\n[SCHED-ASSERT] "); dbg_str(msg);
    dbg_str(" -> "); dbg_dec(a); dbg_str(", "); dbg_dec(b);
    dbg_str("\r\n");
    for (;;) __asm__ volatile("cli; hlt");
}
#define SCHED_ASSERT(cond, msg, a, b) do { if (!(cond)) sched_panic((msg), (a), (b)); } while (0)
#else
#define SCHED_ASSERT(cond, msg, a, b) ((void)0)
#endif

/* KSTACK_SZ comes from vmm.h (kernel stacks are pool-allocated). */

/* A task is RUNNING while a CPU executes it (so other CPUs skip it), and ZOMBIE
 * after it exits (slot + pid kept until the parent reaps it with wait()). */
enum { TASK_UNUSED = 0, TASK_RUNNABLE, TASK_RUNNING, TASK_BLOCKED,
       TASK_SLEEPING, TASK_WAITING, TASK_ZOMBIE };

#define PID_INIT 1                  /* slot 1 is init -- the first user task */

struct task {
    uint64_t krsp;          /* saved kernel rsp -> a struct regs while parked  */
    uint64_t cr3;           /* physical PML4 of this task's address space      */
    uint64_t kstack;        /* base of the pool-allocated kernel stack          */
    uint64_t kstack_top;    /* TSS.rsp0 to use when this task is current        */
    uint64_t wake_tick;     /* timer tick at which a TASK_SLEEPING task wakes   */
    int      state;
    int      id;            /* slot index                                       */
    int      pid;           /* monotonic process id (init = 1)                  */
    int      parent;        /* slot of the creator; reaped by it via wait()     */
    int      uid;           /* owner identity for fs writes (TOS_UID_* in perm.h) */
    int      home;          /* CPU whose queue holds it / last ran it           */
    int      tty;           /* pty channel bound to stdio, or -1 = console/keyboard */
    uint64_t anon_brk;      /* top of this task's anonymous mmap region (0 = none yet) */
    volatile int kill_req;  /* SYS_WM_KILL set this: die on the next return to ring 3 */
};

static struct task tasks[MAX_TASKS];
static uint8_t idle_stacks[MAX_CPUS][KSTACK_SZ] __attribute__((aligned(16)));
static int next_pid = 1;
static volatile int sched_started = 0;   /* read by APs spinning before they schedule */
static uint64_t kernel_cr3;         /* shared kernel/idle address space */
static spinlock_t sched_lock = SPINLOCK_INIT;

/* Ring-3 / ring-0 selectors (| RPL where applicable); USER_VBASE and
 * USER_STACK_TOP come from vmm.h. */
#define UCODE_RPL3 0x1b
#define UDATA_RPL3 0x23
#define KCODE      0x08
#define KDATA      0x10

static void idle_main(void) {
    for (;;) __asm__ volatile("hlt");   /* IF=1 in our frame, so the timer wakes us */
}

/* Lay down a fresh interrupt frame at the top of task `idx`'s kernel stack so
 * the first switch into it "returns" straight to `entry` at the given ring. */
static void frame_init(int idx, uint64_t entry, uint64_t rsp,
                        uint64_t cs, uint64_t ss) {
    struct regs *f = (struct regs *)(tasks[idx].kstack_top - sizeof(struct regs));
    uint64_t *w = (uint64_t *)f;
    for (unsigned i = 0; i < sizeof(struct regs) / 8; i++) w[i] = 0;
    f->rip    = entry;
    f->cs     = cs;
    f->rflags = 0x202;                  /* IF set + reserved bit 1 */
    f->rsp    = rsp;
    f->ss     = ss;
    tasks[idx].krsp = (uint64_t)f;
}

/* Build a CPU's idle context: a ring-0 frame that runs idle_main on the CPU's
 * own idle stack, in the shared kernel address space. */
static void build_idle(int cpu) {
    uint64_t top = (uint64_t)(idle_stacks[cpu] + KSTACK_SZ);
    struct regs *f = (struct regs *)(top - sizeof(struct regs));
    uint64_t *w = (uint64_t *)f;
    for (unsigned i = 0; i < sizeof(struct regs) / 8; i++) w[i] = 0;
    f->rip = (uint64_t)idle_main; f->cs = KCODE; f->rflags = 0x202;
    f->rsp = top; f->ss = KDATA;
    cpus[cpu].idle_krsp = (uint64_t)f;
    cpus[cpu].idle_cr3  = kernel_cr3;
}

void sched_init(void) {
    kernel_cr3 = vmm_kernel_pml4();
    build_idle(0);
    cpus[0].current = -1;
}

uint64_t sched_kernel_cr3(void) { return kernel_cr3; }

static int cur_task(void) { return sched_started ? cpus[this_cpu()].current : 0; }
int sched_current(void)   { return cur_task(); }

static int free_slot(void) {
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED) return i;
    return -1;
}

/* --- per-CPU run queues -------------------------------------------------- */
/* The ring holds at most MAX_TASKS-1 live tasks, so MAX_TASKS slots never wrap
 * onto a live entry. rq_push/rq_pop assume the CPU's rq_lock is held. */

static void rq_push(int c, int t) {
    struct cpu *p = &cpus[c];
    /* Held: c's rq_lock, so this queue + tasks[t].state are stable to read. */
    SCHED_ASSERT(p->rq_count < MAX_TASKS, "run-queue overflow [cpu,count]", c, p->rq_count);
    SCHED_ASSERT(tasks[t].state == TASK_RUNNABLE, "enqueue of non-runnable task [task,state]",
                 t, tasks[t].state);
#ifdef SCHED_DEBUG
    for (int k = 0, i = p->rq_head; k < p->rq_count; k++, i = (i + 1) % MAX_TASKS)
        SCHED_ASSERT(p->rq[i] != t, "double-enqueue onto same CPU [cpu,task]", c, t);
#endif
    p->rq[p->rq_tail] = t;
    p->rq_tail = (p->rq_tail + 1) % MAX_TASKS;
    p->rq_count++;
    tasks[t].home = c;
}

static int rq_pop(int c) {
    struct cpu *p = &cpus[c];
    if (p->rq_count == 0) return -1;
    int t = p->rq[p->rq_head];
    p->rq_head = (p->rq_head + 1) % MAX_TASKS;
    p->rq_count--;
    return t;
}

/* Enqueue a now-RUNNABLE task onto CPU c's queue (locks c's rq_lock). */
static void rq_enqueue(int c, int t) {
    uint64_t f = spin_lock_irqsave(&cpus[c].rq_lock);
    rq_push(c, t);
    spin_unlock_irqrestore(&cpus[c].rq_lock, f);
}

/* Dequeue one task from this CPU's own queue, or -1 if empty. */
static int rq_dequeue(int c) {
    uint64_t f = spin_lock_irqsave(&cpus[c].rq_lock);
    int t = rq_pop(c);
    spin_unlock_irqrestore(&cpus[c].rq_lock, f);
    return t;
}

/* Load balancing: an idle CPU steals one task from the busiest other queue. The
 * count read is an unlocked hint; the steal itself is locked and may come up
 * empty (the victim drained meanwhile), in which case the caller just idles. */
static int rq_steal(int self) {
    int victim = -1, best = 0;
    for (int c = 0; c < ncpu; c++) {
        if (c == self) continue;
        if (cpus[c].rq_count > best) { best = cpus[c].rq_count; victim = c; }
    }
    if (victim < 0) return -1;
    uint64_t f = spin_lock_irqsave(&cpus[victim].rq_lock);
    int t = rq_pop(victim);
    spin_unlock_irqrestore(&cpus[victim].rq_lock, f);
    return t;
}

/* Pick the least-loaded online CPU for a freshly-created task (load = queued +
 * one if it currently has something running). Unlocked counts are fine here --
 * placement is only a hint that stealing later corrects. */
static int pick_home(void) {
    int best = 0, best_load = 0x7fffffff;
    for (int c = 0; c < ncpu; c++) {
        int load = cpus[c].rq_count + (cpus[c].current >= 0 ? 1 : 0);
        if (load < best_load) { best_load = load; best = c; }
    }
    return best;
}

/* Flip a non-runnable task (SLEEPING/BLOCKED/WAITING) back to RUNNABLE and put
 * it on its home CPU's queue. Caller holds sched_lock. */
static void make_runnable(int i) {
    tasks[i].state = TASK_RUNNABLE;
    rq_enqueue(tasks[i].home, i);
}

int sched_spawn(const char *prog) {
    /* Build the new address space (reads the ELF from disk) and its kernel stack
     * BEFORE taking sched_lock: both self-serialise (frame allocator / fs+ata
     * locks) and touch no task-table state, and the ELF read is slow, preemptible
     * disk I/O that must not run with interrupts off under sched_lock (see
     * sched_exit). Only the slot reservation + task-table publish need the lock. */
    uint64_t entry;
    uint64_t cr3 = vmm_create_user(prog, &entry);
    if (!cr3) return -1;
    uint64_t ks = vmm_alloc_kstack();
    if (!ks) { vmm_destroy_user(cr3); return -1; }
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int idx = free_slot();
    if (idx < 0) { spin_unlock_irqrestore(&sched_lock, f); vmm_destroy_user(cr3); vmm_free_kstack(ks); return -1; }
    tasks[idx].cr3        = cr3;
    tasks[idx].kstack     = ks;
    tasks[idx].kstack_top = ks + KSTACK_SZ;
    tasks[idx].id         = idx;
    tasks[idx].pid        = next_pid++;
    tasks[idx].parent     = cur_task();
    tasks[idx].uid        = TOS_UID_SYSTEM;           /* the kernel-spawned boot task (init) is system */
    tasks[idx].tty        = -1;                       /* spawned tasks use the console/keyboard */
    tasks[idx].anon_brk   = 0;                        /* fresh address space: no mmap region yet */
    frame_init(idx, entry, USER_STACK_TOP, UCODE_RPL3, UDATA_RPL3);
    tasks[idx].state      = TASK_RUNNABLE;            /* fields set -> now publish */
    rq_enqueue(pick_home(), idx);
    int pid = tasks[idx].pid;
    spin_unlock_irqrestore(&sched_lock, f);
    return pid;
}

/* Core switch. Saves the outgoing frame, picks a task (or idle) for this CPU,
 * and returns the frame to resume on. The hot callers (sched_tick/sched_yield)
 * hold no sched_lock -- the queue ops take this CPU's rq_lock, and the state
 * writes touch only the task this CPU is running or just dequeued. The syscall
 * callers (exit/wait/sleep/block) hold sched_lock; they set the outgoing task's
 * state before calling, so it is not re-queued here. */
static struct regs *do_switch(struct regs *cur) {
    int cpu  = this_cpu();
    int prev = cpus[cpu].current;
    if (prev >= 0) {
        tasks[prev].krsp = (uint64_t)cur;
        if (tasks[prev].state == TASK_RUNNING) {       /* preempted -> still runs  */
            tasks[prev].state = TASK_RUNNABLE;
            rq_enqueue(cpu, prev);                     /* stays on this CPU        */
        }
    } else {
        cpus[cpu].idle_krsp = (uint64_t)cur;          /* was idle */
    }

    int n = rq_dequeue(cpu);                           /* own queue first ...      */
    if (n < 0) n = rq_steal(cpu);                      /* ... else steal work      */
    uint64_t cr3, krsp;
    if (n < 0) {                                       /* nothing runnable -> idle */
        cpus[cpu].current = -1;
        tss_set_rsp0(cpu, (uint64_t)(idle_stacks[cpu] + KSTACK_SZ));
        cr3 = cpus[cpu].idle_cr3; krsp = cpus[cpu].idle_krsp;
    } else {
        /* n came off a run queue (popped under an rq_lock, so its state is
         * published to us). It MUST be RUNNABLE: a RUNNING value here means it
         * was also live on another CPU -- the two-CPUs bug this guard exists to
         * catch -- and any other state means it was left queued after blocking. */
        SCHED_ASSERT(tasks[n].state == TASK_RUNNABLE, "scheduled a non-runnable task [task,state]",
                     n, tasks[n].state);
        tasks[n].state = TASK_RUNNING;
        tasks[n].home  = cpu;                          /* runs here now            */
        cpus[cpu].current = n;
        tss_set_rsp0(cpu, tasks[n].kstack_top);
        cr3 = tasks[n].cr3; krsp = tasks[n].krsp;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    /* Now off the outgoing PML4: if prev just exited, free its address space.
     * (Only reachable via sched_exit, which holds sched_lock around vmm.) */
    if (prev >= 0 && tasks[prev].state == TASK_ZOMBIE && tasks[prev].cr3) {
        vmm_destroy_user(tasks[prev].cr3);
        tasks[prev].cr3 = 0;
    }
    return (struct regs *)krsp;
}

void sched_start(void) {
    /* init was spawned (and enqueued) while only the BSP existed, so it sits in
     * CPU 0's queue. Claim it as CPU 0's running task *before* opening the gate,
     * so a just-woken AP can't steal it out from under us. */
    int idx = rq_dequeue(0);
    cpus[0].current = idx;
    tasks[idx].state = TASK_RUNNING;
    tasks[idx].home  = 0;
    sched_started = 1;                                  /* APs may schedule now */
    tss_set_rsp0(0, tasks[idx].kstack_top);
    __asm__ volatile("mov %0, %%cr3" : : "r"(tasks[idx].cr3) : "memory");
    sched_run_first(tasks[idx].krsp);
}

/* An application processor joins scheduling: build its idle context, then wait
 * until the BSP has started the scheduler before enabling interrupts. APs must
 * stay interrupt-disabled through the BSP's INIT-SIPI bring-up sequence -- an AP
 * servicing a LAPIC-timer tick mid-bring-up races the still-running sequence and
 * triple-faults the machine. Once sched_started is set (and init is claimed),
 * we drop into the idle context with interrupts on and the timer preempts us. */
void sched_ap_enter(int cpu) {
    uint64_t f = spin_lock_irqsave(&sched_lock);
    build_idle(cpu);
    cpus[cpu].current = -1;
    tss_set_rsp0(cpu, (uint64_t)(idle_stacks[cpu] + KSTACK_SZ));
    spin_unlock_irqrestore(&sched_lock, f);           /* still interrupts-off here */
    while (!sched_started) __asm__ volatile("pause"); /* wait for the BSP to go */
    sched_run_first(cpus[cpu].idle_krsp);             /* now interrupts-on; never returns */
}

/* Wake any sleeping task whose deadline has arrived (sched_lock held by caller).
 * Only the BSP advances the tick counter, so only it needs to run this. */
static void wake_sleepers(void) {
    uint64_t now = timer_ticks();
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_SLEEPING && (int64_t)(now - tasks[i].wake_tick) >= 0)
            make_runnable(i);
}

static int has_live_child(int slot) {
    for (int i = 1; i < MAX_TASKS; i++)
        if (i != slot && tasks[i].parent == slot &&
            (tasks[i].state == TASK_RUNNABLE || tasks[i].state == TASK_RUNNING ||
             tasks[i].state == TASK_BLOCKED  || tasks[i].state == TASK_SLEEPING ||
             tasks[i].state == TASK_WAITING))
            return 1;
    return 0;
}

struct regs *sched_tick(struct regs *r) {
    if (!sched_started) return r;          /* APs idle until sched_start claims init */
    if (this_cpu() == 0) {                 /* the BSP owns the global tick counter */
        uint64_t f = spin_lock_irqsave(&sched_lock);
        wake_sleepers();
        spin_unlock_irqrestore(&sched_lock, f);
    }
    return do_switch(r);                    /* preemption: rq_lock only */
}

struct regs *sched_yield(struct regs *r) {
    return do_switch(r);                    /* voluntary switch: rq_lock only */
}

struct regs *sched_block_read(struct regs *r) {
    uint64_t f = spin_lock_irqsave(&sched_lock);
    tasks[cpus[this_cpu()].current].state = TASK_BLOCKED;
    r->rip -= 2;                  /* rewind over `int 0x80` so the read restarts */
    struct regs *next = do_switch(r);
    spin_unlock_irqrestore(&sched_lock, f);
    return next;
}

struct regs *sched_sleep(struct regs *r, uint64_t nticks) {
    if (nticks == 0) { r->rax = 0; return r; }
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int me = cpus[this_cpu()].current;
    tasks[me].wake_tick = timer_ticks() + nticks;
    tasks[me].state     = TASK_SLEEPING;
    r->rax = 0;
    struct regs *next = do_switch(r);
    spin_unlock_irqrestore(&sched_lock, f);
    return next;
}

struct regs *sched_exit(struct regs *r) {
    int me = cpus[this_cpu()].current;
    if (me == PID_INIT) {                       /* init must never exit */
        console_puts("[kernel] PANIC: init (pid 1) exited -- halting\r\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    /* Flush+close this task's files and tear down its windows BEFORE taking
     * sched_lock. Closing a writing file flushes a draft to disk, and disk I/O is
     * now PREEMPTIBLE (runs with interrupts on -- see the 0x80 case in traps.c and
     * spin_lock_preempt). Doing it under sched_lock (IF=0) would both freeze the
     * machine for the transfer and risk a single-CPU deadlock: an IF=0 spin on the
     * fs/ata lock can never be broken if that lock is held by a task that was
     * preempted mid-I/O. The task is `current` and on its way out, so nothing else
     * touches its open-file table or windows -- no lock is needed for this. */
    fs_close_all(me);
    win_owner_exited(me);                       /* tear down any windows this task owned */
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 1; i < MAX_TASKS; i++)         /* re-parent orphans to init */
        if (i != me && tasks[i].state != TASK_UNUSED && tasks[i].parent == me)
            tasks[i].parent = PID_INIT;
    tasks[me].state = TASK_ZOMBIE;
    int pp = tasks[me].parent;
    if (pp > 0 && tasks[pp].state == TASK_WAITING)  make_runnable(pp);
    if (tasks[PID_INIT].state == TASK_WAITING)      make_runnable(PID_INIT);
    struct regs *next = do_switch(r);
    spin_unlock_irqrestore(&sched_lock, f);
    return next;
}

/* A task that faulted in ring 3 is killed exactly like a voluntary exit: it
 * becomes a zombie, its windows are torn down and orphans reparented, and the
 * scheduler moves on -- so one misbehaving app can't take down the OS. The trap
 * handler has already logged the fault before calling this. */
struct regs *sched_kill(struct regs *r) { return sched_exit(r); }

/* Request an asynchronous kill of an arbitrary task (SYS_WM_KILL / Super+Shift+Q).
 * We can't safely tear a task down from another CPU's context (it may be mid-
 * syscall holding locks), so we just flag it: the next time it returns to the
 * kernel from ring 3 (a syscall or a preemption tick -- at most ~10 ms away) it
 * reaps itself via sched_reap_killed(). A blocked/sleeping target is made runnable
 * so it actually gets there. init is never killable. Returns 0, or -1. */
int sched_request_kill(int slot) {
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int rc = -1;
    if (slot > PID_INIT && slot < MAX_TASKS &&
        tasks[slot].state != TASK_UNUSED && tasks[slot].state != TASK_ZOMBIE) {
        tasks[slot].kill_req = 1;
        if (tasks[slot].state == TASK_BLOCKED || tasks[slot].state == TASK_SLEEPING ||
            tasks[slot].state == TASK_WAITING)
            make_runnable(slot);
        rc = 0;
    }
    spin_unlock_irqrestore(&sched_lock, f);
    return rc;
}

/* Called on every kernel entry from ring 3 (see isr_dispatch). If the current
 * task was flagged for an async kill, terminate it now -- it is `current` here, so
 * this reuses the ordinary exit teardown (zombie + windows freed + orphans
 * reparented) and returns a different task's frame. Returns 0 if nothing to do. */
struct regs *sched_reap_killed(struct regs *r) {
    int me = cpus[this_cpu()].current;
    if (me <= PID_INIT || !tasks[me].kill_req) return 0;
    console_puts("[kernel] task killed by request (pid=");
    console_putdec(tasks[me].pid);
    console_puts(")\r\n");
    tasks[me].kill_req = 0;
    return sched_exit(r);
}

int sched_pid(int slot) { return (slot > 0 && slot < MAX_TASKS) ? tasks[slot].pid : -1; }
/* The running task's pid and its parent's pid (SYS_GETPID/SYS_GETPPID). The picker
 * uses getppid() == the caller's getpid() to namespace its /tmp handoff files. */
int sched_getpid(void) { return sched_pid(cur_task()); }
int sched_ppid(void) {
    int me = cur_task();
    return (me > 0 && me < MAX_TASKS) ? sched_pid(tasks[me].parent) : -1;
}

/* The owner identity of the running task -- consulted by the fs syscalls. */
int sched_uid(void) { return tasks[cur_task()].uid; }

/* SYS_SETUID: a uid-0 (system) task may set any uid; everyone else may only drop
 * privilege (raise the numeric uid, never lower it). Returns 0 or -1. The boot
 * chain (init) uses this once to launch the desktop session as the user. */
int sched_setuid(int uid) {
    if (uid < 0) return -1;
    int me = cur_task();
    if (tasks[me].uid != TOS_UID_SYSTEM && uid < tasks[me].uid) return -1;  /* no privilege gain */
    tasks[me].uid = uid;
    return 0;
}

struct regs *sched_wait(struct regs *r) {
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int me = cpus[this_cpu()].current;
    struct regs *ret = r;
    int reaped = -2;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_ZOMBIE && tasks[i].parent == me) {
            reaped = tasks[i].pid;
            vmm_free_kstack(tasks[i].kstack);
            tasks[i].kstack = 0;
            tasks[i].state = TASK_UNUSED;
            tasks[i].parent = 0;
            break;
        }
    if (reaped != -2) {
        r->rax = (uint64_t)(int64_t)reaped;
    } else if (!has_live_child(me)) {
        r->rax = (uint64_t)-1;                  /* no children */
    } else {
        tasks[me].state = TASK_WAITING;
        r->rip -= 2;
        ret = do_switch(r);
    }
    spin_unlock_irqrestore(&sched_lock, f);
    return ret;
}

/* Non-blocking wait: reap one exited child (return its pid), or return 0 if
 * children are still alive, or -1 if the caller has no children. Lets the GUI
 * supervise the shell from an event loop without blocking on it. */
struct regs *sched_trywait(struct regs *r) {
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int me = cpus[this_cpu()].current;
    int reaped = 0;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_ZOMBIE && tasks[i].parent == me) {
            reaped = tasks[i].pid;
            vmm_free_kstack(tasks[i].kstack);
            tasks[i].kstack = 0;
            tasks[i].state = TASK_UNUSED;
            tasks[i].parent = 0;
            break;
        }
    if (reaped)                       r->rax = (uint64_t)(int64_t)reaped;
    else if (has_live_child(me))      r->rax = 0;
    else                              r->rax = (uint64_t)-1;
    spin_unlock_irqrestore(&sched_lock, f);
    return r;
}

struct regs *sched_fork(struct regs *r) {
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int idx = free_slot();
    if (idx < 0) { r->rax = (uint64_t)-1; spin_unlock_irqrestore(&sched_lock, f); return r; }
    uint64_t cr3 = vmm_fork(tasks[cpus[this_cpu()].current].cr3);
    if (!cr3) { r->rax = (uint64_t)-1; spin_unlock_irqrestore(&sched_lock, f); return r; }
    uint64_t ks = vmm_alloc_kstack();
    if (!ks) { vmm_destroy_user(cr3); r->rax = (uint64_t)-1; spin_unlock_irqrestore(&sched_lock, f); return r; }
    tasks[idx].cr3        = cr3;
    tasks[idx].kstack     = ks;
    tasks[idx].kstack_top = ks + KSTACK_SZ;
    tasks[idx].id         = idx;
    tasks[idx].pid        = next_pid++;
    tasks[idx].parent     = cpus[this_cpu()].current;
    tasks[idx].uid        = tasks[cpus[this_cpu()].current].uid;   /* child inherits the caller's identity */
    tasks[idx].tty        = tasks[cpus[this_cpu()].current].tty;   /* inherit stdio binding */
    tasks[idx].anon_brk   = 0;       /* anon mmap region is NOT inherited (like the fb/surf slots) */
    fs_fork(cpus[this_cpu()].current, idx);                        /* inherit the working dir */
    struct regs *cf = (struct regs *)(tasks[idx].kstack_top - sizeof(struct regs));
    *cf = *r;
    cf->rax = 0;                                /* child returns 0 */
    tasks[idx].krsp = (uint64_t)cf;
    tasks[idx].state = TASK_RUNNABLE;           /* fields set -> now publish */
    rq_enqueue(pick_home(), idx);
    r->rax = (uint64_t)tasks[idx].pid;          /* parent gets the child pid */
    spin_unlock_irqrestore(&sched_lock, f);
    return r;
}

struct regs *sched_exec(struct regs *r, const char *prog) {
    /* Build the replacement address space (reads the ELF from disk) BEFORE taking
     * sched_lock -- the load is slow, preemptible disk I/O that must not freeze the
     * machine under the lock (see sched_exit/sched_spawn). It runs on the caller's
     * still-current CR3 and produces a fresh CR3; the swap below is the only part
     * that needs the task table. A preemption in this window is harmless: the new
     * CR3 is just a local until installed. */
    uint64_t entry;
    uint64_t cr3 = vmm_create_user(prog, &entry);  /* built on the old CR3 */
    if (!cr3) { r->rax = (uint64_t)-1; return r; }
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int me = cpus[this_cpu()].current;
    uint64_t old = tasks[me].cr3;
    tasks[me].cr3 = cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    vmm_destroy_user(old);
    tasks[me].anon_brk = 0;                        /* old anon region died with the old space */
    frame_init(me, entry, USER_STACK_TOP, UCODE_RPL3, UDATA_RPL3);
    struct regs *next = (struct regs *)tasks[me].krsp;
    spin_unlock_irqrestore(&sched_lock, f);
    return next;
}

void sched_wake_readers(void) {
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_BLOCKED) make_runnable(i);
    spin_unlock_irqrestore(&sched_lock, f);
}

/* stdio binding (used by the syscall layer to route SYS_READ/WRITE through a pty)
 * and small queries for SYS_SYSINFO. */
int  sched_get_tty(int slot)          { return tasks[slot].tty; }
void sched_set_tty(int slot, int tty) { tasks[slot].tty = tty; }
uint64_t sched_task_cr3(int slot)     { return tasks[slot].cr3; }
int  sched_task_count(void) {
    int n = 0;
    for (int i = 1; i < MAX_TASKS; i++) if (tasks[i].state != TASK_UNUSED) n++;
    return n;
}

/* Map `nbytes` of fresh private RAM into the calling task's address space and
 * return its base (or 0). Grows the task's anon region; freed when it exits.
 * Runs in syscall context (IF=0, so `current` on this CPU is stable) -- no
 * sched_lock needed: the frame allocator serialises itself and we touch only
 * this task's own brk. */
uint64_t sched_mmap(uint64_t nbytes) {
    if (!nbytes) return 0;
    int n = (int)((nbytes + 0xfff) / 0x1000);
    return vmm_mmap(&tasks[cur_task()].anon_brk, n);
}
