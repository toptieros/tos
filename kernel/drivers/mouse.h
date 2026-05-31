/* PS/2 mouse (8042 aux device), interrupt-driven on IRQ12. Enabled only on a
 * framebuffer boot (there is no GUI in VGA text mode). The IRQ handler decodes
 * 3-byte packets into an absolute position (clamped to the screen) + button
 * state; SYS_MOUSE reports it to the GUI. */
#pragma once

struct mousestate;
void mouse_init(int screen_w, int screen_h);
void mouse_irq(void);                       /* IRQ12 */
void mouse_get(struct mousestate *out);     /* current position + buttons */
