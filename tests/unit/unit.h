/* Tiny host unit-test framework for tOS pure logic (see design/testing.md). Each
 * test is a function run via RUN(); the CHECK* macros tally pass/fail and print the
 * first divergence; main() ends with UNIT_SUMMARY(), exiting nonzero on any failure.
 * Compiled and run with the host cc in milliseconds -- no QEMU. */
#pragma once
#include <stdio.h>
#include <string.h>

static int u_pass, u_fail;
static const char *u_cur = "";

#define CHECK(cond, msg) do { \
    if (cond) u_pass++; \
    else { u_fail++; printf("  FAIL [%s] %s:%d  %s\n", u_cur, __FILE__, __LINE__, msg); } \
} while (0)

#define CHECK_INT(got, want, msg) do { \
    long _g = (long)(got), _w = (long)(want); \
    if (_g == _w) u_pass++; \
    else { u_fail++; printf("  FAIL [%s] %s:%d  %s (got %ld, want %ld)\n", \
                            u_cur, __FILE__, __LINE__, msg, _g, _w); } \
} while (0)

#define CHECK_STR(got, want, msg) do { \
    const char *_g = (got), *_w = (want); \
    if (strcmp(_g, _w) == 0) u_pass++; \
    else { u_fail++; printf("  FAIL [%s] %s:%d  %s (got \"%s\", want \"%s\")\n", \
                            u_cur, __FILE__, __LINE__, msg, _g, _w); } \
} while (0)

#define RUN(fn) do { u_cur = #fn; fn(); } while (0)

#define UNIT_SUMMARY() ( \
    printf("%s: %d passed, %d failed\n", u_fail ? "UNIT FAIL" : "UNIT OK", u_pass, u_fail), \
    u_fail ? 1 : 0 )
