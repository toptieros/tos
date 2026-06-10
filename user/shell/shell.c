/* tOS interactive shell. Started by the terminal emulator as a piped child (or,
 * with no desktop, directly by init as a plain TTY). It reads a line at a time
 * with history and cursor editing -- input arrives as a byte stream, so the
 * arrow / Home / End / Delete keys come in as ANSI escape sequences (ESC [ ...),
 * which readline() parses. On a terminal it runs `fastfetch` at startup. */
#include "ulib.h"
#include "registry.h"
#include "humansize.h"     /* human_bytes: the `df` figures */

#define HISTN 64
#define LINEMAX 512
#define HOME "/Users/user"          /* the single user's home dir (see design/filesystem-layout.md) */
#define HOME_LEN 11                 /* strlen("/Users/user") */
static char hist[HISTN][LINEMAX];
static int  hist_n = 0;

static void hist_add(const char *s) {
    if (!s[0]) return;
    if (hist_n > 0 && streq(hist[hist_n - 1], s)) return;        /* skip repeats */
    if (hist_n == HISTN) {                                       /* drop oldest  */
        for (int i = 1; i < HISTN; i++)
            for (int j = 0; j < LINEMAX; j++) hist[i - 1][j] = hist[i][j];
        hist_n--;
    }
    int i = 0;
    for (; s[i] && i < LINEMAX - 1; i++) hist[hist_n][i] = s[i];
    hist[hist_n][i] = 0;
    hist_n++;
}

/* --- line editor: keeps the on-screen line in sync using only printc and a
 * non-destructive backspace (moves the cursor left without erasing). --------- */
static void ed_insert(char *buf, int *pn, int *ppos, int max, char ch) {
    int n = *pn, pos = *ppos;
    if (n >= max - 1) return;
    for (int i = n; i > pos; i--) buf[i] = buf[i - 1];
    buf[pos] = ch;
    n++;
    for (int i = pos; i < n; i++) printc(buf[i]);        /* redraw tail */
    for (int i = n; i > pos + 1; i--) printc('\b');      /* back to cursor */
    *pn = n; *ppos = pos + 1;
}

static void ed_backspace(char *buf, int *pn, int *ppos) {
    int n = *pn, pos = *ppos;
    if (pos == 0) return;
    pos--;
    for (int i = pos; i < n - 1; i++) buf[i] = buf[i + 1];
    n--;
    printc('\b');
    for (int i = pos; i < n; i++) printc(buf[i]);
    printc(' ');
    for (int i = 0; i < (n - pos) + 1; i++) printc('\b');
    *pn = n; *ppos = pos;
}

/* forward delete: remove the char at the cursor (Delete key) */
static void ed_delete(char *buf, int *pn, int *ppos) {
    int n = *pn, pos = *ppos;
    if (pos >= n) return;
    for (int i = pos; i < n - 1; i++) buf[i] = buf[i + 1];
    n--;
    for (int i = pos; i < n; i++) printc(buf[i]);
    printc(' ');
    for (int i = 0; i < (n - pos) + 1; i++) printc('\b');
    *pn = n;
}

static void ed_replace(char *buf, int *pn, int *ppos, const char *src) {
    int n = *pn, pos = *ppos;
    while (pos > 0) { printc('\b'); pos--; }
    int m = 0;
    while (src[m] && m < LINEMAX - 1) { buf[m] = src[m]; printc(buf[m]); m++; }
    if (n > m) {
        for (int i = 0; i < n - m; i++) printc(' ');
        for (int i = 0; i < n - m; i++) printc('\b');
    }
    buf[m] = 0;
    *pn = m; *ppos = m;
}

static void cur_show(const char *buf, int n, int pos) { paint_cursor(pos < n ? buf[pos] : ' ', 1); }
static void cur_hide(const char *buf, int n, int pos) { paint_cursor(pos < n ? buf[pos] : ' ', 0); }

/* Editing actions decoded from input (escape sequences map onto these). */
enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL };

/* Read one logical key: an ASCII byte (returned 0..255) or, for ESC [ ...
 * sequences, one of the K_* actions (returned as -action). */
static int read_key(void) {
    int c = (unsigned char)getch();
    if (c != 0x1b) return c;
    int c2 = (unsigned char)getch();
    if (c2 != '[' && c2 != 'O') return c2;            /* lone ESC: treat next byte normally */
    int c3 = (unsigned char)getch();
    if (c3 >= '0' && c3 <= '9') {                     /* ESC [ <num> ~ */
        int num = 0;
        while (c3 >= '0' && c3 <= '9') { num = num * 10 + (c3 - '0'); c3 = (unsigned char)getch(); }
        switch (num) { case 1: case 7: return -K_HOME; case 4: case 8: return -K_END;
                       case 3: return -K_DEL; default: return -K_NONE; }
    }
    switch (c3) {
    case 'A': return -K_UP;   case 'B': return -K_DOWN;
    case 'C': return -K_RIGHT;case 'D': return -K_LEFT;
    case 'H': return -K_HOME; case 'F': return -K_END;
    default:  return -K_NONE;
    }
}

static int readline(char *buf, int max) {
    int n = 0, pos = 0;
    int hpos = hist_n;
    char draft[LINEMAX];
    draft[0] = 0;
    cur_show(buf, n, pos);
    for (;;) {
        int k = read_key();
        cur_hide(buf, n, pos);
        if (k >= 0) {
            char c = (char)k;
            if (c == '\r' || c == '\n') { printc('\r'); printc('\n'); break; }
            else if (c == '\b' || c == 127) ed_backspace(buf, &n, &pos);
            else if (c >= 32 && c < 127) ed_insert(buf, &n, &pos, max, c);
        } else switch (-k) {
            case K_LEFT:  if (pos > 0) { printc('\b'); pos--; } break;
            case K_RIGHT: if (pos < n) { printc(buf[pos]); pos++; } break;
            case K_HOME:  while (pos > 0) { printc('\b'); pos--; } break;
            case K_END:   while (pos < n) { printc(buf[pos]); pos++; } break;
            case K_DEL:   ed_delete(buf, &n, &pos); break;
            case K_UP:
                if (hpos > 0) {
                    if (hpos == hist_n) { for (int i = 0; i < n; i++) draft[i] = buf[i]; draft[n] = 0; }
                    hpos--; ed_replace(buf, &n, &pos, hist[hpos]);
                }
                break;
            case K_DOWN:
                if (hpos < hist_n) { hpos++; ed_replace(buf, &n, &pos, hpos == hist_n ? draft : hist[hpos]); }
                break;
        }
        cur_show(buf, n, pos);
    }
    buf[n] = 0;
    return n;
}

/* run an external program as a child and wait for it */
static int run_prog(const char *name) {
    int pid = fork();
    if (pid == 0) { exec(name); print("exec failed: "); print(name); print("\r\n"); proc_exit(); }
    if (pid < 0) { print("fork failed\r\n"); return -1; }
    return wait_child();
}

static void cmd_cat(const char *name) {
    int fd = fopen(name, O_RDONLY);
    if (fd < 0) { print("cat: no such file: "); print(name); print("\r\n"); return; }
    char buf[129]; int n;
    while ((n = fread_(fd, buf, sizeof buf - 1)) > 0) { buf[n] = 0; print(buf); }
    fclose_(fd);
}

static unsigned parse_uint(const char *s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); s++; }
    return v;
}

static void cmd_sleep(const char *arg) {
    sleep_ticks(parse_uint(arg));
    print("slept "); print(arg); print(" ticks\r\n");
}

static void rm_r(const char *path);                            /* defined below */

static void cmd_rm(const char *arg) {
    if (arg[0] == '-' && arg[1] == 'r' && arg[2] == ' ') {     /* rm -r <path>: recursive */
        const char *p = arg + 3;
        rm_r(p);
        print("removed "); print(p); print("\r\n");
        return;
    }
    if (funlink(arg) < 0) {
        struct fstat st;
        if (stat_(arg, &st) == 0 && st.owner == 0 && getuid() != 0)   /* still there + system-owned */
            { print("rm: permission denied (system file): "); print(arg); print("\r\n"); }
        else
            { print("rm: cannot remove "); print(arg); print(" (use rm -r for directories)\r\n"); }
        return;
    }
    print("removed "); print(arg); print("\r\n");
}

static void cmd_write(const char *name) {
    int fd = fopen(name, O_CREATE | O_TRUNC);
    if (fd < 0) { print("write: cannot create "); print(name); print("\r\n"); return; }
    print("enter a line, ENTER to save:\r\n");
    char line[LINEMAX];
    int n = readline(line, sizeof line);
    fwrite_(fd, line, n);
    fwrite_(fd, "\n", 1);
    fclose_(fd);
    print("saved "); print(name); print("\r\n");
}

/* cp <src> <dst>: copy a file. */
static void cmd_cp(char *args) {
    char *dst = args;
    while (*dst && *dst != ' ') dst++;
    if (*dst != ' ') { print("usage: cp <src> <dst>\r\n"); return; }
    *dst++ = 0;
    int in = fopen(args, O_RDONLY);
    if (in < 0) { print("cp: no such file: "); print(args); print("\r\n"); return; }
    int out = fopen(dst, O_CREATE | O_TRUNC);
    if (out < 0) { fclose_(in); print("cp: cannot create "); print(dst); print("\r\n"); return; }
    char buf[256]; int n;
    while ((n = fread_(in, buf, sizeof buf)) > 0) fwrite_(out, buf, n);
    fclose_(in); fclose_(out);
    print("copied "); print(args); print(" -> "); print(dst); print("\r\n");
}

/* --- path helpers + directory commands ----------------------------------- */
/* join "<dir>/<name>" into out (dir "" / "/" handled), capped at cap-1. */
static void path_join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (int i = 0; dir[i] && n < cap - 1; i++) out[n++] = dir[i];
    if (n == 0 || out[n - 1] != '/') { if (n < cap - 1) out[n++] = '/'; }
    for (int i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = 0;
}

static void cmd_ls(const char *path) {
    struct dirent e[64];
    int n = readdir(path, e, 64);
    if (n < 0) { print("ls: cannot open "); print(path[0] ? path : "."); print("\r\n"); return; }
    for (int pass = 0; pass < 2; pass++)               /* directories first, then files */
        for (int i = 0; i < n; i++) {
            int isdir = e[i].type == FT_DIR;
            if ((pass == 0) != isdir) continue;
            if (isdir) { print("\x1b[94m"); print(e[i].name); print("/\x1b[0m\r\n"); }
            else { print(e[i].name); print("\t"); printu(e[i].size); print(" bytes\r\n"); }
        }
}

static void cmd_pwd(void) { char cwd[256]; getcwd(cwd, sizeof cwd); print(cwd); print("\r\n"); }

static void cmd_cd(const char *path) {
    char buf[256];
    if (path[0] == '~' && (path[1] == 0 || path[1] == '/')) {  /* ~ or ~/... -> home */
        int j = 0; for (const char *h = HOME; *h; h++) buf[j++] = *h;
        for (const char *r = path + 1; *r && j < 255; r++) buf[j++] = *r;
        buf[j] = 0;
        path = buf;
    }
    if (chdir(path) < 0) { print("cd: no such directory: "); print(path); print("\r\n"); }
}

static void cmd_mkdir(const char *path) {
    if (mkdir(path) < 0) { print("mkdir: cannot create "); print(path); print("\r\n"); }
}

static void cmd_rmdir(const char *path) {
    if (rmdir(path) < 0) { print("rmdir: cannot remove "); print(path); print(" (not an empty directory?)\r\n"); }
}

static void cmd_mv(char *args) {
    char *dst = args;
    while (*dst && *dst != ' ') dst++;
    if (*dst != ' ') { print("usage: mv <src> <dst>\r\n"); return; }
    *dst++ = 0;
    if (rename_(args, dst) < 0) { print("mv: cannot move "); print(args); print(" -> "); print(dst); print("\r\n"); return; }
    print("moved "); print(args); print(" -> "); print(dst); print("\r\n");
}

/* recursive remove: delete a file, or empty a directory and remove it. */
static void rm_r(const char *path) {
    struct fstat st;
    if (stat_(path, &st) < 0) { print("rm: no such path: "); print(path); print("\r\n"); return; }
    if (st.type == FT_DIR) {
        for (;;) {
            struct dirent e[32];
            int n = readdir(path, e, 32);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                char child[256];
                path_join(child, sizeof child, path, e[i].name);
                rm_r(child);
            }
        }
        rmdir(path);
    } else {
        funlink(path);
    }
}

/* tree: print the directory hierarchy under the cwd, indented by depth. */
static void tree_walk(const char *path, int depth) {
    struct dirent e[64];
    int n = readdir(path, e, 64);
    if (n < 0) return;
    for (int i = 0; i < n; i++) {
        for (int d = 0; d < depth; d++) print("  ");
        if (e[i].type == FT_DIR) {
            print("\x1b[94m"); print(e[i].name); print("/\x1b[0m\r\n");
            char child[256];
            path_join(child, sizeof child, path, e[i].name);
            tree_walk(child, depth + 1);
        } else { print(e[i].name); print("\r\n"); }
    }
}
static void cmd_tree(void) {
    char cwd[256];
    getcwd(cwd, sizeof cwd);
    print(cwd); print("\r\n");
    tree_walk(cwd, 1);
}

static void print2(unsigned v) { printc((char)('0' + (v / 10) % 10)); printc((char)('0' + v % 10)); }

static void cmd_date(void) {
    struct rtctime t; rtc_time(&t);
    printu(t.year); printc('-'); print2(t.month); printc('-'); print2(t.day);
    printc(' '); print2(t.hour); printc(':'); print2(t.min); printc(':'); print2(t.sec);
    print("\r\n");
}

static void cmd_mouse(void) {
    struct mousestate m; mouse(&m);
    print("mouse: x="); printu((unsigned)m.x);
    print(" y=");       printu((unsigned)m.y);
    print(" buttons="); printu(m.buttons);
    print("\r\n");
}

static void cmd_uptime(void) {
    struct sysinfo si; sysinfo(&si);
    unsigned secs = (unsigned)(si.uptime_ticks / si.timer_hz);
    print("up "); printu(secs / 60); print("m "); printu(secs % 60); print("s\r\n");
}

static void cmd_mem(void) {
    struct sysinfo si; sysinfo(&si);
    print("RAM: "); printu((unsigned)(si.ram_bytes / (1024 * 1024))); print(" MiB\r\n");
}
/* df: the mounted tosfs volume's size / used / free (SYS_STATFS). */
static void cmd_df(void) {
    struct statfs sf;
    if (statfs_(&sf) != 0) { print("df: statfs failed\r\n"); return; }
    char t[24], u[24], f[24];
    human_bytes(sf.total_bytes, t, sizeof t);
    human_bytes(sf.total_bytes - sf.free_bytes, u, sizeof u);
    human_bytes(sf.free_bytes, f, sizeof f);
    print("Filesystem: tosfs\r\n");
    print("Size: "); print(t); print("   Used: "); print(u); print("   Free: "); print(f); print("\r\n");
}

static void cmd_uname(void) { print("tOS x86_64 (SMP, preemptive) -- a hobby OS\r\n"); }

static void cmd_sysinfo(void) {
    struct sysinfo si; sysinfo(&si);
    print("cpus:  "); printu(si.ncpu); print("\r\n");
    print("ram:   "); printu((unsigned)(si.ram_bytes / (1024 * 1024))); print(" MiB\r\n");
    print("res:   "); printu(si.fb_w); print("x"); printu(si.fb_h); print("\r\n");
    print("files: "); printu(si.nfiles); print("\r\n");
    print("tasks: "); printu(si.ntasks); print("\r\n");
}

static void cmd_clear(void) { print("\x1b[2J\x1b[H"); }   /* terminal clears + home */

static void cmd_colors(void) {
    for (int i = 0; i < 8; i++) { print("\x1b["); printu(30 + i); print("m##"); }
    print("\x1b[0m  ");
    for (int i = 0; i < 8; i++) { print("\x1b["); printu(90 + i); print("m##"); }
    print("\x1b[0m\r\n");
}

static int starts(const char *line, const char *p) {
    while (*p) { if (*line++ != *p++) return 0; }
    return 1;
}

/* reg set <key> <value>: store a per-user setting and persist it (design/settings.md) */
static void cmd_reg_set(const char *s) {
    char key[REG_KEYMAX]; int k = 0;
    while (*s && *s != ' ' && k < REG_KEYMAX - 1) key[k++] = *s++;
    key[k] = 0;
    while (*s == ' ') s++;
    if (!key[0]) { print("usage: reg set <key> <value>\r\n"); return; }
    reg_set(key, s);
    if (reg_save() == 0) { print("set "); print(key); print(" = "); print(s); print("\r\n"); }
    else print("reg: could not save (is /Users/user/.config present?)\r\n");
}
/* reg list [prefix]: print matching keys and their effective values */
static void cmd_reg_list(const char *prefix) {
    char keys[64][REG_KEYMAX];
    int n = reg_keys(prefix, keys, 64);
    for (int i = 0; i < n; i++) { print(keys[i]); print(" = "); print(reg_get(keys[i], "")); print("\r\n"); }
}

/* Display form of the cwd for the prompt: the home directory shows as "~" (and
 * "$HOME/x" as "~/x"), like a normal shell. */
static const char *home_disp(const char *cwd, char *out) {
    if (starts(cwd, HOME) && (cwd[HOME_LEN] == 0 || cwd[HOME_LEN] == '/')) {
        out[0] = '~';
        int j = 1; for (const char *p = cwd + HOME_LEN; *p; p++) out[j++] = *p;
        out[j] = 0;
        return out;
    }
    return cwd;
}

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    char line[LINEMAX];
    chdir(HOME);                                   /* start in the user's home directory */
    reg_load();                                    /* system + per-user settings (design/settings.md) */
    if (isatty()) run_prog("fastfetch");          /* login banner on a real terminal */
    print("Welcome to the tOS shell. Type 'help'.\r\n");
    for (;;) {
        char cwd[256], disp[256];
        getcwd(cwd, sizeof cwd);                   /* show the working directory ("~" for home) */
        print("\x1b[92mtos\x1b[0m:\x1b[94m"); print(home_disp(cwd, disp)); print("\x1b[0m$ ");
        int n = readline(line, sizeof line);
        if (n == 0) continue;
        hist_add(line);

        if (streq(line, "help")) {
            print("commands:\r\n");
            print("  help ls [d] cd <d> pwd mkdir <d> rmdir <d> tree\r\n");
            print("  cat <f> write <f> rm [-r] <p> cp <s> <d> mv <s> <d> echo <t>\r\n");
            print("  copy <text> paste   (system clipboard; Super+V opens the manager)\r\n");
            print("  clear date uptime mem df sysinfo uname colors mouse lspci beep\r\n");
            print("  fastfetch spawn fork smp sleep <n> reboot halt poweroff crash\r\n");
            print("  reg get/set/list <key> [value]   (system + user settings)\r\n");
            print("  (arrows/Home/End/Del edit; up/down = history)\r\n");
        } else if (streq(line, "ls")) {
            cmd_ls("");
        } else if (starts(line, "ls ")) {
            cmd_ls(line + 3);
        } else if (streq(line, "pwd")) {
            cmd_pwd();
        } else if (starts(line, "cd ")) {
            cmd_cd(line + 3);
        } else if (streq(line, "cd")) {
            cmd_cd(HOME);
        } else if (starts(line, "mkdir ")) {
            cmd_mkdir(line + 6);
        } else if (starts(line, "rmdir ")) {
            cmd_rmdir(line + 6);
        } else if (streq(line, "tree")) {
            cmd_tree();
        } else if (starts(line, "mv ")) {
            cmd_mv(line + 3);
        } else if (starts(line, "cat ")) {
            cmd_cat(line + 4);
        } else if (starts(line, "write ")) {
            cmd_write(line + 6);
        } else if (starts(line, "rm ")) {
            cmd_rm(line + 3);
        } else if (starts(line, "cp ")) {
            cmd_cp(line + 3);
        } else if (starts(line, "sleep ")) {
            cmd_sleep(line + 6);
        } else if (starts(line, "echo ")) {
            print(line + 5); print("\r\n");
        } else if (streq(line, "id")) {
            print("uid="); printu((unsigned)getuid()); print("\r\n");
        } else if (starts(line, "id ")) {
            struct fstat st;
            if (stat_(line + 3, &st) < 0) print("id: no such path\r\n");
            else { print("owner="); printu(st.owner); print("\r\n"); }
        } else if (starts(line, "notify ")) {
            const char *rest = line + 7;             /* "notify <app> <body>": click routes to <app> */
            char app[24]; int an = 0;
            while (rest[an] && rest[an] != ' ' && an < 23) { app[an] = rest[an]; an++; }
            app[an] = 0;
            const char *body = rest[an] == ' ' ? rest + an + 1 : rest;  /* body defaults to the app name */
            notify_to(app, body, app);               /* post a desktop notification (toast), routed on click */
            print("notification sent\r\n");
        } else if (starts(line, "copy ")) {
            const char *t = line + 5; int n = 0; while (t[n]) n++;
            clip_put(CLIP_TEXT, "text", t, n);
            print("copied to clipboard (Super+V to view)\r\n");
        } else if (streq(line, "paste")) {
            char buf[512]; int n = clip_get(clip_active(-1), buf, sizeof buf - 1);
            if (n < 0) print("(clipboard empty)\r\n");
            else { buf[n] = 0; print(buf); print("\r\n"); }
        } else if (streq(line, "reg") || streq(line, "reg list")) {
            cmd_reg_list("");
        } else if (starts(line, "reg list ")) {
            cmd_reg_list(line + 9);
        } else if (starts(line, "reg get ")) {
            print(reg_get(line + 8, "(unset)")); print("\r\n");
        } else if (starts(line, "reg set ")) {
            cmd_reg_set(line + 8);
        } else if (streq(line, "clear")) {
            cmd_clear();
        } else if (streq(line, "date")) {
            cmd_date();
        } else if (streq(line, "uptime")) {
            cmd_uptime();
        } else if (streq(line, "mem")) {
            cmd_mem();
        } else if (streq(line, "df")) {
            cmd_df();
        } else if (streq(line, "uname")) {
            cmd_uname();
        } else if (streq(line, "sysinfo")) {
            cmd_sysinfo();
        } else if (streq(line, "colors")) {
            cmd_colors();
        } else if (streq(line, "mouse")) {
            cmd_mouse();
        } else if (streq(line, "lspci")) {
            lspci();
        } else if (streq(line, "beep")) {
            beep(880); sleep_ms(150); beep(0);
        } else if (streq(line, "fastfetch") || streq(line, "neofetch")) {
            run_prog("fastfetch");
        } else if (streq(line, "halt")) {
            print("closing terminal.\r\n");
            proc_exit();                     /* term notices, closes the window */
        } else if (streq(line, "poweroff")) {
            print("powering off...\r\n");
            shutdown();                      /* ACPI poweroff (whole machine) */
        } else if (streq(line, "spawn")) {
            int pid = fork();
            if (pid == 0) { exec("ticker"); proc_exit(); }
            else if (pid < 0) print("spawn failed\r\n");
            else { print("spawned ticker as pid "); printu((unsigned)pid); print("\r\n"); }
        } else if (streq(line, "fork")) {
            int pid = fork();
            if (pid == 0) { print("[child] hello from the forked child\r\n"); proc_exit(); }
            else if (pid < 0) print("fork failed\r\n");
            else { int rp = wait_child(); print("[parent] reaped child pid "); printu((unsigned)rp); print("\r\n"); }
        } else if (streq(line, "smp")) {
            for (int k = 0; k < 4; k++) {
                int pid = fork();
                if (pid == 0) {
                    char m[] = "[task] running on CPU 0\r\n";
                    m[22] = '0' + (char)(getcpu() % 10);
                    print(m);
                    for (volatile unsigned long d = 0; d < 40000000UL; d++) { }
                    proc_exit();
                } else if (pid < 0) { print("smp: fork failed\r\n"); break; }
            }
            while (wait_child() >= 0) { }
            print("smp: all tasks done\r\n");
        } else if (streq(line, "reboot")) {
            print("rebooting...\r\n"); reboot();
        } else if (streq(line, "crash")) {
            /* fault-injection: fork a child that touches a wild address. The kernel
             * kills just that child (a ring-3 fault), we reap it, and the shell keeps
             * running -- proof that one crashing process doesn't take down the OS. */
            int pid = fork();
            if (pid == 0) {
                volatile char *p = (volatile char *)0x7f0000000000UL;
                (void)*p;                    /* #PF here; never returns */
                proc_exit();
            } else if (pid < 0) print("crash: fork failed\r\n");
            else { int rp = wait_child();
                   print("[shell] child pid "); printu((unsigned)rp);
                   print(" crashed; shell and OS still alive\r\n"); }
        } else if (streq(line, "memtest")) {
            run_prog("memtest");             /* stress the multi-region frame pool (RAM across the 4 GiB hole) */
        } else if (streq(line, "selftest")) {
            run_prog("selftest");            /* in-OS self-checks: fs/registry/proc/clipboard (design/testing.md) */
        } else {
            print("unknown command: "); print(line); print("\r\n");
        }
    }
}
