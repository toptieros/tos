/* Unit tests for the Files details-view column layout (user/lib/colfit.h) -- the pure
 * "three fixed columns + a Date column that fills, shrink-trailing-first when narrow"
 * arithmetic (design/files-app.md §1). Checked on the host instead of by eyeballing the
 * header in a booted Files window. */
#include "unit.h"
#include "../../user/lib/colfit.h"

static void test_normal(void) {
    int cw[3] = { 230, 96, 96 };
    int x[4], w[4];
    colfit(600, cw, x, w);
    CHECK_INT(x[0], 0,   "Name starts at the pane's left edge");
    CHECK_INT(w[0], 230, "Name keeps its width");
    CHECK_INT(x[1], 230, "Kind follows Name");
    CHECK_INT(w[1], 96,  "Kind keeps its width");
    CHECK_INT(x[2], 326, "Size follows Kind");
    CHECK_INT(w[2], 96,  "Size keeps its width");
    CHECK_INT(x[3], 422, "Date follows Size");
    CHECK_INT(w[3], 178, "Date fills the remainder (600-422)");
}

static void test_min_clamp(void) {
    int cw[3] = { 4, 0, 10 };          /* absurdly small -> clamped up */
    int x[4], w[4];
    colfit(600, cw, x, w);
    CHECK_INT(w[0], COLFIT_MINW, "Name clamps up to the minimum");
    CHECK_INT(w[1], COLFIT_MINW, "Kind clamps up to the minimum");
    CHECK_INT(w[2], COLFIT_MINW, "Size clamps up to the minimum");
    CHECK_INT(x[3], 3 * COLFIT_MINW, "Date offset is past the three minimum columns");
}

static void test_narrow_shrinks_trailing_first(void) {
    int cw[3] = { 230, 96, 96 };
    int x[4], w[4];
    colfit(300, cw, x, w);             /* not enough room for 230+96+96 + Date */
    /* Name should be the LAST to give up pixels (Size shrinks first, then Kind) */
    CHECK(w[2] <= 96, "Size shrank");
    CHECK(w[2] <= w[1], "Size shrank at least as much as Kind (trailing-first)");
    CHECK(w[0] >= w[1], "Name is no narrower than Kind");
    CHECK(w[3] >= 0,  "Date width never goes negative");
    CHECK_INT(x[1], w[0], "Kind offset tracks the (possibly shrunk) Name width");
    CHECK_INT(x[3], w[0] + w[1] + w[2], "Date offset is the sum of the three fixed columns");
}

static void test_everything_at_floor(void) {
    int cw[3] = { 56, 56, 56 };
    int x[4], w[4];
    colfit(120, cw, x, w);             /* can't even fit three minimums + Date */
    CHECK_INT(w[0], COLFIT_MINW, "Name stays at the floor");
    CHECK_INT(w[1], COLFIT_MINW, "Kind stays at the floor");
    CHECK_INT(w[2], COLFIT_MINW, "Size stays at the floor (loop is bounded)");
    CHECK(w[3] >= 0, "Date width is floored at 0, never negative");
}

int main(void) {
    RUN(test_normal);
    RUN(test_min_clamp);
    RUN(test_narrow_shrinks_trailing_first);
    RUN(test_everything_at_floor);
    return UNIT_SUMMARY();
}
