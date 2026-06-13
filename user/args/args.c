/* args -- a tiny /System/bin program that prints its own argument vector. It exists
 * to prove the argv mechanism end-to-end: the shell execs "args one two", the kernel
 * splits the path token, seeds the data page with the whole line, and getargs() here
 * carves it back into argv[]. (design/shell.md, band 2.) */
#include "ulib.h"

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    char *argv[32];
    int argc = getargs(argv, 32);
    print("argc="); printu((unsigned)argc); print("\r\n");
    for (int i = 0; i < argc; i++) {
        print("argv["); printu((unsigned)i); print("]="); print(argv[i]); print("\r\n");
    }
    proc_exit();
    for (;;) { }
}
