/* System clipboard: a small ring of recent entries (text or raw bytes, e.g. a
 * file's contents), newest first -- the kernel half of the Windows-Win+V-style
 * clipboard. One entry is the "active" paste target. Backs SYS_CLIP_*. */
#pragma once
#include "syscall.h"

int  clip_put(int type, const char *name, const char *data, int len);  /* -> new index (0), or -1 */
int  clip_count(void);
int  clip_get(int idx, char *buf, int cap);   /* copy entry data out -> bytes copied, or -1 */
int  clip_info(int idx, struct clipinfo *out);/* 0/-1 */
int  clip_active(int idx);                     /* idx>=0 sets the active entry; returns active index */
void clip_clear(void);
