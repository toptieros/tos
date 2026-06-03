/* notepad -- a minimal text editor on the ui:: toolkit. Create or open a note,
 * edit it in a multiline TextField (the usual editing + Ctrl+C/X/V/A clipboard
 * chords), and Save / Save As it to the filesystem. Launched with a document path
 * (Files "Open With"), it opens that file; launched bare, it starts an untitled
 * note. A bare filename is saved under the user's Documents so it is easy to
 * find and doesn't litter the home root. */
#include "ui.h"
#include "app.h"

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/') b = s + 1;
    return *b ? b : p;
}
/* Resolve the name field to an absolute path: absolute stays as-is, a bare name
 * lands in the user's Documents folder (created if missing) so saved notes don't
 * litter $HOME — the shell reaches them as Documents/<name>. Once the file picker
 * (#4) lands this becomes its default folder. */
static void resolve_path(char *dst, int cap, const char *name) {
    if (name[0] == '/') { int i = 0; for (; name[i] && i < cap - 1; i++) dst[i] = name[i]; dst[i] = 0; return; }
    const char *docs = "/Users/user/Documents/";
    mkdir("/Users/user/Documents");            /* harmless if it already exists */
    int i = 0; for (; docs[i] && i < cap - 1; i++) dst[i] = docs[i];
    for (int j = 0; name[j] && i < cap - 1; j++) dst[i++] = name[j];
    dst[i] = 0;
}

enum { PEND_NONE, PEND_NEW, PEND_QUIT };   /* what to do after the unsaved-changes guard resolves */

struct Notepad : ui::Window {
    ui::Panel     bar;
    ui::TextField name;          /* the file name / path (Save As = edit this, then Save) */
    ui::Button    savebtn;
    ui::Label     status;
    ui::TextField editor;        /* the document body */
    ui::ConfirmDialog confirm;   /* the unsaved-changes guard (#5) */
    ui::FileDialog dlg;          /* the reusable Open / Save-As picker (#4) */
    int           dlg_mode = ui::FD_OPEN;   /* which flow the open picker is serving */
    char         *doc = nullptr;
    int           fh = 0, TBH = 0;
    char          statusbuf[64] = {0};
    bool          show_status = true;   /* View > Status Bar toggle (#6 checkable menu) */
    bool          dirty = false;        /* buffer edited since the last save/load */
    int           pending = PEND_NONE;  /* deferred action awaiting the guard's answer */

    void layout() {
        fh = ugfx_font_h(); TBH = fh + 18;
        bar.r    = { 0, 0, w, TBH };
        int bw   = ugfx_text_w("Save") + 24;
        int sx   = w - bw - 10;
        int stw  = 160;
        name.r   = { 10, (TBH - (fh + 8)) / 2, sx - stw - 20, fh + 8 };
        status.r = { sx - stw - 6, (TBH - fh) / 2, stw, fh };
        savebtn.r= { sx, (TBH - (fh + 8)) / 2, bw, fh + 8 };
        editor.r = { 0, TBH, w, h - TBH };
    }
    void on_resize(int, int) override { layout(); }

    void set_status(const char *s) {
        int i = 0; for (; s[i] && i < 63; i++) statusbuf[i] = s[i]; statusbuf[i] = 0;
        status.text = statusbuf; invalidate();
    }
    void save() {
        char path[256]; resolve_path(path, sizeof path, name.text());
        const char *body = editor.text();
        int n = (int)strlen(body);
        if (sys_spit(path, body, n) >= 0) {           /* sys_spit returns bytes written, -1 on error */
            char msg[80]; snprintf(msg, sizeof msg, "Saved %s", basename_of(path));
            set_status(msg);
            dirty = false;
            print("[notepad] saved "); print(path); print(" ("); printu((unsigned)n); print(" bytes)\r\n");
        } else {
            set_status("Save failed");
            print("[notepad] save failed: "); print(path); print("\r\n");
        }
    }
    /* Open / Save As via the reusable file picker (#4). Open browses for an
     * existing note; Save As browses to a folder + names the file. Both default
     * to ~/Documents (where notes live). on_picked() runs when the user confirms. */
    void open_open() { dlg_mode = ui::FD_OPEN; dlg.open_dialog(ui::FD_OPEN, "/Users/user/Documents"); }
    void save_as()   { dlg_mode = ui::FD_SAVE; dlg.open_dialog(ui::FD_SAVE, "/Users/user/Documents", basename_of(name.text())); }
    void on_picked(const char *path) {
        if (!path) return;                               /* cancelled */
        if (dlg_mode == ui::FD_OPEN) {
            int len = 0; char *nb = sys_slurp(path, &len);
            if (doc) free(doc);
            doc = nb;
            editor.set_text(doc ? doc : ""); editor.caret = 0;
            name.set_text(path); dirty = false;
            char msg[80]; snprintf(msg, sizeof msg, "Opened %s", basename_of(path)); set_status(msg);
            focus = &editor; invalidate();
        } else {
            name.set_text(path); save();                 /* save() writes the absolute path as-is */
        }
    }
    void do_new() {                          /* the actual reset, once any guard has cleared */
        name.set_text("untitled.txt"); editor.set_text(""); editor.caret = 0;
        focus = &editor; dirty = false; set_status("New note"); invalidate();
        print("[notepad] reset\r\n");
    }
    /* New on a dirty buffer no longer nukes it silently (#5): raise the guard and
     * defer the reset until the user answers. */
    void new_note() {
        if (dirty) { pending = PEND_NEW; print("[notepad] guard new\r\n"); ask_save(); }
        else do_new();
    }
    void ask_save() { confirm.show("Save changes?", "This note has unsaved changes.", "Save", "Discard", "Cancel"); }
    /* The guard's answer: 0 = Save, 1 = Discard, 2/-1 = Cancel. Save writes first,
     * then (Save or Discard) performs the deferred New / Quit. */
    void resolve_guard(int idx) {
        int act = pending; pending = PEND_NONE;
        if (idx == 2 || idx < 0) return;     /* Cancel: stay put */
        if (idx == 0) save();
        if (act == PEND_NEW) do_new();
        else if (act == PEND_QUIT) running = false;
    }
    /* Veto the compositor's close on a dirty buffer; close after the guard answers. */
    bool on_close() override {
        if (!dirty) return true;
        pending = PEND_QUIT; print("[notepad] guard quit\r\n"); ask_save();
        return false;
    }
    /* Ctrl+S anywhere in the window saves (the editor doesn't consume ^S, so it
     * bubbles up to here). */
    void on_key(int key) override { if (key == 0x13) save(); }
    /* Menu-bar selections (#6): File [New, Save], Edit [Select All]. */
    void on_menu(int menu, int item) override {
        print("[notepad] menu "); printu((unsigned)menu); printc(' '); printu((unsigned)item); print("\r\n");
        if (menu == 0) {                                         /* File: New / Open / Save / Save As */
            if (item == 0) new_note(); else if (item == 1) open_open();
            else if (item == 2) save(); else if (item == 3) save_as();
        }
        else if (menu == 1 && focus) {                           /* Edit: Select All / Undo / Redo */
            if (item == 0) focus->on_key(0x01);
            else if (item == 1) focus->undo();
            else if (item == 2) focus->redo();
            invalidate();
        }
        else if (menu == 2 && item == 0) {                       /* View > Status Bar: toggle + checkmark */
            show_status = !show_status;
            status.visible = show_status;
            menu_set_checked(2, 0, show_status);
            set_status(show_status ? "Status bar shown" : "");
            invalidate();
        }
    }

    bool build(const char *path) {
        struct sysinfo si; sysinfo(&si);
        int cw = (int)si.fb_w - 320, ch = (int)si.fb_h - 220;
        if (cw < 460) cw = 460; if (cw > 820) cw = 820;
        if (ch < 320) ch = 320; if (ch > 640) ch = 640;
        char t[40] = "Notepad";
        if (path && path[0]) { const char *b = basename_of(path); int i = 0; for (; b[i] && i < 39; i++) t[i] = b[i]; t[i] = 0; }
        if (!create(cw, ch, t)) return false;

        bar.color = RGB(30, 34, 46); bar.sep_bottom = true;
        name.multiline = false;
        editor.multiline = true;
        status.fg = TH_MUTED; status.align = 2;
        savebtn.text = "Save"; savebtn.ctx = this;
        savebtn.on_click = [](void *c) { ((Notepad *)c)->save(); };
        editor.ctx = this;                            /* mark the buffer dirty on any edit (#5 guard) */
        editor.on_change = [](void *c) { ((Notepad *)c)->dirty = true; };
        confirm.ctx = this;
        confirm.on_choice = [](void *c, int idx) { ((Notepad *)c)->resolve_guard(idx); };
        dlg.ctx = this;
        dlg.on_pick = [](void *c, const char *p) { ((Notepad *)c)->on_picked(p); };

        if (path && path[0]) {
            int len = 0; doc = sys_slurp(path, &len);
            name.set_text(path);
            editor.set_text(doc ? doc : "");
            set_status("Opened");
        } else {
            name.set_text("untitled.txt");
            editor.set_text("");
            set_status("New note");
        }
        editor.caret = 0;
        dirty = false;                                /* the initial load is not a user edit */

        layout();
        add(&bar); add(&name); add(&status); add(&savebtn); add(&editor);
        add(&confirm); add(&dlg);                     /* last = drawn on top + grab input when shown */
        focus = &editor;

        menu_begin();                                 /* declare a menu bar (#6): accels, a disabled item, a checkable toggle */
        int mf = menu_add("File"); menu_item(mf, "New", 'N'); menu_item(mf, "Open", 'O');
                                   menu_item(mf, "Save", 'S'); menu_item(mf, "Save As", 0);
        int me = menu_add("Edit"); menu_item(me, "Select All", 'A'); menu_item(me, "Undo", 'Z'); menu_item(me, "Redo", 'Y');
        int mv = menu_add("View"); menu_item(mv, "Status Bar", 0, WMI_CHECKED);
        menu_commit();
        return true;
    }
};

int app_main() {
    char path[256]; int have = sys_open_arg(path, sizeof path);
    Notepad *app = new Notepad();
    if (!app->build(have ? path : nullptr)) { print("[notepad] needs the desktop\r\n"); proc_exit(); }
    print("[notepad] up\r\n");
    return app->run();
}
