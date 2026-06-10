/* Unit tests for the tag-index codec (user/lib/tagstore.h) -- the pure
 * "<mask>\t<path>" sidecar behind Finder-style colored tags (design/files-app.md
 * §10): get/set round-trips, mask replacement, dropping a cleared line, and
 * carrying tags across a rename. Checked on the host instead of in a booted VM. */
#include "unit.h"
#include "../../user/lib/tagstore.h"
#include <string.h>

static void test_get_set_roundtrip(void) {
    char idx[256];
    int n = tagstore_set(idx, sizeof idx, "", 0, "/Users/user/a.txt", 5);
    CHECK_STR(idx, "5\t/Users/user/a.txt\n", "set on an empty index appends one line");
    CHECK_INT(n, (int)strlen(idx), "set returns the new length");
    CHECK_INT((int)tagstore_get(idx, n, "/Users/user/a.txt"), 5, "get finds the recorded mask");
    CHECK_INT((int)tagstore_get(idx, n, "/Users/user/b.txt"), 0, "an untagged path reads 0");
    CHECK_INT((int)tagstore_get(idx, n, "/Users/user/a"), 0, "a path prefix does not alias");
}

static void test_set_replaces_and_drops(void) {
    char a[256], b[256];
    int n = tagstore_set(a, sizeof a, "", 0, "/x", 1);
    n = tagstore_set(b, sizeof b, a, n, "/y", 2);
    n = tagstore_set(a, sizeof a, b, n, "/x", 64);          /* replace /x's mask */
    CHECK_INT((int)tagstore_get(a, n, "/x"), 64, "set replaces an existing mask");
    CHECK_INT((int)tagstore_get(a, n, "/y"), 2, "other lines survive a replace");
    n = tagstore_set(b, sizeof b, a, n, "/x", 0);           /* clear -> line dropped */
    CHECK_INT((int)tagstore_get(b, n, "/x"), 0, "a cleared mask reads 0");
    CHECK_STR(b, "2\t/y\n", "clearing drops the whole line");
    n = tagstore_set(a, sizeof a, b, n, "/y", 0);
    CHECK_INT(n, 0, "clearing the last line empties the index");
}

static void test_multiline_scan(void) {
    const char *idx = "1\t/a\n64\t/b/c\n127\t/d d\n";       /* a space in a path is fine */
    int n = (int)strlen(idx);
    CHECK_INT((int)tagstore_get(idx, n, "/a"), 1, "first line found");
    CHECK_INT((int)tagstore_get(idx, n, "/b/c"), 64, "middle line found");
    CHECK_INT((int)tagstore_get(idx, n, "/d d"), 127, "a path with a space round-trips");
    CHECK_INT((int)tagstore_get(idx, n, "/b"), 0, "a path component does not alias");
}

static void test_move_carries_tags(void) {
    char a[256], b[256];
    int n = tagstore_set(a, sizeof a, "", 0, "/old", 9);
    n = tagstore_set(b, sizeof b, a, n, "/keep", 2);
    n = tagstore_move(a, sizeof a, b, n, "/old", "/new");
    CHECK_INT((int)tagstore_get(a, n, "/old"), 0, "the old path is untagged after a move");
    CHECK_INT((int)tagstore_get(a, n, "/new"), 9, "the new path inherits the mask");
    CHECK_INT((int)tagstore_get(a, n, "/keep"), 2, "unrelated lines survive a move");
    n = tagstore_move(b, sizeof b, a, n, "/never-tagged", "/elsewhere");
    CHECK_INT((int)tagstore_get(b, n, "/elsewhere"), 0, "moving an untagged path records nothing");
    CHECK_INT((int)tagstore_get(b, n, "/new"), 9, "a no-op move keeps the index intact");
}

static void test_names_table(void) {
    CHECK_INT(TAG_NCOLORS, 7, "seven fixed tag colors");
    CHECK_STR(tag_names_[0], "Red", "the first tag color is Red");
    CHECK_STR(tag_names_[TAG_NCOLORS - 1], "Gray", "the last tag color is Gray");
}

int main(void) {
    RUN(test_get_set_roundtrip);
    RUN(test_set_replaces_and_drops);
    RUN(test_multiline_scan);
    RUN(test_move_carries_tags);
    RUN(test_names_table);
    return UNIT_SUMMARY();
}
