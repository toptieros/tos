/* fileinfo.h -- pure helpers for the Files "Get Info" pane (design/files-app.md §8):
 * owner labels, the read-only / locked rule, and a singular/plural item-count
 * formatter. No libc, no kernel deps; included by the freestanding Files app and by
 * the host unit tests (tests/unit/t_fileinfo.c), so the rules have one definition.
 *
 * The owner uids mirror kernel/fs/perm.h (0 = the system/OS, 1 = the human user). */
#pragma once

#define INFO_UID_SYSTEM 0u   /* the OS: root tree, /System, shipped /Apps      */
#define INFO_UID_USER   1u   /* the single human user: /Users subtree + /tmp   */

/* Short human label for an owning uid, shown on the Get Info "Owner" line. */
static inline const char *info_owner_label(unsigned owner) {
    if (owner == INFO_UID_SYSTEM) return "System";
    if (owner == INFO_UID_USER)   return "You";
    return "Other";
}

/* True when `me` may not write an item owned by `owner`: the system owns it and I
 * am not the system. Mirrors tos_may_write() in perm.h -> drives the lock badge. */
static inline int info_is_locked(unsigned owner, unsigned me) {
    return owner == INFO_UID_SYSTEM && me != INFO_UID_SYSTEM;
}

/* "1 item" / "N items" (correct singular) into out; always NUL-terminated. Renders
 * the count by hand so it stays usable in the freestanding app (no stdio/itoa). */
static inline void info_count_label(char *out, int cap, unsigned items) {
    if (cap <= 0) return;
    char num[12]; int ni = 0;
    if (items == 0) num[ni++] = '0';
    else { char tmp[12]; int t = 0; unsigned v = items;
           while (v) { tmp[t++] = (char)('0' + v % 10u); v /= 10u; }
           while (t) num[ni++] = tmp[--t]; }
    int p = 0;
    for (int i = 0; i < ni && p < cap - 1; i++) out[p++] = num[i];
    const char *suf = (items == 1) ? " item" : " items";
    for (int i = 0; suf[i] && p < cap - 1; i++) out[p++] = suf[i];
    out[p] = 0;
}
