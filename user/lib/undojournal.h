/* A small undo/redo journal for the Files app's file operations (design/files-app.md §12).
 * It is a pure, OS-free data structure -- like filesort.h / viewmem.h -- so it compiles on
 * the host and is unit-tested directly (tests/unit/t_undojournal). It only stores records
 * (an opaque op `type` + two paths + an is-dir flag) and owns the cursor arithmetic: a new
 * op truncates the redo tail, undo steps the cursor back, redo steps it forward, and a full
 * journal drops its oldest record. Files (files.cpp) decides what each `type` means and runs
 * the actual filesystem inverse/forward (rename / rmrf / copy_tree / un-trash). */
#pragma once

#define UNDO_CAP      24
#define UNDO_PATHMAX  256

struct undo_rec { int type; int isdir; char a[UNDO_PATHMAX], b[UNDO_PATHMAX]; };

/* n   = number of valid records [0, n)
 * cur = how many are currently "applied"; undo inverts rec[cur-1], redo re-applies rec[cur] */
struct undo_journal { struct undo_rec rec[UNDO_CAP]; int n, cur; };

static inline void undo_reset(struct undo_journal *j) { j->n = 0; j->cur = 0; }

static inline void undo_setp(char *dst, const char *src) {
    int i = 0; if (src) for (; src[i] && i < UNDO_PATHMAX - 1; i++) dst[i] = src[i]; dst[i] = 0;
}

/* Record a new operation: drop any redo tail (a fresh op invalidates redos), then append;
 * when the journal is full, evict the oldest so the most recent UNDO_CAP survive. */
static inline void undo_push(struct undo_journal *j, int type, const char *a, const char *b, int isdir) {
    j->n = j->cur;                                   /* a new op clears the redo tail */
    if (j->n == UNDO_CAP) {                           /* full: shift out the oldest record */
        for (int i = 1; i < UNDO_CAP; i++) j->rec[i - 1] = j->rec[i];
        j->n = UNDO_CAP - 1;
    }
    struct undo_rec *r = &j->rec[j->n];
    r->type = type; r->isdir = isdir; undo_setp(r->a, a); undo_setp(r->b, b);
    j->n++; j->cur = j->n;
}

static inline int undo_can_undo(const struct undo_journal *j) { return j->cur > 0; }
static inline int undo_can_redo(const struct undo_journal *j) { return j->cur < j->n; }

/* The record to INVERT (cursor steps back), or 0 if there is nothing to undo. */
static inline struct undo_rec *undo_take_undo(struct undo_journal *j) {
    if (j->cur <= 0) return 0;
    j->cur--; return &j->rec[j->cur];
}
/* The record to RE-APPLY (cursor steps forward), or 0 if there is nothing to redo. */
static inline struct undo_rec *undo_take_redo(struct undo_journal *j) {
    if (j->cur >= j->n) return 0;
    return &j->rec[j->cur++];
}
