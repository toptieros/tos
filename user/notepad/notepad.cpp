/* notepad -- a tabbed text editor on the ui:: toolkit. Each note is a TAB; a `+`
 * button (or File > New / ^N) opens a fresh "untitled" tab, and tabs switch/close
 * independently (^W or the tab's ×). Edit in a multiline TextField (the usual
 * editing + Ctrl+C/X/V/A + undo/redo); Save (^S) writes the active note -- a note
 * that was never saved prompts for a location via the reusable file picker (so does
 * Save As…), and Open… loads a note the same way.
 *
 * Every open tab + its unsaved contents are continuously autosaved to a draft store
 * (~/.cache/notepad), so closing the WINDOW never prompts -- a relaunch restores the
 * whole session. Closing a TAB is what asks about unsaved work: a dirty tab raises a
 * Save / Discard / Cancel guard, and Save on a never-saved tab opens the picker to
 * choose where (the tab closes once the save succeeds). */
#include "ui.h"
#include "app.h"

#define MAXTABS 12

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/') b = s + 1;
    return *b ? b : p;
}
/* Resolve a tab's name to an absolute path: an absolute name stays as-is, a bare
 * name (an untitled note's quick-save) lands in the user's Documents (created if
 * missing) so saved notes don't litter $HOME. */
static void resolve_path(char *dst, int cap, const char *name) {
    if (name[0] == '/') { int i = 0; for (; name[i] && i < cap - 1; i++) dst[i] = name[i]; dst[i] = 0; return; }
    const char *docs = "/Users/user/Documents/";
    mkdir("/Users/user/Documents");            /* harmless if it already exists */
    int i = 0; for (; docs[i] && i < cap - 1; i++) dst[i] = docs[i];
    for (int j = 0; name[j] && i < cap - 1; j++) dst[i++] = name[j];
    dst[i] = 0;
}

enum { PEND_NONE, PEND_QUIT, PEND_CLOSE };  /* what to do after the unsaved-changes guard resolves */

/* session autosave (#5): drafts of every open tab + the session layout are cached
 * here so a relaunch restores the whole session -- even notes never explicitly
 * saved (Windows-Notepad-style draft restore). */
#define NP_CACHE "/Users/user/.cache/notepad"
/* The draft flush is a crash-safety net, not the user's explicit save, so it waits
 * for a genuine "stopped typing" pause rather than firing on every micro-pause --
 * each flush writes the disk, and while a disk write runs this task can't echo new
 * keystrokes (the OS itself stays live now that syscalls are preemptible, see
 * kernel/arch/traps.c, but a too-eager flush still makes typing feel sticky). */
#define NP_IDLE_TICKS     90               /* flush a draft after ~1.3 s without an edit */
#define NP_AUTOSAVE_TICKS 400              /* ...but force one at least this often while typing */

static void np_tabpath(char *out, int cap, int i) { snprintf(out, cap, "%s/tab%d", NP_CACHE, i); }
/* parse a leading (optionally signed) int from *pp, skipping leading blanks, and
 * advance *pp past it -- a tiny scanf for the session file (no libc sscanf). */
static long np_int(const char **pp) {
    const char *p = *pp; while (*p == ' ' || *p == '\t') p++;
    long v = 0; int neg = 0; if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p; return neg ? -v : v;
}

/* one open note. `text`/`caret` snapshot a tab while it is NOT the active one (the
 * active tab's live content lives in the shared `editor`, synced in on switch). */
struct Tab {
    char  name[256] = "untitled.txt";   /* full path once named, else the untitled label */
    bool  named = false;                /* has a real on-disk path (saved or opened)      */
    bool  dirty = false;                /* edited since the last save/load                */
    char *text  = nullptr;              /* content snapshot (inactive tabs)               */
    int   caret = 0;                    /* saved caret (inactive tabs)                    */
};

struct Notepad;                          /* the TabBar talks back to it (defined below) */

/* the tab strip: one rounded tile per note (active highlighted, dirty shows a dot,
 * each has a × to close) plus a trailing + button. Borderless app-local widget;
 * clicks route back into the Notepad. */
static void np_xglyph(int x, int y, int s, uint32_t c) {        /* a 2px × in an s×s box */
    for (int i = 0; i < s; i++) { ugfx_fill(x + i, y + i, 2, 2, c); ugfx_fill(x + (s - 1 - i), y + i, 2, 2, c); }
}
class TabBar : public ui::Widget {
public:
    Notepad *np = nullptr;
    int hover_tab = -1, hover_close = -1; bool hover_plus = false;
    TabBar() { focusable = false; }
    void draw() override;
    bool on_mouse(int x, int y, int btn) override;
    bool on_hover(int x, int y) override;
    void on_leave() override { hovered = false; hover_tab = hover_close = -1; hover_plus = false; }
private:
    void metrics(int &x0, int &tabw, int &plusx, int &plusw) const;
};

struct Notepad : ui::Window {
    TabBar        tabbar;
    ui::Label     status;
    ui::TextField editor;        /* the active document body */
    ui::ConfirmDialog confirm;   /* the unsaved-changes guard (#5) */
    ui::FileDialog dlg;          /* the reusable Open / Save-As picker (#4) */
    int           dlg_mode = ui::FD_OPEN;

    Tab           tabs[MAXTABS];
    int           ntabs = 0, active = 0;
    int           fh = 0, TBH = 0, SBH = 0;
    char          statusbuf[80] = {0};
    bool          show_status = true;   /* View > Status Bar toggle (#6 checkable menu) */
    bool          loading = false;      /* inside set_text(): don't mark the tab dirty   */
    int           pending = PEND_NONE, pending_tab = -1;  /* deferred action awaiting the guard */
    int           close_after_pick = -1;  /* tab to close once a Save-As picker succeeds (-1 none) */
    unsigned      untitled_seq = 0;     /* disambiguates "untitled", "untitled 2" ...     */
    unsigned      last_autosave = 0, autosave_n = 0, last_edit = 0;
    bool          session_changed = false;   /* the session differs from the last draft on disk */

    /* -------------------------------------------------------------- layout */
    void layout() {
        fh = ugfx_font_h(); TBH = fh + 16; SBH = show_status ? fh + 8 : 0;
        tabbar.r = { 0, 0, w, TBH };
        status.r = { 10, h - SBH + (SBH - fh) / 2, w - 20, fh };
        editor.r = { 0, TBH, w, h - TBH - SBH };
    }
    void on_resize(int, int) override { layout(); }

    void set_status(const char *s) {
        int i = 0; for (; s[i] && i < 79; i++) statusbuf[i] = s[i]; statusbuf[i] = 0;
        status.text = statusbuf; invalidate();
    }

    /* ---------------------------------------------------------- tab model */
    Tab &cur() { return tabs[active]; }
    const char *tab_label(int i) const { return basename_of(tabs[i].name); }
    bool any_dirty() const { for (int i = 0; i < ntabs; i++) if (tabs[i].dirty) return true; return false; }

    void make_untitled(char *out, int cap) {
        if (untitled_seq == 0) snprintf(out, cap, "untitled.txt");
        else                   snprintf(out, cap, "untitled %u.txt", untitled_seq + 1);
        untitled_seq++;
    }
    void set_text(const char *s, int caret) {     /* load content without dirtying the tab */
        loading = true;
        editor.set_text(s ? s : "");
        editor.caret = (caret <= editor.length()) ? caret : editor.length();
        loading = false;
    }
    void sync_active() {                           /* editor -> the active tab's snapshot */
        if (active < 0 || active >= ntabs) return;
        Tab &t = tabs[active];
        if (t.text) { free(t.text); t.text = nullptr; }
        const char *e = editor.text(); int n = (int)strlen(e);
        t.text = (char *)malloc((unsigned)n + 1); if (t.text) { memcpy(t.text, e, n); t.text[n] = 0; }
        t.caret = editor.caret;
    }
    void load_active() {                           /* the active tab's snapshot -> editor */
        set_text(cur().text, cur().caret);
        focus = &editor;
        char m[96]; snprintf(m, sizeof m, "%s%s", tab_label(active), cur().dirty ? " — edited" : "");
        set_status(m);
        invalidate();
    }
    void new_tab() {
        if (ntabs >= MAXTABS) { set_status("Maximum tabs open"); return; }
        sync_active();
        Tab &t = tabs[ntabs];
        make_untitled(t.name, sizeof t.name);
        t.named = false; t.dirty = false; t.caret = 0; t.text = nullptr;
        active = ntabs; ntabs++;
        set_text("", 0); focus = &editor;
        set_status("New note"); session_changed = true;
        invalidate();
        printf("[notepad] newtab %d/%d\r\n", active, ntabs);
    }
    void switch_tab(int i) {
        if (i < 0 || i >= ntabs || i == active) return;
        sync_active();
        active = i;
        load_active();
        session_changed = true;
        printf("[notepad] tab %d/%d %s\r\n", active, ntabs, tab_label(active));
    }
    void do_close(int i) {                         /* remove tab i (no guard) */
        if (i < 0 || i >= ntabs) return;
        bool wasactive = (i == active);
        if (tabs[i].text) { free(tabs[i].text); tabs[i].text = nullptr; }
        for (int k = i + 1; k < ntabs; k++) tabs[k - 1] = tabs[k];
        tabs[ntabs - 1] = Tab{};
        ntabs--;
        /* Explicitly closing the LAST tab empties notepad on purpose, so drop the
         * saved session -- a relaunch should start fresh, not resurrect closed tabs
         * (closing the WINDOW, by contrast, keeps the session: see on_close). */
        if (ntabs == 0) { clear_session(); running = false; printf("[notepad] closetab -> quit\r\n"); return; }
        if (active > i) active--;
        else if (active >= ntabs) active = ntabs - 1;
        if (wasactive) load_active();
        session_changed = true;
        invalidate();
        printf("[notepad] closetab %d/%d\r\n", active, ntabs);
    }
    void close_tab(int i) {                         /* guard a dirty tab before removing it */
        if (i < 0 || i >= ntabs) return;
        if (i == active) sync_active();
        if (tabs[i].dirty) {
            if (i != active) switch_tab(i);          /* show what's being closed */
            pending = PEND_CLOSE; pending_tab = active;
            printf("[notepad] guard close %d\r\n", active);
            ask_save();
            return;
        }
        do_close(i);
    }

    /* ------------------------------------------------------------- saving */
    /* Write tab `i` to disk. The active tab's live body comes from the editor; an
     * inactive tab's from its snapshot (kept current by sync_active() on switch).
     * Updates the tab's name to the resolved path and clears its dirty flag. */
    bool save_tab(int i) {
        if (i < 0 || i >= ntabs) return false;
        char path[256]; resolve_path(path, sizeof path, tabs[i].name);
        const char *body = (i == active) ? editor.text() : (tabs[i].text ? tabs[i].text : "");
        int n = (int)strlen(body);
        if (sys_spit(path, body, n) < 0) {
            print("[notepad] save failed: "); print(path); print("\r\n");
            return false;
        }
        int k = 0; for (; path[k] && k < (int)sizeof tabs[i].name - 1; k++) tabs[i].name[k] = path[k]; tabs[i].name[k] = 0;
        tabs[i].named = true; tabs[i].dirty = false; session_changed = true;
        print("[notepad] saved "); print(path); print(" ("); printu((unsigned)n); print(" bytes)\r\n");
        return true;
    }
    void save() {                                   /* ^S / File > Save: write the active tab */
        if (!cur().named) { save_as(); return; }    /* never saved: ask WHERE via the picker */
        if (save_tab(active)) { char msg[96]; snprintf(msg, sizeof msg, "Saved %s", basename_of(cur().name)); set_status(msg); }
        else                  set_status("Save failed");
        invalidate();
    }
    /* Open / Save As via the reusable file picker (#4), both rooted at ~/Documents. */
    void open_open() { dlg_mode = ui::FD_OPEN; dlg.open_dialog(ui::FD_OPEN, "/Users/user/Documents"); }
    void save_as()   { dlg_mode = ui::FD_SAVE; dlg.open_dialog(ui::FD_SAVE, "/Users/user/Documents", basename_of(cur().name)); }
    bool pristine(const Tab &t) const { return !t.named && !t.dirty; }
    void on_picked(const char *path) {
        if (!path) { close_after_pick = -1; return; }   /* cancelled: keep the tab open */
        if (dlg_mode == ui::FD_OPEN) {
            /* open into the current tab if it's pristine, else a fresh tab */
            if (!pristine(cur())) {
                if (ntabs >= MAXTABS) { set_status("Maximum tabs open"); return; }
                sync_active(); active = ntabs; ntabs++; tabs[active] = Tab{};
            }
            int len = 0; char *nb = sys_slurp(path, &len);
            set_text(nb ? nb : "", 0); if (nb) free(nb);
            int i = 0; for (; path[i] && i < (int)sizeof cur().name - 1; i++) cur().name[i] = path[i]; cur().name[i] = 0;
            cur().named = true; cur().dirty = false; session_changed = true;
            char msg[96]; snprintf(msg, sizeof msg, "Opened %s", basename_of(path)); set_status(msg);
            invalidate();
            printf("[notepad] opened %s (tab %d/%d)\r\n", path, active, ntabs);
        } else {                                    /* SAVE: write to the chosen path */
            int i = 0; for (; path[i] && i < (int)sizeof cur().name - 1; i++) cur().name[i] = path[i]; cur().name[i] = 0;
            cur().named = true;
            bool ok = save_tab(active);
            if (ok) { char msg[96]; snprintf(msg, sizeof msg, "Saved %s", basename_of(cur().name)); set_status(msg); }
            else      set_status("Save failed");
            invalidate();
            int t = close_after_pick; close_after_pick = -1;   /* a close deferred until the pick succeeded */
            if (ok && t >= 0) do_close(t);
        }
    }

    /* --------------------------------------------- session autosave (#5) */
    bool session_exists() { char p[300]; snprintf(p, sizeof p, "%s/session", NP_CACHE); return sys_exists(p, 0) != 0; }
    void clear_session() {                          /* drop the draft store (all tabs closed) */
        char p[300];
        for (int i = 0; i < MAXTABS; i++) { np_tabpath(p, sizeof p, i); funlink(p); }
        snprintf(p, sizeof p, "%s/session", NP_CACHE); funlink(p);
        session_changed = false;                    /* nothing left to flush */
    }
    void autosave() {
        mkdir("/Users/user/.cache"); mkdir(NP_CACHE);    /* harmless if they already exist */
        sync_active();                                   /* so every tab's snapshot is current */
        for (int i = 0; i < ntabs; i++) {
            char p[300]; np_tabpath(p, sizeof p, i);
            const char *txt = tabs[i].text ? tabs[i].text : "";
            sys_spit(p, txt, (int)strlen(txt));
        }
        /* metadata: a header line, then 2 lines per tab (flags, then the name on its
         * own line so names with spaces survive). */
        char buf[4096]; int o = 0;
        o += snprintf(buf + o, sizeof buf - o, "%d %d %u\n", ntabs, active, untitled_seq);
        for (int i = 0; i < ntabs && o < (int)sizeof buf - 300; i++)
            o += snprintf(buf + o, sizeof buf - o, "%d %d\n%s\n", tabs[i].named ? 1 : 0, tabs[i].dirty ? 1 : 0, tabs[i].name);
        char sp[300]; snprintf(sp, sizeof sp, "%s/session", NP_CACHE);
        sys_spit(sp, buf, o);
        autosave_n++;
        printf("[notepad] autosave %u %d tabs\r\n", autosave_n, ntabs);
    }
    /* Rebuild the whole session from the draft store. Returns false (and leaves the
     * tab model untouched) if there's no valid session to restore. */
    bool restore() {
        char sp[300]; snprintf(sp, sizeof sp, "%s/session", NP_CACHE);
        int len = 0; char *s = sys_slurp(sp, &len);
        if (!s) return false;
        const char *p = s;
        int n = (int)np_int(&p), act = (int)np_int(&p); unsigned seq = (unsigned)np_int(&p);
        if (n < 1 || n > MAXTABS) { free(s); return false; }
        while (*p && *p != '\n') p++; if (*p == '\n') p++;       /* skip the rest of the header line */
        ntabs = 0;
        for (int i = 0; i < n; i++) {
            int named = (int)np_int(&p), dirty = (int)np_int(&p);
            while (*p && *p != '\n') p++; if (*p == '\n') p++;   /* end of the flags line */
            tabs[i] = Tab{};
            int j = 0; while (*p && *p != '\n' && j < (int)sizeof tabs[i].name - 1) tabs[i].name[j++] = *p++;
            tabs[i].name[j] = 0;
            if (*p == '\n') p++;                                 /* end of the name line */
            tabs[i].named = named != 0; tabs[i].dirty = dirty != 0; tabs[i].caret = 0;
            char tp[300]; np_tabpath(tp, sizeof tp, i);
            int tl = 0; tabs[i].text = sys_slurp(tp, &tl);       /* nullptr for an empty draft */
            ntabs++;
            printf("[notepad] restored tab %d %d %s\r\n", i, tabs[i].text ? (int)strlen(tabs[i].text) : 0, tabs[i].name);
        }
        free(s);
        active = (act >= 0 && act < ntabs) ? act : 0;
        untitled_seq = seq;
        set_text(cur().text, 0);
        set_status("Restored session");
        printf("[notepad] restored %d tabs active %d\r\n", ntabs, active);
        return true;
    }
    /* periodic draft flush. Debounced so a disk write never stalls active typing:
     * flush once the user has paused (NP_IDLE_TICKS since the last edit), or -- as a
     * backstop during nonstop typing -- once NP_AUTOSAVE_TICKS have elapsed. */
    void on_tick(unsigned t) override {
        if (ntabs == 0 || !session_changed) return;
        bool idle   = (t - last_edit)     >= NP_IDLE_TICKS;
        bool forced = (t - last_autosave) >= NP_AUTOSAVE_TICKS;
        if (!idle && !forced) return;
        last_autosave = t;
        session_changed = false;
        autosave();
    }

    /* ----------------------------------------------------- unsaved guard */
    void ask_save() { confirm.show("Save changes?", "This note has unsaved changes.", "Save", "Discard", "Cancel"); }
    /* The guard fires when CLOSING a dirty tab. 0 = Save, 1 = Discard, 2/-1 = Cancel.
     * Save on a never-saved tab opens the picker (choose a location) and defers the
     * close until the pick succeeds; a named tab writes to its path then closes. */
    void resolve_guard(int idx) {
        int pt = pending_tab; pending = PEND_NONE; pending_tab = -1;
        if (idx == 2 || idx < 0) return;            /* Cancel: stay put */
        if (idx == 1) {                             /* Discard: drop edits, then close */
            if (pt >= 0 && pt < ntabs) tabs[pt].dirty = false;
            do_close(pt); return;
        }
        if (pt >= 0 && pt < ntabs && !tabs[pt].named) {   /* Save, never saved: ask where first */
            close_after_pick = pt; save_as(); return;
        }
        save_tab(pt); do_close(pt);                 /* Save to the existing path, then close */
    }
    /* The compositor's close button: the session autosave already preserves every
     * tab + its unsaved contents, so closing the WINDOW never prompts -- flush the
     * latest draft and let it close (a relaunch restores the whole session). */
    bool on_close() override { autosave(); return true; }

    /* -------------------------------------------------------------- input */
    void on_key(int key) override { if (key == 0x13) save(); }   /* raw ^S (if the editor lets it bubble) */
    /* Menu bar (#6): File [New ^N, Open ^O, Save ^S, Save As, Close Tab ^W],
     * Edit [Select All ^A, Undo ^Z, Redo ^Y], View [✓ Status Bar]. */
    void on_menu(int menu, int item) override {
        print("[notepad] menu "); printu((unsigned)menu); printc(' '); printu((unsigned)item); print("\r\n");
        if (menu == 0) {
            if (item == 0) new_tab(); else if (item == 1) open_open();
            else if (item == 2) save(); else if (item == 3) save_as();
            else if (item == 4) close_tab(active);
        }
        else if (menu == 1 && focus) {
            if (item == 0) focus->on_key(0x01);
            else if (item == 1) focus->undo();
            else if (item == 2) focus->redo();
            invalidate();
        }
        else if (menu == 2 && item == 0) {
            show_status = !show_status; status.visible = show_status;
            menu_set_checked(2, 0, show_status);
            layout(); set_status(show_status ? "Status bar shown" : "");
            invalidate();
        }
    }

    /* --------------------------------------------------------------- build */
    bool build(const char *path) {
        struct sysinfo si; sysinfo(&si);
        int cw = (int)si.fb_w - 320, ch = (int)si.fb_h - 220;
        if (cw < 460) cw = 460; if (cw > 820) cw = 820;
        if (ch < 320) ch = 320; if (ch > 640) ch = 640;
        if (!create(cw, ch, "Notepad")) return false;

        tabbar.np = this;
        editor.multiline = true;
        status.fg = TH_MUTED; status.align = 0;
        editor.ctx = this;
        editor.on_change = [](void *c) {
            Notepad *n = (Notepad *)c;
            if (n->loading || n->active < 0 || n->active >= n->ntabs) return;
            n->session_changed = true; n->last_edit = n->ticks;   /* debounce the draft flush */
            if (!n->tabs[n->active].dirty) { n->tabs[n->active].dirty = true; n->invalidate(); }
        };
        confirm.ctx = this;
        confirm.on_choice = [](void *c, int idx) { ((Notepad *)c)->resolve_guard(idx); };
        dlg.ctx = this;
        dlg.on_pick = [](void *c, const char *p) { ((Notepad *)c)->on_picked(p); };

        /* first tab: an opened document (Files "Open With") wins; else restore the
         * autosaved session (#5); else a fresh untitled note. */
        if (path && path[0]) {
            ntabs = 1; active = 0; tabs[0] = Tab{};
            int len = 0; char *nb = sys_slurp(path, &len);
            int i = 0; for (; path[i] && i < (int)sizeof tabs[0].name - 1; i++) tabs[0].name[i] = path[i]; tabs[0].name[i] = 0;
            tabs[0].named = true;
            set_text(nb ? nb : "", 0); if (nb) free(nb);
            set_status("Opened");
        } else if (session_exists() && restore()) {
            /* restore() rebuilt tabs/ntabs/active and loaded the active tab's text */
        } else {
            ntabs = 1; active = 0; tabs[0] = Tab{};
            make_untitled(tabs[0].name, sizeof tabs[0].name);
            set_text("", 0);
            set_status("New note");
        }

        layout();
        add(&tabbar); add(&status); add(&editor);
        add(&confirm); add(&dlg);                     /* last = drawn on top + grab input when shown */
        focus = &editor;

        menu_begin();
        int mf = menu_add("File"); menu_item(mf, "New", 'N'); menu_item(mf, "Open", 'O');
                                   menu_item(mf, "Save", 'S'); menu_item(mf, "Save As", 0);
                                   menu_item(mf, "Close Tab", 'W');
        int me = menu_add("Edit"); menu_item(me, "Select All", 'A'); menu_item(me, "Undo", 'Z'); menu_item(me, "Redo", 'Y');
        int mv = menu_add("View"); menu_item(mv, "Status Bar", 0, WMI_CHECKED);
        menu_commit();
        return true;
    }
};

/* ----------------------------------------------------------- TabBar (out of line) */
void TabBar::metrics(int &x0, int &tabw, int &plusx, int &plusw) const {
    plusw = r.h; plusx = r.x + r.w - plusw - 4; x0 = r.x + 4;
    int n = np->ntabs; if (n < 1) n = 1;
    int avail = plusx - x0 - 4; tabw = avail / n;
    if (tabw > 180) tabw = 180; if (tabw < 56) tabw = 56;
}
void TabBar::draw() {
    if (!visible) return;
    int fh = ugfx_font_h(), fw = ugfx_font_w();
    ugfx_fill(r.x, r.y, r.w, r.h, RGB(30, 34, 46));
    ugfx_fill_a(r.x, r.y + r.h - 1, r.w, 1, ARGB(60, 150, 170, 230));
    int x0, tabw, plusx, plusw; metrics(x0, tabw, plusx, plusw);
    ugfx_set_clip(r.x, r.y, plusx - r.x - 2, r.h);
    for (int i = 0; i < np->ntabs; i++) {
        int tx = x0 + i * tabw, ty = r.y + 3, tw = tabw - 3, tht = r.h - 6;
        bool act = (i == np->active);
        ugfx_rrect_aa(tx, ty, tw, tht, TH_R_SM, act ? TH_SURF_3 : TH_SURF_1);
        if (!act && i == hover_tab) ugfx_state_layer(tx, ty, tw, tht, TH_R_SM, TH_HOVER_A);
        ugfx_rrect_border(tx, ty, tw, tht, TH_R_SM, 1, TH_BORDER);
        if (act) ugfx_fill_a(tx + 6, ty, tw - 12, 2, ARGB(200, 96, 152, 252));   /* active accent edge */
        int lx = tx + 9, closew = fh + 4;
        if (np->tabs[i].dirty) { ugfx_rrect_a(lx, ty + tht / 2 - 2, 5, 5, 2, act ? RGB(255, 255, 255) : TH_ACCENT); lx += 10; }
        const char *nm = np->tab_label(i);
        int avail = tw - (lx - tx) - closew - 6; int maxc = avail / fw; if (maxc < 1) maxc = 1;
        char lbl[48]; int li = 0; for (; nm[li] && li < maxc && li < 47; li++) lbl[li] = nm[li]; lbl[li] = 0;
        ugfx_text(lx, ty + (tht - fh) / 2, lbl, act ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
        int cxr = tx + tw - closew, cyr = ty + (tht - fh) / 2;
        if (i == hover_close) ugfx_rrect_a(cxr - 2, cyr - 1, fh + 2, fh + 2, TH_R_SM, ARGB(150, 200, 96, 96));
        np_xglyph(cxr, cyr + 1, fh - 4, (i == hover_close) ? RGB(255, 255, 255) : (act ? RGB(220, 224, 230) : TH_MUTED));
    }
    ugfx_clip_none();
    int py = r.y + 3, ph = r.h - 6;
    ugfx_rrect_aa(plusx, py, plusw - 3, ph, TH_R_SM, hover_plus ? TH_SURF_3 : TH_SURF_1);
    ugfx_rrect_border(plusx, py, plusw - 3, ph, TH_R_SM, 1, TH_BORDER);
    int pcx = plusx + (plusw - 3) / 2, pcy = r.y + r.h / 2;
    ugfx_fill(pcx - 5, pcy - 1, 11, 2, TH_TEXT); ugfx_fill(pcx - 1, pcy - 5, 2, 11, TH_TEXT);
}
bool TabBar::on_mouse(int x, int, int) {        /* tabs span the full strip height: y is irrelevant */
    int x0, tabw, plusx, plusw; metrics(x0, tabw, plusx, plusw);
    if (x >= plusx && x < plusx + plusw) { np->new_tab(); return true; }
    for (int i = 0; i < np->ntabs; i++) {
        int tx = x0 + i * tabw, tw = tabw - 3;
        if (x >= tx && x < tx + tw) {
            int closew = ugfx_font_h() + 4;
            if (x >= tx + tw - closew) np->close_tab(i); else np->switch_tab(i);
            return true;
        }
    }
    return true;
}
bool TabBar::on_hover(int x, int) {
    int x0, tabw, plusx, plusw; metrics(x0, tabw, plusx, plusw);
    int ht = -1, hc = -1; bool hp = (x >= plusx && x < plusx + plusw);
    if (!hp) for (int i = 0; i < np->ntabs; i++) {
        int tx = x0 + i * tabw, tw = tabw - 3;
        if (x >= tx && x < tx + tw) { ht = i; if (x >= tx + tw - (ugfx_font_h() + 4)) hc = i; break; }
    }
    if (ht == hover_tab && hc == hover_close && hp == hover_plus) return false;
    hover_tab = ht; hover_close = hc; hover_plus = hp; return true;
}

int app_main() {
    char path[256]; int have = sys_open_arg(path, sizeof path);
    Notepad *app = new Notepad();
    if (!app->build(have ? path : nullptr)) { print("[notepad] needs the desktop\r\n"); proc_exit(); }
    print("[notepad] up\r\n");
    return app->run();
}
