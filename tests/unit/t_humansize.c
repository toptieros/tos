/* Unit tests for humansize.h -- the 1024-based byte formatter used by the Files
 * status bar (free space), Get Info, and the Size column. */
#include "../../user/lib/humansize.h"
#include "unit.h"

static void test_bytes(void) {
    char o[24];
    human_bytes(0, o, sizeof o);    CHECK_STR(o, "0 B", "zero bytes");
    human_bytes(1, o, sizeof o);    CHECK_STR(o, "1 B", "one byte");
    human_bytes(900, o, sizeof o);  CHECK_STR(o, "900 B", "sub-KB stays in bytes");
    human_bytes(1023, o, sizeof o); CHECK_STR(o, "1023 B", "just under 1 KB");
}

static void test_kb_mb_gb(void) {
    char o[24];
    human_bytes(1024, o, sizeof o);          CHECK_STR(o, "1.0 KB", "exactly 1 KB");
    human_bytes(1536, o, sizeof o);          CHECK_STR(o, "1.5 KB", "1.5 KB");
    human_bytes(1024u * 1024u, o, sizeof o); CHECK_STR(o, "1.0 MB", "exactly 1 MB");
    human_bytes(1887436, o, sizeof o);       CHECK_STR(o, "1.8 MB", "a tosfs-sized free figure");
    human_bytes(1024u * 1024u * 1024u, o, sizeof o); CHECK_STR(o, "1.0 GB", "exactly 1 GB (no artificial cap)");
}

static void test_truncation_is_safe(void) {
    char o[4];                       /* far too small */
    human_bytes(1887436, o, sizeof o);
    CHECK(o[3] == 0, "stays NUL-terminated under a tiny cap");
}

int main(void) {
    RUN(test_bytes);
    RUN(test_kb_mb_gb);
    RUN(test_truncation_is_safe);
    return UNIT_SUMMARY();
}
