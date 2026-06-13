/* Syscall ABI, shared between the kernel dispatcher and the userspace program.
 *
 * Convention (int 0x80): rax = number, rdi = arg1, return value in rax. */
#pragma once
#include <stdint.h>

#define SYS_EXIT  0   /* arg: ignored        -> does not return                */
#define SYS_WRITE 1   /* arg: char* (NUL-terminated) -> prints to console      */
#define SYS_PUTC  2   /* arg: char           -> prints one character           */
#define SYS_READ  3   /* arg: ignored        -> blocks, returns one char       */
#define SYS_YIELD 4   /* arg: ignored        -> give up the CPU, return 0       */
#define SYS_SPAWN 5   /* arg: char* program  -> load+start a task, return its id */
#define SYS_WAIT  6   /* arg: ignored        -> block until children all exit    */
#define SYS_SHUTDOWN 7 /* arg: ignored       -> halt the machine                 */
#define SYS_LS    8   /* arg: ignored        -> print the FS directory           */
#define SYS_OPEN  9   /* (name, flags)       -> fd >=0, or -1                      */
#define SYS_READ_FD  10 /* (fd, buf, len)    -> bytes read (0=EOF), or -1          */
#define SYS_WRITE_FD 11 /* (fd, buf, len)    -> bytes written, or -1               */
#define SYS_CLOSE 12  /* (fd)                -> 0, or -1                            */
#define SYS_UNLINK 13 /* (name)              -> 0, or -1  (delete a file)           */
#define SYS_SLEEP 14  /* (ticks)             -> park the task ~ticks timer ticks    */
#define SYS_FORK  15  /* arg: ignored        -> child:0, parent:child pid, -1 err   */
#define SYS_EXEC  16  /* arg: char* program  -> replace image; only returns on error */
#define SYS_GETCPU 17 /* arg: ignored        -> index of the CPU running the caller  */
#define SYS_CPAINT 18 /* arg: (inverse<<8)|char -> paint a block cursor at the cursor cell */
#define SYS_FBINFO 19 /* arg: struct fbinfo* -> maps the framebuffer + fills it; 0 ok, -1 none */
#define SYS_CON_WINDOW 20 /* (((x<<16)|y), ((w<<16)|h)) px -> confine text console to a window */
#define SYS_TRYWAIT 21 /* arg: ignored -> reaped child pid, 0 if children live, -1 if none */
#define SYS_MOUSE   22 /* arg: struct mousestate* -> fill with mouse position + buttons; 0 */
#define SYS_TIME    23 /* arg: struct rtctime* -> fill with the wall-clock time (CMOS RTC) */
#define SYS_LSPCI   24 /* arg: ignored        -> print the PCI device list to the console */
#define SYS_BEEP    25 /* arg: freq Hz (0=off)-> drive the PC speaker                      */
#define SYS_REBOOT  26 /* arg: ignored        -> reset the machine (8042)                  */
/* --- compositor / windowing / pty (the desktop is built on these) --------- */
#define SYS_GETKEY     27 /* arg: ignored -> next input byte, or -1 if none (NON-blocking) */
#define SYS_WIN_CREATE 28 /* (struct wininfo*) -> window id, fills the surface mapping     */
#define SYS_WIN_PRESENT 29 /* (id)            -> mark the window's surface as updated       */
#define SYS_WIN_POLL   30 /* (id, struct winevent*) -> 1 if an event was dequeued, else 0   */
#define SYS_WIN_CLOSE  31 /* (id)            -> destroy a window                            */
#define SYS_WM_REGISTER 32 /* arg: ignored   -> become the compositor (suppresses fb console)*/
#define SYS_WM_WINDOWS 33 /* (struct wmwin* buf, max) -> snapshot live windows, count       */
#define SYS_WM_SEND_KEY 34 /* (id, byte)     -> deliver an input byte to a window           */
#define SYS_PTY_OPEN   35 /* arg: ignored    -> allocate a pty channel -> id                */
#define SYS_PTY_ATTACH 36 /* (id)            -> bind the caller's stdio to this pty         */
#define SYS_PTY_READ   37 /* (id, buf, len)  -> bytes from the slave's output (NON-blocking) */
#define SYS_PTY_WRITE  38 /* (id, buf, len)  -> feed bytes to the slave's input             */
#define SYS_PTY_CLOSE  39 /* (id)            -> free a pty channel                          */
#define SYS_SYSINFO    40 /* (struct sysinfo*) -> machine info (for fastfetch)              */
#define SYS_ISATTY     41 /* arg: ignored    -> 1 if stdio is a pty (under a terminal), else 0 */
#define SYS_WIN_RESIZE 42 /* (id, w, h)      -> app resizes its own surface                 */
#define SYS_WM_POST    43 /* (id, type, a)   -> compositor posts an event to a window       */
/* --- hierarchical filesystem (directories, paths) ------------------------- */
#define SYS_MKDIR   44 /* (path)             -> make a directory; 0 or -1                   */
#define SYS_RMDIR   45 /* (path)             -> remove an empty directory; 0 or -1          */
#define SYS_CHDIR   46 /* (path)             -> change the working directory; 0 or -1       */
#define SYS_GETCWD  47 /* (buf, len)         -> write the absolute cwd path; 0 or -1        */
#define SYS_READDIR 48 /* (path, dirent*, max) -> entries in a dir (cwd if path empty); n/-1 */
#define SYS_RENAME  49 /* (oldpath, newpath) -> move/rename a file or directory; 0 or -1    */
#define SYS_STAT    50 /* (path, fstat*)     -> fill type + size; 0 or -1                   */
/* --- dynamic anonymous memory (back buffers, heaps -- sized to the hardware) - */
#define SYS_MMAP    51 /* (nbytes)           -> base vaddr of fresh private RAM, or 0       */

/* --- system clipboard: a small ring of text/byte entries (Windows Win+V style) - */
#define SYS_CLIP_PUT    52 /* (name, data, (type<<24)|len) -> new entry index, or -1        */
#define SYS_CLIP_COUNT  53 /* ()                 -> number of entries held                   */
#define SYS_CLIP_GET    54 /* (idx, buf, cap)    -> bytes of entry idx copied out            */
#define SYS_CLIP_INFO   55 /* (idx, struct clipinfo*) -> 0/-1                                 */
#define SYS_CLIP_ACTIVE 56 /* (idx)              -> set active if idx>=0; returns active idx  */
#define SYS_CLIP_CLEAR  57 /* ()                 -> empty the clipboard; 0                    */

/* --- compositor: force-kill the process owning a window (Super+Shift+Q) -------- */
#define SYS_WM_KILL     58 /* (window id)        -> request an async kill of its owner; 0/-1 *
                            * compositor-only; the task dies on its next return to ring 3.   */

/* --- notifications: any app posts a toast; the compositor drains the queue ------ */
#define SYS_NOTIFY      59 /* (struct notif*)    -> post a notification to the WM; 0/-1       */
#define SYS_WM_NOTIFY   60 /* (struct notif*)    -> compositor: dequeue one; 1 if got, else 0 */
#define SYS_KBD_MODS    61 /* ()                 -> live keyboard modifier bitmask (KMOD_*)   */
#define SYS_SETUID      62 /* (uid)              -> set/drop the caller's owner uid; 0/-1     */
#define SYS_GETUID      63 /* ()                 -> the caller's owner uid (TOS_UID_*)         */
#define SYS_WIN_SETMENU 64 /* (id, struct winmenu*) -> app declares its menu bar; 0/-1        */
#define SYS_WM_GETMENU  65 /* (id, struct winmenu*) -> compositor reads a window's menu; 1/0   */
#define SYS_WIN_PRESENT_RECT 66 /* (id, xy, wh) -> present only a damage rect (packed 16:16)   */
#define SYS_GETPID      67 /* ()                 -> the caller's process id                     */
#define SYS_GETPPID     68 /* ()                 -> the caller's parent process id (0 if none)  */
#define SYS_STATFS      69 /* (struct statfs*)   -> fill volume total/free bytes; 0/-1         */
#define SYS_WIN_SETCURSOR 70 /* (id, shape)      -> app declares the cursor for its client area;
                              * the compositor shows it while the pointer is over that window.
                              * Shape ids match user/lib/cursors.h (CUR_ARROW/IBEAM/RESIZE_*...);
                              * apps update it live from hover, e.g. an I-beam over a text field. */
#define SYS_SETCAPS     71 /* (mask)             -> drop the caller's caps to (caps & mask); new mask */
#define SYS_GETCAPS     72 /* ()                 -> the caller's capability bitmask (cap.h)            */
/* Drag-and-drop (kernel/drag.c). A source arms a typed payload; the compositor
 * runs the ghost + routes the drop; the target reads the bytes on its WEV_DROP. */
#define SYS_DRAG_BEGIN   73 /* (label, data, (type<<24)|len) -> arm a typed drag; 0/-1 (source)        */
#define SYS_DRAG_PAYLOAD 74 /* (int* type, buf, cap) -> copy the dropped payload out; len/-1 (target)  */
#define SYS_DRAG_STATE   75 /* (char* label, cap)  -> active drag type (>0)+label, or 0 (compositor)    */
#define SYS_DRAG_END     76 /* ()                  -> end the session (bytes kept for the drop read)     */
#define SYS_INSTALL      77 /* (target_bdev)       -> clone the boot disk onto block device `target`;
                            *                        sectors written, or -1 (the live->disk installer) */
/* Networking (the native TCP/IP stack in kernel/net) -- all gated by CAP_NET. IPs
 * are packed big-endian into a u32 (a.b.c.d -> (a<<24)|(b<<16)|(c<<8)|d). The TCP
 * calls drive the single kernel connection (one at a time, polled). */
#define SYS_NET_PING     78 /* (ip)                -> one ICMP echo round-trip; 0 reply / -1            */
#define SYS_NET_CONNECT  79 /* (ip, port)          -> TCP active open; 0 established / -1               */
#define SYS_NET_SEND     80 /* (buf, len)          -> TCP send; 0 / -1                                  */
#define SYS_NET_RECV     81 /* (buf, max)          -> TCP recv; bytes, 0 (none yet), -1 (closed/reset)  */
#define SYS_NET_CLOSE    82 /* ()                  -> TCP close (FIN); 0                                 */
#define SYS_NET_LISTEN   83 /* (port)              -> arm a TCP listen port; 0 / -1                     */
#define SYS_NET_ACCEPT   84 /* ()                  -> block for a client; 0 established / -1 (timeout)   */

#include "cap.h"           /* CAP_* bits, shared with userspace's manifest->caps mapping */

/* Cursor shape ids for SYS_WIN_SETCURSOR. Must match the baked theme in
 * user/lib/cursors.h (tools/gencursors.py) -- duplicated (identically) here so
 * apps can name a shape without pulling in the pixmap tables. */
#define CUR_ARROW 0
#define CUR_IBEAM 1
#define CUR_HAND 2
#define CUR_RESIZE_NWSE 3
#define CUR_RESIZE_NESW 4
#define CUR_RESIZE_WE 5
#define CUR_RESIZE_NS 6

/* Keyboard modifier bitmask (SYS_KBD_MODS, and packed into WEV_KEY -- see below). */
#define KMOD_SHIFT 1
#define KMOD_CTRL  2
#define KMOD_ALT   4
#define KMOD_SUPER 8

#define CLIP_TEXT 0
#define CLIP_FILE 1
struct clipinfo { uint32_t type; uint32_t len; uint32_t active; char name[32]; };

/* Drag-and-drop payload types: the typed bytes a drag carries (kernel/drag.c +
 * design/files-and-desktop.md). Mirrors the clipboard's CLIP_* split -- one
 * protocol for files / text / image so it's designed once, generally. */
#define DRAG_FILES 1   /* data = a NUL-terminated absolute path (one item for now) */
#define DRAG_TEXT  2   /* data = UTF-8 text                                        */
#define DRAG_IMAGE 3   /* data = raw ARGB bytes (future)                           */
#define DRAG_PLACE 4   /* data = a Favorites row's path; intra-Files drag-reorder  */

/* A desktop notification: a short title + body an app posts with notify(); the
 * compositor shows the newest as a top-right toast and keeps a ring for the
 * notification center (design/ui.md). Fixed-size so it copies by value. */
#define NOTIF_TITLE  48
#define NOTIF_BODY   128
#define NOTIF_TARGET 24   /* app label to focus/launch when the notification is clicked; "" = no routing */
struct notif { char title[NOTIF_TITLE]; char body[NOTIF_BODY]; char target[NOTIF_TARGET]; };

#define TIMER_HZ  100 /* PIT preemption/sleep ticks per second (see timer.c)        */

/* The keyboard emits these single bytes for chords that aren't plain characters.
 * They sit above ASCII (>0x7f) so they never collide with a real character. Some
 * are GLOBAL (the compositor acts on them and never forwards them): Super+V opens
 * the clipboard, Alt+Tab cycles the window switcher, Super+Q closes / Super+Shift+Q
 * kills the focused window, Super+Space opens Spotlight, a lone Super tap opens the
 * Launchpad. Others are APP-LEVEL edit intents the compositor forwards to the
 * focused window (the terminal acts on them): Ctrl+Shift+C/X/V. (Super+Tab is
 * deliberately NOT emitted -- it is reserved for future desktop switching and falls
 * through to a plain Tab.) */
#define KEY_SUPER_V     0x90   /* global: open/summon the clipboard manager        */
#define KEY_ALT_TAB     0x91   /* global: MRU window switcher                       */
#define KEY_SUPER_Q     0x92   /* global: close the focused window (graceful)       */
#define KEY_SUPER_KILL  0x93   /* global: Super+Shift+Q -- force-kill its process    */
#define KEY_TERM_COPY   0x94   /* app:    Ctrl+Shift+C                              */
#define KEY_TERM_CUT    0x95   /* app:    Ctrl+Shift+X                              */
#define KEY_TERM_PASTE  0x96   /* app:    Ctrl+Shift+V                              */
#define KEY_SUPER_SPACE 0x97   /* global: Spotlight search                          */
#define KEY_LAUNCHPAD   0x98   /* global: Launchpad (Super tapped on its own)        */
#define KEY_TERM_PGUP   0x99   /* app:    Shift+PgUp -- page scrollback up           */
#define KEY_TERM_PGDN   0x9A   /* app:    Shift+PgDn -- page scrollback down         */
#define KEY_SUPER_F     0x9B   /* global: toggle fullscreen on the focused window     */

/* Filled by SYS_FBINFO. On success the GOP framebuffer is mapped into the caller
 * at `vaddr` (32-bit XRGB pixels, `pitch` pixels per scanline) and present=1. On
 * a text-mode (VGA/BIOS) boot the call returns -1 and present stays 0. */
struct fbinfo {
    uint32_t present;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       /* pixels per scanline */
    uint64_t vaddr;       /* user pointer to pixel (0,0) */
};

/* Filled by SYS_MOUSE: absolute cursor position (clamped to the screen) and the
 * button bitmask (bit0 left, bit1 right, bit2 middle, bit3 back/button4,
 * bit4 forward/button5 -- the side buttons, only when a 5-button mouse is found). */
struct mousestate {
    int32_t  x;
    int32_t  y;
    uint32_t buttons;
    int32_t  wheel;       /* scroll ticks accumulated since the last poll (+ = wheel up) */
};

/* Filled by SYS_TIME: the wall-clock time from the CMOS RTC. */
struct rtctime {
    uint16_t year;        /* full year, e.g. 2026 */
    uint8_t  month, day, hour, min, sec;
};

/* open() flags (bitmask) */
#define O_RDONLY 0
#define O_CREATE 1    /* create a new file for writing (fails if it exists)       */
#define O_TRUNC  2    /* with O_CREATE: if the file exists, replace it in place    */

/* Filesystem entry types (match tosfs.h TOSFS_FILE / TOSFS_DIR). */
#define FT_FILE 1
#define FT_DIR  2

/* SYS_READDIR fills an array of these (one per directory entry). The name field
 * is the same width as the on-disk name (tosfs.h TOSFS_NAME_MAX). */
struct dirent {
    char     name[32];
    uint32_t type;        /* FT_FILE / FT_DIR */
    uint32_t size;        /* bytes (0 for a directory) */
    uint32_t mtime;       /* packed modification time (fstime.h); 0 = unknown */
    uint32_t owner;       /* owning uid (TOS_UID_* -- fs/perm.h); for lock badges without a per-row stat */
};

/* Filled by SYS_STATFS: the mounted volume's data capacity + what's free, in bytes
 * (block_size is the allocation unit -- one tosfs sector). */
struct statfs {
    uint32_t total_bytes;   /* usable data capacity (excludes the directory table) */
    uint32_t free_bytes;    /* currently free */
    uint32_t block_size;    /* allocation unit in bytes (512) */
};

/* Filled by SYS_STAT. */
struct fstat {
    uint32_t type;        /* FT_FILE / FT_DIR */
    uint32_t size;
    uint32_t owner;       /* owning uid (TOS_UID_* -- fs/perm.h); for lock badges + messages */
    uint32_t mtime;       /* packed modification time (fstime.h); 0 = unknown */
};

/* Per-task layout: each task's address space maps a private data page here.
 * The boot/spawn path seeds it with the task's role string, which the user
 * program reads to decide what to run. */
#define USER_DATA_VADDR 0x5FF000

/* --- windowing / compositor protocol -------------------------------------- *
 * Apps own a pixel surface (XRGB, tightly packed: pitch == width) that the
 * kernel maps into both the app and the compositor. The app draws into it and
 * SYS_WIN_PRESENT bumps a sequence the compositor watches; the compositor reads
 * the live window set with SYS_WM_WINDOWS and blits each surface to the screen. */
/* Window flags (wininfo.flags). WIN_POPUP makes the compositor draw the window as
 * a borderless, centred overlay (no title bar / controls, not draggable/resizable)
 * that the user dismisses by pressing Esc or clicking outside it -- for the
 * clipboard manager, Spotlight, menus, etc. */
#define WIN_POPUP 1
/* WIN_OVERLAY: a popup the compositor draws ABOVE the dock with a full-screen dim
 * scrim behind it (the Launchpad grid). Set together with WIN_POPUP. */
#define WIN_OVERLAY 2
/* WIN_MODAL: an ordinary decorated window the compositor keeps topmost + focused with
 * a dim scrim over everything behind it; input outside it is swallowed so the windows
 * it covers are inert (the Files Open/Save picker). Unlike WIN_OVERLAY it is NOT a
 * popup -- it keeps its title bar and is dialog-shaped, not full-screen. */
#define WIN_MODAL 4

struct wininfo {              /* SYS_WIN_CREATE: in = w,h,title,flags; out = id,vaddr,pitch */
    uint32_t w, h;            /* in:  surface size in pixels        */
    uint64_t vaddr;           /* out: user pointer to pixel (0,0)   */
    uint32_t pitch;           /* out: pixels per row (== w)         */
    int32_t  id;              /* out: window id (>=0), or <0 on error */
    char     title[32];       /* in:  window title                  */
    uint32_t flags;           /* in:  WIN_* (0 = an ordinary window) */
};

#define WEV_KEY    1          /* key DOWN; a = WEV_KEY_PACK(byte, mods): byte in bits 0-7,
                               * KMOD_* modifier flags in bits 8-11 (compositor packs the
                               * live modifier state). Legacy readers can mask `a & 0xff`. */
#define WEV_CLOSE  2          /* the compositor asked the window to close               */
#define WEV_RESIZE 3          /* winevent.a = (w << 16) | h : new client size requested */
#define WEV_MOUSE  4          /* a click in the client area; a = WEV_MOUSE_PACK(...)    */
#define WEV_NAV    5          /* a navigation gesture: a = 0 back, 1 forward (mouse side buttons) */
#define WEV_SCROLL 6          /* scroll wheel over the client: a = WEV_MOUSE_PACK(x,y,delta&0xff), delta signed (+ up) */
#define WEV_KEYUP  7          /* a modifier key was released; a = the new KMOD_* mask still held */
#define WEV_MENU   8          /* an app menu item was chosen; a = WEV_MENU_PACK(menu, item)     */
#define WEV_DRAG   9          /* a drag is hovering over this window mid-session; a = WEV_MOUSE_PACK(x,y,0),
                               * (0xfff,0xfff) on leave -- the target highlights its drop zone.  */
#define WEV_DROP   10         /* a drag was released on this window; a = WEV_MOUSE_PACK(x,y,mods): the
                               * local drop point + KMOD_* in the button field. The app reads the
                               * payload with drag_payload() and acts (move files / insert text).  */
#define WEV_MENU_PACK(m, i) ((((unsigned)(m) & 0xff) << 8) | ((unsigned)(i) & 0xff))
#define WEV_MENU_M(a) (((a) >> 8) & 0xff)
#define WEV_MENU_I(a) ((a) & 0xff)
#define WEV_KEY_PACK(byte, mods) (((unsigned)(byte) & 0xff) | (((unsigned)(mods) & 0xf) << 8))
#define WEV_KEY_BYTE(a) ((a) & 0xff)
#define WEV_KEY_MODS(a) (((a) >> 8) & 0xf)
/* WEV_MOUSE packs a click into the single event word: client-relative x,y (12
 * bits each) and the button bitmask (bit0 left). The compositor posts one on
 * each press edge inside a window's client area. */
/* WEV_MOUSE button-field bits: bit0 = left, bit1 = right (context menu). The
 * compositor also sets WEV_MOUSE_DRAG on a button-HELD move (a drag), so an app
 * can tell a drag-extend from a fresh press (used for terminal/Files selection).
 * A pure pointer move (hover) arrives with the whole button field 0. */
#define WEV_MOUSE_DRAG 0x40
/* bit2 = middle button. The compositor forwards a middle-PRESS edge inside a
 * window's client area so a widget can paste the X11-style primary selection. */
#define WEV_MOUSE_MID  0x04
/* The compositor also ORs WEV_MOUSE_SHIFT into a PRESS's button field when Shift
 * was held at the click, so a widget can shift-extend a selection (Shift+click). */
#define WEV_MOUSE_SHIFT 0x80
#define WEV_MOUSE_PACK(x, y, b) ((((unsigned)(x) & 0xfff) << 20) | (((unsigned)(y) & 0xfff) << 8) | ((unsigned)(b) & 0xff))
#define WEV_MOUSE_X(a)   (((a) >> 20) & 0xfff)
#define WEV_MOUSE_Y(a)   (((a) >> 8)  & 0xfff)
#define WEV_MOUSE_BTN(a) ((a) & 0xff)
struct winevent {
    uint32_t type;
    uint32_t a;
};

struct wmwin {                /* SYS_WM_WINDOWS snapshot entry (compositor side) */
    int32_t  id;
    uint32_t w, h;
    uint64_t vaddr;           /* surface pointer in the compositor's address space */
    uint32_t seq;             /* present sequence; changes when the app draws      */
    char     title[32];
    uint32_t flags;           /* WIN_* the app created the window with              */
    /* Damage rect (surface-relative) accumulated since the last snapshot: the only
     * region the app repainted this present. dmgw==0 ⇒ no partial info (treat as the
     * whole surface). The compositor composites just this rect instead of the whole
     * client area, so high-frequency partial repaints (hover, caret, typing) are
     * cheap. Reset by the kernel each snapshot. */
    uint32_t dmgx, dmgy, dmgw, dmgh;
    uint32_t cursor;          /* app-declared cursor hint (SYS_WIN_SETCURSOR), CUR_* id */
};

/* An app's menu bar (SYS_WIN_SETMENU): a handful of top-level menus (File / Edit /
 * View / Help) each with a few items. The compositor reads it for the focused
 * window (SYS_WM_GETMENU), draws a tile per menu in the bar, and posts WEV_MENU
 * (packed menu+item index) back to the app when an item is chosen. nmenus==0 ⇒ the
 * app has no menu (the bar shows only the universal About/Quit). */
#define WINMENU_MAX   5       /* top-level menus                                   */
#define WINMENU_ITEMS 12      /* items per menu (Edit carries Copy..Undo/Redo)     */
#define WINMENU_LBL   18      /* label length (incl. NUL)                          */
/* Per-item state flags (winmenu.m[].flags[]). An app sets these when it declares
 * the menu; the compositor greys WMI_DISABLED rows (and ignores clicks/accels on
 * them) and draws a check mark beside WMI_CHECKED rows. */
#define WMI_DISABLED  0x01    /* item is greyed out and non-actionable             */
#define WMI_CHECKED   0x02    /* item shows a leading check mark (a toggle is on)   */
struct winmenu {
    uint32_t nmenus;
    struct {
        char     title[WINMENU_LBL];
        uint32_t nitems;
        char     items[WINMENU_ITEMS][WINMENU_LBL];
        uint8_t  flags[WINMENU_ITEMS];  /* WMI_* per item (0 = normal)              */
        char     accel[WINMENU_ITEMS];  /* Ctrl-accelerator letter, 0 = none        */
    } m[WINMENU_MAX];
};

struct sysinfo {              /* SYS_SYSINFO: machine facts for fastfetch */
    uint32_t ncpu;
    uint32_t timer_hz;
    uint64_t ram_bytes;
    uint64_t uptime_ticks;
    uint32_t fb_w, fb_h;
    uint32_t nfiles;
    uint32_t ntasks;          /* live tasks */
};
