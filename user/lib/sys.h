/* tOS SDK conveniences -- the everyday helpers apps reach for, built on the raw
 * syscall wrappers in ulib.h. Together (ulib = the raw system API, sys = the
 * conveniences, libc = the C runtime) these are the tOS application SDK. */
#pragma once
#include <stddef.h>
#include "syscall.h"
#include "pickreq.h"      /* struct pick_req + the request key=value codec */

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

/* The system file picker (design/file-picker.md): the Open/Save dialog *is* the
 * Files app, launched in a picker mode, returning the chosen path over temp files.
 *
 * sys_pick_begin writes the request to /tmp/.picker-req, unlinks any stale result,
 * then fork+execs Files; returns the picker's child pid (or -1). The caller polls
 * each event-loop tick with sys_pick_poll, which reaps the picker when it exits:
 *    1 = a path was picked (absolute path copied into out[cap]),
 *    0 = still open (poll again next tick),
 *   -1 = cancelled / window-closed / crashed (no result).
 * This extends the existing /tmp/.open-doc hand-off (sys_open_with). */
int   sys_pick_begin(const struct pick_req *r);
int   sys_pick_poll(int pid, char *out, int cap);

/* Files-side mirror of sys_open_arg: if a picker request is pending, fill *out and
 * return 1 (consuming /tmp/.picker-req); else 0. Call once at startup, before
 * sys_open_arg, so a launch-as-picker isn't mistaken for an open-document launch. */
int   sys_pick_req(struct pick_req *out);

#ifdef __cplusplus
}
#endif
