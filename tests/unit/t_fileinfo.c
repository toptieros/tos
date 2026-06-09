/* Unit tests for user/lib/fileinfo.h -- the Get Info owner/lock/count helpers (§8). */
#include "../../user/lib/fileinfo.h"
#include "unit.h"

static void test_owner_label(void) {
    CHECK_STR(info_owner_label(INFO_UID_SYSTEM), "System", "uid 0 is the system");
    CHECK_STR(info_owner_label(INFO_UID_USER),   "You",    "uid 1 is the human user");
    CHECK_STR(info_owner_label(7),               "Other",  "unknown uid falls back");
}

static void test_locked_rule(void) {
    CHECK_INT(info_is_locked(INFO_UID_SYSTEM, INFO_UID_USER),   1, "user can't write /System");
    CHECK_INT(info_is_locked(INFO_UID_USER,   INFO_UID_USER),   0, "my own file is writable");
    CHECK_INT(info_is_locked(INFO_UID_SYSTEM, INFO_UID_SYSTEM), 0, "system writes its own");
    CHECK_INT(info_is_locked(INFO_UID_USER,   INFO_UID_SYSTEM), 0, "system writes a user file");
}

static void test_count_label(void) {
    char b[24];
    info_count_label(b, sizeof b, 0);    CHECK_STR(b, "0 items",    "zero is plural");
    info_count_label(b, sizeof b, 1);    CHECK_STR(b, "1 item",     "one is singular");
    info_count_label(b, sizeof b, 2);    CHECK_STR(b, "2 items",    "two is plural");
    info_count_label(b, sizeof b, 42);   CHECK_STR(b, "42 items",   "multi-digit");
    info_count_label(b, sizeof b, 3702); CHECK_STR(b, "3702 items", "whole-disk count");
}

static void test_truncation_is_safe(void) {
    char s[5];
    info_count_label(s, sizeof s, 12345);
    CHECK_INT((int)s[4], 0, "stays NUL-terminated when it overflows");
    CHECK(s[0] == '1', "keeps the leading digits");
}

int main(void) {
    RUN(test_owner_label);
    RUN(test_locked_rule);
    RUN(test_count_label);
    RUN(test_truncation_is_safe);
    return UNIT_SUMMARY();
}
