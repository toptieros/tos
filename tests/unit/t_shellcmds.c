/* Unit tests for the pure shell helpers in user/lib/shellcmds.h -- the built-in
 * catalog lookup, first-token extraction, prefix matching and common-prefix math
 * behind the shell's Tab completion + first-token syntax highlighting (design/shell.md).
 * The redraw/IO lives in shell.c and is screenshot-verified; this is the logic. */
#include "unit.h"
#include "../../user/lib/shellcmds.h"

static void test_is_builtin(void) {
    CHECK(sh_is_builtin("ls"),       "ls is a built-in");
    CHECK(sh_is_builtin("selftest"), "selftest is a built-in");
    CHECK(!sh_is_builtin("l"),       "a prefix is not the whole name");
    CHECK(!sh_is_builtin("lsx"),     "a superstring is not a name");
    CHECK(!sh_is_builtin(""),        "the empty string is not a command");
    CHECK(!sh_is_builtin("nope"),    "an unknown word is not a built-in");
}

static void test_first_token(void) {
    char t[32];
    CHECK_INT(sh_first_token("ls /tmp", t, sizeof t), 2, "len of 'ls'");
    CHECK_STR(t, "ls", "first token is 'ls'");
    CHECK_INT(sh_first_token("   cd  /x", t, sizeof t), 2, "leading spaces skipped");
    CHECK_STR(t, "cd", "first token after spaces is 'cd'");
    CHECK_INT(sh_first_token("", t, sizeof t), 0, "empty line has no token");
    CHECK_STR(t, "", "empty token");
    sh_first_token("verylongtokenthatistruncated", t, 5);
    CHECK_STR(t, "very", "token is capped to cap-1 bytes");
}

static void test_prefix_and_common(void) {
    CHECK(sh_has_prefix("selftest", "self"),  "selftest starts with self");
    CHECK(sh_has_prefix("ls", ""),            "empty prefix matches");
    CHECK(!sh_has_prefix("ls", "lsx"),        "prefix longer than name fails");
    CHECK_INT(sh_common_len("reboot", "rebase"), 3, "common prefix of reboot/rebase is 'reb'");
    CHECK_INT(sh_common_len("cd", "cp"), 1, "common prefix of cd/cp is 'c'");
    CHECK_INT(sh_common_len("ls", "df"), 0, "no common prefix");
}

static void test_in_first_token(void) {
    CHECK(sh_in_first_token("ls", 2),       "cursor still in the command word");
    CHECK(!sh_in_first_token("ls /tmp", 4), "cursor past a space -> an argument");
    CHECK(sh_in_first_token("ls", 0),       "cursor at start is in the first token");
}

/* The catalog stays sorted so the Tab pager reads tidily. */
static void test_catalog_sorted(void) {
    int sorted = 1;
    for (int i = 1; i < SH_NCMDS; i++) {
        const char *a = SH_CMDS[i - 1].name, *b = SH_CMDS[i].name;
        while (*a && *a == *b) { a++; b++; }
        if ((unsigned char)*a > (unsigned char)*b) sorted = 0;
    }
    CHECK(sorted, "SH_CMDS is in ascending name order");
    CHECK(SH_NCMDS > 20, "the catalog has the expected commands");
}

int main(void) {
    RUN(test_is_builtin);
    RUN(test_first_token);
    RUN(test_prefix_and_common);
    RUN(test_in_first_token);
    RUN(test_catalog_sorted);
    return UNIT_SUMMARY();
}
