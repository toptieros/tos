/* Unit tests for el_coalesce_kind() in user/lib/editlog.h -- the rule that decides
 * whether a new single-char text edit folds into the previous undo step (so a typed
 * word is one Ctrl+Z, not one per letter) and which end it joins. These branches
 * (insert contiguity, backspace vs. forward-delete direction, newline boundaries,
 * op match) used to be exercisable only by booting notepad; here they're instant. */
#include "unit.h"
#include "../../user/lib/editlog.h"

/* A typed run "abc" sits as one INS record spanning [0,3): first 'a', last 'c'. */
static void test_insert_run(void) {
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 3, 'a', 'c', EL_INS, 3, 'd'), EL_APPEND,
              "typing the next char at the run's end appends");
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 1, 'a', 'a', EL_INS, 1, 'b'), EL_APPEND,
              "second char of a two-char run still appends");
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 3, 'a', 'c', EL_INS, 1, 'x'), EL_NONE,
              "inserting in the middle (non-contiguous) starts a new step");
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 3, 'a', 'c', EL_INS, 5, 'y'), EL_NONE,
              "inserting past the run's end is non-contiguous");
}

static void test_newline_breaks(void) {
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 3, 'a', 'c', EL_INS, 3, '\n'), EL_NONE,
              "typing Enter is its own undo step");
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 3, 'a', '\n', EL_INS, 3, 'd'), EL_NONE,
              "a run that ended in a newline does not absorb the next char");
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 3, '\n', 'c', EL_INS, 3, 'd'), EL_NONE,
              "a run that began with a newline does not coalesce");
}

static void test_op_and_empty(void) {
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 3, 'a', 'c', EL_DEL, 3, 'd'), EL_NONE,
              "an insert run never coalesces a delete (op mismatch)");
    CHECK_INT(el_coalesce_kind(EL_DEL, 3, 1, 'a', 'a', EL_INS, 3, 'd'), EL_NONE,
              "a delete run never coalesces an insert (op mismatch)");
    CHECK_INT(el_coalesce_kind(EL_INS, 0, 0, 0, 0, EL_INS, 0, 'a'), EL_NONE,
              "an empty top record can't be coalesced into");
}

/* Backspace deletes the char to the LEFT, so each new delete sits just before the
 * record's start (tpos == npos+1) and prepends, growing leftward. */
static void test_backspace_run(void) {
    CHECK_INT(el_coalesce_kind(EL_DEL, 4, 1, 'e', 'e', EL_DEL, 3, 'l'), EL_PREPEND,
              "the second backspace prepends to the deleted span");
    CHECK_INT(el_coalesce_kind(EL_DEL, 3, 2, 'l', 'e', EL_DEL, 2, 'p'), EL_PREPEND,
              "a longer backspace run keeps prepending");
    CHECK_INT(el_coalesce_kind(EL_DEL, 4, 1, 'e', 'e', EL_DEL, 1, 'x'), EL_NONE,
              "a backspace away from the span is a new step");
}

/* Forward Delete removes the char at the caret without moving it, so each new
 * delete starts at the same position (tpos == npos) and appends, growing right. */
static void test_forward_delete_run(void) {
    CHECK_INT(el_coalesce_kind(EL_DEL, 3, 1, 'a', 'a', EL_DEL, 3, 'b'), EL_APPEND,
              "the second forward-delete appends to the span");
    CHECK_INT(el_coalesce_kind(EL_DEL, 3, 2, 'a', 'b', EL_DEL, 3, 'c'), EL_APPEND,
              "a longer forward-delete run keeps appending");
    CHECK_INT(el_coalesce_kind(EL_DEL, 3, 1, 'a', 'a', EL_DEL, 5, 'z'), EL_NONE,
              "a forward-delete away from the span is a new step");
}

int main(void) {
    RUN(test_insert_run);
    RUN(test_newline_breaks);
    RUN(test_op_and_empty);
    RUN(test_backspace_run);
    RUN(test_forward_delete_run);
    return UNIT_SUMMARY();
}
