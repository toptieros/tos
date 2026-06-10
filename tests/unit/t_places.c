/* Unit tests for the Places sidebar model (user/lib/places.h) -- the codec + list
 * ops behind the editable Favorites section (design/files-app.md §7): registry
 * round-trip ("Label|/path"), default labels from a path, dedupe on add, and
 * removal. Checked on the host instead of by pinning folders in a booted VM. */
#include "unit.h"
#include "../../user/lib/places.h"
#include <string.h>

static void test_codec_roundtrip(void) {
    struct place p, q;
    memset(&p, 0, sizeof p); memset(&q, 0, sizeof q);
    strcpy(p.label, "proj"); strcpy(p.path, "/Users/user/dev/proj");
    char buf[96];
    int len = place_encode(&p, buf, sizeof buf);
    CHECK_STR(buf, "proj|/Users/user/dev/proj", "encode joins label|path");
    CHECK_INT(len, (int)strlen(buf), "encode returns the length");
    CHECK_INT(place_decode(buf, &q), 0, "decode accepts its own encoding");
    CHECK_STR(q.label, "proj", "label round-trips");
    CHECK_STR(q.path, "/Users/user/dev/proj", "path round-trips");
}

static void test_decode_rejects_malformed(void) {
    struct place p;
    CHECK_INT(place_decode("nopipe", &p), -1, "a value without '|' is rejected");
    CHECK_INT(place_decode("|/path", &p), -1, "an empty label is rejected");
    CHECK_INT(place_decode("label|", &p), -1, "an empty path is rejected");
    CHECK_INT(place_decode("a|/b", &p), 0, "the minimal valid value is accepted");
}

static void test_decode_clamps(void) {
    /* an oversized label/path is clamped, not overflowed */
    char big[256]; int pos = 0;
    for (int i = 0; i < 60; i++) big[pos++] = 'L';
    big[pos++] = '|';
    big[pos++] = '/';
    for (int i = 0; i < 200; i++) big[pos++] = 'p';
    big[pos] = 0;
    struct place p;
    CHECK_INT(place_decode(big, &p), 0, "an oversized value still decodes");
    CHECK_INT((int)strlen(p.label), PLACE_LABELMAX - 1, "label clamped to its cap");
    CHECK_INT((int)strlen(p.path), PLACE_PATHMAX - 1, "path clamped to its cap");
}

static void test_label_from_path(void) {
    char out[PLACE_LABELMAX];
    place_label_from("/Users/user/dev/proj", out, sizeof out);
    CHECK_STR(out, "proj", "label is the last path segment");
    place_label_from("/Apps", out, sizeof out);
    CHECK_STR(out, "Apps", "a single-segment path keeps its name");
    place_label_from("/", out, sizeof out);
    CHECK_STR(out, "/", "the root labels as a slash");
    place_label_from("/Users/user/dev/", out, sizeof out);
    CHECK_STR(out, "dev", "a trailing slash is ignored");
}

static void test_add_dedupes_and_removes(void) {
    struct place a[4]; int n = 0;
    n = places_add(a, n, 4, "Home", "/Users/user");
    n = places_add(a, n, 4, "dev", "/Users/user/dev");
    CHECK_INT(n, 2, "two distinct paths pinned");
    n = places_add(a, n, 4, "again", "/Users/user/dev");
    CHECK_INT(n, 2, "a duplicate path is not pinned twice");
    CHECK_INT(places_find(a, n, "/Users/user/dev"), 1, "find locates a pinned path");
    CHECK_INT(places_find(a, n, "/elsewhere"), -1, "find misses an unpinned path");
    n = places_add(a, n, 4, "c", "/c");
    n = places_add(a, n, 4, "d", "/d");
    n = places_add(a, n, 4, "e", "/e");
    CHECK_INT(n, 4, "a full array refuses further adds");
    n = places_remove(a, n, 0);
    CHECK_INT(n, 3, "remove shrinks the list");
    CHECK_STR(a[0].path, "/Users/user/dev", "the tail shifted down");
    n = places_remove(a, n, 7);
    CHECK_INT(n, 3, "an out-of-range remove is a no-op");
}

int main(void) {
    RUN(test_codec_roundtrip);
    RUN(test_decode_rejects_malformed);
    RUN(test_decode_clamps);
    RUN(test_label_from_path);
    RUN(test_add_dedupes_and_removes);
    return UNIT_SUMMARY();
}
