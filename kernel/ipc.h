/* Userspace IPC: pseudo-terminals (pty) and window surfaces.
 *
 * pty: a bidirectional byte channel between a terminal emulator and the program
 * it runs. The program's stdio (SYS_READ/WRITE) is bound to a pty, so it just
 * reads/writes bytes; the emulator drives the other end.
 *
 * windows: an app owns a pixel surface that the kernel maps into both the app
 * and the compositor (shared memory). The app draws into it and presents; the
 * compositor blits all surfaces to the framebuffer. This keeps the window
 * manager a pure compositor -- it never knows what an app is drawing. */
#pragma once
#include <stdint.h>
#include "syscall.h"

/* --- pty channels --------------------------------------------------------- */
int  pty_open(void);                              /* -> id, or -1                 */
int  pty_attach(int id);                          /* bind caller's stdio to a pty  */
int  pty_read(int id, void *buf, int len);        /* drain slave output (nonblock) */
int  pty_write(int id, const void *buf, int len); /* feed bytes to the slave input */
int  pty_close(int id);
/* stdio routing for tty-bound tasks (called from the syscall dispatcher) */
int  pty_in_getc(int tty);                        /* one input byte, or -1 if empty */
void pty_out_putc(int tty, char c);               /* slave output (+ serial mirror) */
void pty_out_write(int tty, const char *s);

/* --- windows / compositor ------------------------------------------------- */
int  win_create(struct wininfo *wi);              /* app: window + shared surface  */
int  win_present(int id);                         /* mark the surface updated      */
int  win_present_rect(int id, int x, int y, int w, int h);  /* present only a damage rect */
int  win_resize(int id, int w, int h);            /* app: resize its own surface   */
int  win_poll_event(int id, struct winevent *ev); /* 1 if dequeued, else 0         */
int  win_close(int id);
void win_owner_exited(int task);                  /* sched hook on task exit       */
void wm_register(void);                           /* caller becomes the compositor */
int  wm_windows(struct wmwin *buf, int max);      /* snapshot live windows         */
int  wm_send_key(int id, int byte);               /* deliver an input byte         */
int  wm_post_event(int id, int type, unsigned a); /* deliver a window event        */
int  win_set_menu(int id, const struct winmenu *m); /* app: declare a window's menu  */
int  wm_get_menu(int id, struct winmenu *out);    /* compositor: read it; 1 if any  */
int  wm_kill_window(int id);                      /* compositor: kill a window's owner */
int  notify_post(const struct notif *n);          /* any app: post a notification  */
int  wm_poll_notify(struct notif *out);           /* compositor: dequeue one; 1/0  */
