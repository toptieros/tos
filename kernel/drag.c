/* Drag-and-drop session (see drag.h). A single typed payload guarded by its own
 * spinlock -- mirrors kernel/clipboard.c. Lifecycle: a source arms it with
 * drag_begin (active=1); the compositor sees it via drag_state and runs the
 * ghost/hit-test; on release it posts WEV_DROP to the target and calls drag_end
 * (active=0). The bytes are KEPT after drag_end so the target -- a separate
 * process that runs after the drop event is queued -- can still drag_payload()
 * them; the next drag_begin overwrites. */
#include "drag.h"
#include "spinlock.h"
#include <stdint.h>

#define DRAG_BYTES 4096

static struct {
    int  active, type, len;
    char label[32];
    char data[DRAG_BYTES];
} drag;
static spinlock_t drag_lock = SPINLOCK_INIT;

int drag_begin(int type, const char *label, const char *data, int len) {
    if (type <= 0) return -1;
    if (len < 0) len = 0;
    if (len > DRAG_BYTES) len = DRAG_BYTES;
    uint64_t f = spin_lock_irqsave(&drag_lock);
    drag.active = 1; drag.type = type; drag.len = len;
    int i = 0; if (label) for (; label[i] && i < 31; i++) drag.label[i] = label[i];
    drag.label[i] = 0;
    for (int j = 0; j < len; j++) drag.data[j] = data[j];
    spin_unlock_irqrestore(&drag_lock, f);
    return 0;
}

int drag_payload(int *type_out, char *buf, int cap) {
    uint64_t f = spin_lock_irqsave(&drag_lock);
    if (drag.type == 0) { spin_unlock_irqrestore(&drag_lock, f); return -1; }  /* no drag ever armed */
    if (type_out) *type_out = drag.type;
    int len = drag.len; if (len > cap) len = cap; if (len < 0) len = 0;
    for (int j = 0; j < len; j++) buf[j] = drag.data[j];
    spin_unlock_irqrestore(&drag_lock, f);
    return len;
}

int drag_state(char *label_out, int cap) {
    uint64_t f = spin_lock_irqsave(&drag_lock);
    int t = drag.active ? drag.type : 0;
    if (label_out && cap > 0) {
        int i = 0;
        if (drag.active) for (; i < cap - 1 && drag.label[i]; i++) label_out[i] = drag.label[i];
        label_out[i] = 0;
    }
    spin_unlock_irqrestore(&drag_lock, f);
    return t;
}

void drag_end(void) {
    uint64_t f = spin_lock_irqsave(&drag_lock);
    drag.active = 0;                  /* keep type/len/data so the target's post-drop read still works */
    spin_unlock_irqrestore(&drag_lock, f);
}
