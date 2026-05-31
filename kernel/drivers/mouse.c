#include "mouse.h"
#include "syscall.h"
#include "cpu.h"
#include "timer.h"
#include <stdint.h>

/* 8042 controller ports: 0x60 data, 0x64 status/command. Status bit0 = output
 * buffer full (data to read), bit1 = input buffer full (don't write yet),
 * bit5 = the byte in the output buffer came from the aux device (the mouse). */
#define PS2_DATA 0x60
#define PS2_CMD  0x64
#define RESYNC_TICKS 3                            /* flush a stale partial packet after ~30ms */

static int       scr_w = 640, scr_h = 480;
static int       mx, my;
static uint32_t  btn;
static uint8_t   pkt[4];
static int       pkt_idx;
static int       mouse_id;                        /* 0 = standard 3-byte, 3 = wheel, 4 = 5-button */
static int       pkt_len = 3;                     /* bytes per packet (4 once a wheel/explorer is found) */
static int       mwheel;                          /* accumulated scroll-wheel delta since the last mouse_get */
static uint64_t  last_tick;                       /* tick of the previous packet byte (for resync) */

static void ps2_wait_in(void)  { for (int i = 0; i < 200000; i++) if (!(inb(PS2_CMD) & 2)) return; }
static void ps2_wait_out(void) { for (int i = 0; i < 200000; i++) if (inb(PS2_CMD) & 1)  return; }

/* Send a command byte to the mouse (0xD4 routes the next data byte to the aux
 * device) and consume its ACK (0xFA). */
static void mouse_cmd(uint8_t b) {
    ps2_wait_in();  outb(PS2_CMD, 0xD4);
    ps2_wait_in();  outb(PS2_DATA, b);
    ps2_wait_out(); (void)inb(PS2_DATA);
}
/* Set the mouse sample rate (0xF3 + value), both ACK'd. */
static void mouse_rate(uint8_t r) { mouse_cmd(0xF3); mouse_cmd(r); }
/* Read the device id (0xF2): ACK byte, then the id byte. */
static uint8_t mouse_read_id(void) {
    ps2_wait_in();  outb(PS2_CMD, 0xD4);
    ps2_wait_in();  outb(PS2_DATA, 0xF2);
    ps2_wait_out(); (void)inb(PS2_DATA);          /* ACK 0xFA          */
    ps2_wait_out(); return inb(PS2_DATA);         /* device id         */
}

void mouse_init(int w, int h) {
    scr_w = w; scr_h = h;
    mx = w / 2; my = h / 2; btn = 0; pkt_idx = 0;

    ps2_wait_in();  outb(PS2_CMD, 0xA8);          /* enable the aux device       */

    ps2_wait_in();  outb(PS2_CMD, 0x20);          /* read controller config byte */
    ps2_wait_out(); uint8_t cfg = inb(PS2_DATA);
    cfg |=  0x02;                                 /* enable IRQ12 (aux interrupt) */
    cfg &= ~0x20;                                 /* enable the aux clock         */
    ps2_wait_in();  outb(PS2_CMD, 0x60);          /* write controller config byte */
    ps2_wait_in();  outb(PS2_DATA, cfg);

    mouse_cmd(0xF6);                              /* set defaults (reporting off) */

    /* Negotiate the IntelliMouse Explorer (5-button) protocol with the magic
     * sample-rate "knock" (the same sequence the Linux psmouse driver uses):
     * 200,100,80 unlocks the scroll wheel (id 3, 4-byte packets), then 200,200,80
     * unlocks the two side buttons (id 4). We only decode the side buttons for
     * id 4; if the knock is ignored (id stays 0) we keep the plain 3-byte form. */
    mouse_rate(200); mouse_rate(100); mouse_rate(80);
    if (mouse_read_id() == 3) { mouse_rate(200); mouse_rate(200); mouse_rate(80); }
    mouse_id = mouse_read_id();
    pkt_len = (mouse_id >= 3) ? 4 : 3;
    mouse_rate(60);                               /* a sane reporting rate        */

    /* Drain stray negotiation bytes and start the stream aligned at byte 0. */
    for (int i = 0; i < 32 && (inb(PS2_CMD) & 1); i++) (void)inb(PS2_DATA);
    pkt_idx = 0;

    mouse_cmd(0xF4);                              /* enable data reporting        */

    outb(0x21, inb(0x21) & ~0x04);                /* unmask IRQ2 cascade (master) */
    outb(0xA1, inb(0xA1) & ~0x10);                /* unmask IRQ12        (slave)  */
}

static void apply(void) {
    uint8_t f = pkt[0];
    /* Apply the (bounded) delta even when the overflow bit is set -- a fast flick
     * still moves the cursor instead of freezing it. */
    int dx = (int)pkt[1] - ((f & 0x10) ? 256 : 0);
    int dy = (int)pkt[2] - ((f & 0x20) ? 256 : 0);
    mx += dx;
    my -= dy;                                     /* mouse +Y is up; screen +Y down */
    if (mx < 0) mx = 0; else if (mx > scr_w - 1) mx = scr_w - 1;
    if (my < 0) my = 0; else if (my > scr_h - 1) my = scr_h - 1;
    uint32_t b = f & 0x07;                         /* L=1, R=2, M=4 */
    if (mouse_id == 4) {                           /* the 4th byte carries the side buttons */
        if (pkt[3] & 0x10) b |= 0x08;              /* button 4 -> back    (bit3) */
        if (pkt[3] & 0x20) b |= 0x10;              /* button 5 -> forward (bit4) */
    }
    /* The scroll wheel's Z movement lives in byte 4: a full signed 8-bit value on a
     * plain wheel mouse (id 3), or a 4-bit two's-complement nibble (the high nibble
     * carries the side buttons) on a 5-button Explorer (id 4). PS/2 reports a wheel-up
     * roll as a NEGATIVE Z, so negate it -> mwheel > 0 means "scroll up". */
    int dz = 0;
    if (mouse_id == 3)      dz = (int)(int8_t)pkt[3];
    else if (mouse_id == 4) dz = (pkt[3] & 0x08) ? (int)(pkt[3] & 0x0f) - 16 : (int)(pkt[3] & 0x0f);
    mwheel -= dz;
    btn = b;
}

void mouse_irq(void) {
    uint64_t now = timer_ticks();
    /* Drain every pending aux byte. A 4-byte (Explorer) stream cannot resync on
     * its own -- a movement data byte can have the sync bit set, so a single
     * dropped byte would desync it forever. Like Linux's psmouse, we resync on a
     * GAP: packet bytes arrive in a tight burst, so if we are mid-packet and a
     * while has passed since the last byte, the partial packet is stale -> flush
     * it and treat this byte as a fresh byte 0. */
    for (;;) {
        uint8_t st = inb(PS2_CMD);
        if ((st & 0x21) != 0x21) break;           /* empty, or head is a keyboard byte */
        uint8_t b = inb(PS2_DATA);
        if (pkt_idx > 0 && now - last_tick >= RESYNC_TICKS) pkt_idx = 0;
        last_tick = now;
        if (pkt_idx == 0 && !(b & 0x08)) continue; /* byte 0 always has bit3 set */
        pkt[pkt_idx++] = b;
        if (pkt_idx == pkt_len) { pkt_idx = 0; apply(); }
    }
    outb(0xA0, 0x20);                             /* EOI to slave PIC  */
    outb(0x20, 0x20);                             /* EOI to master PIC (cascade) */
}

void mouse_get(struct mousestate *out) {
    out->x = mx; out->y = my; out->buttons = btn;
    out->wheel = mwheel; mwheel = 0;               /* consume the accumulated scroll ticks */
}
