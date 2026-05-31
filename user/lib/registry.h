/* tOS system registry -- a small layered key/value store for system + per-user
 * settings and state (see design/settings.md). Defaults ship in
 * /System/etc/registry; per-user overrides live in /Users/user/.config/registry.
 * reg_get returns the user value if set, else the system default, else the caller's
 * fallback. reg_set writes the user layer only; reg_save flushes it to disk.
 *
 * Pure userspace over the file syscalls -- part of the SDK (linked into every app
 * via ulib), so the compositor, the shell, and apps share one settings store. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REG_MAX     256     /* entries held in memory */
#define REG_KEYMAX  64
#define REG_VALMAX  96

#define REG_SYS_PATH  "/System/etc/registry"
#define REG_USER_PATH "/Users/user/.config/registry"

void        reg_load(void);                              /* (re)load system + user layers */
const char *reg_get(const char *key, const char *fallback);
int         reg_int(const char *key, int fallback);
int         reg_bool(const char *key, int fallback);     /* true/1/yes/on -> 1 */
uint32_t    reg_color(const char *key, uint32_t fallback);/* "#RRGGBB" -> 0xFFRRGGBB */
void        reg_set(const char *key, const char *val);   /* user layer, in memory */
void        reg_set_int(const char *key, int val);
int         reg_save(void);                              /* write user layer to disk; 0/-1 */
int         reg_keys(const char *prefix, char out[][REG_KEYMAX], int max); /* enumerate -> n */

#ifdef __cplusplus
}
#endif
