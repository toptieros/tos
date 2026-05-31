/* tOS SDK conveniences -- the everyday helpers apps reach for, built on the raw
 * syscall wrappers in ulib.h. Together (ulib = the raw system API, sys = the
 * conveniences, libc = the C runtime) these are the tOS application SDK. */
#pragma once
#include <stddef.h>
#include "syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read a whole file into a fresh malloc'd, NUL-terminated buffer (free it with
 * free()). Returns the buffer and writes the byte count to *len_out, or 0. */
char *sys_slurp(const char *path, int *len_out);

/* Create/truncate a file and write the whole buffer. Returns bytes written, -1. */
int   sys_spit(const char *path, const void *buf, int len);

/* fork+exec a program as a detached child (the desktop launch pattern).
 * Returns the child pid in the parent, or -1. */
int   sys_launch(const char *prog);

/* 1 if the path exists (fills *st with type+size if non-NULL), else 0. */
int   sys_exists(const char *path, struct fstat *st);

/* Document hand-off ("open <path> with <prog>"): the launcher records the path,
 * then launches prog; the target reads it once at startup. Returns the child pid
 * (or -1). Used by the Files "Open With" flow. */
int   sys_open_with(const char *prog, const char *path);

/* If this app was launched to open a document, copy its path into out[cap] and
 * return 1 (consuming the request so a later launch won't see it); else 0. Call
 * once at startup. */
int   sys_open_arg(char *out, int cap);

#ifdef __cplusplus
}
#endif
