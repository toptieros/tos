/* Unit tests for argv_split (user/lib/argv.h) -- the pure in-place tokenizer behind
 * getargs(), which carves a task's kernel-seeded command line into argv[]. The data-
 * page read + the kernel path/arg split are exercised by the in-OS `args` program and
 * a boot screenshot; this pins the tokenization logic. (design/shell.md, band 2.) */
#include "unit.h"
#include "../../user/lib/argv.h"

static void test_basic(void) {
    char buf[64]; char *av[8];
    const char *src = "ls /tmp foo";
    int i = 0; for (; src[i]; i++) buf[i] = src[i];
    buf[i] = 0;
    int argc = argv_split(buf, av, 8);
    CHECK_INT(argc, 3, "three tokens");
    CHECK_STR(av[0], "ls", "argv[0] is the path");
    CHECK_STR(av[1], "/tmp", "argv[1]");
    CHECK_STR(av[2], "foo", "argv[2]");
}

static void test_whitespace(void) {
    char buf[64]; char *av[8];
    const char *src = "   a\tb   c  ";
    int i = 0; for (; src[i]; i++) buf[i] = src[i]; buf[i] = 0;
    int argc = argv_split(buf, av, 8);
    CHECK_INT(argc, 3, "leading/repeated/trailing whitespace collapsed");
    CHECK_STR(av[0], "a", "first token");
    CHECK_STR(av[1], "b", "tab is a separator too");
    CHECK_STR(av[2], "c", "third token");
}

static void test_edges(void) {
    char *av[8];
    char empty[1] = {0};
    CHECK_INT(argv_split(empty, av, 8), 0, "empty line -> argc 0");
    char blank[5] = {' ', ' ', '\t', ' ', 0};
    CHECK_INT(argv_split(blank, av, 8), 0, "all-whitespace -> argc 0");
    char one[8]; for (int i = 0; "solo"[i]; i++) one[i] = "solo"[i]; one[4] = 0;
    CHECK_INT(argv_split(one, av, 8), 1, "single token -> argc 1");
    CHECK_STR(av[0], "solo", "single token value");
}

static void test_maxv_cap(void) {
    char buf[64]; char *av[2];
    const char *src = "a b c d";
    int i = 0; for (; src[i]; i++) buf[i] = src[i]; buf[i] = 0;
    int argc = argv_split(buf, av, 2);                /* cap at 2 */
    CHECK_INT(argc, 2, "argc is capped to maxv");
    CHECK_STR(av[0], "a", "capped argv[0]");
    CHECK_STR(av[1], "b", "capped argv[1]");
}

int main(void) {
    RUN(test_basic);
    RUN(test_whitespace);
    RUN(test_edges);
    RUN(test_maxv_cap);
    return UNIT_SUMMARY();
}
