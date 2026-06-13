/* argv.h -- the pure in-place argv tokenizer behind getargs() (ulib.c). Split out so
 * the host can unit-test it (no syscalls, no globals). The kernel seeds a task's data
 * page (USER_DATA_VADDR) with its full command line ("ls /tmp"); getargs() copies that
 * into a buffer and calls this to carve it into a NUL-terminated argv[]. See
 * design/shell.md (band 2: argv passing). */
#pragma once

/* Tokenize `buf` in place on runs of spaces/tabs: writes a NUL after each token and
 * stores a pointer to each into argv[] (up to maxv). Returns argc. `buf` is mutated.
 * Leading/trailing/repeated whitespace is collapsed; an empty/blank line yields 0. */
static inline int argv_split(char *buf, char **argv, int maxv) {
    int argc = 0, i = 0;
    for (;;) {
        while (buf[i] == ' ' || buf[i] == '\t') i++;     /* skip separators */
        if (!buf[i]) break;
        if (argc >= maxv) break;
        argv[argc++] = &buf[i];                          /* token start */
        while (buf[i] && buf[i] != ' ' && buf[i] != '\t') i++;
        if (buf[i]) buf[i++] = 0;                         /* terminate this token */
    }
    return argc;
}
