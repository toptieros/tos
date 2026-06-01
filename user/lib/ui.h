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
    void (*on_select)(void *ctx, int index)   = nullptr;
    void (*on_activate)(void *ctx, int index) = nullptr;   /* double-click / Enter */
    ListView();
    void draw() override;
    bool on_mouse(int x, int y, int btn) override;
    bool on_key(int key) override;
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
    bool        on_key(int key) override;
    bool        on_mouse(int x, int y, int btn) override;
    void        on_drag(int x, int y) override { if (sb.dragging) sb_set_top_from_y(y); else drag_to(x, y); }
    void        on_button_up() override { sb.dragging = false; }   /* end a scrollbar drag */
    bool        on_scroll(int delta) override;                     /* multiline: scroll the viewport */
    bool        shows_caret() const override { return visible; }    /* keeps the caret blinking when idle */
    void        drag_to(int x, int y);
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
    char csi[8] = {0};                          /* CSI parameter bytes (e.g. "1;5" for Ctrl) */
    int  csi_n = 0;
};

} // namespace ui
