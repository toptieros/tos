/* Pseudo-terminals and window surfaces -- the kernel side of the desktop.
 *
 * A pty is two byte rings: `in` (emulator -> program) and `out` (program ->
 * emulator). A program whose stdio is bound to a pty (task.tty) reads `in` on
 * SYS_READ and writes `out` on SYS_WRITE; the emulator drives the other ends.
 * Program output is also mirrored to COM1 so the serial-log tests still see it.
 *
 * A window owns a pixel surface: a run of contiguous frames mapped (US=1) into
 * both the owning app and the compositor via the surface PDPT slot (see vmm.c).
 * The app draws and SYS_WIN_PRESENT bumps a sequence; the compositor reads the
 * window set (SYS_WM_WINDOWS), lazily maps each surface into its own address
 * space, and blits. Per-window event rings carry input bytes the compositor
 * routes to the focused window. */
#include "ipc.h"
#include "sched.h"
#include "vmm.h"
#include "console.h"
#include "spinlock.h"
#include <stdint.h>

static spinlock_t ipc_lock = SPINLOCK_INIT;

/* --- pty channels --------------------------------------------------------- */
#define NPTY    4
#define PTY_BUF 8192
struct pty {
    int used;
    volatile int ih, it;          /* input ring head/tail  (emulator -> program) */
    volatile int oh, ot;          /* output ring head/tail (program -> emulator) */
    char in[PTY_BUF];
    char out[PTY_BUF];
};
static struct pty ptys[NPTY];

static int ring_push(char *buf, volatile int *h, int t, char c) {
    int n = (*h + 1) % PTY_BUF;
    if (n == t) return 0;                 /* full: drop */
    buf[*h] = c; *h = n; return 1;
}
static int ring_pop(const char *buf, int h, volatile int *t) {
    if (h == *t) return -1;               /* empty */
    char c = buf[*t]; *t = (*t + 1) % PTY_BUF; return (int)(uint8_t)c;
}

int pty_open(void) {
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    int id = -1;
    for (int i = 0; i < NPTY; i++) if (!ptys[i].used) { id = i; break; }
    if (id >= 0) {
        ptys[id].used = 1;
        ptys[id].ih = ptys[id].it = ptys[id].oh = ptys[id].ot = 0;
    }
    spin_unlock_irqrestore(&ipc_lock, f);
    return id;
}

int pty_attach(int id) {
    if (id < 0 || id >= NPTY || !ptys[id].used) return -1;
    sched_set_tty(sched_current(), id);
    return 0;
}

int pty_close(int id) {
    if (id < 0 || id >= NPTY) return -1;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    ptys[id].used = 0;
    spin_unlock_irqrestore(&ipc_lock, f);
    return 0;
}

/* emulator: drain the program's output */
int pty_read(int id, void *buf, int len) {
    if (id < 0 || id >= NPTY || !ptys[id].used) return -1;
    char *d = buf;
    int n = 0;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    while (n < len) {
        int c = ring_pop(ptys[id].out, ptys[id].oh, &ptys[id].ot);
        if (c < 0) break;
        d[n++] = (char)c;
    }
    spin_unlock_irqrestore(&ipc_lock, f);
    return n;
}

/* emulator: feed input to the program, then wake it if it was blocked reading */
int pty_write(int id, const void *buf, int len) {
    if (id < 0 || id >= NPTY || !ptys[id].used) return -1;
    const char *s = buf;
    int n = 0;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    while (n < len && ring_push(ptys[id].in, &ptys[id].ih, ptys[id].it, s[n])) n++;
    spin_unlock_irqrestore(&ipc_lock, f);
    if (n) sched_wake_readers();          /* outside the lock: avoids ipc<->sched nesting */
    return n;
}

/* stdio routing for a tty-bound program (called by the syscall dispatcher) */
int pty_in_getc(int tty) {
    if (tty < 0 || tty >= NPTY || !ptys[tty].used) return -1;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    int c = ring_pop(ptys[tty].in, ptys[tty].ih, &ptys[tty].it);
    spin_unlock_irqrestore(&ipc_lock, f);
    return c;
}
void pty_out_putc(int tty, char c) {
    if (tty < 0 || tty >= NPTY || !ptys[tty].used) return;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    ring_push(ptys[tty].out, &ptys[tty].oh, ptys[tty].ot, c);
    spin_unlock_irqrestore(&ipc_lock, f);
    console_serial_putc(c);               /* mirror to COM1 so the tests still read it */
}
void pty_out_write(int tty, const char *s) {
    for (; *s; s++) pty_out_putc(tty, *s);
}

/* --- windows / compositor ------------------------------------------------- */
#define MAX_WINDOWS 8
#define WEVQ        256
#define SURF_MAX_FRAMES 2048              /* matches SURF_SLOT_BYTES (8 MiB) in vmm.c */
enum { W_FREE = 0, W_ALIVE, W_DYING };
struct window {
    int      state;
    int      owner;                       /* task slot that created it */
    uint32_t w, h;
    int      nframes;                     /* frames backing the current surface */
    uint64_t phys;                        /* surface frames base */
    uint64_t seq;                         /* present sequence */
    uint64_t comp_vaddr;                  /* surface vaddr in the compositor, 0 = (re)map needed */
    int      comp_nf;                     /* frames currently mapped in the compositor */
    uint64_t old_phys;                    /* a resize left these old frames to free (in comp ctx) */
    int      old_nf;
    uint32_t flags;                       /* WIN_* the app created the window with */
    char     title[32];
    volatile int eh, et;                  /* input event ring (compositor -> app) */
    struct winevent ev[WEVQ];
};
static struct window wins[MAX_WINDOWS];
static int compositor = -1;

static void copy_title(char *dst, const char *src) {
    int i = 0;
    for (; i < 31 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

int win_create(struct wininfo *wi) {
    uint32_t w = wi->w, h = wi->h;
    if (w == 0 || h == 0) return -1;
    uint64_t bytes = (uint64_t)w * h * 4;
    int nframes = (int)((bytes + 0xfff) / 0x1000);
    if (nframes > SURF_MAX_FRAMES) return -1;

    uint64_t phys = vmm_alloc_surface(nframes);          /* zeroed (black) */
    if (!phys) return -1;
    int caller = sched_current();

    uint64_t f = spin_lock_irqsave(&ipc_lock);
    int id = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (wins[i].state == W_FREE) { id = i; break; }
    if (id < 0) { spin_unlock_irqrestore(&ipc_lock, f); vmm_free_surface(phys, nframes); return -1; }
    struct window *win = &wins[id];
    win->state = W_ALIVE; win->owner = caller; win->w = w; win->h = h;
    win->nframes = nframes; win->phys = phys; win->seq = 1; win->comp_vaddr = 0;
    win->comp_nf = 0; win->old_phys = 0; win->old_nf = 0;
    win->eh = win->et = 0;
    win->flags = wi->flags;
    copy_title(win->title, wi->title);
    uint64_t v = vmm_map_surface(vmm_current_pml4(), id, phys, nframes);   /* into the app */
    vmm_flush_self();
    spin_unlock_irqrestore(&ipc_lock, f);

    wi->id = id; wi->vaddr = v; wi->pitch = w;
    return id;
}

int win_present(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return -1;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    if (wins[id].state == W_ALIVE && wins[id].owner == sched_current()) wins[id].seq++;
    spin_unlock_irqrestore(&ipc_lock, f);
    return 0;
}

/* App resizes its own window's surface. The surface keeps the SAME vaddr (it is
 * fixed per id), so the app just sees new dimensions. New frames are allocated
 * and mapped into the app now; the old frames are handed to the compositor to
 * free + remap on its next snapshot (in its own context, where it isn't
 * blitting them). Returns 0, or -1 on error. */
int win_resize(int id, int w, int h) {
    if (id < 0 || id >= MAX_WINDOWS || w <= 0 || h <= 0) return -1;
    int newn = (int)(((uint64_t)w * h * 4 + 0xfff) / 0x1000);
    if (newn > SURF_MAX_FRAMES) return -1;
    uint64_t newphys = vmm_alloc_surface(newn);
    if (!newphys) return -1;

    uint64_t f = spin_lock_irqsave(&ipc_lock);
    struct window *win = &wins[id];
    if (win->state != W_ALIVE || win->owner != sched_current()) {
        spin_unlock_irqrestore(&ipc_lock, f);
        vmm_free_surface(newphys, newn);
        return -1;
    }
    uint64_t app = vmm_current_pml4();
    vmm_unmap_surface(app, id, win->nframes);          /* drop old app mapping */
    vmm_map_surface(app, id, newphys, newn);           /* map new at the same vaddr */
    vmm_flush_self();
    if (!win->old_phys) { win->old_phys = win->phys; win->old_nf = win->nframes; }
    else vmm_free_surface(win->phys, win->nframes);    /* a second resize before the comp caught up */
    win->phys = newphys; win->nframes = newn;
    win->w = (uint32_t)w; win->h = (uint32_t)h;
    win->comp_vaddr = 0;                               /* force the compositor to remap */
    win->seq++;
    spin_unlock_irqrestore(&ipc_lock, f);
    return 0;
}

int win_poll_event(int id, struct winevent *ev) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    int got = 0;
    struct window *win = &wins[id];
    if (win->state == W_ALIVE && win->owner == sched_current() && win->et != win->eh) {
        *ev = win->ev[win->et];
        win->et = (win->et + 1) % WEVQ;
        got = 1;
    }
    spin_unlock_irqrestore(&ipc_lock, f);
    return got;
}

int win_close(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return -1;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    if (wins[id].owner == sched_current() && wins[id].state == W_ALIVE) wins[id].state = W_DYING;
    spin_unlock_irqrestore(&ipc_lock, f);
    return 0;
}

/* Called from sched when a task exits: mark its windows dying (the compositor
 * reclaims the surface on its next snapshot, when it is not mid-blit). */
void win_owner_exited(int task) {
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].state == W_ALIVE && wins[i].owner == task) wins[i].state = W_DYING;
    spin_unlock_irqrestore(&ipc_lock, f);
}

void wm_register(void) {
    compositor = sched_current();
    console_set_gui(1);                   /* kernel text no longer paints the framebuffer */
}

/* Compositor: snapshot the live windows. Lazily maps each surface into the
 * compositor's address space, and reclaims any window whose owner has exited
 * (here, where the compositor is not blitting it). */
int wm_windows(struct wmwin *buf, int max) {
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    uint64_t comp = vmm_current_pml4();
    int n = 0, flush = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = &wins[i];
        if (win->state == W_DYING) {
            if (win->comp_nf) { vmm_unmap_surface(comp, i, win->comp_nf); flush = 1; }
            vmm_free_surface(win->phys, win->nframes);
            if (win->old_phys) vmm_free_surface(win->old_phys, win->old_nf);
            win->state = W_FREE; win->comp_vaddr = 0; win->comp_nf = 0; win->old_phys = 0;
            continue;
        }
        if (win->state != W_ALIVE || n >= max) continue;
        if (win->old_phys) {                              /* a resize: free old frames here */
            if (win->comp_nf) vmm_unmap_surface(comp, i, win->comp_nf);
            vmm_free_surface(win->old_phys, win->old_nf);
            win->old_phys = 0; win->comp_nf = 0; flush = 1;
        }
        if (!win->comp_vaddr) {
            win->comp_vaddr = vmm_map_surface(comp, i, win->phys, win->nframes);
            win->comp_nf = win->nframes; flush = 1;
        }
        buf[n].id = i; buf[n].w = win->w; buf[n].h = win->h;
        buf[n].vaddr = win->comp_vaddr; buf[n].seq = (uint32_t)win->seq;
        buf[n].flags = win->flags;
        copy_title(buf[n].title, win->title);
        n++;
    }
    if (flush) vmm_flush_self();
    spin_unlock_irqrestore(&ipc_lock, f);
    return n;
}

int wm_post_event(int id, int type, unsigned a) {
    if (id < 0 || id >= MAX_WINDOWS) return -1;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    struct window *win = &wins[id];
    if (win->state == W_ALIVE) {
        int n = (win->eh + 1) % WEVQ;
        if (n != win->et) {
            win->ev[win->eh].type = (uint32_t)type;
            win->ev[win->eh].a = a;
            win->eh = n;
        }
    }
    spin_unlock_irqrestore(&ipc_lock, f);
    return 0;
}
int wm_send_key(int id, int byte) { return wm_post_event(id, WEV_KEY, (unsigned)byte); }

/* Compositor-only: force-kill the process that owns window `id` (Super+Shift+Q).
 * We look up the owning task and ask the scheduler to terminate it asynchronously;
 * the window itself is torn down by the normal owner-exited path once the task
 * dies. Returns 0, or -1 if the caller isn't the compositor or the id is stale. */
int wm_kill_window(int id) {
    if (sched_current() != compositor) return -1;
    if (id < 0 || id >= MAX_WINDOWS) return -1;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    int owner = (wins[id].state == W_ALIVE) ? wins[id].owner : -1;
    spin_unlock_irqrestore(&ipc_lock, f);
    if (owner < 0) return -1;
    return sched_request_kill(owner);
}

/* --- notifications -------------------------------------------------------- *
 * A small global ring of pending notifications. Any task posts with notify();
 * the compositor drains it one per frame (wm_poll_notify) and turns each into a
 * toast + a notification-center entry. Overwrites the oldest if it overflows, so
 * a burst always surfaces the newest. */
#define NOTIFQ 16
static struct notif notif_ring[NOTIFQ];
static volatile int notif_h, notif_t;

static void ncopy(char *dst, const char *src, int cap) {
    int i = 0; for (; i < cap - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = 0;
}

int notify_post(const struct notif *n) {
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    int next = (notif_h + 1) % NOTIFQ;
    if (next == notif_t) notif_t = (notif_t + 1) % NOTIFQ;   /* full: drop the oldest */
    ncopy(notif_ring[notif_h].title, n->title, NOTIF_TITLE);
    ncopy(notif_ring[notif_h].body,  n->body,  NOTIF_BODY);
    notif_h = next;
    spin_unlock_irqrestore(&ipc_lock, f);
    return 0;
}

int wm_poll_notify(struct notif *out) {
    if (sched_current() != compositor) return 0;
    uint64_t f = spin_lock_irqsave(&ipc_lock);
    int got = 0;
    if (notif_t != notif_h) {
        *out = notif_ring[notif_t];
        notif_t = (notif_t + 1) % NOTIFQ;
        got = 1;
    }
    spin_unlock_irqrestore(&ipc_lock, f);
    return got;
}
