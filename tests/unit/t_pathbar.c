/* Unit tests for the Files location-bar path splitter (user/lib/pathbar.h) -- the
 * pure logic under the clickable breadcrumb (design/files-app.md §3). Splitting a
 * path into (label, navigate-to-path) crumbs is checked here on the host instead of
 * by booting Files. */
#include "unit.h"
#include "../../user/lib/pathbar.h"

static void test_root(void) {
    struct crumb c[16];
    int n = pathbar_split("/", c, 16);
    CHECK_INT(n, 1, "root is a single crumb");
    CHECK_STR(c[0].label, "/", "root label");
    CHECK_STR(c[0].path, "/", "root navigates to /");
}

static void test_one_level(void) {
    struct crumb c[16];
    int n = pathbar_split("/Users", c, 16);
    CHECK_INT(n, 2, "root + one segment");
    CHECK_STR(c[1].label, "Users", "segment label");
    CHECK_STR(c[1].path, "/Users", "segment navigates to /Users");
}

static void test_deep(void) {
    struct crumb c[16];
    int n = pathbar_split("/Users/user/Documents", c, 16);
    CHECK_INT(n, 4, "root + three segments");
    CHECK_STR(c[0].path, "/", "crumb 0 -> /");
    CHECK_STR(c[1].path, "/Users", "crumb 1 -> /Users");
    CHECK_STR(c[2].path, "/Users/user", "crumb 2 -> /Users/user");
    CHECK_STR(c[3].path, "/Users/user/Documents", "crumb 3 -> full path");
    CHECK_STR(c[3].label, "Documents", "last crumb label is the leaf");
}

static void test_trailing_and_double_slash(void) {
    struct crumb c[16];
    CHECK_INT(pathbar_split("/Users/user/", c, 16), 3, "a trailing slash adds no empty crumb");
    CHECK_STR(c[2].path, "/Users/user", "trailing slash collapses");
    int n = pathbar_split("//Users//user", c, 16);
    CHECK_INT(n, 3, "repeated slashes collapse");
    CHECK_STR(c[1].path, "/Users", "double slash after root collapses");
    CHECK_STR(c[2].path, "/Users/user", "double slash mid-path collapses");
}

static void test_cap(void) {
    struct crumb c[2];
    int n = pathbar_split("/Users/user/Documents", c, 2);
    CHECK_INT(n, 2, "split stops at the crumb cap");
    CHECK_STR(c[1].path, "/Users", "the capped split still fills what it can");
}

int main(void) {
    RUN(test_root);
    RUN(test_one_level);
    RUN(test_deep);
    RUN(test_trailing_and_double_slash);
    RUN(test_cap);
    return UNIT_SUMMARY();
}
