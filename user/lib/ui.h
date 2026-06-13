/* tOS UI toolkit -- a small Qt-like retained-mode widget layer in freestanding
 * C++ on top of the C ugfx rasterizer. A Window owns a shared-memory surface and
 * an event loop; widgets are a flat list drawn into that surface (absolute,
 * client-relative coordinates) and styled from theme.h. The compositor (twm)
 * composites the surface like any other window, so toolkit apps get the desktop
 * chrome (min/max/close, drag, resize) for free.
 *
 * Callbacks are plain function pointers + a ctx pointer; apps use non-capturing
 * lambdas (which convert to function pointers) with ctx = this. No STL, no
 * exceptions, no RTTI. */
#pragma once
#include <stdint.h>
#include "ulib.h"     /* syscalls + libc + sys conveniences (extern "C") */
#include "ugfx.h"     /* the rasterizer (extern "C") */
#include "theme.h"    /* shared design tokens (TH_*) */

/* caret on/off period, in event-loop ticks (~0.5s). Shared: TextField phases its
 * caret by it, and Window::redraw repaints a focused caret on its boundary. */
#define TF_BLINK 32

namespace ui {

typedef uint32_t Color;

/* a tiny dynamic array (elements must be trivially copyable: pointers / PODs) */
template <typename T>
class Vec {
    T  *d = nullptr;
    int n = 0, cap = 0;
public:
    int  size() const { return n; }
    T   &operator[](int i) { return d[i]; }
    void push(const T &v) {
        if (n == cap) { cap = cap ? cap * 2 : 8; d = (T *)realloc(d, (size_t)cap * sizeof(T)); }
        d[n++] = v;
    }
    void clear() { n = 0; }
};

struct Rect {
    int x, y, w, h;
    bool has(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

/* decoded key codes delivered to on_key(); printable keys arrive as their ASCII */
enum {
    UK_BACK = 8, UK_ENTER = 13, UK_ESC = 27,
    UK_UP = 0x100, UK_DOWN, UK_LEFT, UK_RIGHT, UK_DEL, UK_HOME, UK_END,
    UK_WORD_LEFT, UK_WORD_RIGHT,    /* Ctrl+Left/Right: jump word-by-word        */
    UK_WORD_DEL                     /* Ctrl+Delete: delete the word to the right  */
};

class Window;

/* ------------------------------------------------------------------- Widget */
class Widget {
public:
    Rect    r{0, 0, 0, 0};
    bool    visible = true;
    Window *win = nullptr;          /* set when added to a window */
    bool    focusable = false;      /* whether a click gives this widget key focus */
    bool    hovered  = false;       /* pointer is over this widget (drives state layers) */
    int     cursor = CUR_ARROW;     /* pointer shape over this widget (CUR_*): the Window
                                     * relays it to the compositor on hover, so e.g. every
                                     * TextField shows an I-beam with zero per-app code */
    virtual ~Widget() {}
    virtual void draw() {}
    virtual bool on_mouse(int x, int y, int btn) { (void)x; (void)y; (void)btn; return false; }
    virtual void on_drag(int x, int y) { (void)x; (void)y; }     /* button-held move over this widget */
    virtual void on_button_up() {}                              /* pointer button released (end a drag) */
    virtual bool on_key(int key, bool shift = false) { (void)key; (void)shift; return false; }  /* shift: held-Shift (selection extend); true if consumed */
    /* per-widget undo/redo (the global text contract: any editable widget can
     * implement these and inherit Ctrl+Z/Ctrl+Y + the menu items for free). */
    virtual bool undo() { return false; }
    virtual bool redo() { return false; }
    /* pointer moved over the widget (absolute coords); return true if its look
     * changed and the window must repaint. The base hover/leave (the `hovered`
     * flag) is managed by Window; override only for sub-element hover (rows etc.). */
    virtual bool on_hover(int x, int y) { (void)x; (void)y; return false; }
    virtual void on_leave() { hovered = false; }
    /* scroll wheel over the widget; delta>0 = wheel up. Return true if it scrolled. */
    virtual bool on_scroll(int delta) { (void)delta; return false; }
    /* A DRAG_TEXT drop landed on this widget (Window::on_drop routes it here); insert
     * the text and return true if accepted. Default: not a drop target. */
    virtual bool accept_text_drop(int x, int y, const char *s, int n) { (void)x; (void)y; (void)s; (void)n; return false; }
    /* the pointer shape at (x,y) inside this widget; defaults to the static `cursor`
     * field. Override for sub-element shapes (a header's ⇔ only over its dividers). */
    virtual int cursor_at(int x, int y) { (void)x; (void)y; return cursor; }
    /* true if this widget draws a blinking caret while focused; Window pulses a
     * repaint at the blink cadence so the caret keeps blinking when idle. */
    virtual bool shows_caret() const { return false; }
};

/* -------------------------------------------------------------------- Label */
class Label : public Widget {
public:
    const char *text = "";
    Color fg;
    int   align = 0;                /* 0 left, 1 centre, 2 right */
    Label();
    void draw() override;
};

/* ------------------------------------------------------------------- Button */
class Button : public Widget {
public:
    const char *text = "";
    void (*on_click)(void *) = nullptr;
    void  *ctx = nullptr;
    bool   enabled = true;
    /* Optional leading glyph (a white alpha mask -- e.g. a baked Lucide icon from
     * glyphs.h -- recoloured to icon_tint via ugfx_blit_tint) and a right-aligned
     * `value` string. When either is set the button left-aligns its label for a
     * settings-row look ([icon] label .......... value); with neither it stays a
     * plain centred button, so existing callers are unaffected. */
    const uint32_t *icon = nullptr;
    int             icon_sz = 0;
    Color           icon_tint;
    const char     *value = nullptr;
    Color           value_fg;
    Button();
    void draw() override;
    bool on_mouse(int x, int y, int btn) override;
};

/* -------------------------------------------------------------------- Panel */
/* a flat filled rectangle with an optional 1px bottom separator (toolbars, bars) */
class Panel : public Widget {
public:
    Color color;
    bool  sep_bottom = false;
    Color sep;
    Panel();
    void draw() override;
};

/* ----------------------------------------------------------------- TextView */
/* read-only word/char-wrapped monospace text, clipped to its rect (file viewer) */
class TextView : public Widget {
public:
    const char *text = nullptr;
    int   len = 0;
    Color bg, fg;
    TextView();
    void draw() override;
};

/* ---------------------------------------------------------------- ScrollBar */
/* The one scrollbar used everywhere. A scroll container embeds it, calls set()
 * each draw with its viewport rect + content metrics (in row units), then draw().
 * hit()/top_from_y() turn a press or drag on the right-edge track into a new top.
 * Renders through ugfx_scroll_thumb so every scrollbar in the OS looks identical. */
struct ScrollBar {
    static const int STRIP = 8;     /* hit-test width hugging the content's right edge */
    bool dragging = false;
    Rect area{0, 0, 0, 0};          /* the content rect; the bar rides its right edge   */
    int  top = 0, total = 1, vis = 1, maxtop = 0;
    void set(Rect content, int top_, int total_, int vis_) {
        area = content; top = top_;
        total = total_ < 1 ? 1 : total_;
        vis   = vis_   < 1 ? 1 : vis_;
        maxtop = total - vis; if (maxtop < 0) maxtop = 0;
    }
    bool needed() const { return total > vis && maxtop > 0; }
    bool hit(int px) const { return needed() && px >= area.x + area.w - STRIP; }
    void draw() const {
        if (!needed()) return;
        int w = dragging ? 6 : 4;
        ugfx_scroll_thumb(area.x + area.w - w - 2, area.y + 2, w, area.h - 4,
                          top, total, vis, dragging);
    }
    int top_from_y(int py) const {  /* pointer y on the track -> clamped top (thumb-centred) */
        int sy = area.y + 2, sh = area.h - 4;
        int th = sh * vis / total; if (th < 16) th = 16; if (th > sh) th = sh;
        int travel = sh - th; if (travel <= 0) return top;
        int yy = py - sy - th / 2; if (yy < 0) yy = 0; if (yy > travel) yy = travel;
        int nt = (int)((long)yy * maxtop / travel);
        if (nt < 0) nt = 0; if (nt > maxtop) nt = maxtop;
        return nt;
    }
};

/* ----------------------------------------------------------------- ListView */
/* The toolkit handles layout, scrolling, selection and hit-testing; the app
 * draws each row via render_row (so the list stays content-agnostic). */
class ListView : public Widget {
public:
    int   count = 0;                /* app sets the number of rows */
    int   row_h = 26;
    int   sel = -1;
    int   top = 0;                  /* first visible row (scroll) */
    Color bg, sel_bg;
    void *ctx = nullptr;
    void (*render_row)(void *ctx, int index, Rect cell, bool selected) = nullptr;
    /* Optional multi-selection predicate: when set, it (not `sel`) decides whether a
     * row draws selected, so a FileView can show a whole selection set (filesel.h).
     * nullptr keeps the legacy single-`sel` highlight (Spotlight, split panes). */
    bool (*is_sel)(void *ctx, int index) = nullptr;
    void (*on_select)(void *ctx, int index)   = nullptr;
    void (*on_activate)(void *ctx, int index) = nullptr;   /* double-click / Enter */
    ListView();
    void draw() override;
    bool on_mouse(int x, int y, int btn) override;
    bool on_key(int key, bool shift = false) override;
    bool on_hover(int x, int y) override;
    void on_leave() override;
    bool on_scroll(int delta) override;
    void on_drag(int x, int y) override;        /* drag the shared scroll thumb (when focused) */
    void on_button_up() override { sb.dragging = false; }
    int  rows_visible() const { return r.h / row_h; }
    void ensure_visible(int i);
    int  hover_row = -1;            /* row under the pointer (-1 none): faint hover layer */
private:
    ScrollBar sb;                  /* the shared scrollbar (Files + Spotlight get a real thumb) */
    unsigned last_tick = 0;
    int      last_row = -1;
};

/* ---------------------------------------------------------------- TextField */
/* An editable text box: a caret, click-to-place + click-drag selection, the usual
 * editing keys, and clipboard chords over the kernel ring -- Ctrl+C/X/V arrive as
 * the C0 controls ^C/^X/^V (0x03/0x18/0x16), Ctrl+A as ^A select-all. `multiline`
 * char-wraps and scrolls vertically; single-line scrolls horizontally and submits
 * on Enter. Backs notepad's editor and the Spotlight search box. */
class TextField : public Widget {
public:
    bool   multiline = false;
    bool   want_drag = true;        /* receive drag-select packets (on_drag wiring is in Window) */
    int    radius = TH_R_SM;        /* corner radius; search boxes set TH_R_PILL for a Google-style pill */
    Color  bg, fg;
    void  *ctx = nullptr;
    void (*on_submit)(void *) = nullptr;    /* single-line: Enter           */
    void (*on_change)(void *) = nullptr;    /* text edited                  */
    TextField();
    ~TextField() override;
    void        set_text(const char *s);
    const char *text() const { return buf ? buf : ""; }
    int         length() const { return len; }
    void        draw() override;
    bool        on_key(int key, bool shift = false) override;
    bool        on_mouse(int x, int y, int btn) override;
    void        on_drag(int x, int y) override { if (sb.dragging) sb_set_top_from_y(y); else drag_to(x, y); }
    void        on_button_up() override;         /* end a scrollbar drag, or a drag-the-selection-out gesture */
    bool        accept_text_drop(int x, int y, const char *s, int n) override;  /* DRAG_TEXT drop: insert at the point */
    bool        paste_primary(int x, int y);     /* middle-click: paste the X11-style primary selection at the point */
    bool        on_scroll(int delta) override;                     /* multiline: scroll the viewport */
    bool        shows_caret() const override { return visible; }    /* keeps the caret blinking when idle */
    bool        force_focus = false;        /* draw as focused (caret + accent edge) even when the
                                             * window's focus is elsewhere -- for a field embedded in
                                             * a modal that owns focus and forwards keys here. */
    void        drag_to(int x, int y);
    /* Undo / redo (global text contract): two bounded stacks of insert/delete
     * span records; a run of single-char typing or backspacing coalesces into one
     * step; a fresh edit clears the redo stack. */
    bool        undo() override;
    bool        redo() override;
    bool        can_undo() const { return un > 0; }
    bool        can_redo() const { return rn > 0; }
    int         caret = 0;
private:
    char *buf = nullptr;
    int   len = 0, cap = 0;
    int   anchor = -1;              /* selection anchor (-1 = none) */
    int   top = 0, hoff = 0;        /* multiline vertical / single-line horizontal scroll */
    ScrollBar sb;                   /* the shared scroll thumb (#12, now the global ScrollBar) */
    int   cols_cache = 1;
    int   last_caret = -1;          /* snap the view to the caret only when it moves (free wheel-scroll) */
    unsigned last_click_t = 0;      /* tick of the last press, for double-click word-select */
    int   last_click_i = -1;        /* caret index of the last press                         */
    bool  press_in_sel = false;     /* the press landed inside the current selection (maybe drag it out) */
    bool  drag_armed = false;       /* a DRAG_TEXT drag-out has been armed for this gesture   */
    void  ensure(int need);
    void  ins(const char *s, int n);
    void  del_range(int a, int b);
    bool  has_sel() const { return anchor >= 0 && anchor != caret; }
    void  sel_bounds(int &a, int &b) const;
    void  drop_sel_if(bool shift);
    void  copy_sel(bool cut);
    void  paste();
    int   index_at(int px, int py);         /* pixel -> caret index */
    /* scrollbar (#12, now the shared ScrollBar): press hit-test on the right-edge
     * strip, and mapping a pointer y on the track to the scroll `top`. */
    bool  in_scrollbar(int px) const { return sb.hit(px); }
    void  sb_set_top_from_y(int py);
    void  select_word(int idx);             /* double-click: select the word around idx */
    int   word_prev(int i) const;           /* Ctrl+Left:  start of the previous word    */
    int   word_next(int i) const;           /* Ctrl+Right: start of the next word        */
    void  changed();
    /* --- edit history (undo/redo) ----------------------------------------- */
    struct Edit { uint8_t op; int pos, n, caret0; char *txt; };  /* op 0 = insert, 1 = delete */
    static const int UNDO_MAX = 64;         /* bounded ring; oldest steps drop off */
    Edit *ust = nullptr; int un = 0;        /* undo stack (lazily allocated)       */
    Edit *rst = nullptr; int rn = 0;        /* redo stack                          */
    Edit *cur_stk = nullptr; int *cur_n = nullptr;   /* where record() pushes      */
    bool  applying = false;                 /* inside undo()/redo(): don't coalesce/clear-redo */
    bool  brk = true;                       /* a click/jump broke the coalesce run */
    void  hist_init();                      /* allocate the two stacks on first edit */
    void  hist_push(Edit *stk, int &n, uint8_t op, int pos, const char *txt, int cnt, int caret0);
    void  hist_clear(Edit *stk, int &n);
    bool  hist_coalesce(uint8_t op, int pos, const char *txt, int cnt);
    void  record(uint8_t op, int pos, const char *txt, int cnt, int caret0);
    bool  pop_apply(Edit *from, int &fn, Edit *to, int &tn);
};

/* ------------------------------------------------------------ ConfirmDialog */
/* A modal confirmation sheet: a dim scrim over the window + a centred card with a
 * title, a message, and up to three buttons (e.g. Save / Discard / Cancel). Add it
 * LAST to a Window (so it draws on top and grabs every click), then call show();
 * on_choice(ctx, idx) fires with the button index, or -1 for Esc/Cancel. While open
 * it captures keyboard focus (Enter = the primary button, Esc = cancel) so the rest
 * of the window is inert. Reused by Notepad's unsaved-changes guard and the file
 * dialog's overwrite warning. */
class ConfirmDialog : public Widget {
public:
    bool  open = false;
    void *ctx = nullptr;
    void (*on_choice)(void *, int) = nullptr;
    ConfirmDialog() { visible = false; focusable = true; }
    /* `b0` is the primary (default) button; b1/b2 optional. Pass cancel last so Esc
     * maps to it -- show() records the index of a button labelled "Cancel" as the
     * Esc target (else -1). */
    void show(const char *title, const char *msg,
              const char *b0, const char *b1 = nullptr, const char *b2 = nullptr);
    void dismiss();
    void draw() override;
    bool on_mouse(int x, int y, int btn) override;
    bool on_key(int key, bool shift = false) override;
    bool on_hover(int x, int y) override;
    void on_leave() override { hovered = false; hover = -1; }
private:
    char    title[48] = {0}, msg[192] = {0}, btn[3][20] = {{0}};
    int     nbtn = 0, hover = -1, esc_btn = -1;
    Rect    card{0,0,0,0}, brect[3];
    Widget *prev_focus = nullptr;
    void    layout();
    int     btn_at(int x, int y) const;
    void    choose(int idx);
};

/* ------------------------------------------------------------------- Window */
class Window {
public:
    int       id = -1;
    uint32_t *surf = nullptr;
    int       w = 0, h = 0;
    char      title[32] = {0};
    Color     bg;
    Widget   *focus = nullptr;
    bool      running = true, dirty = true;
    /* Damage tracking: a frame's repaint is either the WHOLE surface (dmg_full) or
     * the union of the invalidated widget rects (dmg). High-frequency local changes
     * -- hover state layers, the blinking caret, typing -- invalidate only their
     * widget's rect so redraw() composites + presents just that region (see
     * win_present_rect), instead of re-blitting the whole client area every frame. */
    bool      dmg_full = true;
    Rect      dmg{0, 0, 0, 0};
    bool      popup = false;        /* WIN_POPUP: borderless centred overlay (set before create()) */
    bool      overlay = false;      /* WIN_OVERLAY: drawn above the dock with a dim scrim (Launchpad) */
    bool      modal = false;        /* WIN_MODAL: decorated dialog kept topmost + input-locked, dim scrim behind (the picker) */
    unsigned  ticks = 0;

    Window();
    bool create(int w, int h, const char *title);
    void add(Widget *c);
    void invalidate() { dirty = true; dmg_full = true; }     /* repaint the whole surface */
    /* Repaint only this rect (unioned with any other partial damage this frame).
     * A bare invalidate() anywhere in the frame promotes it to a full repaint. */
    void invalidate(int x, int y, int w, int h) {
        dirty = true;
        if (dmg_full) return;
        if (dmg.w == 0 || dmg.h == 0) { dmg = { x, y, w, h }; return; }
        int x0 = dmg.x, y0 = dmg.y, x1 = dmg.x + dmg.w, y1 = dmg.y + dmg.h;
        if (x < x0) x0 = x; if (y < y0) y0 = y;
        if (x + w > x1) x1 = x + w; if (y + h > y1) y1 = y + h;
        dmg = { x0, y0, x1 - x0, y1 - y0 };
    }
    void invalidate(const Rect &rc) { invalidate(rc.x, rc.y, rc.w, rc.h); }
    int  run();                                 /* event loop until closed */

    virtual void on_resize(int nw, int nh) { (void)nw; (void)nh; }
    /* Called once per event-loop iteration with the running tick count (the loop
     * sleeps ~15 ms between ticks). Default no-op; apps override for periodic work
     * like Notepad's session autosave. Keep it cheap -- it runs every frame. */
    virtual void on_tick(unsigned t) { (void)t; }
    /* The compositor's close button (WEV_CLOSE). Return true to let the window close
     * (the default); return false to veto it -- e.g. raise an unsaved-changes guard
     * first and close later by clearing `running`. */
    virtual bool on_close() { return true; }
    virtual void on_key(int key) { (void)key; } /* keys when nothing is focused */
    virtual void on_nav(int dir) { (void)dir; } /* WEV_NAV: 0 = back, 1 = forward (mouse side buttons) */
    virtual void on_context(int x, int y) { (void)x; (void)y; }  /* right-click (client-relative) */
    /* A left-button press, before it is routed to a child widget. Default: ignored.
     * Apps override to capture where a gesture began (e.g. Files notes which Favorites
     * row a press landed on so a subsequent drag reorders it, not the file list). */
    virtual void on_press(int x, int y, int btn) { (void)x; (void)y; (void)btn; }
    /* A button-held drag (WEV_MOUSE_DRAG) over the client area. Default: ignored,
     * so ordinary widgets don't fire on every drag step. Apps that want rubber-band
     * selection (Files) override this. */
    virtual void on_drag(int x, int y, int btn) { (void)x; (void)y; (void)btn; }
    /* The pointer button was released (mirrors on_press). Default: ignored. Apps that
     * track a gesture across the whole press..release (Files' rubber-band) override it
     * to finalise/clear that state. Fires on every release, drag or not. */
    virtual void on_release() {}
    virtual void on_scroll(int delta) { (void)delta; }   /* wheel not over any scrollable widget */
    /* Drag-and-drop (design/files-and-desktop.md). A source arms a typed payload with
     * begin_drag() from its on_drag (once the gesture leaves a draggable item); the
     * compositor draws the ghost + routes the drop. on_drag_over() fires while a drag
     * hovers this window so it can highlight a drop zone (x<0 == the drag left); on_drop()
     * delivers the payload on release -- read `data`/`len` (typed by DRAG_*) and act. */
    void begin_drag(int type, const char *label, const void *data, int len) { drag_begin(type, label, data, len); }
    int  drop_modifiers() const { return drop_mods; }   /* KMOD_* on the in-flight WEV_DROP (copy-on-Ctrl) */
    virtual void on_drag_over(int x, int y) { (void)x; (void)y; }
    /* Default: a DRAG_TEXT drop is routed to the topmost widget under (x,y) that
     * accepts text (any TextField). Apps override to handle other payload types. */
    virtual void on_drop(int x, int y, int type, const void *data, int len);
    /* App menu bar (design/ui.md #6): declare top-level menus the compositor shows
     * as bar tiles; on_menu() fires when the user picks an item. Build with
     * menu_begin / menu_add / menu_item, then menu_commit(). */
    void menu_begin();                           /* start a fresh menu spec            */
    int  menu_add(const char *title);            /* add a top-level menu -> its index  */
    /* Add an item. `accel` is the Ctrl-shortcut letter shown + handled by the
     * compositor (e.g. 'S' => Ctrl+S; 0 = none); `flags` are WMI_* (disabled/checked). */
    void menu_item(int menu, const char *label, char accel = 0, unsigned flags = 0);
    void menu_set_checked(int menu, int item, bool on);  /* flip WMI_CHECKED + re-publish */
    void menu_set_enabled(int menu, int item, bool on);  /* flip WMI_DISABLED + re-publish */
    bool menu_is_checked(int menu, int item) const;
    void menu_commit();                          /* publish to the compositor          */
    virtual void on_menu(int menu, int item) { (void)menu; (void)item; }  /* item chosen */

protected:
    struct winmenu menu_spec;                    /* built by menu_*(), sent by menu_commit() */
    void redraw();
    virtual void dispatch_mouse(int x, int y, int btn);   /* apps may hook click routing (e.g. click-away dismiss) */
    void dispatch_hover(int x, int y);          /* pointer move (btn==0) from the compositor */
    void dispatch_scroll(int x, int y, int delta);   /* wheel: route to the widget under the cursor */
    void feed_key(int byte);
    Vec<Widget *> kids;
    Widget *hot = nullptr;                      /* widget currently under the pointer */
    int cur_shape = CUR_ARROW;                  /* cursor hint last sent to the compositor */
    int drop_mods = 0;                          /* modifier mask on the in-flight WEV_DROP (copy-on-Ctrl) */
    int esc = 0;                                /* ESC-sequence decoder state */
    int esc_pend = 0;                           /* event-loop drains a lone ESC has survived */
    char csi[8] = {0};                          /* CSI parameter bytes (e.g. "1;5" for Ctrl) */
    int  csi_n = 0;
};

/* ---------------------------------------------------------------- Layout -----
 * The toolkit's layout system: a linear stack calculator. It positions a sequence
 * of items along a main axis -- a column (LAY_COL) or a row (LAY_ROW) -- inside a
 * bounds rect, with `gap` between items and `pad` inside all four edges. Each item
 * has a main-axis `extent`; extent 0 means "stretch", and the leftover main-axis
 * space is split evenly among the stretch items. On the cross axis items fill the
 * padded bounds. arrange() writes each item's widget `r` in place, so apps keep
 * adding the leaf widgets to the Window as usual (draw + hit-test routing is
 * unchanged) and just call arrange() in layout_widgets() instead of hand-computing
 * every rect. An item with a null widget reserves space only -- query rect_of(i)
 * to use it as the bounds for a nested Layout (a Row of Columns, etc.). */
enum { LAY_COL = 0, LAY_ROW = 1 };
class Layout {
public:
    int dir = LAY_COL, gap = 6, pad = 8;
    Layout() {}
    Layout(int d, int g = 6, int p = 8) : dir(d), gap(g), pad(p) {}
    void reset() { n = 0; }
    Layout &add(Widget *w, int extent = 0) { if (n < MAXI) { it[n].w = w; it[n].ext = extent; n++; } return *this; }
    Layout &space(int extent) { return add(nullptr, extent); }   /* reserve a region / fixed gap */
    Rect rect_of(int i) const { return (i >= 0 && i < n) ? it[i].out : Rect{0, 0, 0, 0}; }
    void arrange(Rect b) {
        int ix = b.x + pad, iy = b.y + pad, iw = b.w - 2 * pad, ih = b.h - 2 * pad;
        if (iw < 0) iw = 0; if (ih < 0) ih = 0;
        int span  = (dir == LAY_ROW) ? iw : ih;
        int avail = span - gap * (n > 0 ? n - 1 : 0);
        int fixed = 0, stretch = 0;
        for (int i = 0; i < n; i++) { if (it[i].ext > 0) fixed += it[i].ext; else stretch++; }
        int slack = avail - fixed; if (slack < 0) slack = 0;
        int each = stretch ? slack / stretch : 0, rem = stretch ? slack % stretch : 0;
        int pos = (dir == LAY_ROW) ? ix : iy;
        for (int i = 0; i < n; i++) {
            int ext;
            if (it[i].ext > 0) ext = it[i].ext;
            else { ext = each; if (rem > 0) { ext++; rem--; } }   /* spread the remainder */
            Rect rc = (dir == LAY_ROW) ? Rect{ pos, iy, ext, ih } : Rect{ ix, pos, iw, ext };
            it[i].out = rc;
            if (it[i].w) it[i].w->r = rc;
            pos += ext + gap;
        }
    }
private:
    static const int MAXI = 24;
    struct Item { Widget *w; int ext; Rect out; };
    Item it[MAXI]; int n = 0;
};

} // namespace ui
