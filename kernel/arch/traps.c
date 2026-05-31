/* Single entry point for everything that funnels through isr_common (cpu.asm):
 * CPU exceptions, the timer and keyboard IRQs, and the int 0x80 syscall. The
 * dispatcher returns the register frame to resume -- the same one for an
 * ordinary syscall, or a different task's frame to perform a context switch. */
#include "console.h"
#include "syscall.h"
#include "sched.h"
#include "keyboard.h"
#include "timer.h"
#include "fs.h"
#include "cpu.h"
#include "apic.h"
#include "percpu.h"
#include "vmm.h"
#include "mouse.h"
#include "rtc.h"
#include "pci.h"
#include "speaker.h"
#include "ipc.h"
#include "smp.h"
#include "vmm.h"
#include "clipboard.h"
#include <stdint.h>

static const char *const exc_names[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
    "#DF", "---", "#TS", "#NP", "#SS", "#GP", "#PF", "---",
    "#MF", "#AC", "#MC", "#XF", "#VE", "#CP", "---", "---",
    "---", "---", "---", "---", "#HV", "#VC", "#SX", "---",
};

/* A CPU exception. If it came from ring 3 (CPL=3), only the offending user task
 * is at fault: we log it and kill just that task (sched_kill), then reschedule --
 * one moody app can't take down the whole OS, the way a kernel-mode fault must.
 * A fault from ring 0 is a genuine kernel bug, so we still halt. */
static struct regs *exception_handler(struct regs *r) {
    uint64_t cr2 = 0;
    if (r->vec == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    console_puts("\r\n[EXCEPTION] ");
    console_puts(r->vec < 32 ? exc_names[r->vec] : "???");
    console_puts(" vector=");   console_putdec(r->vec);
    console_puts(" error=");    console_puthex(r->err);
    console_puts(" rip=");      console_puthex(r->rip);
    console_puts(" cs=");       console_puthex(r->cs);
    if (r->vec == 14) { console_puts(" cr2="); console_puthex(cr2); }

    if ((r->cs & 3) == 3) {                   /* fault in ring 3 -> kill that task only */
        console_puts("  -- killing faulting task pid=");
        console_putdec((uint64_t)sched_pid(sched_current()));
        console_puts(" (OS stays up)\r\n");
        return sched_kill(r);                 /* zombie + reschedule; never resumes here */
    }
    console_puts("  -- kernel-mode fault, system halted\r\n");
    for (;;) __asm__ volatile("cli; hlt");
}

/* Reject a bad user pointer (fail the syscall) instead of dereferencing it and
 * taking a kernel-mode fault that halts the machine. STR validates a path/string
 * argument; BUF validates a struct/array argument of `n` bytes. */
#define UFAIL      do { r->rax = (uint64_t)-1; return r; } while (0)
#define STR(p)     do { if (!vmm_user_str_ok((uint64_t)(p), 8192)) UFAIL; } while (0)
#define BUF(p, n)  do { if (!vmm_user_ok((uint64_t)(p), (uint64_t)(n))) UFAIL; } while (0)

static struct regs *syscall_dispatch(struct regs *r) {
    uint64_t num = r->rax, arg = r->rdi;
    switch (num) {
    case SYS_WRITE: {                        /* stdout: pty if bound, else the console */
        STR(arg);
        int tty = sched_get_tty(sched_current());
        if (tty >= 0) pty_out_write(tty, (const char *)arg);
        else          console_puts((const char *)arg);
        r->rax = 0;
        return r;
    }
    case SYS_PUTC: {
        int tty = sched_get_tty(sched_current());
        if (tty >= 0) pty_out_putc(tty, (char)arg);
        else          console_putc((char)arg);
        r->rax = 0;
        return r;
    }
    case SYS_READ: {                         /* stdin: pty if bound, else the keyboard */
        int tty = sched_get_tty(sched_current());
        int c = (tty >= 0) ? pty_in_getc(tty) : kbd_getc();
        if (c >= 0) { r->rax = (uint64_t)(uint8_t)c; return r; }
        return sched_block_read(r);          /* nothing buffered -> sleep until woken */
    }
    case SYS_YIELD:
        r->rax = 0;
        return sched_yield(r);
    case SYS_SPAWN: {
        STR(arg);
        char prog[128];
        const char *s = (const char *)arg;
        int i = 0;
        for (; i < (int)sizeof(prog) - 1 && s[i]; i++) prog[i] = s[i];
        prog[i] = 0;
        r->rax = (uint64_t)sched_spawn(prog);
        return r;
    }
    case SYS_FORK:
        return sched_fork(r);
    case SYS_EXEC: {
        STR(arg);                            /* a bad path fails exec, never faults the kernel */
        char prog[128];                      /* copy before exec frees the old space */
        const char *s = (const char *)arg;
        int i = 0;
        for (; i < (int)sizeof(prog) - 1 && s[i]; i++) prog[i] = s[i];
        prog[i] = 0;
        return sched_exec(r, prog);
    }
    case SYS_WAIT:
        return sched_wait(r);
    case SYS_SHUTDOWN:
        console_puts("[kernel] shutdown requested -- powering off\r\n");
        outw(0x604, 0x2000);                  /* ACPI poweroff (modern QEMU) */
        outw(0xB004, 0x2000);                 /* ACPI poweroff (older QEMU)  */
        for (;;) __asm__ volatile("cli; hlt");  /* fall back to halting the CPU */
    case SYS_LS:
        fs_ls();
        r->rax = 0;
        return r;
    case SYS_OPEN:
        STR(r->rdi);
        r->rax = (uint64_t)(int64_t)fs_open((const char *)r->rdi, (int)r->rsi);
        return r;
    case SYS_READ_FD:
        BUF(r->rsi, r->rdx);
        r->rax = (uint64_t)(int64_t)fs_read((int)r->rdi, (void *)r->rsi, (uint32_t)r->rdx);
        return r;
    case SYS_WRITE_FD:
        BUF(r->rsi, r->rdx);
        r->rax = (uint64_t)(int64_t)fs_write((int)r->rdi, (const void *)r->rsi, (uint32_t)r->rdx);
        return r;
    case SYS_CLOSE:
        r->rax = (uint64_t)(int64_t)fs_close((int)r->rdi);
        return r;
    case SYS_UNLINK:
        STR(r->rdi);
        r->rax = (uint64_t)(int64_t)fs_unlink((const char *)r->rdi);
        return r;
    case SYS_SLEEP:
        return sched_sleep(r, r->rdi);
    case SYS_GETCPU:
        r->rax = (uint64_t)this_cpu();
        return r;
    case SYS_CPAINT:                         /* the block cursor belongs to the kernel
                                              * console; under a terminal emulator the
                                              * emulator draws its own cursor, so no-op */
        if (sched_get_tty(sched_current()) < 0)
            console_paint_cursor((char)(arg & 0xff), (int)((arg >> 8) & 1));
        r->rax = 0;
        return r;
    case SYS_FBINFO:
        BUF(arg, sizeof(struct fbinfo));
        r->rax = (uint64_t)(int64_t)vmm_map_user_fb((struct fbinfo *)arg);
        return r;
    case SYS_CON_WINDOW:
        console_set_window((int)((r->rdi >> 16) & 0xffff), (int)(r->rdi & 0xffff),
                           (int)((r->rsi >> 16) & 0xffff), (int)(r->rsi & 0xffff));
        r->rax = 0;
        return r;
    case SYS_TRYWAIT:
        return sched_trywait(r);
    case SYS_MOUSE:
        BUF(arg, sizeof(struct mousestate));
        mouse_get((struct mousestate *)arg);
        r->rax = 0;
        return r;
    case SYS_TIME:
        BUF(arg, sizeof(struct rtctime));
        rtc_now((struct rtctime *)arg);
        r->rax = 0;
        return r;
    case SYS_LSPCI:
        pci_list();
        r->rax = 0;
        return r;
    case SYS_BEEP:
        speaker_set((uint32_t)arg);
        r->rax = 0;
        return r;
    case SYS_REBOOT:
        console_puts("[kernel] reboot requested\r\n");
        outb(0x64, 0xFE);                     /* pulse the 8042 reset line */
        for (;;) __asm__ volatile("cli; hlt");
    case SYS_GETKEY:                          /* non-blocking keyboard read (compositor) */
        r->rax = (uint64_t)(int64_t)kbd_getc();
        return r;
    case SYS_WIN_CREATE:
        BUF(arg, sizeof(struct wininfo));
        r->rax = (uint64_t)(int64_t)win_create((struct wininfo *)arg);
        return r;
    case SYS_WIN_PRESENT:
        r->rax = (uint64_t)(int64_t)win_present((int)arg);
        return r;
    case SYS_WIN_POLL:
        BUF(r->rsi, sizeof(struct winevent));
        r->rax = (uint64_t)(int64_t)win_poll_event((int)r->rdi, (struct winevent *)r->rsi);
        return r;
    case SYS_WIN_CLOSE:
        r->rax = (uint64_t)(int64_t)win_close((int)arg);
        return r;
    case SYS_WM_REGISTER:
        wm_register();
        r->rax = 0;
        return r;
    case SYS_WM_WINDOWS:
        BUF(r->rdi, r->rsi * sizeof(struct wmwin));
        r->rax = (uint64_t)(int64_t)wm_windows((struct wmwin *)r->rdi, (int)r->rsi);
        return r;
    case SYS_WM_SEND_KEY:
        r->rax = (uint64_t)(int64_t)wm_send_key((int)r->rdi, (int)r->rsi);
        return r;
    case SYS_WM_KILL:
        r->rax = (uint64_t)(int64_t)wm_kill_window((int)r->rdi);
        return r;
    case SYS_PTY_OPEN:
        r->rax = (uint64_t)(int64_t)pty_open();
        return r;
    case SYS_PTY_ATTACH:
        r->rax = (uint64_t)(int64_t)pty_attach((int)arg);
        return r;
    case SYS_PTY_READ:
        BUF(r->rsi, r->rdx);
        r->rax = (uint64_t)(int64_t)pty_read((int)r->rdi, (void *)r->rsi, (int)r->rdx);
        return r;
    case SYS_PTY_WRITE:
        BUF(r->rsi, r->rdx);
        r->rax = (uint64_t)(int64_t)pty_write((int)r->rdi, (const void *)r->rsi, (int)r->rdx);
        return r;
    case SYS_PTY_CLOSE:
        r->rax = (uint64_t)(int64_t)pty_close((int)arg);
        return r;
    case SYS_SYSINFO: {
        BUF(arg, sizeof(struct sysinfo));
        struct sysinfo *si = (struct sysinfo *)arg;
        uint32_t w, h; vmm_fb_size(&w, &h);
        si->ncpu = (uint32_t)smp_cpu_count();
        si->timer_hz = TIMER_HZ;
        si->ram_bytes = vmm_ram_bytes();
        si->uptime_ticks = timer_ticks();
        si->fb_w = w; si->fb_h = h;
        si->nfiles = fs_nfiles();
        si->ntasks = (uint32_t)sched_task_count();
        r->rax = 0;
        return r;
    }
    case SYS_ISATTY:
        r->rax = (sched_get_tty(sched_current()) >= 0) ? 1 : 0;
        return r;
    case SYS_WIN_RESIZE:
        r->rax = (uint64_t)(int64_t)win_resize((int)r->rdi, (int)r->rsi, (int)r->rdx);
        return r;
    case SYS_WM_POST:
        r->rax = (uint64_t)(int64_t)wm_post_event((int)r->rdi, (int)r->rsi, (unsigned)r->rdx);
        return r;
    case SYS_MKDIR:
        STR(r->rdi);
        r->rax = (uint64_t)(int64_t)fs_mkdir((const char *)r->rdi);
        return r;
    case SYS_RMDIR:
        STR(r->rdi);
        r->rax = (uint64_t)(int64_t)fs_rmdir((const char *)r->rdi);
        return r;
    case SYS_CHDIR:
        STR(r->rdi);
        r->rax = (uint64_t)(int64_t)fs_chdir((const char *)r->rdi);
        return r;
    case SYS_GETCWD:
        BUF(r->rdi, r->rsi);
        r->rax = (uint64_t)(int64_t)fs_getcwd((char *)r->rdi, (int)r->rsi);
        return r;
    case SYS_READDIR:
        STR(r->rdi); BUF(r->rsi, r->rdx * sizeof(struct dirent));
        r->rax = (uint64_t)(int64_t)fs_readdir((const char *)r->rdi, (struct dirent *)r->rsi, (int)r->rdx);
        return r;
    case SYS_RENAME:
        STR(r->rdi); STR(r->rsi);
        r->rax = (uint64_t)(int64_t)fs_rename((const char *)r->rdi, (const char *)r->rsi);
        return r;
    case SYS_STAT:
        STR(r->rdi); BUF(r->rsi, sizeof(struct fstat));
        r->rax = (uint64_t)(int64_t)fs_stat((const char *)r->rdi, (struct fstat *)r->rsi);
        return r;
    case SYS_MMAP:
        r->rax = sched_mmap(r->rdi);
        return r;
    case SYS_CLIP_PUT: {
        int len = (int)(r->rdx & 0xffffff), type = (int)(r->rdx >> 24);
        if (r->rdi) STR(r->rdi);
        BUF(r->rsi, (uint64_t)len);
        r->rax = (uint64_t)(int64_t)clip_put(type, (const char *)r->rdi, (const char *)r->rsi, len);
        return r;
    }
    case SYS_CLIP_COUNT:
        r->rax = (uint64_t)clip_count();
        return r;
    case SYS_CLIP_GET:
        BUF(r->rsi, r->rdx);
        r->rax = (uint64_t)(int64_t)clip_get((int)r->rdi, (char *)r->rsi, (int)r->rdx);
        return r;
    case SYS_CLIP_INFO:
        BUF(r->rsi, sizeof(struct clipinfo));
        r->rax = (uint64_t)(int64_t)clip_info((int)r->rdi, (struct clipinfo *)r->rsi);
        return r;
    case SYS_CLIP_ACTIVE:
        r->rax = (uint64_t)(int64_t)clip_active((int)r->rdi);
        return r;
    case SYS_CLIP_CLEAR:
        clip_clear();
        r->rax = 0;
        return r;
    case SYS_EXIT:
        console_puts("[kernel] task exited (ran at CPL=");
        console_putdec(r->cs & 3);
        console_puts(")\r\n");
        return sched_exit(r);                 /* switch away; never resumes here */
    default:
        r->rax = (uint64_t)-1;
        return r;
    }
}

struct regs *isr_dispatch(struct regs *r) {
    /* Entering the kernel from ring 3: if the current task was asynchronously
     * killed (SYS_WM_KILL / Super+Shift+Q), reap it here -- it is `current`, so the
     * normal exit teardown applies and we resume a different task. */
    if ((r->cs & 3) == 3) {
        struct regs *killed = sched_reap_killed(r);
        if (killed) return killed;
    }
    switch (r->vec) {
    case 0x20:                                /* PIT timer */
        timer_tick();
        outb(0x20, 0x20);                     /* EOI to master PIC */
        return sched_tick(r);
    case 0x21:                                /* PS/2 keyboard */
        keyboard_irq();                       /* reads scancode, sends its own EOI */
        return sched_yield(r);                /* let a just-woken reader run */
    case 0x2C:                                /* PS/2 mouse (IRQ12) */
        mouse_irq();                          /* decodes a packet, sends its own EOI */
        return r;
    case 0x22:                                /* LAPIC timer (per-CPU preemption) */
        lapic_eoi();
        return sched_tick(r);
    case 0x80:                                /* syscall */
        return syscall_dispatch(r);
    default:
        return exception_handler(r);          /* kills a ring-3 task, or halts on a kernel fault */
    }
}
