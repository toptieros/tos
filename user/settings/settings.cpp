/* settings -- a small control panel on the ui:: toolkit. Each row is a button that
 * shows a setting's current value; clicking it toggles/cycles the value, writes the
 * user registry layer and persists it. The compositor (twm) re-reads the registry
 * about once a second and applies changes live (shadows, wallpaper, accent, the
 * auto-hide flags, the clock). This is the GUI front-end for the `reg` shell command. */
#include "ui.h"
#include "app.h"
#include "ulib.h"
#include "registry.h"
#include "glyphs.h"     /* baked Lucide app glyphs (tools/genglyphs.py) */

using ui::Window; using ui::Panel; using ui::Label; using ui::Button;

/* Wallpaper presets -- the key strings must match twm's wallpaper table. */
static const char *WALLS[] = { "slate", "midnight", "forest", "plum", "graphite" };
static const int   NWALL   = 5;
/* Accent presets (#RRGGBB) with friendly names. */
static const char *ACCENTS[]  = { "#5AA0FC", "#34C759", "#FF9F0A", "#FF375F", "#BF5AF2" };
static const char *ACCENTN[]  = { "Blue", "Green", "Orange", "Pink", "Purple" };
static const int   NACC       = 5;

enum { S_TOGGLE, S_CLOCK, S_WALL, S_ACCENT };
struct Row { const char *label; const char *key; int kind; int dflt; int icon; };
static Row ROWS[] = {
    { "Drop shadows",       "ui.shadows",        S_TOGGLE, 1, GLYPH_SHADOWS   },
    { "Wallpaper",          "desktop.wallpaper", S_WALL,   0, GLYPH_WALLPAPER },
    { "Accent colour",      "theme.accent",      S_ACCENT, 0, GLYPH_ACCENT    },
    { "Auto-hide Dock",     "ui.dock.autohide",  S_TOGGLE, 0, GLYPH_DOCK      },
    { "Auto-hide menu bar", "ui.bar.autohide",   S_TOGGLE, 0, GLYPH_MENUBAR   },
    { "24-hour clock",      "clock.format",      S_CLOCK,  1, GLYPH_CLOCK     },
    { "Clock seconds",      "clock.seconds",     S_TOGGLE, 1, GLYPH_SECONDS   },
    { "Clock weekday",      "clock.weekday",     S_TOGGLE, 1, GLYPH_WEEKDAY   },
};
static const int NROWS = (int)(sizeof ROWS / sizeof ROWS[0]);

static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void cat(char *d, int cap, const char *s) { int i = 0; while (d[i]) i++; for (int j = 0; s[j] && i < cap - 1; j++) d[i++] = s[j]; d[i] = 0; }

struct SettingsApp : Window {
    Panel  topbar;
    Label  title;
    Button btn[NROWS];
    char   val[NROWS][32];          /* each row's value text, drawn right-aligned */
    struct Bind { SettingsApp *app; int row; } binds[NROWS];

    int accent_index() {
        const char *s = reg_get("theme.accent", "");
        for (int k = 0; k < NACC; k++) if (seq(s, ACCENTS[k])) return k;
        return -1;
    }
    const char *state_of(int i) {
        Row &r0 = ROWS[i];
        if (r0.kind == S_TOGGLE) return reg_bool(r0.key, r0.dflt) ? "On" : "Off";
        if (r0.kind == S_CLOCK)  return seq(reg_get("clock.format", "24h"), "24h") ? "24-hour" : "12-hour";
        if (r0.kind == S_WALL)   return reg_get("desktop.wallpaper", WALLS[0]);
        int a = accent_index();  return a >= 0 ? ACCENTN[a] : "Custom";
    }
    void refresh() {
        for (int i = 0; i < NROWS; i++) {
            Row &r0 = ROWS[i];
            btn[i].text = r0.label;
            val[i][0] = 0; cat(val[i], sizeof val[i], state_of(i));
            btn[i].value = val[i];
            btn[i].icon = glyphs_argb[r0.icon];     /* leading Lucide glyph, theme ink */
            btn[i].icon_sz = GLYPH_SZ;
            btn[i].icon_tint = TH_TEXT;
            /* an "On" toggle reads in the accent; everything else stays muted-secondary */
            bool on = (r0.kind == S_TOGGLE) && reg_bool(r0.key, r0.dflt);
            btn[i].value_fg = on ? TH_ACCENT : TH_MUTED;
        }
        invalidate();
    }
    void activate(int i) {
        Row &r0 = ROWS[i];
        if (r0.kind == S_TOGGLE)      reg_set(r0.key, reg_bool(r0.key, r0.dflt) ? "false" : "true");
        else if (r0.kind == S_CLOCK)  reg_set("clock.format", seq(reg_get("clock.format", "24h"), "24h") ? "12h" : "24h");
        else if (r0.kind == S_WALL) {
            const char *w = reg_get("desktop.wallpaper", WALLS[0]); int idx = 0;
            for (int k = 0; k < NWALL; k++) if (seq(w, WALLS[k])) idx = k;
            reg_set("desktop.wallpaper", WALLS[(idx + 1) % NWALL]);
        } else {
            int idx = accent_index(); idx = (idx < 0) ? 0 : (idx + 1) % NACC;
            reg_set("theme.accent", ACCENTS[idx]);
        }
        reg_save(); refresh();
        print("[settings] set "); print(r0.key); print(" = "); print(state_of(i)); print("\r\n");
    }
    static void on_row(void *c) { Bind *b = (Bind *)c; b->app->activate(b->row); }

    /* Placed by the toolkit's ui::Layout: an outer column splits the full-bleed
     * top bar from the body; a padded inner column lays the rows out with even
     * gaps -- no hand-rolled y-cursor or per-row width arithmetic. */
    void layout() {
        ui::Layout col(ui::LAY_COL, 0, 0);
        col.add(&topbar, 36);                 /* fixed-height title bar */
        col.space(0);                         /* the body stretches to fill */
        col.arrange(ui::Rect{ 0, 0, w, h });

        ui::Rect bar = col.rect_of(0);        /* title sits in the bar, left-aligned */
        title.r = { bar.x + 16, bar.y + 9, bar.w - 32, 20 };

        ui::Layout rows(ui::LAY_COL, 8, 14);  /* one padded column, even 8px gaps */
        for (int i = 0; i < NROWS; i++) rows.add(&btn[i], 30);
        rows.arrange(col.rect_of(1));
    }
    bool build() {
        reg_load();
        int ch = 46 + NROWS * 38 + 12;
        if (!create(480, ch, "Settings")) return false;
        topbar.color = RGB(30, 34, 46); topbar.sep_bottom = true; topbar.sep = RGB(54, 60, 78);
        title.text = "Settings"; title.fg = TH_TEXT;
        for (int i = 0; i < NROWS; i++) {
            binds[i].app = this; binds[i].row = i;
            btn[i].ctx = &binds[i]; btn[i].on_click = on_row;
        }
        refresh();
        layout();
        add(&topbar); add(&title);
        for (int i = 0; i < NROWS; i++) add(&btn[i]);
        return true;
    }
    void on_resize(int, int) override { layout(); }
};

int app_main() {
    SettingsApp *app = new SettingsApp();
    if (!app->build()) { print("[settings] needs the desktop\r\n"); proc_exit(); }
    print("[settings] up\r\n");
    return app->run();
}
