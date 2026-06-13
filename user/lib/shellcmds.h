/* shellcmds.h -- the shell's built-in command catalog + pure line-editor helpers,
 * split out of shell.c so the host can unit-test them (no syscalls, no globals).
 * The catalog (name + one-line description) powers three things at once: the `help`
 * listing, Tab completion of the FIRST token, and syntax highlighting (a first token
 * that resolves to a builtin is drawn green). See design/shell.md. */
#pragma once

struct shcmd { const char *name; const char *desc; };

/* The built-in command names, each with the one-line description Tab completion
 * shows. Keep in sync with shell.c's dispatch chain. Sorted for a tidy pager. */
static const struct shcmd SH_CMDS[] = {
    { "beep",     "play a short tone" },
    { "cat",      "print a file's contents" },
    { "cd",       "change the working directory" },
    { "clear",    "clear the screen" },
    { "colors",   "show the ANSI colour palette" },
    { "copy",     "copy text to the clipboard" },
    { "cp",       "copy a file" },
    { "crash",    "fault-isolation demo (a child segfaults)" },
    { "date",     "show the date and time" },
    { "df",       "show filesystem usage" },
    { "echo",     "print its arguments" },
    { "fastfetch","show the system info banner" },
    { "fork",     "fork/wait demo" },
    { "get",      "fetch a file over HTTP (needs the net cap)" },
    { "halt",     "close the terminal" },
    { "help",     "list the built-in commands" },
    { "id",       "show a uid / a path's owner" },
    { "install",  "clone the boot disk onto a target" },
    { "ls",       "list a directory" },
    { "lspci",    "list PCI devices" },
    { "mem",      "show memory usage" },
    { "memtest",  "stress the frame allocator" },
    { "mkdir",    "make a directory" },
    { "mouse",    "show the mouse state" },
    { "mv",       "move/rename a file" },
    { "notify",   "post a desktop notification" },
    { "paste",    "print the clipboard contents" },
    { "ping",     "ICMP echo a host (needs the net cap)" },
    { "poweroff", "power off the machine" },
    { "pwd",      "print the working directory" },
    { "reboot",   "reboot the machine" },
    { "reg",      "get/set/list the settings registry" },
    { "rm",       "remove a file (-r for a tree)" },
    { "rmdir",    "remove an empty directory" },
    { "selftest", "run the in-OS self-checks" },
    { "serve",    "serve one HTTP request (needs the net cap)" },
    { "sleep",    "sleep for N milliseconds" },
    { "smp",      "multi-core fork demo" },
    { "spawn",    "spawn a background ticker" },
    { "sysinfo",  "show a system summary" },
    { "tree",     "print the directory tree" },
    { "uname",    "show the OS name" },
    { "uptime",   "show how long the OS has run" },
    { "write",    "write a line to a file" },
};
#define SH_NCMDS ((int)(sizeof(SH_CMDS) / sizeof(SH_CMDS[0])))

/* Is `tok` (a NUL-terminated word) an exact built-in command name? */
static inline int sh_is_builtin(const char *tok) {
    for (int i = 0; i < SH_NCMDS; i++) {
        const char *a = SH_CMDS[i].name, *b = tok;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return 1;
    }
    return 0;
}

/* Copy the first whitespace-delimited token of `line` into `out` (capped), and
 * return its length. Leading spaces are skipped. `out` is always NUL-terminated. */
static inline int sh_first_token(const char *line, char *out, int cap) {
    int i = 0; while (line[i] == ' ' || line[i] == '\t') i++;
    int n = 0; while (line[i] && line[i] != ' ' && line[i] != '\t' && n < cap - 1) out[n++] = line[i++];
    out[n] = 0;
    return n;
}

/* Does `name` start with `prefix`? (case-sensitive; an empty prefix matches all.) */
static inline int sh_has_prefix(const char *name, const char *prefix) {
    for (int i = 0; prefix[i]; i++) if (name[i] != prefix[i]) return 0;
    return 1;
}

/* The length of the longest common prefix of two strings -- used so Tab extends the
 * line by the unambiguous part shared by all candidates before showing the pager. */
static inline int sh_common_len(const char *a, const char *b) {
    int i = 0; while (a[i] && a[i] == b[i]) i++;
    return i;
}

/* Whether a line is "still on the first token" at cursor offset `pos` (no space yet
 * before the cursor) -- decides command-name vs path completion. */
static inline int sh_in_first_token(const char *line, int pos) {
    for (int i = 0; i < pos; i++) if (line[i] == ' ' || line[i] == '\t') return 0;
    return 1;
}
