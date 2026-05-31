/* init -- the first user program the kernel starts (PID 1). Its job is to bring
 * up the rest of userspace and supervise it: it spawns the shell, then sleeps
 * until every task it started has exited, at which point it powers the machine
 * down. If init itself ever exits, the kernel treats that as fatal. */
#include "ulib.h"

/* Make sure the standard directory tree exists (see design/filesystem-layout.md).
 * mkfs seeds it on a normal disk; this self-heals a fresh or damaged one. mkdir of
 * an existing directory just fails harmlessly, and parents are created first. */
static void ensure_tree(void) {
    static const char *dirs[] = {
        "/System", "/System/bin", "/System/etc", "/System/lib", "/Apps", "/tmp",
        "/Users", "/Users/user", "/Users/user/Desktop", "/Users/user/Documents",
        "/Users/user/Downloads", "/Users/user/Pictures", "/Users/user/.config", 0,
    };
    for (int i = 0; dirs[i]; i++) mkdir(dirs[i]);
}

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    print("\r\n[init] pid 1 is up; bringing up the system\r\n");
    ensure_tree();

    int pid = fork();
    if (pid < 0) {
        print("[init] FATAL: fork failed\r\n");
        shutdown();
    }
    if (pid == 0) {                 /* child becomes the window manager, or the shell if it can't */
        exec("twm");                /* twm falls back to the shell when there's no framebuffer */
        exec("shell");              /* only reached if twm is missing/unloadable */
        print("[init] FATAL: exec(twm/shell) failed\r\n");
        proc_exit();
    }
    print("[init] started the desktop; now supervising\r\n");

    /* Reap children (the shell plus any orphans re-parented to us) until none
     * remain -- wait() blocks without spinning and returns -1 when we are done. */
    while (wait_child() >= 0) { }

    print("[init] all children have exited; shutting down\r\n");
    shutdown();
    for (;;) { }
}
