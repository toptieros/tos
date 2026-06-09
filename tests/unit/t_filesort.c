/* Unit tests for the Files directory comparator (user/lib/filesort.h) -- the pure
 * logic under Sort by Name/Kind/Size + direction + folders-first (design/files-app.md
 * §2). Checked on the host instead of by eyeballing a booted Files window. */
#include "unit.h"
#include "../../user/lib/filesort.h"

/* convenience: sign of the comparator for a key/dir/ff combo (mtime irrelevant here) */
static int cmp(const char *an, int ad, unsigned asz, const char *bn, int bd, unsigned bsz,
               int key, int desc, int ff) {
    int c = filesort_cmp(an, ad, asz, 0, bn, bd, bsz, 0, key, desc, ff);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}
/* date-aware wrapper: sign of FSORT_DATE for two mtimes (folders-first on) */
static int cmpd(const char *an, int ad, unsigned amt, const char *bn, int bd, unsigned bmt, int desc) {
    int c = filesort_cmp(an, ad, 0, amt, bn, bd, 0, bmt, FSORT_DATE, desc, 1);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

static void test_natcmp(void) {
    CHECK(fs_natcmp("file2", "file10") < 0,  "natural: file2 before file10");
    CHECK(fs_natcmp("file10", "file2") > 0,  "natural: file10 after file2");
    CHECK_INT(fs_natcmp("abc", "abc"), 0,    "equal names compare equal");
    CHECK(fs_natcmp("Apple", "apple") == 0,  "case-insensitive equality");
    CHECK(fs_natcmp("apple", "banana") < 0,  "alphabetical a < b");
    CHECK(fs_natcmp("img9", "img12") < 0,    "natural within a stem");
    CHECK(fs_natcmp("a", "ab") < 0,          "prefix sorts before the longer name");
}

static void test_extcmp(void) {
    CHECK(fs_extcmp("a.txt", "b.png") > 0,   "kind: png before txt");
    CHECK(fs_extcmp("z.md", "a.txt") < 0,    "kind ignores the stem (md before txt)");
    CHECK_INT(fs_extcmp("a.txt", "b.txt"), 0,"same extension compares equal");
    CHECK(fs_extcmp("README", "a.txt") < 0,  "no-extension sorts before an extension");
}

static void test_name_sort(void) {
    /* default: folders first, ascending, by name */
    CHECK_INT(cmp("dir", 1, 0, "aaa.txt", 0, 0, FSORT_NAME, 0, 1), -1, "a folder sorts before a file");
    CHECK_INT(cmp("aaa.txt", 0, 0, "dir", 1, 0, FSORT_NAME, 0, 1),  1, "a file sorts after a folder");
    CHECK_INT(cmp("apple.txt", 0, 0, "banana.txt", 0, 0, FSORT_NAME, 0, 1), -1, "files by name asc");
    CHECK_INT(cmp("apple.txt", 0, 0, "banana.txt", 0, 0, FSORT_NAME, 1, 1),  1, "descending flips files");
    /* folders stay first even when descending */
    CHECK_INT(cmp("dir", 1, 0, "zzz.txt", 0, 0, FSORT_NAME, 1, 1), -1, "folders-first survives descending");
}

static void test_size_sort(void) {
    CHECK_INT(cmp("a.txt", 0, 100, "b.txt", 0, 200, FSORT_SIZE, 0, 1), -1, "smaller size first (asc)");
    CHECK_INT(cmp("a.txt", 0, 200, "b.txt", 0, 100, FSORT_SIZE, 0, 1),  1, "larger size later (asc)");
    CHECK_INT(cmp("a.txt", 0, 100, "b.txt", 0, 200, FSORT_SIZE, 1, 1),  1, "descending reverses size");
    CHECK_INT(cmp("a.txt", 0, 50, "b.txt", 0, 50, FSORT_SIZE, 0, 1), -1, "size tie breaks by name");
}

static void test_kind_sort(void) {
    CHECK_INT(cmp("z.md", 0, 0, "a.txt", 0, 0, FSORT_KIND, 0, 1), -1, "by kind: md before txt regardless of stem");
    CHECK_INT(cmp("a.txt", 0, 0, "b.txt", 0, 0, FSORT_KIND, 0, 1), -1, "same kind tie breaks by name");
}

static void test_date_sort(void) {
    /* ascending = older first (smaller packed mtime first); desc flips */
    CHECK_INT(cmpd("old.txt", 0, 100, "new.txt", 0, 200, 0), -1, "older file first (asc)");
    CHECK_INT(cmpd("new.txt", 0, 200, "old.txt", 0, 100, 0),  1, "newer file later (asc)");
    CHECK_INT(cmpd("old.txt", 0, 100, "new.txt", 0, 200, 1),  1, "descending shows newer first");
    CHECK_INT(cmpd("a.txt", 0, 500, "b.txt", 0, 500, 0), -1, "equal mtime tie-breaks by name");
    CHECK_INT(cmpd("dir", 1, 100, "z.txt", 0, 999, 0), -1, "folders-first survives date sort");
}

static void test_no_folders_first(void) {
    /* folders_first off: a folder named 'm' sorts among files by name */
    CHECK_INT(cmp("apple.txt", 0, 0, "mfolder", 1, 0, FSORT_NAME, 0, 0), -1, "ff off: apple before mfolder");
    CHECK_INT(cmp("zebra.txt", 0, 0, "mfolder", 1, 0, FSORT_NAME, 0, 0),  1, "ff off: zebra after mfolder");
}

int main(void) {
    RUN(test_natcmp);
    RUN(test_extcmp);
    RUN(test_name_sort);
    RUN(test_size_sort);
    RUN(test_kind_sort);
    RUN(test_date_sort);
    RUN(test_no_folders_first);
    return UNIT_SUMMARY();
}
