/* tosfs ownership -- who may write what. Shared by the host mkfs tool, the kernel
 * fs, and the host unit tests, so the rule has a single definition (design/
 * system-ownership.md). Pure logic, no libc, no kernel deps.
 *
 * The coarse model has two identities: the hidden `system` user (uid 0) owns the
 * OS -- the root tree, /System, and the /Apps bundles that ship with it -- and a
 * single human user (uid 1) owns the /Users subtree and the /tmp scratch area. A mutating
 * fs syscall is allowed only if the caller owns the target or is `system`. Reads
 * and exec are unguarded, so /System stays world-readable/executable. */
#pragma once

#define TOS_UID_SYSTEM 0   /* the OS: owns the root tree, /System, shipped /Apps  */
#define TOS_UID_USER   1   /* the single human user: owns the /Users subtree + /tmp */

/* Coarse write check: a task may write an entry it owns; `system` writes anything. */
static inline int tos_may_write(int uid, int owner) {
    return uid == TOS_UID_SYSTEM || uid == owner;
}

/* True when `path` is `pre` itself or a child of it (matched on a '/' boundary,
 * so "/Users" matches "/Users" and "/Users/x" but not "/UsersX"). */
static inline int tos_path_under(const char *path, const char *pre) {
    int i = 0;
    while (pre[i]) { if (path[i] != pre[i]) return 0; i++; }
    return path[i] == 0 || path[i] == '/';
}

/* The owner uid mkfs stamps an absolute path with when it seeds the disk: the
 * user owns the /Users subtree and /tmp; the system owns everything else (the root tree,
 * /System and its subtree, the shipped /Apps bundles). (Runtime-created entries
 * instead inherit their creator's uid -- see fs.c.) */
static inline int tos_owner_for(const char *path) {
    if (tos_path_under(path, "/Users")) return TOS_UID_USER;
    if (tos_path_under(path, "/tmp"))   return TOS_UID_USER;
    return TOS_UID_SYSTEM;
}
