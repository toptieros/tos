/* Unit tests for the pure capability fs-jail mapping in kernel/cap.h -- the
 * region->cap table the kernel jail check (kernel/fs/fs.c cap_may_reach) uses to
 * decide which cap a top-level fs region needs (design/app-runtime.md Phase 3). The
 * slot-walk that finds a path's top-level ancestor needs the live fs, so it's proven
 * by the in-OS selftest (group_fsjail); the table itself runs on the host here. */
#include "unit.h"
#include "../../kernel/cap.h"

/* cap_fs_region_need: the three jailed regions map to their fs cap; everything else
 * (root, /tmp, an unknown top-level dir) is ungated (0). */
static void test_region_need(void) {
    CHECK_INT(cap_fs_region_need("System"), CAP_FS_SYSTEM, "/System needs CAP_FS_SYSTEM");
    CHECK_INT(cap_fs_region_need("Users"),  CAP_FS_HOME,   "/Users needs CAP_FS_HOME");
    CHECK_INT(cap_fs_region_need("Apps"),   CAP_FS_BUNDLE, "/Apps needs CAP_FS_BUNDLE");
    CHECK_INT(cap_fs_region_need("tmp"),    0,             "/tmp is ungated scratch");
    CHECK_INT(cap_fs_region_need(""),       0,             "the root dir is ungated");
    CHECK_INT(cap_fs_region_need("Userspace"), 0,          "a shared-prefix sibling is ungated (exact match only)");
    CHECK_INT(cap_fs_region_need("System2"), 0,            "'System2' is not 'System'");
}

/* The fast-path rule the kernel uses: a task holding every fs cap is unrestricted,
 * so the jail only bites a task that dropped one. (CAP_NORMAL holds all three.) */
static void test_normal_holds_all_fs(void) {
    unsigned all = CAP_FS_HOME | CAP_FS_SYSTEM | CAP_FS_BUNDLE;
    CHECK((CAP_NORMAL & all) == all,  "CAP_NORMAL holds every fs cap (jail is a no-op for it)");
    CHECK((CAP_ALL   & all) == all,   "CAP_ALL holds every fs cap");
    /* a CAP_FS_SYSTEM-dropped task: still reaches Users, no longer reaches System */
    unsigned dropped = CAP_NORMAL & ~CAP_FS_SYSTEM;
    CHECK((dropped & cap_fs_region_need("System")) == 0, "dropping CAP_FS_SYSTEM blocks /System");
    CHECK((dropped & cap_fs_region_need("Users")) != 0,  "...but /Users is still reachable");
}

int main(void) {
    RUN(test_region_need);
    RUN(test_normal_holds_all_fs);
    return UNIT_SUMMARY();
}
