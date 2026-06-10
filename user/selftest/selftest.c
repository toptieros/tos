/* selftest: in-OS self-checks, dozens of kernel/fs/syscall assertions in ONE boot
 * (design/testing.md). The e2e suite used to burn a full QEMU boot per area (fs
 * CRUD, registry, statfs, fork, clipboard, ...); this program runs them all
 * natively in a few hundred ms and prints one PASS/FAIL line per group. The
 * harness types `selftest` into the terminal and asserts on the final tally:
 *
 *   selftest: 42/42 OK          (or "selftest: FAILED n/42")
 *
 * Rules of the house: every check is silent when green; a failure prints the
 * expression so the serial log pinpoints it. Scratch state lives under
 * ~/.selftest (dot-hidden) and is removed at the end; the registry probe uses
 * its own key. Add checks here BEFORE reaching for a new e2e boot test. */
#include "ulib.h"
#include "registry.h"

static int npass = 0, nfail = 0;
#define CHECK(cond) do { \
    if (cond) npass++; \
    else { nfail++; print("selftest FAIL: " #cond "\r\n"); } \
} while (0)

#define SCRATCH "/Users/user/.selftest"

static int write_file(const char *path, const char *bytes, int len) {
    int fd = fopen(path, O_CREATE | O_TRUNC);
    if (fd < 0) return -1;
    int w = fwrite_(fd, bytes, len);
    fclose_(fd);
    return w;
}
static int read_file(const char *path, char *buf, int cap) {
    int fd = fopen(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0, r;
    while (n < cap && (r = fread_(fd, buf + n, cap - n)) > 0) n += r;
    fclose_(fd);
    return n;
}
static int dir_has(const char *dir, const char *name) {
    struct dirent e[48];
    int n = readdir(dir, e, 48);
    for (int i = 0; i < n; i++) if (streq(e[i].name, name)) return 1;
    return 0;
}

static void group_fs(void) {
    CHECK(mkdir(SCRATCH) == 0);
    CHECK(write_file(SCRATCH "/a.txt", "hello tos", 9) == 9);
    char buf[64];
    CHECK(read_file(SCRATCH "/a.txt", buf, sizeof buf) == 9);
    buf[9] = 0; CHECK(streq(buf, "hello tos"));
    struct fstat st;
    CHECK(stat_(SCRATCH "/a.txt", &st) == 0 && st.size == 9 && st.type == FT_FILE);
    CHECK(dir_has(SCRATCH, "a.txt"));
    /* rewrite in place shrinks */
    CHECK(write_file(SCRATCH "/a.txt", "hi", 2) == 2);
    CHECK(stat_(SCRATCH "/a.txt", &st) == 0 && st.size == 2);
    /* zero-byte files persist as real entries (the tosfs close_l fix) */
    CHECK(write_file(SCRATCH "/empty", "", 0) == 0);
    CHECK(stat_(SCRATCH "/empty", &st) == 0 && st.size == 0 && st.type == FT_FILE);
    /* rename within a dir, then across dirs */
    CHECK(rename_(SCRATCH "/a.txt", SCRATCH "/b.txt") == 0);
    CHECK(!dir_has(SCRATCH, "a.txt") && dir_has(SCRATCH, "b.txt"));
    CHECK(mkdir(SCRATCH "/sub") == 0);
    CHECK(stat_(SCRATCH "/sub", &st) == 0 && st.type == FT_DIR);
    CHECK(rename_(SCRATCH "/b.txt", SCRATCH "/sub/b.txt") == 0);
    CHECK(dir_has(SCRATCH "/sub", "b.txt"));
    /* cwd round-trip */
    char cwd[256];
    CHECK(chdir(SCRATCH "/sub") == 0 && getcwd(cwd, sizeof cwd) == 0 && streq(cwd, SCRATCH "/sub"));
    CHECK(chdir("/Users/user") == 0);
    /* teardown (rmdir refuses non-empty) */
    CHECK(rmdir(SCRATCH) < 0);
    CHECK(funlink(SCRATCH "/sub/b.txt") == 0);
    CHECK(rmdir(SCRATCH "/sub") == 0);
    CHECK(funlink(SCRATCH "/empty") == 0);
    CHECK(rmdir(SCRATCH) == 0);
    CHECK(!dir_has("/Users/user", ".selftest"));
}

static void group_statfs(void) {
    struct statfs sf;
    CHECK(statfs_(&sf) == 0);
    CHECK(sf.total_bytes > 0 && sf.free_bytes <= sf.total_bytes);
}

static void group_registry(void) {
    reg_load();
    reg_set("selftest.probe", "42");
    CHECK(streq(reg_get("selftest.probe", ""), "42"));
    CHECK(reg_int("selftest.probe", -1) == 42);
    CHECK(reg_save() == 0);
    reg_load();                                   /* reload from disk */
    CHECK(reg_int("selftest.probe", -1) == 42);
    reg_set("selftest.probe", "");                /* leave no residue */
    reg_save();
    CHECK(reg_int("selftest.nosuch", 7) == 7);
}

static void group_proc(void) {
    int me = getpid();
    CHECK(me > 0);
    CHECK(trywait() == -1);                       /* no children yet */
    int pid = fork();
    if (pid == 0) {                               /* child: report ppid via a file, exit */
        char b[16]; int n = 0; int v = getppid();
        do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        char out[16]; for (int i = 0; i < n; i++) out[i] = b[n - 1 - i];
        write_file("/Users/user/.st_ppid", out, n);
        proc_exit();
    }
    CHECK(pid > 0);
    CHECK(wait_child() == pid);                   /* reaps the zombie */
    char buf[16]; int n = read_file("/Users/user/.st_ppid", buf, sizeof buf - 1);
    int rep = 0; for (int i = 0; i < n; i++) rep = rep * 10 + (buf[i] - '0');
    CHECK(rep == me);                             /* the child saw us as its parent */
    funlink("/Users/user/.st_ppid");
    CHECK(trywait() == -1);                       /* nothing left to reap */
    struct sysinfo si; sysinfo(&si);
    CHECK(getcpu() >= 0 && getcpu() < (int)si.ncpu);
    sleep_ms(20);                                 /* returns, doesn't wedge the task */
    CHECK(1);
}

static void group_clipboard(void) {
    clip_clear();
    CHECK(clip_count() == 0);
    int idx = clip_put(CLIP_TEXT, "probe", "snippet", 7);
    CHECK(idx >= 0 && clip_count() == 1);
    char buf[16]; int n = clip_get(idx, buf, sizeof buf);
    CHECK(n == 7);
    buf[7] = 0; CHECK(streq(buf, "snippet"));
    struct clipinfo ci;
    CHECK(clip_info(idx, &ci) == 0 && ci.type == CLIP_TEXT && ci.len == 7 && streq(ci.name, "probe"));
    clip_clear();
    CHECK(clip_count() == 0);
}

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    print("[selftest] running\r\n");
    group_fs();
    group_statfs();
    group_registry();
    group_proc();
    group_clipboard();
    if (nfail == 0) {
        print("selftest: "); printu((unsigned)npass); print("/"); printu((unsigned)npass);
        print(" OK\r\n");
    } else {
        print("selftest: FAILED "); printu((unsigned)nfail); print("/");
        printu((unsigned)(npass + nfail)); print("\r\n");
    }
    proc_exit();
    for (;;) {}
}
