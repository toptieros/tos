/* Unit tests for the pure text/index helpers in user/lib/textutil.h -- the logic
 * under the launcher search filter (Spotlight/Launchpad) and the toolkit's
 * word-wise caret motion (Ctrl+Left/Right, Ctrl+Backspace/Delete). These used to
 * be checked only by booting the whole OS (t_spotlight_search, t_launchpad_search,
 * t_notepad_wordedit); here they run in microseconds on the host. */
#include "unit.h"
#include "../../user/lib/textutil.h"

static void test_ci_contains(void) {
    CHECK(tu_ci_contains("Notepad", ""),      "empty query matches everything");
    CHECK(tu_ci_contains("Notepad", "note"),  "case-insensitive prefix");
    CHECK(tu_ci_contains("Notepad", "PAD"),   "case-insensitive suffix");
    CHECK(tu_ci_contains("Notepad", "tep"),   "substring in the middle");
    CHECK(!tu_ci_contains("Notepad", "xyz"),  "non-substring rejected");
    CHECK(!tu_ci_contains("Files", "filesys"),"query longer than name rejected");
    CHECK(tu_ci_contains("Files", "files"),   "full (case-insensitive) match");
    CHECK(!tu_ci_contains("Files", "z"),      "single non-matching char rejected");
}

/* "alpha beta gamma": alpha 0-4, sp 5, beta 6-9, sp 10, gamma 11-15; len 16. */
static const char *SENT = "alpha beta gamma";
static const int   SLEN = 16;

static void test_word_prev(void) {
    CHECK_INT(tu_word_prev(SENT, 16), 11, "from end jumps to start of 'gamma'");
    CHECK_INT(tu_word_prev(SENT, 11), 6,  "from 'gamma' jumps to start of 'beta'");
    CHECK_INT(tu_word_prev(SENT, 6),  0,  "from 'beta' jumps to start of 'alpha'");
    CHECK_INT(tu_word_prev(SENT, 0),  0,  "at the start stays at the start");
    CHECK_INT(tu_word_prev(SENT, 3),  0,  "mid-word jumps to that word's start");
}

static void test_word_next(void) {
    CHECK_INT(tu_word_next(SENT, SLEN, 0),  5,  "from start jumps past 'alpha'");
    CHECK_INT(tu_word_next(SENT, SLEN, 5),  10, "from a space jumps past 'beta'");
    CHECK_INT(tu_word_next(SENT, SLEN, 6),  10, "from 'beta' start jumps to its end");
    CHECK_INT(tu_word_next(SENT, SLEN, 11), 16, "from 'gamma' jumps to end of buffer");
    CHECK_INT(tu_word_next(SENT, SLEN, 16), 16, "at the end stays at the end");
}

static void test_wordch(void) {
    CHECK(tu_wordch('a') && tu_wordch('Z') && tu_wordch('7') && tu_wordch('_'),
          "alnum + underscore are word chars");
    CHECK(!tu_wordch(' ') && !tu_wordch('-') && !tu_wordch('.') && !tu_wordch('\n'),
          "separators are not word chars");
}

/* tu_wrap (toast body word-wrap). A fixed 7px/char ruler keeps the math obvious:
 * maxw=30 => 4 chars/line. The recorded lines let us assert the exact breaks. */
static int  measure7(const char *s) { return (int)strlen(s) * 7; }
static char wrap_lines[8][256];
static int  wrap_n;
static void wrap_emit(void *ctx, const char *line, int idx) {
    (void)ctx; (void)idx;
    if (wrap_n < 8) { strncpy(wrap_lines[wrap_n], line, 255); wrap_lines[wrap_n][255] = 0; wrap_n++; }
}

static void test_wrap_no_glue(void) {
    /* The regression this guards: a word forced onto the next line used to glue to
     * the following word ("notification" + "body" -> "notificationbody"). */
    wrap_n = 0;
    int n = tu_wrap("aaaa bbbb cccc", 30, 6, measure7, wrap_emit, 0);
    CHECK_INT(n, 3, "three words wrap to three lines at 4 chars/line");
    CHECK_STR(wrap_lines[0], "aaaa ", "line 0 is the first word + its space");
    CHECK_STR(wrap_lines[1], "bbbb ", "line 1 starts a fresh word, NOT glued to line 0");
    CHECK_STR(wrap_lines[2], "cccc",  "line 2 is the last word");
}

static void test_wrap_fits_one_line(void) {
    wrap_n = 0;
    int n = tu_wrap("hi yo", 100, 6, measure7, wrap_emit, 0);   /* 5 chars, fits */
    CHECK_INT(n, 1, "short text stays on one line");
    CHECK_STR(wrap_lines[0], "hi yo", "the whole string on line 0");
}

static void test_wrap_maxlines(void) {
    wrap_n = 0;
    int n = tu_wrap("aaaa bbbb cccc dddd", 30, 2, measure7, wrap_emit, 0);
    CHECK_INT(n, 2, "wrap stops at maxlines");
}

static void test_wrap_count_only(void) {
    CHECK_INT(tu_wrap("aaaa bbbb cccc", 30, 6, measure7, 0, 0), 3,
              "count-only mode (no emit) returns the line count");
}

int main(void) {
    RUN(test_ci_contains);
    RUN(test_word_prev);
    RUN(test_word_next);
    RUN(test_wordch);
    RUN(test_wrap_no_glue);
    RUN(test_wrap_fits_one_line);
    RUN(test_wrap_maxlines);
    RUN(test_wrap_count_only);
    return UNIT_SUMMARY();
}
