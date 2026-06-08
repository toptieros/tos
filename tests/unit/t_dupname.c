/* Unit tests for dupname.h -- the pure "Duplicate" name builder (files-app §12).
 * The recursive byte/dir copy itself is FS-integration (covered by the e2e
 * t_files_newdup); here we pin the naming edge cases that are easy to get wrong:
 * extension placement, the k>=2 numbering, dot-less folders, multi-dot names. */
#include "../../user/lib/dupname.h"
#include "unit.h"

static void test_first_copy(void) {
    char out[256];
    dup_candidate(out, sizeof out, "/Users/user/note.txt", 1);
    CHECK_STR(out, "/Users/user/note copy.txt", "k=1 inserts ' copy' before the extension");
    dup_candidate(out, sizeof out, "/Users/user/box", 1);
    CHECK_STR(out, "/Users/user/box copy", "a dot-less name (folder) gets no extension");
    dup_candidate(out, sizeof out, "report.md", 1);
    CHECK_STR(out, "report copy.md", "a bare basename (no directory) still works");
}

static void test_numbered(void) {
    char out[256];
    dup_candidate(out, sizeof out, "/Users/user/note.txt", 2);
    CHECK_STR(out, "/Users/user/note copy 2.txt", "k=2 is ' copy 2'");
    dup_candidate(out, sizeof out, "/Users/user/note.txt", 3);
    CHECK_STR(out, "/Users/user/note copy 3.txt", "k=3 is ' copy 3'");
    dup_candidate(out, sizeof out, "/Users/user/box", 5);
    CHECK_STR(out, "/Users/user/box copy 5", "numbered duplicate of a dot-less name");
    dup_candidate(out, sizeof out, "/a/note.txt", 12);
    CHECK_STR(out, "/a/note copy 12.txt", "multi-digit k");
}

static void test_extensions(void) {
    char out[256];
    dup_candidate(out, sizeof out, "/x/a.tar.gz", 1);
    CHECK_STR(out, "/x/a.tar copy.gz", "only the LAST dot is the extension");
    dup_candidate(out, sizeof out, "/x/archive.", 1);
    CHECK_STR(out, "/x/archive copy.", "a trailing dot is an empty extension");
    dup_candidate(out, sizeof out, "/photos/IMG.PNG", 1);
    CHECK_STR(out, "/photos/IMG copy.PNG", "case is preserved verbatim");
}

static void test_bounds(void) {
    char out[16];
    /* cap forces truncation; the result must stay NUL-terminated within out[16] */
    dup_candidate(out, sizeof out, "/Users/user/longname.txt", 1);
    CHECK_INT((int)strlen(out), 15, "output is truncated to cap-1");
    CHECK(out[15] == 0, "output stays NUL-terminated under truncation");
}

int main(void) {
    RUN(test_first_copy);
    RUN(test_numbered);
    RUN(test_extensions);
    RUN(test_bounds);
    return UNIT_SUMMARY();
}
