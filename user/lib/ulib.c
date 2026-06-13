#include "ulib.h"
#include "argv.h"      /* argv_split: the pure tokenizer behind getargs() */

uint64_t sc(uint64_t num, uint64_t arg) {
    uint64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret) : "a"(num), "D"(arg) : "rcx", "r11", "memory");
    return ret;
}

uint64_t sc3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

void print(const char *s)     { sc(SYS_WRITE, (uint64_t)s); }
void printc(char c)           { sc(SYS_PUTC, (uint64_t)(uint8_t)c); }
char getch(void)              { return (char)sc(SYS_READ, 0); }
long spawn(const char *prog)  { return (long)sc(SYS_SPAWN, (uint64_t)prog); }
int  fork(void)               { return (int)sc(SYS_FORK, 0); }
int  exec(const char *prog)   { return (int)sc(SYS_EXEC, (uint64_t)prog); }
/* The kernel seeds this task's data page (USER_DATA_VADDR) with its full command
 * line; cmdline() points at it and getargs() tokenizes a private copy into argv[]. */
const char *cmdline(void)     { return (const char *)USER_DATA_VADDR; }
int  getargs(char **argv, int maxv) {
    static char buf[1024];
    const char *src = cmdline();
    int n = 0; while (src[n] && n < (int)sizeof(buf) - 1) { buf[n] = src[n]; n++; }
    buf[n] = 0;
    return argv_split(buf, argv, maxv);
}
int  wait_child(void)         { return (int)sc(SYS_WAIT, 0); }
void yield(void)              { sc(SYS_YIELD, 0); }
void sleep_ticks(unsigned n)  { sc(SYS_SLEEP, (uint64_t)n); }
void sleep_ms(unsigned ms)    { unsigned t = (ms * TIMER_HZ) / 1000; sc(SYS_SLEEP, t ? t : 1); }
int  getcpu(void)             { return (int)sc(SYS_GETCPU, 0); }
long install_disk(int target) { return (long)sc(SYS_INSTALL, (uint64_t)(unsigned)target); }
/* Networking (needs CAP_NET). IPs are packed big-endian: net_ip(a,b,c,d). */
unsigned net_ip(int a, int b, int c, int d) {
    return ((unsigned)(a & 0xff) << 24) | ((unsigned)(b & 0xff) << 16)
         | ((unsigned)(c & 0xff) << 8)  |  (unsigned)(d & 0xff);
}
int  net_ping(unsigned ip)               { return (int)sc(SYS_NET_PING, (uint64_t)ip); }
int  net_connect(unsigned ip, int port)  { return (int)sc3(SYS_NET_CONNECT, (uint64_t)ip, (uint64_t)(unsigned)port, 0); }
int  net_send(const void *buf, int len)  { return (int)sc3(SYS_NET_SEND, (uint64_t)buf, (uint64_t)(unsigned)len, 0); }
int  net_recv(void *buf, int max)        { return (int)sc3(SYS_NET_RECV, (uint64_t)buf, (uint64_t)(unsigned)max, 0); }
void net_close(void)                     { sc(SYS_NET_CLOSE, 0); }
int  net_listen(int port)                { return (int)sc(SYS_NET_LISTEN, (uint64_t)(unsigned)port); }
int  net_accept(void)                    { return (int)sc(SYS_NET_ACCEPT, 0); }
void paint_cursor(char c, int inverse) { sc(SYS_CPAINT, ((uint64_t)(inverse & 1) << 8) | (uint8_t)c); }
int  fbinfo(struct fbinfo *fb)         { return (int)sc(SYS_FBINFO, (uint64_t)fb); }
void con_window(int x, int y, int w, int h) {
    sc3(SYS_CON_WINDOW, ((uint64_t)(x & 0xffff) << 16) | (uint32_t)(y & 0xffff),
                        ((uint64_t)(w & 0xffff) << 16) | (uint32_t)(h & 0xffff), 0);
}
int  trywait(void)             { return (int)sc(SYS_TRYWAIT, 0); }
void mouse(struct mousestate *m) { sc(SYS_MOUSE, (uint64_t)m); }
void rtc_time(struct rtctime *t) { sc(SYS_TIME, (uint64_t)t); }
void lspci(void)               { sc(SYS_LSPCI, 0); }
void beep(unsigned freq)       { sc(SYS_BEEP, (uint64_t)freq); }
void reboot(void)              { sc(SYS_REBOOT, 0); for (;;) { } }

int  getkey(void)              { return (int)sc(SYS_GETKEY, 0); }
int  win_create(struct wininfo *wi) { return (int)sc(SYS_WIN_CREATE, (uint64_t)wi); }
void win_present(int id)       { sc(SYS_WIN_PRESENT, (uint64_t)id); }
void win_present_rect(int id, int x, int y, int w, int h) {
    sc3(SYS_WIN_PRESENT_RECT, (uint64_t)id,
        ((uint64_t)(x & 0xffff) << 16) | (uint32_t)(y & 0xffff),
        ((uint64_t)(w & 0xffff) << 16) | (uint32_t)(h & 0xffff));
}
int  win_resize(int id, int w, int h) { return (int)sc3(SYS_WIN_RESIZE, (uint64_t)id, (uint64_t)w, (uint64_t)h); }
int  win_poll(int id, struct winevent *ev) { return (int)sc3(SYS_WIN_POLL, (uint64_t)id, (uint64_t)ev, 0); }
void win_close(int id)         { sc(SYS_WIN_CLOSE, (uint64_t)id); }
void wm_register(void)         { sc(SYS_WM_REGISTER, 0); }
int  wm_windows(struct wmwin *buf, int max) { return (int)sc3(SYS_WM_WINDOWS, (uint64_t)buf, (uint64_t)max, 0); }
void wm_send_key(int id, int byte) { sc3(SYS_WM_SEND_KEY, (uint64_t)id, (uint64_t)byte, 0); }
void wm_post(int id, int type, unsigned a) { sc3(SYS_WM_POST, (uint64_t)id, (uint64_t)type, (uint64_t)a); }
int  wm_kill(int id)           { return (int)sc(SYS_WM_KILL, (uint64_t)id); }
int  pty_open(void)            { return (int)sc(SYS_PTY_OPEN, 0); }
void pty_attach(int id)        { sc(SYS_PTY_ATTACH, (uint64_t)id); }
int  pty_read(int id, void *buf, int len)        { return (int)sc3(SYS_PTY_READ,  (uint64_t)id, (uint64_t)buf, (uint64_t)len); }
int  pty_write(int id, const void *buf, int len) { return (int)sc3(SYS_PTY_WRITE, (uint64_t)id, (uint64_t)buf, (uint64_t)len); }
void pty_close(int id)         { sc(SYS_PTY_CLOSE, (uint64_t)id); }
void sysinfo(struct sysinfo *si) { sc(SYS_SYSINFO, (uint64_t)si); }
void notify_to(const char *title, const char *body, const char *target) {
    struct notif n; int i = 0;
    for (; i < NOTIF_TITLE - 1 && title && title[i]; i++) n.title[i] = title[i];
    n.title[i] = 0;
    for (i = 0; i < NOTIF_BODY - 1 && body && body[i]; i++) n.body[i] = body[i];
    n.body[i] = 0;
    for (i = 0; i < NOTIF_TARGET - 1 && target && target[i]; i++) n.target[i] = target[i];
    n.target[i] = 0;
    sc(SYS_NOTIFY, (uint64_t)&n);
}
void notify(const char *title, const char *body) { notify_to(title, body, ""); }
int wm_poll_notify(struct notif *out) { return (int)sc(SYS_WM_NOTIFY, (uint64_t)out); }
int setuid(int uid)            { return (int)sc(SYS_SETUID, (uint64_t)(int64_t)uid); }
int getuid(void)               { return (int)sc(SYS_GETUID, 0); }
int getpid(void)               { return (int)sc(SYS_GETPID, 0); }
int getppid(void)              { return (int)sc(SYS_GETPPID, 0); }
int win_setmenu(int id, const struct winmenu *m) { return (int)sc3(SYS_WIN_SETMENU, (uint64_t)id, (uint64_t)m, 0); }
int wm_getmenu(int id, struct winmenu *out)      { return (int)sc3(SYS_WM_GETMENU, (uint64_t)id, (uint64_t)out, 0); }
int win_setcursor(int id, int shape)             { return (int)sc3(SYS_WIN_SETCURSOR, (uint64_t)id, (uint64_t)shape, 0); }
unsigned kbd_mods(void)        { return (unsigned)sc(SYS_KBD_MODS, 0); }

/* system clipboard */
int  clip_put(int type, const char *name, const void *data, int len) {
    return (int)sc3(SYS_CLIP_PUT, (uint64_t)name, (uint64_t)data,
                    ((uint64_t)(type & 0xff) << 24) | (uint64_t)(len & 0xffffff));
}
int  clip_count(void)                        { return (int)sc3(SYS_CLIP_COUNT, 0, 0, 0); }
int  clip_get(int idx, void *buf, int cap)   { return (int)sc3(SYS_CLIP_GET, (uint64_t)idx, (uint64_t)buf, (uint64_t)cap); }
int  clip_info(int idx, struct clipinfo *ci) { return (int)sc3(SYS_CLIP_INFO, (uint64_t)idx, (uint64_t)ci, 0); }
int  clip_active(int idx)                    { return (int)sc3(SYS_CLIP_ACTIVE, (uint64_t)idx, 0, 0); }
void clip_clear(void)                        { sc3(SYS_CLIP_CLEAR, 0, 0, 0); }

int  drag_begin(int type, const char *label, const void *data, int len) {
    return (int)sc3(SYS_DRAG_BEGIN, (uint64_t)label, (uint64_t)data,
                    ((uint64_t)(type & 0xff) << 24) | (uint64_t)(len & 0xffffff));
}
int  drag_payload(int *type_out, void *buf, int cap) { return (int)sc3(SYS_DRAG_PAYLOAD, (uint64_t)type_out, (uint64_t)buf, (uint64_t)cap); }
int  drag_state(char *label_out, int cap)            { return (int)sc3(SYS_DRAG_STATE, (uint64_t)label_out, (uint64_t)cap, 0); }
void drag_end(void)                                  { sc3(SYS_DRAG_END, 0, 0, 0); }
int  isatty(void)              { return (int)sc(SYS_ISATTY, 0); }
void *mmap_(unsigned long nbytes) { return (void *)sc(SYS_MMAP, (uint64_t)nbytes); }

void printu(unsigned v) {
    char buf[11];
    int i = 10;
    buf[i] = 0;
    if (v == 0) buf[--i] = '0';
    else while (v && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    print(&buf[i]);
}

void proc_exit(void) { sc(SYS_EXIT, 0);     for (;;) { } }
void shutdown(void)  { sc(SYS_SHUTDOWN, 0); for (;;) { } }
void ls(void)              { sc(SYS_LS, 0); }

int fopen(const char *name, int flags)        { return (int)sc3(SYS_OPEN, (uint64_t)name, (uint64_t)flags, 0); }
int fread_(int fd, void *buf, int len)        { return (int)sc3(SYS_READ_FD,  (uint64_t)fd, (uint64_t)buf, (uint64_t)len); }
int fwrite_(int fd, const void *buf, int len) { return (int)sc3(SYS_WRITE_FD, (uint64_t)fd, (uint64_t)buf, (uint64_t)len); }
int fclose_(int fd)                           { return (int)sc(SYS_CLOSE, (uint64_t)fd); }
int funlink(const char *name)                 { return (int)sc(SYS_UNLINK, (uint64_t)name); }

int mkdir(const char *path)                    { return (int)sc(SYS_MKDIR, (uint64_t)path); }
int rmdir(const char *path)                    { return (int)sc(SYS_RMDIR, (uint64_t)path); }
int chdir(const char *path)                    { return (int)sc(SYS_CHDIR, (uint64_t)path); }
int getcwd(char *buf, int len)                 { return (int)sc3(SYS_GETCWD, (uint64_t)buf, (uint64_t)len, 0); }
int readdir(const char *path, struct dirent *out, int max) { return (int)sc3(SYS_READDIR, (uint64_t)path, (uint64_t)out, (uint64_t)max); }
int rename_(const char *oldp, const char *newp) { return (int)sc3(SYS_RENAME, (uint64_t)oldp, (uint64_t)newp, 0); }
int stat_(const char *path, struct fstat *st)  { return (int)sc3(SYS_STAT, (uint64_t)path, (uint64_t)st, 0); }
int statfs_(struct statfs *st) { return (int)sc(SYS_STATFS, (uint64_t)st); }
unsigned setcaps(unsigned mask) { return (unsigned)sc(SYS_SETCAPS, (uint64_t)mask); }  /* drop-only */
unsigned getcaps(void)          { return (unsigned)sc(SYS_GETCAPS, 0); }

int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
