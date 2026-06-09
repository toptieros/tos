/* Unit tests for user/lib/tabtitle.h -- the Files tab-strip pill label (§4). */
#include "../../user/lib/tabtitle.h"
#include "unit.h"

static void test_basename(void) {
    char b[32];
    tab_title("/Users/user/Documents", b, sizeof b); CHECK_STR(b, "Documents", "deep path -> last component");
    tab_title("/Users/user", b, sizeof b);           CHECK_STR(b, "user",      "home folder");
    tab_title("/Apps", b, sizeof b);                 CHECK_STR(b, "Apps",      "top-level folder");
}

static void test_roots_and_slashes(void) {
    char b[32];
    tab_title("/", b, sizeof b);                      CHECK_STR(b, "Computer",  "volume root is Computer");
    tab_title("/Users/user/Documents/", b, sizeof b); CHECK_STR(b, "Documents", "trailing slash ignored");
    tab_title("//", b, sizeof b);                     CHECK_STR(b, "Computer",  "all-slashes -> Computer");
}

static void test_truncation_is_safe(void) {
    char s[5];
    tab_title("/Users/user/Downloads", s, sizeof s);
    CHECK_INT((int)s[4], 0, "stays NUL-terminated when it overflows");
    CHECK(s[0] == 'D', "keeps the leading characters");
}

int main(void) {
    RUN(test_basename);
    RUN(test_roots_and_slashes);
    RUN(test_truncation_is_safe);
    return UNIT_SUMMARY();
}
