/* Unit tests for the FileView selection-set algebra (user/lib/filesel.h) -- the pure
 * logic behind multi-select in the Files app and the desktop: click replaces,
 * Ctrl-click toggles, Shift-click range-extends from the anchor, marquee bands,
 * Select-All, and arrow/Shift-arrow cursor moves (design/files-and-desktop.md). The
 * on-screen wiring is screenshot-verified in a boot; this pins the set algebra. */
#include "unit.h"
#include "../../user/lib/filesel.h"

/* count selected rows the long way + confirm fsel_count agrees */
static int sel_eq(struct filesel *s, const char *pattern) {   /* pattern: '1'/'0' per row */
    for (int i = 0; pattern[i]; i++)
        if ((s->mark[i] != 0) != (pattern[i] == '1')) return 0;
    return 1;
}

static void test_init(void) {
    struct filesel s; fsel_init(&s, 10);
    CHECK_INT(s.n, 10, "row count stored");
    CHECK_INT(fsel_count(&s), 0, "starts empty");
    CHECK_INT(s.anchor, -1, "no anchor");
    CHECK_INT(s.cursor, -1, "no cursor");
    fsel_init(&s, 9999);
    CHECK_INT(s.n, FILESEL_MAX, "row count clamped to capacity");
}

static void test_click(void) {
    struct filesel s; fsel_init(&s, 6);
    fsel_click(&s, 3);
    CHECK(sel_eq(&s, "000100"), "click selects exactly that row");
    CHECK_INT(fsel_count(&s), 1, "one selected");
    CHECK_INT(s.anchor, 3, "anchor at click");
    CHECK_INT(s.cursor, 3, "cursor at click");
    fsel_click(&s, 1);
    CHECK(sel_eq(&s, "010000"), "a second click replaces the selection");
    CHECK_INT(fsel_count(&s), 1, "still one");
}

static void test_click_empty_clears(void) {
    struct filesel s; fsel_init(&s, 4);
    fsel_click(&s, 2);
    fsel_click(&s, 9);                       /* below the last row = empty space */
    CHECK_INT(fsel_count(&s), 0, "click on empty space clears");
    CHECK_INT(s.anchor, -1, "anchor cleared");
}

static void test_toggle(void) {
    struct filesel s; fsel_init(&s, 6);
    fsel_click(&s, 1);
    fsel_toggle(&s, 3);
    fsel_toggle(&s, 4);
    CHECK(sel_eq(&s, "010110"), "Ctrl-click adds without clearing");
    CHECK_INT(fsel_count(&s), 3, "three selected");
    CHECK_INT(s.anchor, 4, "anchor follows the last Ctrl-click");
    fsel_toggle(&s, 3);
    CHECK(sel_eq(&s, "010010"), "Ctrl-click again removes it");
    CHECK_INT(fsel_count(&s), 2, "two left");
}

static void test_shift_range(void) {
    struct filesel s; fsel_init(&s, 8);
    fsel_click(&s, 2);                       /* anchor = 2 */
    fsel_range(&s, 5, 0);
    CHECK(sel_eq(&s, "00111100"), "Shift-click selects anchor..i inclusive");
    CHECK_INT(s.anchor, 2, "anchor stays put");
    CHECK_INT(s.cursor, 5, "cursor moves to the click");
    fsel_range(&s, 0, 0);                    /* re-pivot from the same anchor, downward */
    CHECK(sel_eq(&s, "11100000"), "a second Shift-click re-ranges from the same anchor (replacing)");
}

static void test_shift_range_additive(void) {
    struct filesel s; fsel_init(&s, 8);
    fsel_click(&s, 0);
    fsel_toggle(&s, 6);                      /* anchor now 6 */
    fsel_range(&s, 4, 1);                    /* Ctrl+Shift: add 4..6 to the set */
    CHECK(sel_eq(&s, "10001110"), "additive Shift-range unions rows 4..6, keeps row 0");
}

static void test_range_no_anchor(void) {
    struct filesel s; fsel_init(&s, 5);
    fsel_range(&s, 3, 0);                    /* Shift-click with no prior anchor */
    CHECK(sel_eq(&s, "00010"), "with no anchor, i becomes the anchor (single row)");
    CHECK_INT(s.anchor, 3, "anchor set to i");
}

static void test_select_all(void) {
    struct filesel s; fsel_init(&s, 5);
    fsel_click(&s, 2);
    fsel_all(&s);
    CHECK(sel_eq(&s, "11111"), "Ctrl+A selects every row");
    CHECK_INT(fsel_count(&s), 5, "all five");
    CHECK_INT(s.anchor, 0, "anchor to top");
    CHECK_INT(s.cursor, 4, "cursor to bottom");
}

static void test_cursor_move(void) {
    struct filesel s; fsel_init(&s, 6);
    fsel_click(&s, 1);
    fsel_move(&s, 2, 0);                     /* plain arrow down */
    CHECK(sel_eq(&s, "001000"), "plain arrow replaces with the new row");
    CHECK_INT(s.anchor, 2, "arrow re-anchors");
    fsel_move(&s, 4, 1);                     /* Shift+arrow extends from anchor 2 */
    CHECK(sel_eq(&s, "001110"), "Shift+arrow extends the range from the anchor");
    CHECK_INT(s.cursor, 4, "cursor at the new edge");
    CHECK_INT(s.anchor, 2, "anchor unchanged by Shift+arrow");
}

static void test_marquee(void) {
    struct filesel s; fsel_init(&s, 8);
    fsel_marquee(&s, 2, 4, 0);
    CHECK(sel_eq(&s, "00111000"), "marquee selects the touched row band");
    fsel_marquee(&s, 5, 6, 0);
    CHECK(sel_eq(&s, "00000110"), "a fresh marquee replaces");
    fsel_click(&s, 0);
    fsel_marquee(&s, 5, 6, 1);               /* additive (modifier held during drag) */
    CHECK(sel_eq(&s, "10000110"), "additive marquee unions with the existing selection");
    fsel_marquee(&s, -1, -1, 0);             /* band over empty space */
    CHECK_INT(fsel_count(&s), 0, "an empty marquee clears");
}

static void test_band_clamps(void) {
    struct filesel s; fsel_init(&s, 4);
    fsel_band(&s, -3, 99, 1);                /* way out of range either side */
    CHECK(sel_eq(&s, "1111"), "band clamps to [0,n) without overrunning");
    CHECK_INT(fsel_count(&s), 4, "all in-range rows set, no OOB write");
}

static void test_single(void) {
    struct filesel s; fsel_init(&s, 5);
    CHECK_INT(fsel_single(&s), -1, "empty -> no single row");
    fsel_click(&s, 3);
    CHECK_INT(fsel_single(&s), 3, "exactly one -> that row");
    fsel_toggle(&s, 1);
    CHECK_INT(fsel_single(&s), -1, "two selected -> not single");
}

int main(void) {
    RUN(test_init);
    RUN(test_click);
    RUN(test_click_empty_clears);
    RUN(test_toggle);
    RUN(test_shift_range);
    RUN(test_shift_range_additive);
    RUN(test_range_no_anchor);
    RUN(test_select_all);
    RUN(test_cursor_move);
    RUN(test_marquee);
    RUN(test_band_clamps);
    RUN(test_single);
    return UNIT_SUMMARY();
}
