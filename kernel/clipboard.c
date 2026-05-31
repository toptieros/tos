/* System clipboard ring (see clipboard.h). Entries are held newest-first; a new
 * put inserts at index 0 and evicts the oldest when full. Guarded by its own
 * spinlock since apps copy/paste from ordinary syscall context (any CPU). */
#include "clipboard.h"
#include "spinlock.h"
#include <stdint.h>

#define CLIP_MAX   8
#define CLIP_BYTES 8192

struct ent { int used, type, len; char name[32]; char data[CLIP_BYTES]; };
static struct ent  ents[CLIP_MAX];
static int         n_ent, active;
static spinlock_t  clip_lock = SPINLOCK_INIT;

int clip_put(int type, const char *name, const char *data, int len) {
    if (len < 0) len = 0;
    if (len > CLIP_BYTES) len = CLIP_BYTES;
    uint64_t f = spin_lock_irqsave(&clip_lock);
    int keep = n_ent < CLIP_MAX ? n_ent : CLIP_MAX - 1;
    for (int i = keep; i > 0; i--) ents[i] = ents[i - 1];     /* shift down, drop oldest */
    struct ent *e = &ents[0];
    e->used = 1; e->type = type; e->len = len;
    int i = 0; if (name) for (; name[i] && i < 31; i++) e->name[i] = name[i];
    e->name[i] = 0;
    for (int j = 0; j < len; j++) e->data[j] = data[j];
    n_ent = keep + 1;
    active = 0;
    spin_unlock_irqrestore(&clip_lock, f);
    return 0;
}
int clip_count(void) { return n_ent; }

int clip_get(int idx, char *buf, int cap) {
    uint64_t f = spin_lock_irqsave(&clip_lock);
    if (idx < 0 || idx >= n_ent) { spin_unlock_irqrestore(&clip_lock, f); return -1; }
    int len = ents[idx].len; if (len > cap) len = cap;
    for (int j = 0; j < len; j++) buf[j] = ents[idx].data[j];
    spin_unlock_irqrestore(&clip_lock, f);
    return len;
}
int clip_info(int idx, struct clipinfo *out) {
    uint64_t f = spin_lock_irqsave(&clip_lock);
    if (idx < 0 || idx >= n_ent) { spin_unlock_irqrestore(&clip_lock, f); return -1; }
    out->type = (uint32_t)ents[idx].type;
    out->len = (uint32_t)ents[idx].len;
    out->active = (uint32_t)(idx == active);
    for (int i = 0; i < 32; i++) out->name[i] = ents[idx].name[i];
    spin_unlock_irqrestore(&clip_lock, f);
    return 0;
}
int clip_active(int idx) {
    uint64_t f = spin_lock_irqsave(&clip_lock);
    if (idx >= 0 && idx < n_ent) active = idx;
    int a = active;
    spin_unlock_irqrestore(&clip_lock, f);
    return a;
}
void clip_clear(void) {
    uint64_t f = spin_lock_irqsave(&clip_lock);
    n_ent = 0; active = 0;
    spin_unlock_irqrestore(&clip_lock, f);
}
