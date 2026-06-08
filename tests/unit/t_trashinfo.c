/* Unit tests for the Files Trash sidecar codec (user/lib/trashinfo.h) -- the pure
 * string logic behind move-to-trash / Put Back (design/files-app.md §9). The sidecar
 * is one "<trashedname>\t<originalpath>\n" line per trashed item; add/find/drop are
 * exercised here on the host instead of by booting Files and shuffling real files. */
#include "unit.h"
#include "../../user/lib/trashinfo.h"
#include <string.h>

static void test_add(void) {
    char buf[256];
    int n = trashinfo_add(buf, sizeof buf, "", 0, "notes.txt", "/Users/user/notes.txt");
    CHECK_STR(buf, "notes.txt\t/Users/user/notes.txt\n", "first record");
    CHECK_INT(n, (int)strlen(buf), "returned length matches");
    /* appending keeps the earlier line and adds a second */
    int n2 = trashinfo_add(buf, sizeof buf, buf, n, "pics", "/Users/user/Pictures/pics");
    CHECK_STR(buf,
              "notes.txt\t/Users/user/notes.txt\n"
              "pics\t/Users/user/Pictures/pics\n",
              "second record appended");
    CHECK_INT(n2, (int)strlen(buf), "two-record length");
}

static void test_find(void) {
    const char *info =
        "notes.txt\t/Users/user/notes.txt\n"
        "pics\t/Users/user/Pictures/pics\n"
        "deep\t/Users/user/a/b/c/deep\n";
    int len = (int)strlen(info);
    char out[256];
    CHECK_INT(trashinfo_find(info, len, "notes.txt", out, sizeof out), 1, "first found");
    CHECK_STR(out, "/Users/user/notes.txt", "first origin path");
    CHECK_INT(trashinfo_find(info, len, "pics", out, sizeof out), 1, "middle found");
    CHECK_STR(out, "/Users/user/Pictures/pics", "middle origin path");
    CHECK_INT(trashinfo_find(info, len, "deep", out, sizeof out), 1, "last found");
    CHECK_STR(out, "/Users/user/a/b/c/deep", "last origin path");
    /* a miss clears out and returns 0; a prefix of a real name is NOT a match */
    CHECK_INT(trashinfo_find(info, len, "missing", out, sizeof out), 0, "absent name");
    CHECK_STR(out, "", "miss clears the output");
    CHECK_INT(trashinfo_find(info, len, "note", out, sizeof out), 0, "prefix is not a match");
    CHECK_INT(trashinfo_find(info, len, "pic", out, sizeof out), 0, "shorter prefix is not a match");
}

static void test_drop(void) {
    const char *info =
        "a\t/x/a\n"
        "b\t/x/b\n"
        "c\t/x/c\n";
    int len = (int)strlen(info);
    char buf[256];
    int n = trashinfo_drop(buf, sizeof buf, info, len, "b");        /* remove the middle line */
    CHECK_STR(buf, "a\t/x/a\nc\t/x/c\n", "middle record dropped, others intact");
    CHECK_INT(n, (int)strlen(buf), "drop returns the new length");
    /* dropping the only line leaves an empty sidecar */
    const char *solo = "solo\t/x/solo\n";
    int e = trashinfo_drop(buf, sizeof buf, solo, (int)strlen(solo), "solo");
    CHECK_INT(e, 0, "dropping the last record empties the sidecar");
    CHECK_STR(buf, "", "empty sidecar string");
    /* dropping an absent name is a no-op copy */
    int u = trashinfo_drop(buf, sizeof buf, info, len, "zzz");
    CHECK_INT(u, len, "absent name leaves every record");
}

static void test_roundtrip(void) {
    /* trash three, put back the middle one: find it, then drop its line */
    char buf[256] = ""; int n = 0;
    n = trashinfo_add(buf, sizeof buf, buf, n, "one",   "/d/one");
    n = trashinfo_add(buf, sizeof buf, buf, n, "two",   "/d/two");
    n = trashinfo_add(buf, sizeof buf, buf, n, "three", "/d/three");
    char dst[256];
    CHECK_INT(trashinfo_find(buf, n, "two", dst, sizeof dst), 1, "locate the item to restore");
    CHECK_STR(dst, "/d/two", "its recorded origin");
    char buf2[256];
    int n2 = trashinfo_drop(buf2, sizeof buf2, buf, n, "two");
    CHECK_STR(buf2, "one\t/d/one\nthree\t/d/three\n", "restored item's record is gone");
    /* the restored name is no longer findable; the survivors still are */
    char tmp[256];
    CHECK_INT(trashinfo_find(buf2, n2, "two", tmp, sizeof tmp), 0, "restored item no longer recorded");
    CHECK_INT(trashinfo_find(buf2, n2, "one", tmp, sizeof tmp), 1, "survivor still recorded");
}

int main(void) {
    RUN(test_add);
    RUN(test_find);
    RUN(test_drop);
    RUN(test_roundtrip);
    return UNIT_SUMMARY();
}
