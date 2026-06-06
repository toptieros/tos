/* Unit tests for the file-picker request codec (user/lib/pickreq.h) -- the pure
 * string logic under the Files-as-picker hand-off (design/file-picker.md). The
 * key=value request encode/parse and the extension-filter predicate are checked
 * here on the host in microseconds instead of by booting Files under QEMU. */
#include "unit.h"
#include "../../user/lib/pickreq.h"

static void test_encode_format(void) {
    struct pick_req r = { PICK_SAVE, "/Users/user/Documents", "untitled.txt", "txt,md", "Save Note" };
    char buf[512];
    pickreq_encode(&r, buf, sizeof buf);
    CHECK_STR(buf,
        "mode=save\n"
        "dir=/Users/user/Documents\n"
        "name=untitled.txt\n"
        "ext=txt,md\n"
        "title=Save Note\n",
        "encode emits the deterministic key=value blob");
}

static void test_roundtrip_save(void) {
    struct pick_req in = { PICK_SAVE, "/Users/user/Documents", "note.md", "md", "Save As" };
    char buf[512]; pickreq_encode(&in, buf, sizeof buf);
    struct pick_req out;
    CHECK(pickreq_parse(buf, &out), "parse reports a request present");
    CHECK_INT(out.mode, PICK_SAVE, "mode round-trips");
    CHECK_STR(out.dir, "/Users/user/Documents", "dir round-trips");
    CHECK_STR(out.name, "note.md", "name round-trips");
    CHECK_STR(out.ext, "md", "ext round-trips");
    CHECK_STR(out.title, "Save As", "title round-trips");
}

static void test_roundtrip_open_minimal(void) {
    /* open mode, empty optional fields: still a clean round-trip */
    struct pick_req in = { PICK_OPEN, "/", "", "", "" };
    char buf[512]; pickreq_encode(&in, buf, sizeof buf);
    struct pick_req out;
    CHECK(pickreq_parse(buf, &out), "minimal request parses");
    CHECK_INT(out.mode, PICK_OPEN, "open mode round-trips");
    CHECK_STR(out.dir, "/", "root dir round-trips");
    CHECK_STR(out.name, "", "empty name stays empty");
    CHECK_STR(out.ext, "", "empty ext stays empty");
}

static void test_parse_forgiving(void) {
    /* unknown keys ignored (forward-compat); missing fields default empty */
    struct pick_req out;
    CHECK(pickreq_parse("mode=open\ncaller=7\ndir=/tmp\nfuture=x\n", &out),
          "unknown keys don't break the parse");
    CHECK_INT(out.mode, PICK_OPEN, "mode read past an unknown key");
    CHECK_STR(out.dir, "/tmp", "dir read past an unknown key");
    CHECK_STR(out.name, "", "absent name defaults empty");
    /* a malformed line with no '=' is skipped */
    CHECK(pickreq_parse("garbage-no-equals\nmode=save\n", &out), "blob with junk line still parses");
    CHECK_INT(out.mode, PICK_SAVE, "mode found after a junk line");
}

static void test_parse_empty(void) {
    struct pick_req out;
    CHECK(!pickreq_parse("", &out), "empty blob => no request (0)");
    CHECK_INT(out.mode, PICK_OPEN, "empty parse leaves a safe default mode");
}

static void test_ext_match(void) {
    CHECK(pickreq_ext_match("anything", ""),        "empty filter matches everything");
    CHECK(pickreq_ext_match("notes.txt", "txt"),    "single ext matches");
    CHECK(pickreq_ext_match("readme.MD", "md"),     "match is case-insensitive on the name");
    CHECK(pickreq_ext_match("readme.md", "TXT,MD"), "match is case-insensitive on the filter");
    CHECK(pickreq_ext_match("a.md", "txt,md,png"),  "matches a later csv token");
    CHECK(pickreq_ext_match("p.png", ".png"),       "a leading dot in the filter is tolerated");
    CHECK(!pickreq_ext_match("a.txt", "md"),        "non-listed ext rejected");
    CHECK(!pickreq_ext_match("Makefile", "txt"),    "extensionless name rejected by a non-empty filter");
    CHECK(!pickreq_ext_match("a.tx", "txt"),        "prefix of a token is not a match (a.tx != txt)");
    CHECK(!pickreq_ext_match("a.txtx", "txt"),      "superstring of a token is not a match");
}

int main(void) {
    RUN(test_encode_format);
    RUN(test_roundtrip_save);
    RUN(test_roundtrip_open_minimal);
    RUN(test_parse_forgiving);
    RUN(test_parse_empty);
    RUN(test_ext_match);
    return UNIT_SUMMARY();
}
