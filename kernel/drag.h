/* Drag-and-drop session: ONE in-flight typed payload (file paths, text, image
 * bytes) the kernel holds between a drag SOURCE and its DROP target -- the way
 * the clipboard holds a copied entry. The source arms it (drag_begin); the
 * compositor runs the visual session + routes the drop; the target reads the
 * bytes (drag_payload) when it receives a WEV_DROP. The payload outlives
 * drag_end so the target can still read it on the post-drop frame.
 * Backs SYS_DRAG_*; see design/files-and-desktop.md. */
#pragma once

int  drag_begin(int type, const char *label, const char *data, int len); /* source: arm a drag; 0/-1 */
int  drag_payload(int *type_out, char *buf, int cap); /* target: copy the bytes out -> len, or -1 */
int  drag_state(char *label_out, int cap);            /* -> active drag type (>0) + label, or 0 if idle */
void drag_end(void);                                  /* compositor: the session concluded (bytes kept) */
