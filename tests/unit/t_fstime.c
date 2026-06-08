/* Unit tests for fstime.h -- the packed file-mtime codec (files-app §8). The kernel
 * packs the CMOS time on write and the Files app unpacks it for the "Modified" line,
 * so a wrong field width or shift would silently corrupt every timestamp. */
#include "../../kernel/fstime.h"
#include "unit.h"

static void test_roundtrip(void) {
    int y, mo, d, h, mi;
    uint32_t p = fstime_pack(2026, 6, 8, 19, 55);
    fstime_unpack(p, &y, &mo, &d, &h, &mi);
    CHECK_INT(y, 2026, "year survives the round-trip");
    CHECK_INT(mo, 6,   "month survives the round-trip");
    CHECK_INT(d, 8,    "day survives the round-trip");
    CHECK_INT(h, 19,   "hour survives the round-trip");
    CHECK_INT(mi, 55,  "minute survives the round-trip");
}

static void test_field_independence(void) {
    /* the max value of each field must not bleed into its neighbour */
    int y, mo, d, h, mi;
    fstime_unpack(fstime_pack(4095, 12, 31, 23, 59), &y, &mo, &d, &h, &mi);
    CHECK_INT(y, 4095, "max year");
    CHECK_INT(mo, 12,  "max month");
    CHECK_INT(d, 31,   "max day");
    CHECK_INT(h, 23,   "max hour");
    CHECK_INT(mi, 59,  "max minute");
}

static void test_zero_is_unknown(void) {
    int y, mo, d, h, mi;
    fstime_unpack(0, &y, &mo, &d, &h, &mi);
    CHECK_INT(y, 0, "a zero packed value unpacks to year 0 (= unknown)");
    CHECK_INT(mo, 0, "zero month");
    CHECK_INT(mi, 0, "zero minute");
}

static void test_null_outs(void) {
    /* unpack must tolerate NULL out-pointers (callers that want only some fields) */
    int mi = -1;
    fstime_unpack(fstime_pack(2000, 1, 1, 0, 42), 0, 0, 0, 0, &mi);
    CHECK_INT(mi, 42, "selective unpack with NULL fields");
}

int main(void) {
    RUN(test_roundtrip);
    RUN(test_field_independence);
    RUN(test_zero_is_unknown);
    RUN(test_null_outs);
    return UNIT_SUMMARY();
}
