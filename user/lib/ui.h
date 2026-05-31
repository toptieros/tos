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
    UK_UP = 0x100, UK_DOWN, UK_LEFT, UK_RIGHT, UK_DEL, UK_HOME, UK_END
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
    virtual ~Widget() {}
    virtual void draw() {}
    virtual bool on_mouse(int x, int y, int btn) { (void)x; (void)y; (void)btn; return false; }
    virtual void on_drag(int x, int y) { (void)x; (void)y; }     /* button-held move over this widget */
    virtual void on_button_up() {}                              /* pointer button released (end a drag) */
    virtual bool on_key(int key) { (void)key; return false; }   /* true if consumed */
    /* pointer moved over the widget (absolute coords); return true if its look
     * changed and the window must repaint. The base hover/leave (the `hovered`
     * flag) is managed by Window; override only for sub-element hover (rows etc.). */
    virtual bool on_hover(int x, int y) { (void)x; (void)y; return false; }
    virtual void on_leave() { hovered = false; }
    /* scroll wheel over the widget; delta>0 = wheel up. Return true if it scrolled. */
    virtual bool on_scroll(int delta) { (void)delta; return false; }
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
    void (*on_select)(void *ctx, int index)   = nullptr;
    void (*on_activate)(void *ctx, int index) = nullptr;   /* double-click / Enter */
    ListView();
    void draw() override;
    bool on_mouse(int x, int y, int btn) override;
    bool on_key(int key) override;
    bool on_hover(int x, int y) override;
    void on_leave() override;
    bool on_scroll(int delta) override;
    int  rows_visible() const { return r.h / row_h; }
    void ensure_visible(int i);
    int  hover_row = -1;            /* row under the pointer (-1 none): faint hover layer */
private:
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
    bool        on_key(int key) override;
    bool        on_mouse(int x, int y, int btn) override;
    void        on_drag(int x, int y) override { if (sb_drag) sb_set_top_from_y(y); else drag_to(x, y); }
    void        on_button_up() override { sb_drag = false; }       /* end a scrollbar drag */
    bool        on_scroll(int delta) override;                     /* multiline: scroll the viewport */
    void        drag_to(int x, int y);
    int         caret = 0;
private:
    char *buf = nullptr;
    int   len = 0, cap = 0;
    int   anchor = -1;              /* selection anchor (-1 = none) */
    int   top = 0, hoff = 0;        /* multiline vertical / single-line horizontal scroll */
    bool  sb_drag = false;          /* dragging the right-edge scroll indicator (#12) */
    int   sb_sy = 0, sb_sh = 0, sb_thumbh = 0, sb_maxtop = 0;  /* thumb geometry, cached by draw() */
    int   cols_cache = 1;
    int   last_caret = -1;          /* snap the view to the caret only when it moves (free wheel-scroll) */
    void  ensure(int need);
    void  ins(const char *s, int n);
    void  del_range(int a, int b);
    bool  has_sel() const { return anchor >= 0 && anchor != caret; }
    void  sel_bounds(int &a, int &b) const;
    void  drop_sel_if(bool shift);
    void  copy_sel(bool cut);
    void  paste();
    int   index_at(int px, int py);         /* pixel -> caret index */
    /* scroll-indicator (#12): geometry of the right-edge thumb (false if not
     * scrollable), the px hit-test for the thumb strip, and mapping a pointer y to
     * the scroll position while dragging it. */
    bool  sb_geom(int &sy, int &sh, int &thumby, int &thumbh, int &maxtop);
    bool  in_scrollbar(int px) const { return px >= r.x + r.w - 8; }
    void  sb_set_top_from_y(int py);
    void  changed();
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
    bool      popup = false;        /* WIN_POPUP: borderless centred overlay (set before create()) */
    bool      overlay = false;      /* WIN_OVERLAY: drawn above the dock with a dim scrim (Launchpad) */
    unsigned  ticks = 0;

    Window();
    bool create(int w, int h, const char *title);
    void add(Widget *c);
    void invalidate() { dirty = true; }
    int  run();                                 /* event loop until closed */

    virtual void on_resize(int nw, int nh) { (void)nw; (void)nh; }
    virtual void on_key(int key) { (void)key; } /* keys when nothing is focused */
    virtual void on_nav(int dir) { (void)dir; } /* WEV_NAV: 0 = back, 1 = forward (mouse side buttons) */
    virtual void on_context(int x, int y) { (void)x; (void)y; }  /* right-click (client-relative) */
    /* A button-held drag (WEV_MOUSE_DRAG) over the client area. Default: ignored,
     * so ordinary widgets don't fire on every drag step. Apps that want rubber-band
     * selection (Files) override this. */
    virtual void on_drag(int x, int y, int btn) { (void)x; (void)y; (void)btn; }
    virtual void on_scroll(int delta) { (void)delta; }   /* wheel not over any scrollable widget */

protected:
    void redraw();
    void dispatch_mouse(int x, int y, int btn);
    void dispatch_hover(int x, int y);          /* pointer move (btn==0) from the compositor */
    void dispatch_scroll(int x, int y, int delta);   /* wheel: route to the widget under the cursor */
    void feed_key(int byte);
    Vec<Widget *> kids;
    Widget *hot = nullptr;                      /* widget currently under the pointer */
    int esc = 0;                                /* ESC-sequence decoder state */
};

} // namespace ui
