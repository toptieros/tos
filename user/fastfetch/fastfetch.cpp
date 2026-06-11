/* fastfetch -- a system-information banner. DUAL-MODE: run in a terminal (stdio is a
 * pty) it prints the classic ANSI logo + facts the shell shows at login; launched as
 * a desktop app (no pty) it opens a toolkit WINDOW built with the ui::Layout system --
 * the logo beside a column of key/value facts, with a row of palette blocks below.
 * Same facts, two presentations; the GUI window is the toolkit layout-system showcase
 * (a Column of [Row[logo, info-Column], colour bar], all placed by ui::Layout). */
#include "ui.h"
#include "ulib.h"

/* ---- the facts, gathered once from the kernel -------------------------------- */
struct Facts { char os[24], kernel[24], uptime[24], cpu[32], mem[24], disp[24], files[24], tasks[24]; };
static void gather(Facts *f) {
    struct sysinfo si; sysinfo(&si);
    unsigned up = (unsigned)(si.uptime_ticks / (si.timer_hz ? si.timer_hz : 100));
    snprintf(f->os,     sizeof f->os,     "tOS x86_64");
    snprintf(f->kernel, sizeof f->kernel, "tOS 1.0");
    snprintf(f->uptime, sizeof f->uptime, "%um %us", up / 60, up % 60);
    snprintf(f->cpu,    sizeof f->cpu,    "x86_64 (%u cores)", si.ncpu);
    snprintf(f->mem,    sizeof f->mem,    "%u MiB", (unsigned)(si.ram_bytes / (1024 * 1024)));
    snprintf(f->disp,   sizeof f->disp,   "%ux%u", si.fb_w, si.fb_h);
    snprintf(f->files,  sizeof f->files,  "%u", si.nfiles);
    snprintf(f->tasks,  sizeof f->tasks,  "%u", si.ntasks);
}

static const char *LOGO[] = {
    "    _      ___  ____  ",
    "   | |_   / _ \\/ ___| ",
    "   | __| | | | \\___ \\ ",
    "   | |_  | |_| |___) |",
    "    \\__|  \\___/|____/ ",
    0,
};

/* ---- terminal mode: the ANSI login banner ----------------------------------- */
static int cli_banner(const Facts &f) {
    static const char *RESET = "\x1b[0m", *LG = "\x1b[1;36m", *KEY = "\x1b[1;34m", *ACC = "\x1b[1;92m";
    const char *vals[6] = { 0, 0, f.os, f.kernel, f.uptime, "tsh" };
    const char *keys[6] = { 0, 0, "OS:       ", "Kernel:   ", "Uptime:   ", "Shell:    " };
    for (int i = 0; LOGO[i]; i++) {
        print(LG); print(LOGO[i]); print(RESET); print("   ");
        if (i == 0)      { print(ACC); print("root@tos"); print(RESET); }
        else if (i == 1) print("---------------");
        else { print(KEY); print(keys[i]); print(RESET); print(vals[i]); }
        print("\r\n");
    }
    const char *k2[5] = { "Terminal: ", "CPU:      ", "Memory:   ", "Display:  ", "Files:    " };
    const char *v2[5] = { "term", f.cpu, f.mem, f.disp, f.files };
    for (int i = 0; i < 5; i++) { print(KEY); print(k2[i]); print(RESET); print(v2[i]); print("\r\n"); }
    print("\r\n");
    for (int c = 0; c < 8; c++) { print("\x1b[4"); printc((char)('0' + c)); print("m  "); }
    print(RESET); print("\r\n");
    return 0;
}

/* ---- desktop mode: a toolkit window laid out by ui::Layout ------------------- */
class LogoW : public ui::Widget {
public:
    void draw() override {
        int fh = ugfx_font_h(), y = r.y;
        for (int i = 0; LOGO[i]; i++) { ugfx_text(r.x, y, LOGO[i], RGB(96, 204, 236), UGFX_TRANSPARENT); y += fh + 3; }
    }
};
class InfoRow : public ui::Widget {
public:
    const char *key = "", *val = "";
    void draw() override {
        int ty = r.y + (r.h - ugfx_font_h()) / 2;
        ugfx_text(r.x, ty, key, TH_ACCENT, UGFX_TRANSPARENT);
        ugfx_text(r.x + 96, ty, val, TH_TEXT, UGFX_TRANSPARENT);
    }
};
class ColorsW : public ui::Widget {
public:
    void draw() override {
        static const uint32_t pal[8] = {
            RGB(40, 44, 58), RGB(232, 90, 90), RGB(120, 200, 120), RGB(232, 200, 110),
            RGB(96, 152, 252), RGB(190, 130, 230), RGB(110, 200, 220), RGB(220, 226, 236) };
        int n = 8, gap = 5, bw = (r.w - (n - 1) * gap) / n; if (bw < 6) bw = 6;
        for (int i = 0; i < n; i++) ugfx_rrect_aa(r.x + i * (bw + gap), r.y, bw, r.h, 4, pal[i]);
    }
};

class FastfetchApp : public ui::Window {
    LogoW    logo;
    ColorsW  colors;
    ui::Label title, sep;
    InfoRow  rows[8];
    Facts    f;
public:
    bool build() {
        gather(&f);
        if (!create(560, 290, "Fastfetch")) return false;
        bg = RGB(22, 25, 33);
        title.text = "root@tOS";   title.fg = RGB(122, 214, 152);
        sep.text   = "-----------------------"; sep.fg = TH_MUTED;
        static const char *keys[8] = { "OS", "Kernel", "Uptime", "CPU", "Memory", "Display", "Files", "Tasks" };
        const char *vals[8] = { f.os, f.kernel, f.uptime, f.cpu, f.mem, f.disp, f.files, f.tasks };
        for (int i = 0; i < 8; i++) { rows[i].key = keys[i]; rows[i].val = vals[i]; }
        layout();
        add(&logo); add(&title); add(&sep);
        for (int i = 0; i < 8; i++) add(&rows[i]);
        add(&colors);
        return true;
    }
    /* The layout-system showcase: an outer Column, a Row inside it, an inner Column. */
    void layout() {
        ui::Layout col(ui::LAY_COL, 0, 18);          /* outer: body (stretch) + colour bar */
        col.space(0);
        col.add(&colors, 22);
        col.arrange(ui::Rect{ 0, 0, w, h });

        ui::Layout row(ui::LAY_ROW, 24, 0);           /* body: logo (fixed) + info (stretch) */
        row.add(&logo, 196);
        row.space(0);
        row.arrange(col.rect_of(0));

        ui::Layout info(ui::LAY_COL, 3, 0);           /* info: title, rule, then the rows */
        info.add(&title, 22);
        info.add(&sep, 14);
        for (int i = 0; i < 8; i++) info.add(&rows[i], 20);
        info.arrange(row.rect_of(1));
    }
    void on_resize(int, int) override { layout(); }
};

extern "C" int app_main() {
    Facts f; gather(&f);
    if (isatty()) return cli_banner(f);           /* terminal: the classic banner */
    FastfetchApp *app = new FastfetchApp();        /* desktop: the toolkit window  */
    if (!app->build()) { print("[fastfetch] needs the desktop\r\n"); return 0; }
    print("[fastfetch] up\r\n");
    return app->run();
}
