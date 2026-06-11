/* tOS UI toolkit implementation (see ui.h). Styling comes from theme.h, drawing
 * from the C ugfx backend. */
#include "ui.h"
#include "theme.h"
#include "textutil.h"
#include "editlog.h"

namespace ui {

static int text_cy(Rect r) { return r.y + (r.h - ugfx_font_h()) / 2; }   /* vertically centre text in a row */

/* -------------------------------------------------------------------- Label */
Label::Label() { fg = TH_TEXT; }
void Label::draw() {
    if (!visible || !text) return;
    int tx = r.x;
    if (align == 1) tx = r.x + (r.w - ugfx_text_w(text)) / 2;
    else if (align == 2) tx = r.x + r.w - ugfx_text_w(text);
    ugfx_text(tx, text_cy(r), text, fg, UGFX_TRANSPARENT);
}

/* ------------------------------------------------------------------- Button */
Button::Button() { icon_tint = TH_TEXT; value_fg = TH_MUTED; }
void Button::draw() {
    if (!visible) return;
    int rad = TH_R_SM;
    ugfx_rrect_aa(r.x, r.y, r.w, r.h, rad, enabled ? TH_SURF_3 : TH_SURF_2);
    if (enabled && hovered) ugfx_state_layer(r.x, r.y, r.w, r.h, rad, TH_HOVER_A);  /* hover lift */
    ugfx_rrect_border(r.x, r.y, r.w, r.h, rad, 1, TH_BORDER);                       /* crisp edge */
    ugfx_fill_a(r.x + rad, r.y + 1, r.w - 2 * rad, 1, ARGB(48, 255, 255, 255));     /* top sheen  */
    uint32_t fg = enabled ? TH_TEXT : TH_MUTED;
    if (icon || value) {                          /* settings-row: [icon] label .... value */
        int pad = 12, lx = r.x + pad;
        if (icon && icon_sz) {
            ugfx_blit_tint(lx, r.y + (r.h - icon_sz) / 2, icon_sz, icon_sz, icon, enabled ? icon_tint : TH_MUTED);
            lx += icon_sz + 12;
        }
        ugfx_text(lx, text_cy(r), text, fg, UGFX_TRANSPARENT);
        if (value) {
            int vx = r.x + r.w - pad - ugfx_text_w(value);
            ugfx_text(vx, text_cy(r), value, enabled ? value_fg : TH_MUTED, UGFX_TRANSPARENT);
        }
    } else {
        int tx = r.x + (r.w - ugfx_text_w(text)) / 2;
        ugfx_text(tx, text_cy(r), text, fg, UGFX_TRANSPARENT);
    }
}
bool Button::on_mouse(int x, int y, int btn) {
    (void)x; (void)y; (void)btn;
    if (enabled && on_click) on_click(ctx);
    return true;
}

/* -------------------------------------------------------------------- Panel */
Panel::Panel() { color = TH_CHROME; sep = ARGB(60, 150, 170, 230); }
void Panel::draw() {
    if (!visible) return;
    ugfx_fill(r.x, r.y, r.w, r.h, color);
    if (sep_bottom) ugfx_fill_a(r.x, r.y + r.h - 1, r.w, 1, sep);
}

/* ----------------------------------------------------------------- TextView */
TextView::TextView() { bg = RGB(26, 30, 40); fg = TH_TEXT; }
void TextView::draw() {
    if (!visible) return;
    ugfx_fill(r.x, r.y, r.w, r.h, bg);
    if (!text) return;
    int fw = ugfx_font_w(), fh = ugfx_font_h();
    int cols = fw ? (r.w - 12) / fw : 0;
    int x0 = r.x + 6, y = r.y + 4, col = 0;
    ugfx_set_clip(r.x, r.y, r.w, r.h);
    for (int i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n' || (cols > 0 && col >= cols)) { y += fh; col = 0; if (c == '\n') continue; }
        if (y > r.y + r.h - fh) break;
        if (c == '\r') continue;
        if (c == '\t') { col = (col + 4) & ~3; continue; }
        ugfx_char(x0 + col * fw, y, c, fg, bg);
        col++;
    }
    ugfx_clip_none();
}

/* ----------------------------------------------------------------- ListView */
ListView::ListView() { bg = TH_SURF_0; sel_bg = ARGB(235, 96, 152, 252); focusable = true; }
void ListView::ensure_visible(int i) {
    if (i < top) top = i;
    int vis = rows_visible();
    if (vis > 0 && i >= top + vis) top = i - vis + 1;
    if (top < 0) top = 0;
}
void ListView::draw() {
    if (!visible) return;
    ugfx_fill(r.x, r.y, r.w, r.h, bg);
    int vis = rows_visible();
    for (int k = 0; k < vis; k++) {
        int i = top + k;
        if (i >= count) break;
        Rect cell = { r.x, r.y + k * row_h, r.w, row_h };
        /* selection = an inset accent pill (Finder-style), under the row content so
         * its text reads on top; hover = a faint state layer painted OVER the content
         * (incl. any zebra fill the app draws), as a state layer should sit on top. */
        if (i == sel)
            ugfx_rrect_a(cell.x + TH_SP_XS, cell.y + 1, cell.w - 2 * TH_SP_XS, cell.h - 2, TH_R_SM, sel_bg);
        if (render_row) render_row(ctx, i, cell, i == sel);
        if (i != sel && i == hover_row)
            ugfx_state_layer(cell.x + TH_SP_XS, cell.y + 1, cell.w - 2 * TH_SP_XS, cell.h - 2, TH_R_SM, TH_HOVER_A);
    }
    sb.set(r, top, count, rows_visible()); sb.draw();        /* the shared scroll thumb (Files / Spotlight) */
}
bool ListView::on_mouse(int x, int y, int btn) {
    (void)btn;
    if (sb.hit(x)) { sb.dragging = true; top = sb.top_from_y(y); if (win) win->invalidate(); return true; }  /* scroll track */
    int row = top + (y - r.y) / row_h;
    if (row < 0 || row >= count) return true;
    sel = row;
    if (on_select) on_select(ctx, row);
    unsigned now = win ? win->ticks : 0;
    if (row == last_row && now - last_tick < 26) {       /* double-click -> activate */
        if (on_activate) on_activate(ctx, row);
        last_row = -1;
    } else {
        last_row = row; last_tick = now;
    }
    return true;
}
bool ListView::on_hover(int x, int y) {
    (void)x;
    int row = top + (y - r.y) / row_h;
    if (row < top || row >= count || row >= top + rows_visible()) row = -1;
    if (row == hover_row) return false;
    hover_row = row;
    return true;
}
void ListView::on_leave() { hovered = false; hover_row = -1; }
bool ListView::on_scroll(int delta) {
    int vis = rows_visible(), maxtop = count - vis; if (maxtop < 0) maxtop = 0;
    int nt = top - delta * 3; if (nt < 0) nt = 0; if (nt > maxtop) nt = maxtop;   /* wheel up -> earlier rows */
    if (nt == top) return false;
    top = nt; return true;
}
void ListView::on_drag(int x, int y) {
    (void)x;
    if (sb.dragging) { int nt = sb.top_from_y(y); if (nt != top) { top = nt; if (win) win->invalidate(); } }
}
bool ListView::on_key(int key, bool shift) {
    (void)shift;
    if (key == UK_UP)    { if (sel > 0) { sel--; ensure_visible(sel); if (on_select) on_select(ctx, sel); } return true; }
    if (key == UK_DOWN)  { if (sel + 1 < count) { sel++; ensure_visible(sel); if (on_select) on_select(ctx, sel); } return true; }
    if (key == UK_ENTER || key == '\n') { if (sel >= 0 && on_activate) on_activate(ctx, sel); return true; }
    return false;
}


/* ------------------------------------------------------------------- Window */
Window::Window() { bg = TH_CHROME; }
bool Window::create(int cw, int ch, const char *t) {
    struct wininfo wi;
    wi.w = (uint32_t)cw; wi.h = (uint32_t)ch;
    wi.flags = ((popup || overlay) ? WIN_POPUP : 0) | (overlay ? WIN_OVERLAY : 0) | (modal ? WIN_MODAL : 0);
    int i = 0; for (; t && t[i] && i < 31; i++) { title[i] = t[i]; wi.title[i] = t[i]; }
    title[i] = 0; wi.title[i] = 0;
    id = win_create(&wi);
    if (id < 0) return false;
    surf = (uint32_t *)wi.vaddr; w = (int)wi.w; h = (int)wi.h;
    return true;
}
void Window::add(Widget *c) {
    c->win = this;
    kids.push(c);
    if (!focus) focus = c;
}
static bool rect_overlap(const Rect &a, const Rect &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
void Window::redraw() {
    ugfx_set_target(surf, w, h, w);
    Rect c = dmg;
    bool partial = !dmg_full && c.w > 0 && c.h > 0;
    if (partial) {                                  /* clamp the damage rect to the surface */
        if (c.x < 0) { c.w += c.x; c.x = 0; }
        if (c.y < 0) { c.h += c.y; c.y = 0; }
        if (c.x + c.w > w) c.w = w - c.x;
        if (c.y + c.h > h) c.h = h - c.y;
        if (c.w <= 0 || c.h <= 0) partial = false;
    }
    if (partial) {
        ugfx_set_clip(c.x, c.y, c.w, c.h);
        ugfx_fill(c.x, c.y, c.w, c.h, bg);          /* repaint just the dirty band */
        for (int i = 0; i < kids.size(); i++)
            if (kids[i]->visible && rect_overlap(kids[i]->r, c)) kids[i]->draw();
        ugfx_clip_none();
        win_present_rect(id, c.x, c.y, c.w, c.h);   /* compositor blits only this rect */
    } else {
        ugfx_clip_none();
        ugfx_clear(bg);
        for (int i = 0; i < kids.size(); i++) if (kids[i]->visible) kids[i]->draw();
        win_present(id);
    }
    dmg_full = false; dmg = { 0, 0, 0, 0 };         /* start a fresh accumulation next frame */
}
void Window::dispatch_mouse(int x, int y, int btn) {
    if (btn & 2) { on_context(x, y); dirty = true; return; }   /* right-click -> context menu */
    on_press(x, y, btn);                                  /* app hook: note where a gesture began */
    for (int i = kids.size() - 1; i >= 0; i--) {          /* topmost (last-added) first */
        Widget *c = kids[i];
        if (c->visible && c->r.has(x, y)) {
            if (c->focusable) focus = c;
            c->on_mouse(x, y, btn);
            dirty = true;
            return;
        }
    }
}
/* A pointer move (no button) from the compositor: track which widget is under the
 * cursor so it can draw a hover state layer. The compositor sends an out-of-range
 * coordinate (>= 0xfff) when the pointer leaves the window, which clears the hover. */
void Window::dispatch_hover(int x, int y) {
    Widget *t = nullptr;
    if (x < 0xfff && y < 0xfff)
        for (int i = kids.size() - 1; i >= 0; i--) {          /* topmost first */
            Widget *c = kids[i];
            if (c->visible && c->r.has(x, y)) { t = c; break; }
        }
    if (t != hot) {
        if (hot) { hot->on_leave(); invalidate(hot->r); }    /* clear the old widget's hover layer */
        hot = t;
        if (hot) { hot->hovered = true; invalidate(hot->r); }
    }
    if (hot && hot->on_hover(x, y)) invalidate(hot->r);      /* sub-element hover (rows / tabs) */
    /* relay the hovered widget's cursor hint to the compositor (an I-beam over a
     * text field, ⇔ over a column divider). Deduped, so the common arrow-over-
     * arrow move costs nothing. While a button is held the compositor stops
     * sending hovers, which conveniently freezes the shape for the drag. */
    int cs = hot ? hot->cursor_at(x, y) : CUR_ARROW;
    if (cs != cur_shape) { cur_shape = cs; win_setcursor(id, cs); }
}
void Window::dispatch_scroll(int x, int y, int delta) {
    for (int i = kids.size() - 1; i >= 0; i--) {              /* topmost widget under the cursor */
        Widget *c = kids[i];
        if (c->visible && c->r.has(x, y)) { if (c->on_scroll(delta)) dirty = true; return; }
    }
    on_scroll(delta);                                         /* nothing scrollable -> app-level */
}
void Window::on_drop(int x, int y, int type, const void *data, int len) {
    if (type != DRAG_TEXT || !data || len <= 0) return;
    for (int i = kids.size() - 1; i >= 0; i--) {              /* topmost widget under the drop */
        Widget *c = kids[i];
        if (c->visible && c->r.has(x, y) && c->accept_text_drop(x, y, (const char *)data, len)) {
            dirty = true; return;
        }
    }
}
void Window::feed_key(int b) {
    int key = -1;
    bool shift = false;                                /* set from the CSI modifier param (Shift => bit 0) */
    if (esc == 0) {
        if (b == 27) { esc = 1; return; }
        key = b;
    } else if (esc == 1) {
        if (b == '[') { esc = 2; csi_n = 0; return; }
        esc = 0; key = b;
    } else { /* esc == 2: collect CSI parameters (digits + ';'), then a final byte */
        if ((b >= '0' && b <= '9') || b == ';') {
            if (csi_n < (int)sizeof(csi) - 1) csi[csi_n++] = (char)b;
            return;
        }
        esc = 0; csi[csi_n] = 0;
        int mod = 0;                                   /* xterm modifier param after ';' (Ctrl => 5) */
        for (int i = 0; i < csi_n; i++) if (csi[i] == ';') {
            for (int j = i + 1; j < csi_n && csi[j] >= '0' && csi[j] <= '9'; j++) mod = mod * 10 + (csi[j] - '0');
            break;
        }
        bool ctrl = mod >= 2 && ((mod - 1) & 4);       /* (param-1) is a bitmask: Ctrl = bit 2 (4) */
        shift     = mod >= 2 && ((mod - 1) & 1);        /* Shift = bit 0 (xterm param 2 = Shift)     */
        switch (b) {
        case 'A': key = UK_UP;    break;
        case 'B': key = UK_DOWN;  break;
        case 'C': key = ctrl ? UK_WORD_RIGHT : UK_RIGHT; break;
        case 'D': key = ctrl ? UK_WORD_LEFT  : UK_LEFT;  break;
        case 'H': key = UK_HOME;  break;
        case 'F': key = UK_END;   break;
        case '~': if (csi_n >= 1 && csi[0] == '3') key = ctrl ? UK_WORD_DEL : UK_DEL;  /* ESC[3~ Del; ESC[3;5~ Ctrl+Del */
                  else if (csi_n >= 3 && csi[0] == '1' && csi[1] == '2' && csi[2] == '7') key = 0x17;  /* ESC[127~ Ctrl+Backspace: word-delete back */
                  else return;                                    /* Insert/PgUp/PgDn: ignored for now */
                  break;
        default:  return;
        }
    }
    if (key < 0) return;
    bool handled = focus ? focus->on_key(key, shift) : false;
    if (!handled) on_key(key);                       /* unconsumed keys -> app-level */
    dirty = true;
}
/* --- app menu bar builder (design/ui.md #6) ------------------------------- */
static void menu_cpy(char *dst, const char *src, int cap) {
    int i = 0; for (; i < cap - 1 && src && src[i]; i++) dst[i] = src[i]; dst[i] = 0;
}
void Window::menu_begin() { menu_spec.nmenus = 0; }
int Window::menu_add(const char *title) {
    if (menu_spec.nmenus >= WINMENU_MAX) return -1;
    int i = (int)menu_spec.nmenus++;
    menu_cpy(menu_spec.m[i].title, title, WINMENU_LBL);
    menu_spec.m[i].nitems = 0;
    return i;
}
void Window::menu_item(int menu, const char *label, char accel, unsigned flags) {
    if (menu < 0 || menu >= (int)menu_spec.nmenus) return;
    unsigned n = menu_spec.m[menu].nitems;
    if (n >= WINMENU_ITEMS) return;
    menu_cpy(menu_spec.m[menu].items[n], label, WINMENU_LBL);
    menu_spec.m[menu].flags[n] = (uint8_t)flags;
    menu_spec.m[menu].accel[n] = accel;
    menu_spec.m[menu].nitems = n + 1;
}
/* Flip a per-item flag bit and re-publish so the compositor's next read reflects it. */
static void menu_flag(struct winmenu &s, int menu, int item, unsigned bit, bool on, int id) {
    if (menu < 0 || menu >= (int)s.nmenus) return;
    if (item < 0 || item >= (int)s.m[menu].nitems) return;
    if (on) s.m[menu].flags[item] |= (uint8_t)bit;
    else    s.m[menu].flags[item] &= (uint8_t)~bit;
    if (id >= 0) win_setmenu(id, &s);
}
void Window::menu_set_checked(int menu, int item, bool on) { menu_flag(menu_spec, menu, item, WMI_CHECKED, on, id); }
void Window::menu_set_enabled(int menu, int item, bool on) { menu_flag(menu_spec, menu, item, WMI_DISABLED, !on, id); }
bool Window::menu_is_checked(int menu, int item) const {
    if (menu < 0 || menu >= (int)menu_spec.nmenus) return false;
    if (item < 0 || item >= (int)menu_spec.m[menu].nitems) return false;
    return (menu_spec.m[menu].flags[item] & WMI_CHECKED) != 0;
}
void Window::menu_commit() { if (id >= 0) win_setmenu(id, &menu_spec); }

/* --- ConfirmDialog (modal sheet) ----------------------------------------- */
void ConfirmDialog::show(const char *t, const char *m, const char *b0, const char *b1, const char *b2) {
    menu_cpy(title, t, sizeof title);
    menu_cpy(msg, m, sizeof msg);
    const char *bs[3] = { b0, b1, b2 };
    nbtn = 0; esc_btn = -1;
    for (int i = 0; i < 3; i++) if (bs[i]) {
        menu_cpy(btn[nbtn], bs[i], sizeof btn[nbtn]);
        if (btn[nbtn][0] == 'C' && btn[nbtn][1] == 'a') esc_btn = nbtn;   /* "Cancel" -> the Esc target */
        nbtn++;
    }
    open = true; visible = true; hover = -1;
    if (win) { r = { 0, 0, win->w, win->h }; prev_focus = win->focus; win->focus = this; win->invalidate(); }
    layout();
    /* report each button's window-local centre so a test can click it (client-relative,
     * pairs with the compositor's "[twm] win" client rect). */
    for (int i = 0; i < nbtn; i++)
        printf("[ui] dlgbtn %d %d %d\r\n", i, brect[i].x + brect[i].w / 2, brect[i].y + brect[i].h / 2);
}
void ConfirmDialog::dismiss() {
    open = false; visible = false;
    if (win) { if (win->focus == this) win->focus = prev_focus; win->invalidate(); }
}
void ConfirmDialog::layout() {
    if (!win) return;
    int fh = ugfx_font_h(), pad = 20, gap = 10, bh = fh + 16;
    int bw[3], btot = 0;
    for (int i = 0; i < nbtn; i++) { bw[i] = ugfx_text_w(btn[i]) + 30; if (bw[i] < 78) bw[i] = 78; btot += bw[i] + (i ? gap : 0); }
    int cw = ugfx_text_w(title); int mw = ugfx_text_w(msg); if (mw > cw) cw = mw; if (btot > cw) cw = btot;
    cw += pad * 2; if (cw < 300) cw = 300; if (cw > win->w - 40) cw = win->w - 40;
    int ch = pad + fh + 12 + fh + 18 + bh + pad;
    int cx = (win->w - cw) / 2, cy = (win->h - ch) / 2;
    card = { cx, cy, cw, ch };
    int bx = cx + cw - pad, by = cy + ch - pad - bh;     /* primary (i=0) at the far right */
    for (int i = 0; i < nbtn; i++) { bx -= bw[i]; brect[i] = { bx, by, bw[i], bh }; bx -= gap; }
}
int ConfirmDialog::btn_at(int x, int y) const {
    for (int i = 0; i < nbtn; i++) if (brect[i].has(x, y)) return i;
    return -1;
}
void ConfirmDialog::choose(int idx) {
    dismiss();                                            /* restore focus first, so the callback may re-show */
    printf("[ui] confirm %d\r\n", idx);
    if (on_choice) on_choice(ctx, idx);
}
void ConfirmDialog::draw() {
    if (!open || !win) return;
    layout();
    int fh = ugfx_font_h(), pad = 20, rad = TH_R_MD;
    ugfx_fill_a(0, 0, win->w, win->h, ARGB(150, 8, 10, 16));        /* dim the window behind the sheet */
    ugfx_elevation(card.x, card.y, card.w, card.h, rad, 6);
    ugfx_rrect_aa(card.x, card.y, card.w, card.h, rad, TH_SURF_3);
    ugfx_rrect_border(card.x, card.y, card.w, card.h, rad, 1, TH_BORDER);
    ugfx_text(card.x + pad, card.y + pad, title, TH_TEXT, UGFX_TRANSPARENT);
    ugfx_text(card.x + pad, card.y + pad + fh + 12, msg, TH_MUTED, UGFX_TRANSPARENT);
    for (int i = 0; i < nbtn; i++) {
        Rect b = brect[i]; bool primary = (i == 0);
        ugfx_rrect_aa(b.x, b.y, b.w, b.h, TH_R_SM, primary ? TH_ACCENT : TH_SURF_4);
        if (!primary) ugfx_rrect_border(b.x, b.y, b.w, b.h, TH_R_SM, 1, TH_BORDER);
        if (i == hover) ugfx_state_layer(b.x, b.y, b.w, b.h, TH_R_SM, TH_HOVER_A);
        uint32_t tc = primary ? RGB(255, 255, 255) : TH_TEXT;
        ugfx_text(b.x + (b.w - ugfx_text_w(btn[i])) / 2, b.y + (b.h - fh) / 2, btn[i], tc, UGFX_TRANSPARENT);
    }
}
bool ConfirmDialog::on_mouse(int x, int y, int) {
    if (!open) return true;
    int b = btn_at(x, y);
    if (b >= 0) choose(b);                                /* clicks off the buttons are swallowed (stay modal) */
    return true;
}
bool ConfirmDialog::on_hover(int x, int y) {
    if (!open) return false;
    int b = btn_at(x, y);
    if (b == hover) return false;
    hover = b; return true;
}
bool ConfirmDialog::on_key(int key, bool) {
    if (!open) return false;
    if (key == UK_ESC)   { choose(esc_btn); return true; }
    if (key == UK_ENTER || key == '\n') { choose(0); return true; }   /* Enter = the primary button */
    return true;                                          /* swallow everything else while modal */
}

int Window::run() {
    redraw(); dirty = false;
    while (running) {
        struct winevent ev;
        while (win_poll(id, &ev)) {
            switch (ev.type) {
            case WEV_KEY:   feed_key((int)(ev.a & 0xff)); break;
            case WEV_MOUSE: {
                int mx = (int)WEV_MOUSE_X(ev.a), my = (int)WEV_MOUSE_Y(ev.a), mb = (int)WEV_MOUSE_BTN(ev.a);
                if (mb == 0)                  { if (focus) focus->on_button_up(); dispatch_hover(mx, my); }  /* move/release -> end drag, hover */
                else if (mb & WEV_MOUSE_DRAG)   {                                /* drag-select */
                    if (focus) focus->on_drag(mx, my);       /* widget-level (TextField selection)   */
                    on_drag(mx, my, mb & ~WEV_MOUSE_DRAG);   /* window-level (Files rubber-band)      */
                    dirty = true;
                }
                else                            dispatch_mouse(mx, my, mb);
                break;
            }
            case WEV_RESIZE: {
                int nw = (int)(ev.a >> 16), nh = (int)(ev.a & 0xffff);
                if (nw > 0 && nh > 0 && win_resize(id, nw, nh) == 0) {
                    w = nw; h = nh;                       /* win_resize keeps the same vaddr */
                    on_resize(w, h);
                    invalidate();   /* whole new surface, not just dirty widgets: a partial
                                     * damage rect from another event in this same frame drain
                                     * (e.g. a hover while exiting fullscreen) would otherwise
                                     * leave the rest of the resized surface black until hover */
                }
                break;
            }
            case WEV_SCROLL: {
                int mx = (int)WEV_MOUSE_X(ev.a), my = (int)WEV_MOUSE_Y(ev.a);
                int d = (int)WEV_MOUSE_BTN(ev.a); if (d > 127) d -= 256;   /* signed 8-bit delta */
                dispatch_scroll(mx, my, d);
                break;
            }
            case WEV_NAV:   on_nav((int)ev.a); dirty = true; break;
            case WEV_MENU:  on_menu((int)WEV_MENU_M(ev.a), (int)WEV_MENU_I(ev.a)); dirty = true; break;
            case WEV_DRAG: {                              /* a drag is hovering this window */
                int mx = (int)WEV_MOUSE_X(ev.a), my = (int)WEV_MOUSE_Y(ev.a);
                if (mx == 0xfff && my == 0xfff) on_drag_over(-1, -1);   /* the drag left */
                else                            on_drag_over(mx, my);
                dirty = true;
                break;
            }
            case WEV_DROP: {                             /* a drag was released on this window */
                int mx = (int)WEV_MOUSE_X(ev.a), my = (int)WEV_MOUSE_Y(ev.a);
                static char buf[4096]; int type = 0;
                int n = drag_payload(&type, buf, sizeof buf);
                on_drag_over(-1, -1);                                   /* clear any highlight */
                if (n >= 0) on_drop(mx, my, type, buf, n);
                dirty = true;
                break;
            }
            case WEV_CLOSE: if (on_close()) running = false; break;
            }
        }
        /* A bare ESC byte can't be told from an ESC-sequence prefix at the byte
         * level, so feed_key latches it (esc == 1). The kernel posts a special
         * key's whole sequence in one burst, so if the latch survives two full
         * drains, it was a real lone Esc press -- deliver it. */
        if (esc == 1) {
            if (++esc_pend >= 2) {
                esc = 0; esc_pend = 0;
                bool handled = focus ? focus->on_key(UK_ESC, false) : false;
                if (!handled) on_key(UK_ESC);
                dirty = true;
            }
        } else esc_pend = 0;
        ticks++;
        on_tick(ticks);                       /* periodic app work (e.g. Notepad session autosave) */
        if (focus && focus->shows_caret() && (ticks % TF_BLINK) == 0)
            invalidate(focus->r);         /* pulse a repaint (focused field only) so the caret blinks */
        if (dirty) { redraw(); dirty = false; }
        sleep_ms(15);
    }
    win_close(id);
    return 0;
}

} // namespace ui
