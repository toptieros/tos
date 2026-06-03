#pragma once
/* editlog.h -- the pure coalescing rule behind the toolkit's text undo/redo.
 *
 * A TextField records every edit as an insert/delete span; a run of single-char
 * typing or backspacing should collapse into ONE undo step (press Ctrl+Z once to
 * drop the whole word, not letter by letter). Deciding whether a new single-char
 * edit merges into the previous record -- and which end it joins -- is the subtle
 * part (contiguity, direction, newline boundaries, op match), so it lives here as
 * a stateless function the widget calls and the host unit tests pin down.
 *
 * Ops: EL_INS = the span was inserted, EL_DEL = it was deleted. */
enum { EL_INS = 0, EL_DEL = 1 };
enum { EL_NONE = 0, EL_APPEND = 1, EL_PREPEND = 2 };  /* how the new char joins the top record */

/* Given the most recent (top) undo record -- op `top_op`, spanning [tpos, tpos+tn)
 * with first/last chars `tfirst`/`tlast` -- and a NEW single-char edit (op `nop`,
 * char `nc` at position `npos`), return how they coalesce:
 *   EL_APPEND  -> grow the record on its right end (typing forward, forward-Delete)
 *   EL_PREPEND -> grow it on its left end, record start moves to npos (Backspace run)
 *   EL_NONE    -> start a fresh undo step.
 * Newlines never coalesce, so each line is its own step; a caret jump/click is
 * handled by the caller (it just doesn't call this). */
static inline int el_coalesce_kind(int top_op, int tpos, int tn, char tfirst, char tlast,
                                   int nop, int npos, char nc) {
    if (tn <= 0 || nc == '\n' || tfirst == '\n' || tlast == '\n') return EL_NONE;
    if (top_op != nop) return EL_NONE;
    if (nop == EL_INS && tpos + tn == npos) return EL_APPEND;   /* contiguous insert run   */
    if (nop == EL_DEL && tpos == npos + 1)  return EL_PREPEND;  /* backspace run (deletes left) */
    if (nop == EL_DEL && tpos == npos)      return EL_APPEND;   /* forward-delete run      */
    return EL_NONE;
}
