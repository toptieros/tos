/* Unit tests for the Files per-folder view memory codec (user/lib/viewmem.h) -- the
 * pure logic behind remembering each folder's view mode/sort/zoom in the registry
 * (design/files-app.md §2). Encode/decode round-trips and the path->key derivation
 * (literal vs. hashed) are checked on the host instead of by booting Files. */
#include "unit.h"
#include "../../user/lib/viewmem.h"
#include <string.h>

static void test_defaults(void) {
    struct view_prefs d = viewmem_defaults();
    CHECK_INT(d.mode, 0,      "default view mode is list");
    CHECK_INT(d.sort_key, 0,  "default sort is by name");
    CHECK_INT(d.sort_desc, 0, "default direction is ascending");
    CHECK_INT(d.sort_ff, 1,   "default folders-first is on");
    CHECK_INT(d.zoom, 1,      "default zoom is actual size");
    /* an empty/garbage value decodes to the defaults */
    struct view_prefs e = viewmem_decode("");
    CHECK_INT(e.mode, 0,    "empty string -> default mode");
    CHECK_INT(e.sort_ff, 1, "empty string -> default folders-first");
    struct view_prefs n = viewmem_decode(0);
    CHECK_INT(n.zoom, 1,    "NULL -> default zoom");
}

static void test_roundtrip(void) {
    struct view_prefs in = { 1 /*icons*/, 2 /*size*/, 1 /*desc*/, 0 /*ff off*/, 2 /*zoomed*/ };
    char buf[32];
    viewmem_encode(&in, buf, sizeof buf);
    CHECK_STR(buf, "1;2;1;0;2", "encode packs the five fields");
    struct view_prefs out = viewmem_decode(buf);
    CHECK_INT(out.mode, 1,      "round-trip mode");
    CHECK_INT(out.sort_key, 2,  "round-trip sort key");
    CHECK_INT(out.sort_desc, 1, "round-trip direction");
    CHECK_INT(out.sort_ff, 0,   "round-trip folders-first");
    CHECK_INT(out.zoom, 2,      "round-trip zoom");
}

static void test_partial(void) {
    /* a truncated value keeps defaults for the missing trailing fields */
    struct view_prefs p = viewmem_decode("1");
    CHECK_INT(p.mode, 1,    "leading field parsed");
    CHECK_INT(p.sort_ff, 1, "omitted folders-first keeps its default (on)");
}

static void test_key_literal(void) {
    char k[VIEWMEM_KEYMAX];
    viewmem_key("/Users/user", k, sizeof k);
    CHECK_STR(k, "view./Users/user", "short path embeds literally");
    viewmem_key("/", k, sizeof k);
    CHECK_STR(k, "view./", "root path embeds literally");
}

static void test_key_hashed(void) {
    /* a path that would overflow REG_KEYMAX collapses to a stable "view~<hash8>" */
    char deep[200];
    memset(deep, 'a', sizeof deep); deep[0] = '/';
    deep[120] = 0;                              /* 120 chars: "view." + 120 > 64 */
    char k[VIEWMEM_KEYMAX];
    viewmem_key(deep, k, sizeof k);
    CHECK(strncmp(k, "view~", 5) == 0, "long path uses the hashed key form");
    CHECK_INT((int)strlen(k), 13, "hashed key is view~ + 8 hex");
    char k2[VIEWMEM_KEYMAX];
    viewmem_key(deep, k2, sizeof k2);
    CHECK_STR(k, k2, "hashing is deterministic");
    /* a different deep path hashes to a different key */
    deep[60] = 'b';
    char k3[VIEWMEM_KEYMAX];
    viewmem_key(deep, k3, sizeof k3);
    CHECK(strcmp(k, k3) != 0, "distinct long paths get distinct keys");
}

int main(void) {
    RUN(test_defaults);
    RUN(test_roundtrip);
    RUN(test_partial);
    RUN(test_key_literal);
    RUN(test_key_hashed);
    return UNIT_SUMMARY();
}
