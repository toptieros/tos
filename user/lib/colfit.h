/* Pure column layout for the Files details view (design/files-app.md §1). The header
 * has four columns -- Name, Kind, Size, Date Modified. Name/Kind/Size carry explicit,
 * user-resizable widths; the Date column fills whatever is left. This file owns the one
 * piece of real arithmetic: clamp the three fixed widths to a sane minimum and, when the
 * pane is too narrow to also leave the Date column a minimum, shrink the trailing fixed
 * columns first. It has no OS deps -- like filesort.h / viewmem.h -- so it compiles on the
 * host and is unit-tested directly (tests/unit/t_colfit). The widget and the row renderer
 * both call colfit() so the header cells and the row cells always line up. */
#pragma once

#define COLFIT_NCOL 4
#define COLFIT_MINW 56          /* a column never shrinks below this many pixels */

/* Given the content width `total` and the three fixed widths `cw_in` (Name, Kind, Size),
 * write each column's x offset (from the pane's left edge) into colx[4] and its width into
 * colw[4]. colw[3] is the Date column = the remainder. Inputs are not modified. */
static inline void colfit(int total, const int cw_in[3], int colx[4], int colw[4]) {
    int cw[3];
    for (int i = 0; i < 3; i++) { cw[i] = cw_in[i]; if (cw[i] < COLFIT_MINW) cw[i] = COLFIT_MINW; }
    int sum = cw[0] + cw[1] + cw[2];
    /* keep at least COLFIT_MINW for Date: shrink Size, then Kind, then Name */
    while (sum + COLFIT_MINW > total &&
           (cw[2] > COLFIT_MINW || cw[1] > COLFIT_MINW || cw[0] > COLFIT_MINW)) {
        if      (cw[2] > COLFIT_MINW) cw[2]--;
        else if (cw[1] > COLFIT_MINW) cw[1]--;
        else                          cw[0]--;
        sum = cw[0] + cw[1] + cw[2];
    }
    colx[0] = 0;             colw[0] = cw[0];
    colx[1] = cw[0];         colw[1] = cw[1];
    colx[2] = cw[0] + cw[1]; colw[2] = cw[2];
    colx[3] = sum;           colw[3] = total - sum; if (colw[3] < 0) colw[3] = 0;
}
