/* Unit tests for the Files undo/redo journal (user/lib/undojournal.h) -- the pure cursor
 * arithmetic behind Ctrl+Z / Ctrl+Y (design/files-app.md §12): push clears the redo tail,
 * undo/redo walk the cursor, and a full journal evicts its oldest record. Checked on the
 * host instead of by driving a booted Files window through 24 operations. */
#include "unit.h"
#include "../../user/lib/undojournal.h"
#include <string.h>

static void test_basic_walk(void) {
    struct undo_journal j; undo_reset(&j);
    CHECK(!undo_can_undo(&j), "empty journal cannot undo");
    CHECK(!undo_can_redo(&j), "empty journal cannot redo");
    undo_push(&j, 1, "/a", "/A", 0);
    undo_push(&j, 1, "/b", "/B", 0);
    undo_push(&j, 1, "/c", "/C", 1);
    CHECK_INT(j.n, 3,   "three records stored");
    CHECK_INT(j.cur, 3, "all three applied");
    CHECK(undo_can_undo(&j), "can undo after pushes");
    CHECK(!undo_can_redo(&j), "nothing to redo yet");

    struct undo_rec *r = undo_take_undo(&j);
    CHECK_STR(r->b, "/C", "undo returns the most recent record");
    CHECK_INT(r->isdir, 1, "record carries the is-dir flag");
    CHECK_INT(j.cur, 2, "cursor stepped back");
    CHECK(undo_can_redo(&j), "can redo after an undo");

    r = undo_take_undo(&j);
    CHECK_STR(r->b, "/B", "second undo returns the next-most-recent");
    CHECK_INT(j.cur, 1, "cursor stepped back again");

    r = undo_take_redo(&j);
    CHECK_STR(r->b, "/B", "redo re-applies the last-undone record");
    CHECK_INT(j.cur, 2, "redo stepped the cursor forward");
}

static void test_push_truncates_redo(void) {
    struct undo_journal j; undo_reset(&j);
    undo_push(&j, 0, "/a", "/A", 0);
    undo_push(&j, 0, "/b", "/B", 0);
    undo_take_undo(&j);                       /* cur = 1, one record in the redo tail */
    CHECK(undo_can_redo(&j), "redo available before a new push");
    undo_push(&j, 0, "/c", "/C", 0);          /* a new op must drop the redo tail */
    CHECK(!undo_can_redo(&j), "a fresh op clears the redo tail");
    CHECK_INT(j.n, 2,   "the redone-away record was overwritten (n back to 2)");
    CHECK_INT(j.cur, 2, "cursor at the new top");
    struct undo_rec *r = undo_take_undo(&j);
    CHECK_STR(r->b, "/C", "the new op sits where the redo tail was");
}

static void test_capacity_evicts_oldest(void) {
    struct undo_journal j; undo_reset(&j);
    char p[32];
    for (int i = 0; i < UNDO_CAP + 5; i++) { snprintf(p, sizeof p, "/p%d", i); undo_push(&j, 0, p, p, 0); }
    CHECK_INT(j.n, UNDO_CAP, "journal never exceeds its capacity");
    CHECK_INT(j.cur, UNDO_CAP, "cursor pinned at the top after overflow");
    /* the most recent push must still be at the top */
    struct undo_rec *r = undo_take_undo(&j);
    snprintf(p, sizeof p, "/p%d", UNDO_CAP + 4);
    CHECK_STR(r->b, p, "the newest record survives eviction");
    /* walk all the way back: the oldest survivor is p5 (p0..p4 were evicted) */
    while (undo_can_undo(&j)) r = undo_take_undo(&j);
    CHECK_STR(r->b, "/p5", "the oldest five records were evicted");
}

int main(void) {
    RUN(test_basic_walk);
    RUN(test_push_truncates_redo);
    RUN(test_capacity_evicts_oldest);
    return UNIT_SUMMARY();
}
