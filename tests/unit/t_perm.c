/* Unit tests for the tosfs ownership rules in kernel/fs/perm.h -- the pure logic
 * behind "who may delete /System" (design/system-ownership.md). The truth table
 * (tos_may_write) and the mkfs owner-assignment rule (tos_owner_for) used to be
 * exercisable only by booting the OS and trying an rm; here they run on the host
 * in microseconds. The e2e canary (t_system_ownership) still proves enforcement. */
#include "unit.h"
#include "../../kernel/fs/perm.h"

/* may_write: you may write what you own; system (uid 0) may write anything. */
static void test_may_write(void) {
    CHECK(tos_may_write(TOS_UID_USER,   TOS_UID_USER),    "user writes a user-owned entry");
    CHECK(!tos_may_write(TOS_UID_USER,  TOS_UID_SYSTEM),  "user may NOT write a system entry");
    CHECK(tos_may_write(TOS_UID_SYSTEM, TOS_UID_USER),    "system writes a user entry");
    CHECK(tos_may_write(TOS_UID_SYSTEM, TOS_UID_SYSTEM),  "system writes a system entry");
}

/* owner_for: /Users + /tmp are the user's; the rest of the tree is the system's. */
static void test_owner_for(void) {
    CHECK_INT(tos_owner_for("/"),                            TOS_UID_SYSTEM, "root is system-owned");
    CHECK_INT(tos_owner_for("/System"),                      TOS_UID_SYSTEM, "/System is system");
    CHECK_INT(tos_owner_for("/System/bin/twm"),              TOS_UID_SYSTEM, "/System/bin/twm is system");
    CHECK_INT(tos_owner_for("/Apps"),                        TOS_UID_USER,   "the /Apps dir itself is user-writable (install apps)");
    CHECK_INT(tos_owner_for("/Apps/Files.app"),              TOS_UID_SYSTEM, "a shipped bundle is system-owned (protected)");
    CHECK_INT(tos_owner_for("/Apps/Files.app/bin/files"),    TOS_UID_SYSTEM, "shipped app bundle is system");
    CHECK_INT(tos_owner_for("/Users"),                       TOS_UID_USER,   "/Users is the user's");
    CHECK_INT(tos_owner_for("/Users/user"),                  TOS_UID_USER,   "the home dir is the user's");
    CHECK_INT(tos_owner_for("/Users/user/Documents/a.txt"),  TOS_UID_USER,   "files under home are the user's");
    CHECK_INT(tos_owner_for("/tmp"),                         TOS_UID_USER,   "/tmp is user-writable");
    CHECK_INT(tos_owner_for("/tmp/.open-doc"),               TOS_UID_USER,   "/tmp scratch is the user's");
}

/* The prefix match is on a component boundary, so a sibling that merely shares a
 * prefix is NOT treated as inside (no "/UsersX" sneaking into the user's space). */
static void test_boundary(void) {
    CHECK_INT(tos_owner_for("/UsersX"),     TOS_UID_SYSTEM, "/UsersX is not under /Users");
    CHECK_INT(tos_owner_for("/tmpfoo"),     TOS_UID_SYSTEM, "/tmpfoo is not under /tmp");
    CHECK(tos_path_under("/Users", "/Users"),         "a path is under itself");
    CHECK(tos_path_under("/Users/user", "/Users"),    "a child is under its ancestor");
    CHECK(!tos_path_under("/UsersX", "/Users"),       "boundary: shared prefix is not under");
}

int main(void) {
    RUN(test_may_write);
    RUN(test_owner_for);
    RUN(test_boundary);
    return UNIT_SUMMARY();
}
