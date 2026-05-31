/* tOS userspace runtime, statically linked into every program. The only way to
 * touch the machine from ring 3 is the int 0x80 syscall; everything here is a
 * thin wrapper around it. */
#pragma once
#include <stdint.h>
#include "syscall.h"
#include "libc.h"     /* mem/str, malloc family, printf -- shared C runtime */
#include "sys.h"      /* SDK conveniences: file slurp/spit, launch, stat      */

#ifdef __cplusplus
extern "C" {
#endif

uint64_t sc(uint64_t num, uint64_t arg);   /* raw 1-arg syscall */
uint64_t sc3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);  /* 3-arg */

void print(const char *s);                  /* SYS_WRITE: NUL-terminated string */
void printc(char c);                        /* SYS_PUTC                          */
char getch(void);                           /* SYS_READ: blocking single key     */
long spawn(const char *prog);               /* SYS_SPAWN: load+run a child -> pid */
int  fork(void);                            /* SYS_FORK: 0 in child, child pid in parent, -1 err */
int  exec(const char *prog);                /* SYS_EXEC: replace image; returns -1 on failure */
int  wait_child(void);                      /* SYS_WAIT: reap one child -> its pid, or -1 */
void yield(void);                           /* SYS_YIELD                         */
void sleep_ticks(unsigned n);               /* SYS_SLEEP: park ~n timer ticks    */
void sleep_ms(unsigned ms);                 /* SYS_SLEEP: park ~ms milliseconds  */
int  getcpu(void);                          /* SYS_GETCPU: index of the running CPU */
void paint_cursor(char c, int inverse);     /* SYS_CPAINT: block cursor at the cursor cell */
int  fbinfo(struct fbinfo *fb);             /* SYS_FBINFO: map the framebuffer; 0 ok, -1 none */
void con_window(int x, int y, int w, int h);/* SYS_CON_WINDOW: confine text console to a px rect */
int  trywait(void);                         /* SYS_TRYWAIT: reap pid / 0 (busy) / -1 (none) */
void mouse(struct mousestate *m);           /* SYS_MOUSE: current cursor position + buttons */
void rtc_time(struct rtctime *t);           /* SYS_TIME: wall-clock date/time               */
void lspci(void);                           /* SYS_LSPCI: list PCI devices                  */
void beep(unsigned freq);                   /* SYS_BEEP: PC speaker tone (0 = off)          */
void reboot(void) __attribute__((noreturn));/* SYS_REBOOT: reset the machine                */

/* --- compositor / windowing / pty (the desktop is built on these) -------- */
int  getkey(void);                          /* SYS_GETKEY: next input byte, or -1 (non-blocking) */
int  win_create(struct wininfo *wi);        /* SYS_WIN_CREATE: window + shared surface -> id     */
void win_present(int id);                   /* SYS_WIN_PRESENT: surface updated                  */
int  win_resize(int id, int w, int h);      /* SYS_WIN_RESIZE: resize own surface                */
int  win_poll(int id, struct winevent *ev); /* SYS_WIN_POLL: 1 if an event was dequeued          */
void win_close(int id);                     /* SYS_WIN_CLOSE                                      */
void wm_register(void);                     /* SYS_WM_REGISTER: become the compositor            */
int  wm_windows(struct wmwin *buf, int max);/* SYS_WM_WINDOWS: snapshot live windows -> count    */
void wm_send_key(int id, int byte);         /* SYS_WM_SEND_KEY: deliver an input byte to a window */
void wm_post(int id, int type, unsigned a); /* SYS_WM_POST: deliver an event to a window         */
int  wm_kill(int id);                       /* SYS_WM_KILL: kill the process owning a window      */
int  pty_open(void);                        /* SYS_PTY_OPEN -> id                                */
void pty_attach(int id);                    /* SYS_PTY_ATTACH: bind caller's stdio               */
int  pty_read(int id, void *buf, int len);  /* SYS_PTY_READ: slave output (non-blocking)         */
int  pty_write(int id, const void *buf, int len); /* SYS_PTY_WRITE: feed the slave input         */
void pty_close(int id);                     /* SYS_PTY_CLOSE                                      */
void sysinfo(struct sysinfo *si);           /* SYS_SYSINFO                                        */
int  isatty(void);                          /* SYS_ISATTY: 1 if stdio is a pty                    */

/* Map a block of fresh private RAM, sized to whatever the caller asks for (the
 * frame pool is all of RAM, so this scales with the machine -- it is NOT bounded
 * by the program's code/stack window). Returns a pointer, or 0 on failure. Backs
 * the compositor's full-screen back buffer and (later) a userspace heap. */
void *mmap_(unsigned long nbytes);          /* SYS_MMAP */

/* system clipboard (Win+V-style ring of text/byte entries) */
int  clip_put(int type, const char *name, const void *data, int len); /* type=CLIP_TEXT/CLIP_FILE; ->idx/-1 */
int  clip_count(void);
int  clip_get(int idx, void *buf, int cap);          /* -> bytes copied, or -1 */
int  clip_info(int idx, struct clipinfo *ci);        /* -> 0/-1 */
int  clip_active(int idx);                           /* idx>=0 sets active; -> active index */
void clip_clear(void);

void printu(unsigned v);                    /* print an unsigned decimal to the console     */
void proc_exit(void) __attribute__((noreturn));      /* SYS_EXIT                 */
void shutdown(void) __attribute__((noreturn));       /* SYS_SHUTDOWN: stop CPU   */
void ls(void);                              /* SYS_LS:  print FS directory       */

/* file API */
int  fopen(const char *name, int flags);              /* -> fd, or -1           */
int  fread_(int fd, void *buf, int len);              /* -> bytes (0=EOF) or -1 */
int  fwrite_(int fd, const void *buf, int len);       /* -> bytes, or -1        */
int  fclose_(int fd);
int  funlink(const char *name);                       /* delete a file; 0 or -1 */

/* directory / path API */
int  mkdir(const char *path);                         /* make a directory; 0/-1 */
int  rmdir(const char *path);                         /* remove an empty dir    */
int  chdir(const char *path);                         /* change working dir     */
int  getcwd(char *buf, int len);                      /* absolute cwd -> 0/-1   */
int  readdir(const char *path, struct dirent *out, int max); /* list a dir -> n */
int  rename_(const char *oldp, const char *newp);     /* move/rename; 0/-1      */
int  stat_(const char *path, struct fstat *st);       /* type+size; 0/-1        */

int streq(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
