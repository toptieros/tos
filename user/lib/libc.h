/* tOS userspace libc core: the freestanding C runtime shared by every app.
 * Declarations for the mem/str helpers, the SYS_MMAP-backed heap, and a small
 * printf family. Included by ulib.h, so every app (C and C++) sees it. */
#pragma once
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);

void  *malloc(size_t n);
void   free(void *p);
void  *calloc(size_t a, size_t b);
void  *realloc(void *p, size_t n);

int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int    printf(const char *fmt, ...);     /* formats then writes to stdout */

#ifdef __cplusplus
}
#endif
