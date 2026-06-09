/* files -- the tOS file manager (macOS Finder-like) on the C++ widget toolkit.
 *
 * Layout: a left SIDEBAR of pinned locations, an icon TOOLBAR (back / forward /
 * up | new-folder / delete), the directory LISTVIEW, and a right DETAILS panel
 * (single-click an item to inspect it). Files no longer opens documents itself --
 * double-clicking a file hands it to another app ("Open With"); .app bundles show
 * without the extension, with their own icon, and launch directly. New Folder /
 * Delete / Open With are also on a right-click CONTEXT MENU. Back/forward (the
 * toolbar arrows or the mouse side buttons) walk a navigation history. */
#include "app.h"          /* placement new + the app_main entry the crt calls   */
#include "fileswidgets.h" /* the custom widgets + colour palette (pulls in ui.h,  *
                           * icons.h, pathbar.h, filesutil.h)                     */
#include "manifest.h"
#include "registry.h"
#include "textutil.h"      /* tu_ci_contains: the filter-bar substring matcher */
#include "perm.h"          /* tos_may_write / TOS_UID_*: grey Save in a system-owned folder */
#include "filesort.h"      /* filesort_cmp: data-driven Sort by Name/Kind/Size (§2) */
#include "viewmem.h"       /* view_prefs: per-folder view memory in the registry (§2) */
#include "trashinfo.h"     /* trashinfo_*: the Trash sidecar codec (§9) */
#include "dupname.h"       /* dup_candidate: Finder-style "X copy" naming (§12) */
#include "humansize.h"     /* human_bytes: status-bar free-space figure (§6) */
#include "fileinfo.h"      /* info_is_locked: Get Info owner/lock fields (§8) */
#include "tabtitle.h"      /* tab_title: the tab-strip pill label (§4)       */

#define NMAX    256
#define HISTN   32
#define HOMEDIR "/Users/user"
#define TRASHDIR  HOMEDIR "/.Trash"        /* move-to-trash store (§9) */
#define TRASHINFO TRASHDIR "/.trashinfo"   /* sidecar: "<trashedname>\t<origpath>" per line */


/* an app discovered under /Apps, for the Open With chooser */
struct AppEntry { char label[32], exec[160]; uint32_t *icon; int iw, ih; };

/* ------------------------------------------------------------------ FilesApp */
struct FilesApp : ui::Window {
    ui::Panel    bar;
    IconButton   back, fwd, up, newf, del;
    ui::Button   info;
    Breadcrumb   crumbbar;                    /* the clickable location bar (§3)       */
    ui::TextField pathfld;                    /* editable path mode (Ctrl+L / click empty) */
    bool         editing_path = false;
    ui::TextField renamefld;                  /* in-place rename overlay               */
    bool         renaming = false;
    int          rename_idx = -1;             /* ents[] index being renamed            */
    ui::TextField filterfld;                 /* the "/" live name-filter field */
    ui::ListView list;
    IconGrid     grid;                        /* the icon (grid) view (§1)             */
    int          view_mode = 0;               /* 0 = list, 1 = icons                   */
    int          zoom = 1;                    /* icon-view zoom level (0..2)           */
    Sidebar      side;
    DetailsPanel details;
    StatusBar    status;
    TabStrip     tabstrip;                    /* the folder tab strip (§4)             */
    ui::ListView list2;                       /* dual-pane: the second pane (§4)        */
    SplitDecor   splitdecor;                  /* splitter + active-pane accent (§4)     */
    Popup        menu;

    /* ---- picker mode (#11): Files run as the system Open/Save dialog ---------
     * When launched with a /tmp/.picker-req pending, Files is a *dialog*: the same
     * chrome (sidebar, list, breadcrumb, filter, status) plus a footer with a Name
     * field (save) and Cancel / Open·Save buttons, returning the chosen path over
     * /tmp/.picker-res. See design/file-picker.md. */
    bool             picker = false;
    struct pick_req  preq;                       /* the parsed request (mode/dir/name/ext/title) */
    ui::Panel        footer;                      /* the picker-only bottom bar  */
    ui::Label        nameLbl;                     /* "Name:" (save mode)         */
    ui::TextField    nameFld;                     /* the filename field (save)   */
    ui::Button       okBtn, cancelBtn;            /* Open/Save + Cancel          */
    ui::ConfirmDialog overwrite;                  /* save: Replace / Keep Both / Cancel */
    char             pending_save[256] = {0};     /* save target awaiting the overwrite answer */
    int              FTH = 0;                     /* footer height (0 outside picker mode) */

    char          path[256] = HOMEDIR;
    struct dirent ents[NMAX];
    int           nents = 0;
    int           view[NMAX]; int nview = 0;     /* ents indices currently shown (after filter) */
    int           nshown = 0;                    /* non-hidden, picker-allowed item count (status "N items") */
    bool          filter_open = false;           /* the live filter bar is up ("/") */
    bool          loading = false;               /* guard: set_text() fires on_change mid-load */
    uint32_t     *appicon[NMAX] = {};
    int           appiw[NMAX] = {}, appih[NMAX] = {};
    char          appexec[NMAX][160] = {};
    bool          details_open = true;
    int           fw = 0, fh = 0, TBH = 0, SBW = 0, DPW = 0, STH = 0, listy = 0;
    char          hist[HISTN][256] = {};
    int           hist_n = 0, hist_i = -1;

    /* Tabs (§4): each tab is a saved nav state (folder + its own history + selection).
     * The live `path`/`hist`/`hist_i`/`hist_n` above are the *active* tab's working copy;
     * switching saves them into the outgoing tab and loads the incoming one. The store is
     * a growable heap array -- no fixed ceiling on how many tabs you can open. */
    struct Tab { char path[256]; char hist[HISTN][256]; int hist_n, hist_i, sel; };
    Tab          *tabs = nullptr; int ntabs = 0, curtab = 0, tabcap = 0;
    char          tabtitle_buf[TabStrip::VIS][32];
    const char   *tabtitle_ptr[TabStrip::VIS];

    /* Dual-pane / split view (§4): a self-contained second pane (its own folder + listing
     * + selection) beside the primary one. Files copy/move *between* the panes; `active`
     * is the pane with keyboard focus. The primary pane keeps all its machinery; pane 2 is
     * a lean navigable list (.. + folder drill) that shares the sort + copy helpers. */
    bool          split = false;
    int           active = 0;                    /* 0 = primary pane, 1 = pane 2 */
    char          path2[256] = HOMEDIR;
    struct dirent ents2[NMAX]; int nents2 = 0;   /* pane 2: dotfiles compacted out, sorted */
    int           sel2 = -1;

    AppEntry      apps[MAXAPPS]; int napps = 0;
    int           menu_mode = 0;                 /* 0 = context actions, 1 = open-with */
    char          ow_path[256] = {0};            /* file the open-with chooser targets */
    char          ow_ext[16] = {0};
    char          cut_src[256] = {0};            /* full path of a cut file, pending a paste-move */
    bool          have_cut = false;              /* the active clipboard file came from a Cut (Ctrl+X) */
    int           sort_key = FSORT_NAME, sort_desc = 0, sort_ff = 1;  /* the Sort menu state (§2) */

    int at_root()  const { return path[0] == '/' && path[1] == 0; }
    int has_up()   const { return at_root() ? 0 : 1; }
    int dpw_now()  const { return details_open ? DPW : 0; }

    /* the readdir comparator, driven by the user's Sort menu choice (§2) */
    int ent_cmp(const struct dirent *a, const struct dirent *b) const {
        int ad = (a->type == FT_DIR), bd = (b->type == FT_DIR);
        return filesort_cmp(a->name, ad, a->size, b->name, bd, b->size, sort_key, sort_desc, sort_ff);
    }
    /* Flip a View/Sort check mark on menu_spec in-place WITHOUT publishing -- so a batch
     * of updates costs a single win_setmenu (via sync_menus' menu_commit) instead of one
     * per item. (menu_set_checked re-publishes every call, which thrashed the bar -- and
     * raced menu clicks -- when re-syncing on every navigation.) */
    void menu_check_local(int m, int i, bool on) {
        if (m < 0 || m >= (int)menu_spec.nmenus) return;
        if (i < 0 || i >= (int)menu_spec.m[m].nitems) return;
        if (on) menu_spec.m[m].flags[i] |= (uint8_t)WMI_CHECKED;
        else    menu_spec.m[m].flags[i] &= (uint8_t)~WMI_CHECKED;
    }
    /* Reflect the active view mode + sort in the menu check marks (View = index 3, Sort =
     * index 4) and publish once. Called on navigation (restored prefs) and the setters. */
    void sync_menus() {
        if (picker) return;                          /* picker mode has no menu bar */
        menu_check_local(3, 0, view_mode == 1);      /* View: as Icons */
        menu_check_local(3, 1, view_mode == 0);      /* View: as List  */
        menu_check_local(3, 5, split);               /* View: Split View (§4) */
        menu_check_local(4, 0, sort_key == FSORT_NAME);
        menu_check_local(4, 1, sort_key == FSORT_KIND);
        menu_check_local(4, 2, sort_key == FSORT_SIZE);
        menu_check_local(4, 3, sort_desc != 0);
        menu_check_local(4, 4, sort_ff   != 0);
        menu_commit();                               /* single publish for the whole batch */
    }
    /* change the sort, re-read the folder (re-sorted, with icons re-aligned), update
     * the menu checks, and log a canary for the e2e. */
    void set_sort(int key, int desc, int ff) {
        sort_key = key; sort_desc = desc; sort_ff = ff;
        sync_menus();
        load_dir();
        persist_view();                              /* remember this folder's sort (§2) */
        const char *kn = key == FSORT_KIND ? "kind" : key == FSORT_SIZE ? "size" : "name";
        print("[files] sort "); print(kn); printc(' '); print(desc ? "desc" : "asc"); printc(' '); printu((unsigned)ff); print("\r\n");
        invalidate();
    }

    /* scan /Apps once for the Open With chooser */
    void load_apps() {
        struct dirent e[MAXAPPS * 2];
        int n = readdir("/Apps", e, MAXAPPS * 2);
        for (int i = 0; i < n && napps < MAXAPPS; i++) {
            if (!is_app_dir(e[i].type, e[i].name)) continue;
            char base[160]; join(base, sizeof base, "/Apps", e[i].name);
            char mp[200]; join(mp, sizeof mp, base, "manifest");
            int ml = 0; char *mb = sys_slurp(mp, &ml);
            if (!mb) continue;
            AppEntry *a = &apps[napps];
            char val[120];
            disp_name(e[i].name, a->label, sizeof a->label);
            if (manifest_get(mb, "name", val, sizeof val)) { int j = 0; for (; val[j] && j < 31; j++) a->label[j] = val[j]; a->label[j] = 0; }
            a->exec[0] = 0; a->icon = 0; a->iw = a->ih = 0;
            if (manifest_get(mb, "exec", val, sizeof val)) join(a->exec, sizeof a->exec, base, val);
            char ic[64];
            if (manifest_get(mb, "icon", ic, sizeof ic) && ic[0]) { char ip[200]; join(ip, sizeof ip, base, ic); a->icon = load_icon_argb(ip, &a->iw, &a->ih); }
            if (a->exec[0]) napps++;
            free(mb);
        }
    }

    void free_appcache() { for (int i = 0; i < NMAX; i++) { if (appicon[i]) { free(appicon[i]); appicon[i] = 0; } appexec[i][0] = 0; } }
    void load_dir() {
        free_appcache();
        nents = readdir(path, ents, NMAX);
        if (nents < 0) nents = 0;
        for (int i = 0; i < nents; i++)
            for (int j = i + 1; j < nents; j++)
                if (ent_cmp(&ents[j], &ents[i]) < 0) { struct dirent t = ents[i]; ents[i] = ents[j]; ents[j] = t; }
        for (int i = 0; i < nents; i++) {
            if (!is_app_dir(ents[i].type, ents[i].name)) continue;
            char base[256]; join(base, sizeof base, path, ents[i].name);
            char mpath[300]; join(mpath, sizeof mpath, base, "manifest");
            int ml = 0; char *mbuf = sys_slurp(mpath, &ml);
            if (!mbuf) continue;
            char val[120];
            if (manifest_get(mbuf, "exec", val, sizeof val)) join(appexec[i], sizeof appexec[i], base, val);
            char iconrel[64];
            if (manifest_get(mbuf, "icon", iconrel, sizeof iconrel) && iconrel[0]) {
                char ipath[320]; join(ipath, sizeof ipath, base, iconrel);
                appicon[i] = load_icon_argb(ipath, &appiw[i], &appih[i]);
            }
            free(mbuf);
        }
        apply_filter();                 /* sets list.count, resets sel, refreshes status */
        /* canary: the list content rect + row height (window-relative), so e2e can click
         * a given row deterministically -- row r's centre is (list.r.x+_, list.r.y + r*rowh
         * + rowh/2). Row 0 is the synthetic ".." up-entry when has_up(). */
        print("[files] listrect "); printu((unsigned)list.r.x); printc(' '); printu((unsigned)list.r.y);
        printc(' '); printu((unsigned)list.r.w); printc(' '); printu((unsigned)list.row_h); print("\r\n");
        struct statfs sf;                                /* §6 canary: the volume free bytes Files sees */
        if (statfs_(&sf) == 0) { print("[files] free "); printu(sf.free_bytes); print("\r\n"); }
    }
    /* map a visible list row to an ents[] index, or -1 for the ".." row / off-list */
    int ent_at(int row) const {
        int hu = has_up();
        if (row < 0 || (hu && row == 0)) return -1;
        int v = row - hu;
        return (v >= 0 && v < nview) ? view[v] : -1;
    }
    bool filtering() const { return filter_open && filterfld.length() > 0; }
    /* rebuild the visible set from the current filter text (case-insensitive substring
     * over the display name); resets the selection, like Finder/Dolphin's filter. */
    void apply_filter() {
        const char *q = filter_open ? filterfld.text() : "";
        nview = 0; nshown = 0;
        for (int i = 0; i < nents; i++) {
            if (ents[i].name[0] == '.') continue;            /* hide dotfiles (.Trash, .trashinfo, ...) */
            /* picker mode: directories always navigable; files only if their extension
             * is in the request's allowed list (empty list = all). */
            if (picker && ents[i].type == FT_FILE && !pickreq_ext_match(ents[i].name, preq.ext)) continue;
            nshown++;                                        /* a real (non-hidden, allowed) item */
            char label[64]; disp_name(ents[i].name, label, sizeof label);
            if (!tu_ci_contains(label, q)) continue;
            view[nview++] = i;
        }
        list.count = nview + has_up();
        list.sel = -1; list.top = 0;
        grid.count = list.count; grid.sel = -1; grid.top = 0;   /* keep the icon view in sync */
        details.has = false;
        update_status();
        if (q[0]) { print("[files] filter "); printu((unsigned)nview); print("\r\n"); }
        invalidate();
    }
    /* refresh the bottom status bar from the folder count + current selection */
    void update_status() {
        if (filtering()) snprintf(status.left, sizeof status.left, "%d of %d shown", nview, nshown);
        else             snprintf(status.left, sizeof status.left, "%d item%s", nshown, nshown == 1 ? "" : "s");
        struct statfs sf;                                /* §6: the volume's free space, shown as a */
        if (statfs_(&sf) == 0) { char fb[24]; human_bytes(sf.free_bytes, fb, sizeof fb);   /* details footer */
                                 snprintf(details.freeline, sizeof details.freeline, "%s free", fb); }
        else details.freeline[0] = 0;
        int hu = has_up();
        if (details.has && list.sel >= 0 && !(hu && list.sel == 0)) {
            if (details.is_file)
                snprintf(status.right, sizeof status.right, "%s selected  --  %u bytes", details.name, details.size);
            else {                                       /* a folder: show its recursive size (§8) */
                char hb[24]; human_bytes(details.size, hb, sizeof hb);
                snprintf(status.right, sizeof status.right, "%s selected  --  %s", details.name, hb);
            }
        } else status.right[0] = 0;
    }
    /* "/" opens the filter bar (or refocuses it if already open) */
    void open_filter() {
        if (!filter_open) {
            filter_open = true; filterfld.visible = true;
            loading = true; filterfld.set_text(""); loading = false;
            layout_widgets();
        }
        focus = &filterfld;
        apply_filter();
    }
    /* Esc (or "/" toggle) closes it, clearing the filter and restoring the full list */
    void close_filter() {
        if (!filter_open) return;
        filter_open = false; filterfld.visible = false;
        loading = true; filterfld.set_text(""); loading = false;
        layout_widgets();
        focus = &list;
        apply_filter();
    }
    /* Location bar edit mode (§3): the breadcrumb becomes a path field (Ctrl+L, or a
     * click on the bar's empty area / ellipsis). Enter navigates; Esc reverts. */
    void enter_path_edit() {
        if (editing_path) { focus = &pathfld; return; }
        editing_path = true; crumbbar.visible = false; pathfld.visible = true;
        pathfld.set_text(path); pathfld.caret = pathfld.length();
        pathfld.on_key(0x01);                 /* select-all so a fresh path replaces it */
        focus = &pathfld;
        print("[files] pathedit\r\n");
        layout_widgets(); invalidate();
    }
    void leave_path_edit() {
        if (!editing_path) return;
        editing_path = false; pathfld.visible = false; crumbbar.visible = true;
        focus = &list;
        print("[files] pathleave\r\n");          /* editor closed (Esc / click-away / before a commit's cd) */
        layout_widgets(); invalidate();
    }
    void commit_path_edit() {
        char p[256]; strncpy(p, pathfld.text(), sizeof p - 1); p[sizeof p - 1] = 0;
        leave_path_edit();
        struct fstat st;
        if (p[0] && stat_(p, &st) == 0 && st.type == FT_DIR) nav_to(p);
    }
    void load_path(const char *p) {
        strncpy(path, p, sizeof path - 1); path[sizeof path - 1] = 0;
        if (!path[0]) { path[0] = '/'; path[1] = 0; }
        if (filterfld.length() > 0) { loading = true; filterfld.set_text(""); loading = false; }  /* fresh folder, fresh filter */
        if (!picker) {                                       /* §2: restore this folder's remembered view */
            struct view_prefs v = load_view_prefs();
            view_mode = v.mode; sort_key = v.sort_key; sort_desc = v.sort_desc; sort_ff = v.sort_ff; zoom = v.zoom;
        }
        load_dir();                                          /* sorts with the restored sort_key */
        crumbbar.set_path(path); side.cur = path;
        if (!picker) {
            apply_view_state();                              /* reflect mode/zoom in the widgets + menus */
            print("[files] viewmem "); print(path); printc(' '); print(view_mode == 1 ? "icons" : "list");
            print(" zoom "); printu((unsigned)zoom); print("\r\n");   /* restored-view telemetry (§2) */
        }
        print("[files] cd "); print(path); print("\r\n");   /* navigation telemetry (breadcrumb/sidebar/edit) */
        if (tabs && curtab < ntabs) {                        /* §4: relabel the active tab's pill */
            strncpy(tabs[curtab].path, path, 255); tabs[curtab].path[255] = 0;
            sync_tabs();
        }
    }
    void update_nav() { back.enabled = hist_i > 0; fwd.enabled = hist_i < hist_n - 1; }
    void nav_to(const char *p) {
        load_path(p);
        if (hist_i >= 0 && eqn(hist[hist_i], path)) { update_nav(); return; }
        if (hist_i + 1 >= HISTN) { for (int i = 1; i < HISTN; i++) { strncpy(hist[i - 1], hist[i], 255); hist[i - 1][255] = 0; } hist_i = HISTN - 2; }
        hist_i++; strncpy(hist[hist_i], path, 255); hist[hist_i][255] = 0; hist_n = hist_i + 1;
        update_nav();
    }
    void go_back() { if (hist_i > 0)          { hist_i--; load_path(hist[hist_i]); update_nav(); invalidate(); } }
    void go_fwd()  { if (hist_i < hist_n - 1) { hist_i++; load_path(hist[hist_i]); update_nav(); invalidate(); } }
    void go_up() {
        if (at_root()) return;
        char parent[256]; strncpy(parent, path, sizeof parent - 1); parent[sizeof parent - 1] = 0;
        int last = -1; for (int i = 0; parent[i]; i++) if (parent[i] == '/') last = i;
        if (last <= 0) { parent[0] = '/'; parent[1] = 0; } else parent[last] = 0;
        nav_to(parent);
    }

    /* ---- Tabs (§4) ---------------------------------------------------------- *
     * The live nav state is the active tab; switch/new/close shuttle it to/from the
     * heap `tabs` store and reload the folder. The tab strip is hidden with one tab. */
    int  cur_sel() const { return (view_mode == 1) ? grid.sel : list.sel; }
    void tab_save(int i) {                                /* live nav state -> tabs[i] */
        if (i < 0 || i >= ntabs) return;
        Tab &T = tabs[i];
        strncpy(T.path, path, 255); T.path[255] = 0;
        for (int k = 0; k < HISTN; k++) { strncpy(T.hist[k], hist[k], 255); T.hist[k][255] = 0; }
        T.hist_n = hist_n; T.hist_i = hist_i; T.sel = cur_sel();
    }
    void tab_apply(int i) {                               /* tabs[i] -> live, reload its folder */
        Tab &T = tabs[i];
        for (int k = 0; k < HISTN; k++) { strncpy(hist[k], T.hist[k], 255); hist[k][255] = 0; }
        hist_n = T.hist_n; hist_i = T.hist_i;
        load_path(T.path); update_nav();
        if (T.sel >= 0 && T.sel < list.count) select_row(T.sel);
        else { details.has = false; list.sel = grid.sel = -1; update_status(); invalidate(); }
    }
    void grow_tabs() {
        if (ntabs < tabcap) return;
        int nc = tabcap ? tabcap * 2 : 4;
        Tab *nt = (Tab *)malloc(sizeof(Tab) * nc); if (!nt) return;
        for (int i = 0; i < ntabs; i++) nt[i] = tabs[i];
        if (tabs) free(tabs);
        tabs = nt; tabcap = nc;
    }
    void sync_tabs() {                                    /* refresh the strip from the store */
        int shown = ntabs > TabStrip::VIS ? TabStrip::VIS : ntabs;
        for (int i = 0; i < shown; i++) { tab_title(tabs[i].path, tabtitle_buf[i], 32); tabtitle_ptr[i] = tabtitle_buf[i]; }
        tabstrip.set(tabtitle_ptr, shown, curtab < shown ? curtab : shown - 1);
        layout_widgets();
        if (tabstrip.visible) {                           /* pill geometry canaries (e2e click targets) */
            tabstrip.relayout();
            print("[files] tabbar "); printu((unsigned)tabstrip.r.y); printc(' '); printu((unsigned)tabstrip.r.h);
            printc(' '); printu((unsigned)ntabs); printc(' '); printu((unsigned)curtab); print("\r\n");
            for (int i = 0; i < shown; i++) {
                print("[files] tabpos "); printu((unsigned)i); printc(' ');
                printu((unsigned)(tabstrip.r.x + tabstrip.px[i])); printc(' '); printu((unsigned)tabstrip.pw[i]); print("\r\n");
            }
            print("[files] tabplus "); printu((unsigned)(tabstrip.r.x + tabstrip.plusx)); printc(' ');
            printu((unsigned)tabstrip.plusw); print("\r\n");
        }
        invalidate();
    }
    void switch_tab(int j) {
        if (j < 0 || j >= ntabs || j == curtab) return;
        tab_save(curtab); curtab = j; tab_apply(curtab);
        print("[files] tab sel "); printu((unsigned)curtab); printc(' '); print(path); print("\r\n");
        sync_tabs();
    }
    void new_tab(const char *p) {
        if (picker) return;
        tab_save(curtab);
        grow_tabs(); if (ntabs >= tabcap) return;
        int idx = ntabs++; Tab &T = tabs[idx];
        strncpy(T.path, p, 255); T.path[255] = 0;
        for (int k = 0; k < HISTN; k++) T.hist[k][0] = 0;
        strncpy(T.hist[0], p, 255); T.hist[0][255] = 0; T.hist_n = 1; T.hist_i = 0; T.sel = -1;
        curtab = idx; tab_apply(curtab);
        print("[files] tab new "); print(p); print("\r\n");
        sync_tabs();
    }
    void new_tab_here() { new_tab(path); }
    void close_tab(int i) {
        if (picker || ntabs <= 1 || i < 0 || i >= ntabs) return;
        print("[files] tab close "); printu((unsigned)i); print("\r\n");
        for (int k = i; k < ntabs - 1; k++) tabs[k] = tabs[k + 1];
        ntabs--;
        if (curtab > i) curtab--;
        else if (curtab == i) { if (curtab >= ntabs) curtab = ntabs - 1; }
        tab_apply(curtab);
        sync_tabs();
    }
    void close_cur_tab() { close_tab(curtab); }
    void open_sel_new_tab() {                             /* context menu: Open in New Tab */
        int hu = has_up(); if (list.sel < 0 || (hu && list.sel == 0)) return;
        struct dirent *e = &ents[list.sel - hu];
        if (e->type != FT_DIR) return;
        char t[256]; join(t, sizeof t, path, e->name); new_tab(t);
    }

    /* ---- Split / dual pane (§4) -------------------------------------------- */
    static bool is_root(const char *p) { return p[0] == '/' && p[1] == 0; }
    int  has_up2() const { return is_root(path2) ? 0 : 1; }
    void load_dir2() {                                    /* read + compact + sort pane 2 */
        int n = readdir(path2, ents2, NMAX); if (n < 0) n = 0;
        int w = 0; for (int i = 0; i < n; i++) if (ents2[i].name[0] != '.') ents2[w++] = ents2[i];
        nents2 = w;
        for (int i = 0; i < nents2; i++)
            for (int j = i + 1; j < nents2; j++)
                if (ent_cmp(&ents2[j], &ents2[i]) < 0) { struct dirent t = ents2[i]; ents2[i] = ents2[j]; ents2[j] = t; }
        list2.count = nents2 + has_up2();
        if (sel2 >= list2.count) sel2 = -1;
        list2.sel = sel2; list2.top = 0;
        print("[files] pane2 cd "); print(path2); print("\r\n");
        print("[files] listrect2 "); printu((unsigned)list2.r.x); printc(' '); printu((unsigned)list2.r.y);
        printc(' '); printu((unsigned)list2.r.w); printc(' '); printu((unsigned)list2.row_h); print("\r\n");
    }
    void nav2(const char *p) { strncpy(path2, p, sizeof path2 - 1); path2[sizeof path2 - 1] = 0;
                               if (!path2[0]) { path2[0] = '/'; path2[1] = 0; } sel2 = -1; load_dir2(); invalidate(); }
    void go_up2() {
        if (is_root(path2)) return;
        char par[256]; strncpy(par, path2, sizeof par - 1); par[sizeof par - 1] = 0;
        int last = -1; for (int i = 0; par[i]; i++) if (par[i] == '/') last = i;
        if (last <= 0) { par[0] = '/'; par[1] = 0; } else par[last] = 0;
        nav2(par);
    }
    void select2(int row) { active = 1; splitdecor.active = 1; sel2 = list2.sel = row; invalidate(); }
    void enter2(int row) {
        active = 1; splitdecor.active = 1;
        if (has_up2() && row == 0) { go_up2(); return; }
        int idx = row - has_up2(); if (idx < 0 || idx >= nents2) return;
        struct dirent *e = &ents2[idx];
        if (e->type == FT_DIR && !is_app_dir(e->type, e->name)) { char t[256]; join(t, sizeof t, path2, e->name); nav2(t); }
    }
    void set_split(bool on) {
        if (split == on) return;
        split = on;
        if (split) { strncpy(path2, path, sizeof path2 - 1); path2[sizeof path2 - 1] = 0; sel2 = -1;
                     active = 0; splitdecor.active = 0; if (view_mode == 1) set_view(0); }  /* split is list-only */
        layout_widgets();
        if (split) load_dir2();
        sync_menus();                                      /* publish the View ▸ Split View tick */
        /* the primary list rect just changed (half width <-> full); re-emit so e2e clicks
         * land on the correct pane. */
        print("[files] listrect "); printu((unsigned)list.r.x); printc(' '); printu((unsigned)list.r.y);
        printc(' '); printu((unsigned)list.r.w); printc(' '); printu((unsigned)list.row_h); print("\r\n");
        print("[files] split "); printu(split ? 1u : 0u); print("\r\n");
        invalidate();
    }
    /* copy `name` from `srcdir` into `dstdir` (recursive; dedupes a colliding name) */
    void copy_across(const char *srcdir, const char *name, const char *dstdir) {
        char src[256], dst[256]; join(src, sizeof src, srcdir, name); join(dst, sizeof dst, dstdir, name);
        if (sys_exists(dst, 0)) { char dd[256]; dedup_path(dd, sizeof dd, dst); strncpy(dst, dd, sizeof dst - 1); dst[sizeof dst - 1] = 0; }
        if (copy_tree(src, dst) == 0) { print("[files] copy-across "); print(name); print(" -> "); print(dstdir); print("\r\n"); }
    }
    void copy_to_other(bool move) {                        /* §4: send active selection to the other pane */
        if (!split) return;
        if (active == 0) {
            int hu = has_up(); if (list.sel < 0 || (hu && list.sel == 0)) return;
            struct dirent *e = &ents[list.sel - hu];
            copy_across(path, e->name, path2);
            if (move) { char s[256]; join(s, sizeof s, path, e->name); rmrf(s); load_dir(); }
            load_dir2();
        } else {
            if (sel2 < 0 || (has_up2() && sel2 == 0)) return;
            int idx = sel2 - has_up2(); if (idx < 0 || idx >= nents2) return;
            struct dirent *e = &ents2[idx];
            copy_across(path2, e->name, path);
            if (move) { char s[256]; join(s, sizeof s, path2, e->name); rmrf(s); load_dir2(); }
            load_dir();
        }
        invalidate();
    }
    static void render_row2(void *ctx, int i, ui::Rect cell, bool sel) {
        FilesApp *a = (FilesApp *)ctx;
        if (!sel && (i & 1)) ugfx_fill(cell.x, cell.y, cell.w, cell.h, C_ZEBRA);
        int ix = cell.x + 12, ty = cell.y + (cell.h - a->fh) / 2, iyy = cell.y + (cell.h - 20) / 2;
        const char *name; int type; unsigned size = 0;
        if (a->has_up2() && i == 0) { name = ".."; type = FT_DIR; }
        else { int idx = i - a->has_up2(); if (idx < 0 || idx >= a->nents2) return;
               struct dirent *e = &a->ents2[idx]; name = e->name; type = e->type; size = e->size; }
        char label[64]; disp_name(name, label, sizeof label);
        blit_scaled(ix, iyy, 20, 20, fileicons_argb[file_icon_for(type, name)], FILEICON_SZ, FILEICON_SZ);
        ugfx_text(ix + 30, ty, label, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
        if (type == FT_FILE) { char sz[20]; snprintf(sz, sizeof sz, "%u B", size);
            ugfx_text(cell.x + cell.w - ugfx_text_w(sz) - 14, ty, sz, sel ? RGB(230, 238, 250) : TH_MUTED, UGFX_TRANSPARENT); }
    }

    /* open a non-.app file: use a remembered default app for its extension, else
     * pop the Open With chooser. */
    void open_file(const char *name) {
        char full[256]; join(full, sizeof full, path, name);
        char ext[16]; ext_of(name, ext, sizeof ext);
        char key[40]; snprintf(key, sizeof key, "open.default.%s", ext[0] ? ext : "_");
        const char *def = reg_get(key, "");
        if (def && def[0]) { struct fstat st; if (stat_(def, &st) == 0) { sys_open_with(def, full); return; } }
        open_with_chooser(name);
    }
    void open_with_chooser(const char *name) {
        char full[256]; join(full, sizeof full, path, name);
        strncpy(ow_path, full, sizeof ow_path - 1); ow_path[sizeof ow_path - 1] = 0;
        ext_of(name, ow_ext, sizeof ow_ext);
        menu_mode = 1;
        menu.reset();
        for (int i = 0; i < napps; i++) menu.add(apps[i].label, PK_ACTION, i, apps[i].icon, apps[i].iw, apps[i].ih);
        if (napps == 0) menu.add("No apps found", PK_ACTION, -1, 0, 0, 0);
        menu.add("", PK_SEP, 0, 0, 0, 0);
        char tl[40]; snprintf(tl, sizeof tl, "Always use for .%s", ow_ext[0] ? ow_ext : "file");
        menu.add(tl, PK_TOGGLE, -2, 0, 0, 0);
        menu.show(w / 2 - 90, h / 2 - 60);
    }

    void enter(int row) {
        if (has_up() && row == 0) { go_up(); return; }
        int idx = ent_at(row);
        if (idx < 0) return;
        struct dirent *e = &ents[idx];
        if (picker) {                                       /* in a picker, never launch apps */
            if (e->type == FT_DIR) { char t[256]; join(t, sizeof t, path, e->name); nav_to(t); return; }
            if (preq.mode == PICK_OPEN) { char f[256]; join(f, sizeof f, path, e->name); finish_pick(f); }
            else { nameFld.set_text(e->name); nameFld.caret = nameFld.length(); do_save(); }  /* save over a file */
            return;
        }
        if (is_app_dir(e->type, e->name)) { if (appexec[idx][0]) sys_launch(appexec[idx]); return; }
        if (e->type == FT_DIR) { char t[256]; join(t, sizeof t, path, e->name); nav_to(t); }
        else open_file(e->name);
    }
    /* A row click mutates the details pane + status bar, which lie outside the
     * list's own rect; without a repaint request the damage-tracked frame would
     * only refresh the list. Repaint the whole window (selection is low-frequency). */
    void select_row(int row) {
        active = 0; splitdecor.active = 0;               /* a click in the primary pane makes it active (§4) */
        list.sel = grid.sel = row;                       /* single selection, shared by both views */
        int idx = ent_at(row);
        if (idx < 0) { details.has = false; update_status(); update_pick_btn(); invalidate(); return; }
        struct dirent *e = &ents[idx];
        if (picker && preq.mode == PICK_SAVE && e->type == FT_FILE) {    /* click a file -> its name into the field */
            nameFld.set_text(e->name); nameFld.caret = nameFld.length();
        }
        disp_name(e->name, details.name, sizeof details.name);
        details.kind = kind_for(e->type, e->name);
        details.is_file = (e->type == FT_FILE);
        details.size = e->size;
        details.mtime = e->mtime;                  /* §8: surface the file's modified time */
        details.icon = appicon[idx]; details.iw = appiw[idx]; details.ih = appih[idx];
        details.file_icon = file_icon_for(e->type, e->name);
        join(details.where, sizeof details.where, path, e->name);
        /* §8 Get Info: owner + read-only lock, recursive folder size, default app */
        struct fstat st;
        if (stat_(details.where, &st) == 0) {
            details.owner = st.owner;
            details.locked = info_is_locked(st.owner, (unsigned)getuid()) != 0;
        } else { details.owner = INFO_UID_USER; details.locked = false; }
        details.dir_items = 0;
        if (e->type == FT_DIR) {                          /* du-walk the folder for its size */
            unsigned b = 0, it = 0; dir_usage(details.where, &b, &it);
            details.size = b; details.dir_items = it;
        }
        details.opens_with[0] = 0;                        /* default app for this file's type */
        if (e->type == FT_FILE) {
            char ext[16]; ext_of(e->name, ext, sizeof ext);
            char k[40]; snprintf(k, sizeof k, "open.default.%s", ext[0] ? ext : "_");
            const char *def = reg_get(k, "");
            if (def && def[0]) disp_name(basename_of(def), details.opens_with, sizeof details.opens_with);
        }
        details.has = true;
        print("[files] sel "); print(details.name);
        print(details.locked ? " ro owner=" : " rw owner="); printu(details.owner);
        print(" size="); printu(details.size);
        if (e->type == FT_DIR) { print(" items="); printu(details.dir_items); }
        print("\r\n");
        update_status();
        update_pick_btn();
        invalidate();
    }
    static void rmrf(const char *p) {
        struct fstat st; if (stat_(p, &st) < 0) return;
        if (st.type == FT_DIR) {
            for (;;) {
                struct dirent e[64]; int n = readdir(p, e, 64);
                if (n <= 0) break;
                for (int i = 0; i < n; i++) { char c[256]; join(c, sizeof c, p, e[i].name); rmrf(c); }
            }
            rmdir(p);
        } else funlink(p);
    }
    /* recursive copy `src` -> `dst` (dst must not already exist): a file copies its
     * bytes, a directory is recreated and each child copied into it. This is the
     * "foundation recursive copy" Duplicate (§12) and folder paste build on. The
     * per-level listing is heap-allocated (NMAX dirents = 10K) so a deep tree can't
     * blow the small userspace stack. Returns 0 on success, -1 if anything failed. */
    static int copy_tree(const char *src, const char *dst) {
        struct fstat st; if (stat_(src, &st) < 0) return -1;
        if (st.type != FT_DIR) {                                   /* a plain file: slurp + spit */
            int n = 0; char *b = sys_slurp(src, &n);
            if (!b) { sys_spit(dst, "", 0); return 0; }            /* empty/zero-length source */
            int w = sys_spit(dst, b, n); free(b);
            return (w == n) ? 0 : -1;
        }
        if (mkdir(dst) != 0) return -1;
        struct dirent *e = (struct dirent *)malloc(sizeof(struct dirent) * NMAX);
        if (!e) return -1;
        int n = readdir(src, e, NMAX), rc = 0;
        for (int i = 0; i < n; i++) {
            char cs[256], cd[256];
            join(cs, sizeof cs, src, e[i].name);
            join(cd, sizeof cd, dst, e[i].name);
            if (copy_tree(cs, cd) != 0) rc = -1;
        }
        free(e);
        return rc;
    }
    /* du-style recursive usage under `path`: accumulates total bytes + the number of
     * entries (descendants, not the folder itself). Drives the Get Info recursive
     * folder size (§8). The volume is tiny (a few MiB) so a synchronous walk on
     * selection is fine; like copy_tree the per-level listing is heap-allocated. */
    static void dir_usage(const char *path, unsigned *bytes, unsigned *items) {
        struct dirent *e = (struct dirent *)malloc(sizeof(struct dirent) * NMAX);
        if (!e) return;
        int n = readdir(path, e, NMAX);
        for (int i = 0; i < n; i++) {
            (*items)++;
            char c[256]; join(c, sizeof c, path, e[i].name);
            if (e[i].type == FT_DIR) dir_usage(c, bytes, items);
            else                     *bytes += e[i].size;
        }
        free(e);
    }
    /* ---- Trash (§9) --------------------------------------------------------- *
     * Delete in a normal folder MOVES the item to ~/.Trash (a rename, so it's cheap
     * and works for whole directories); a sidecar (~/.Trash/.trashinfo) records each
     * item's origin so Put Back can restore it. Delete *inside* the Trash, or Empty
     * Trash, removes for good. The Trash and its sidecar are hidden dotfiles. */
    static const char *basename_of(const char *full) {
        const char *b = full; for (const char *p = full; *p; p++) if (*p == '/') b = p + 1; return b;
    }
    void ensure_trash() { struct fstat st; if (stat_(TRASHDIR, &st) != 0) mkdir(TRASHDIR); }
    /* are we viewing the Trash (or a folder inside it)? */
    int in_trash() const {
        int n = (int)strlen(TRASHDIR);
        return strncmp(path, TRASHDIR, n) == 0 && (path[n] == 0 || path[n] == '/');
    }
    /* thin FS wrappers over the pure trashinfo.h codec (read-modify-write the small file) */
    void trashinfo_append(const char *tname, const char *origpath) {
        int old = 0; char *cur = sys_slurp(TRASHINFO, &old);
        int need = old + (int)strlen(tname) + (int)strlen(origpath) + 3;
        char *buf = (char *)malloc(need);
        if (buf) { int n = trashinfo_add(buf, need, cur ? cur : "", cur ? old : 0, tname, origpath);
                   sys_spit(TRASHINFO, buf, n); free(buf); }
        free(cur);
    }
    int trashinfo_lookup(const char *tname, char *out, int cap) {
        int len = 0; char *cur = sys_slurp(TRASHINFO, &len);
        if (!cur) { if (cap > 0) out[0] = 0; return 0; }
        int r = trashinfo_find(cur, len, tname, out, cap);
        free(cur);
        return r;
    }
    void trashinfo_remove(const char *tname) {
        int len = 0; char *cur = sys_slurp(TRASHINFO, &len);
        if (!cur) return;
        char *buf = (char *)malloc(len + 1);
        if (buf) { int n = trashinfo_drop(buf, len + 1, cur, len, tname);
                   if (n == 0) funlink(TRASHINFO); else sys_spit(TRASHINFO, buf, n); free(buf); }
        free(cur);
    }
    /* move `full` into the Trash (deduping its name there) and record where it came from */
    void move_to_trash(const char *full) {
        ensure_trash();
        char dst[256]; join(dst, sizeof dst, TRASHDIR, basename_of(full));
        if (sys_exists(dst, 0)) { char d2[256]; dedup_path(d2, sizeof d2, dst);
                                  strncpy(dst, d2, sizeof dst - 1); dst[sizeof dst - 1] = 0; }
        if (rename_(full, dst) != 0) { rmrf(full); return; }           /* fall back to a hard delete */
        trashinfo_append(basename_of(dst), full);
        print("[files] trash "); print(basename_of(full)); print("\r\n");
    }
    void do_delete() {
        int idx = ent_at(list.sel);
        if (idx < 0) return;
        struct dirent *e = &ents[idx];
        char child[256]; join(child, sizeof child, path, e->name);
        if (in_trash()) { rmrf(child); trashinfo_remove(e->name); }    /* already trashed: delete for good */
        else            move_to_trash(child);                          /* normal folder: move to Trash */
        load_dir();
    }
    /* "Put Back": rename a trashed item to its recorded origin (deduping on collision),
     * dropping its sidecar line. With no record, fall back to dropping it in Home. */
    void restore_from_trash() {
        int idx = ent_at(list.sel);
        if (idx < 0) return;
        char tname[128]; strncpy(tname, ents[idx].name, sizeof tname - 1); tname[sizeof tname - 1] = 0;
        char src[256]; join(src, sizeof src, TRASHDIR, tname);
        char dst[256]; if (!trashinfo_lookup(tname, dst, sizeof dst)) join(dst, sizeof dst, HOMEDIR, tname);
        if (sys_exists(dst, 0)) { char d2[256]; dedup_path(d2, sizeof d2, dst);
                                  strncpy(dst, d2, sizeof dst - 1); dst[sizeof dst - 1] = 0; }
        if (rename_(src, dst) == 0) { trashinfo_remove(tname);
            print("[files] untrash "); print(tname); print("\r\n"); }
        load_dir();
    }
    /* Empty Trash: hard-remove every item in ~/.Trash and clear the sidecar. */
    void empty_trash() {
        ensure_trash();
        for (;;) {
            struct dirent e[64]; int n = readdir(TRASHDIR, e, 64);
            if (n <= 0) break;
            int did = 0;
            for (int i = 0; i < n; i++) {
                if (eqn(e[i].name, ".trashinfo")) continue;            /* keep until the end */
                char c[256]; join(c, sizeof c, TRASHDIR, e[i].name); rmrf(c); did = 1;
            }
            if (!did) break;                                           /* only the sidecar is left */
        }
        funlink(TRASHINFO);
        print("[files] trash empty\r\n");
        if (in_trash()) load_dir();
    }
    /* Ctrl+C / Ctrl+X: stash the selected file on the clipboard ring; Cut also
     * remembers the source so a later Paste moves it. Files only for now -- copying a
     * whole directory needs a recursive walk we don't do yet. */
    void copy_sel(bool cut) {
        int idx = ent_at(list.sel);
        if (idx < 0) return;
        struct dirent *e = &ents[idx];
        if (e->type != FT_FILE) return;
        char full[256]; join(full, sizeof full, path, e->name);
        int n = 0; char *b = sys_slurp(full, &n);
        if (!b) return;
        clip_put(CLIP_FILE, e->name, b, n);
        free(b);
        have_cut = cut;
        if (cut) { strncpy(cut_src, full, sizeof cut_src - 1); cut_src[sizeof cut_src - 1] = 0; }
        else cut_src[0] = 0;
        print(cut ? "[files] cut " : "[files] copy "); print(e->name); print("\r\n");
    }
    /* Ctrl+V: drop the active clipboard file into the current directory. Dedupes the
     * name ("copy of X") rather than clobbering; a pending Cut deletes the source. */
    void paste() {
        if (clip_count() <= 0) return;
        int idx = clip_active(-1);
        struct clipinfo ci;
        if (idx < 0 || clip_info(idx, &ci) != 0 || ci.type != CLIP_FILE || ci.len == 0) return;
        char name[40]; strncpy(name, ci.name, sizeof name - 1); name[sizeof name - 1] = 0;
        char dst[256]; join(dst, sizeof dst, path, name);
        if (have_cut && cut_src[0] && eqn(dst, cut_src)) {     /* cut+paste in the same dir: no-op */
            have_cut = false; cut_src[0] = 0; return;
        }
        if (sys_exists(dst, 0)) {                              /* don't clobber an existing file */
            char alt[80]; snprintf(alt, sizeof alt, "copy of %s", name);
            join(dst, sizeof dst, path, alt);
        }
        char *buf = (char *)malloc(ci.len);
        if (!buf) return;
        int n = clip_get(idx, buf, ci.len);
        if (n > 0) sys_spit(dst, buf, n);
        free(buf);
        if (n > 0 && have_cut && cut_src[0]) { rmrf(cut_src); have_cut = false; cut_src[0] = 0; }
        print("[files] paste "); print(dst); print("\r\n");
        load_dir(); invalidate();
    }
    /* select the visible entry named `name` (after a load_dir) and return its row, or -1 */
    int select_named(const char *name) {
        for (int v = 0; v < nview; v++) if (eqn(ents[view[v]].name, name)) {
            int row = v + has_up(); select_row(row);
            if (view_mode == 1) grid.ensure_visible(row); else list.ensure_visible(row);
            return row;
        }
        return -1;
    }
    void make_folder() {
        char name[40], child[256];
        for (int k = 0; k < 1000; k++) {
            if (k == 0) strncpy(name, "newfolder", sizeof name); else snprintf(name, sizeof name, "newfolder%d", k);
            join(child, sizeof child, path, name);
            struct fstat st;
            if (stat_(child, &st) < 0) {
                mkdir(child); load_dir();
                if (select_named(name) >= 0) start_rename();   /* Finder-style: name the new folder now */
                return;
            }
        }
    }
    /* §12: New (Text) File -- create an empty "newfile.txt" here (deduped so repeats
     * never clobber) and drop into rename over it, exactly like New Folder. */
    void make_file() {
        char name[40], child[256];
        for (int k = 0; k < 1000; k++) {
            if (k == 0) strncpy(name, "newfile.txt", sizeof name); else snprintf(name, sizeof name, "newfile%d.txt", k);
            join(child, sizeof child, path, name);
            struct fstat st;
            if (stat_(child, &st) < 0) {
                sys_spit(child, "", 0); load_dir();
                if (select_named(name) >= 0) start_rename();   /* name it now, Finder-style */
                return;
            }
        }
    }
    /* §12: Duplicate -- clone the selected item beside itself as "<name> copy" (files
     * copy their bytes, folders copy recursively via copy_tree). Picks the first free
     * "copy"/"copy 2"/... name, then selects the new item. Not offered in the Trash. */
    void duplicate_sel() {
        int idx = ent_at(list.sel);
        if (idx < 0 || in_trash() || !dir_writable()) return;
        char src[256]; join(src, sizeof src, path, ents[idx].name);
        char dst[256];
        int k = 1; for (; k < 1000; k++) { dup_candidate(dst, sizeof dst, src, k); if (!sys_exists(dst, 0)) break; }
        if (k >= 1000 || copy_tree(src, dst) != 0) return;
        print("[files] duplicate "); print(basename_of(dst)); print("\r\n");
        load_dir();
        select_named(basename_of(dst));
        invalidate();
    }

    /* ---- in-place rename ---------------------------------------------------- */
    /* screen rect for the rename field over the focused item in the active view */
    ui::Rect rename_rect() {
        if (view_mode == 1) {                                /* icon view: over the tile's name */
            int sel = grid.sel; if (sel < 0) return { 0, 0, 0, 0 };
            int c = grid.cols(), col = sel % c, row = sel / c;
            int x = grid.r.x + col * grid.tile_w, y = grid.r.y + row * grid.tile_h - grid.top;
            return { x + 3, y + 8 + grid.icon_box + 2, grid.tile_w - 6, fh + 6 };
        }
        int row = list.sel; if (row < 0) return { 0, 0, 0, 0 };   /* list view: over the row's name */
        int y = list.r.y + (row - list.top) * list.row_h;
        return { list.r.x + 42, y + 3, list.r.w - 84, list.row_h - 6 };
    }
    void start_rename() {
        if (picker || editing_path || filter_open) return;
        int sel = (view_mode == 1) ? grid.sel : list.sel;
        int idx = ent_at(sel);
        if (idx < 0) return;                                 /* can't rename the ".." row */
        if (view_mode == 1) grid.ensure_visible(sel); else list.ensure_visible(sel);
        rename_idx = idx; renaming = true; renamefld.visible = true;
        renamefld.r = rename_rect();
        renamefld.set_text(ents[idx].name);
        renamefld.caret = renamefld.length();
        renamefld.on_key(0x01);                              /* select-all so a fresh name replaces it */
        focus = &renamefld;
        print("[files] renaming "); print(ents[idx].name); print("\r\n");
        invalidate();
    }
    void end_rename() {
        renaming = false; renamefld.visible = false;
        focus = (view_mode == 1) ? (ui::Widget *)&grid : (ui::Widget *)&list;
    }
    void commit_rename() {
        if (!renaming) return;
        int idx = rename_idx; end_rename();
        if (idx < 0 || idx >= nents) { invalidate(); return; }
        char oldn[256]; strncpy(oldn, ents[idx].name, sizeof oldn - 1); oldn[sizeof oldn - 1] = 0;
        char nn[256]; strncpy(nn, renamefld.text(), sizeof nn - 1); nn[sizeof nn - 1] = 0;
        if (!nn[0] || eqn(nn, oldn)) { invalidate(); return; }   /* empty or unchanged */
        char oldp[512], newp[512]; join(oldp, sizeof oldp, path, oldn); join(newp, sizeof newp, path, nn);
        if (sys_exists(newp, 0)) { invalidate(); return; }       /* don't clobber an existing name */
        if (rename_(oldp, newp) == 0) { print("[files] rename "); print(oldn); print(" -> "); print(nn); print("\r\n"); }
        load_dir(); invalidate();
    }
    void cancel_rename() { if (renaming) { end_rename(); invalidate(); } }

    /* ---- picker mode helpers (#11) ----------------------------------------- */
    /* may the user create/overwrite in the current folder? (system-owned => no) */
    bool dir_writable() const {
        struct fstat st;
        if (stat_(path, &st) != 0) return true;            /* unknown -> let the write decide */
        return tos_may_write(getuid(), (int)st.owner) != 0;
    }
    /* "Keep Both": build a non-colliding "<stem> (N)<ext>" sibling of `full`
     * (picked.txt -> "picked (2).txt"; the dot is only a suffix in the basename). */
    void dedup_path(char *out, int cap, const char *full) {
        int n = (int)strlen(full), slash = -1, dot = -1;
        for (int i = 0; i < n; i++) if (full[i] == '/') slash = i;
        for (int i = slash + 1; i < n; i++) if (full[i] == '.') dot = i;
        char dir[256] = {0}, stem[128] = {0}, ext[40] = {0};
        int di = 0; for (int i = 0; i <= slash && di < (int)sizeof dir - 1; i++) dir[di++] = full[i]; dir[di] = 0;
        int stem_end = (dot > slash) ? dot : n;
        int si = 0; for (int i = slash + 1; i < stem_end && si < (int)sizeof stem - 1; i++) stem[si++] = full[i]; stem[si] = 0;
        int ei = 0; if (dot > slash) for (int i = dot; i < n && ei < (int)sizeof ext - 1; i++) ext[ei++] = full[i]; ext[ei] = 0;
        for (int k = 2; k < 1000; k++) { snprintf(out, cap, "%s%s (%d)%s", dir, stem, k, ext); if (!sys_exists(out, 0)) return; }
        snprintf(out, cap, "%s%s (copy)%s", dir, stem, ext);
    }
    /* hand a chosen path back to the caller and quit: write /tmp/.picker-res (read by
     * sys_pick_poll) then end the event loop so the process exits and the caller reaps it. */
    void finish_pick(const char *p) {
        sys_pick_result(p);                                /* write the pid-namespaced result temp file */
        print("[files] picked "); print(p); print("\r\n");
        running = false;
    }
    /* Cancel / close: leave no result file (sys_pick_begin already unlinked it), so the
     * caller's poll reports -1. */
    void cancel_pick() { print("[files] pick cancel\r\n"); running = false; }
    /* Save: validate the name + folder, warn on overwrite, else finish. */
    void do_save() {
        const char *nm = nameFld.text();
        if (!nm[0] || !dir_writable()) return;             /* nothing typed / not writable here */
        char target[256]; join(target, sizeof target, path, nm);
        struct fstat st;
        if (stat_(target, &st) == 0 && st.type == FT_FILE) {
            strncpy(pending_save, target, sizeof pending_save - 1); pending_save[sizeof pending_save - 1] = 0;
            char msg[160]; snprintf(msg, sizeof msg, "\"%s\" already exists in this folder.", nm);
            overwrite.show("Replace file?", msg, "Replace", "Keep Both", "Cancel");
            invalidate(); return;
        }
        finish_pick(target);
    }
    /* enable Open/Save when the action is valid (a row picked / a name typed in a
     * writable folder); the footer button greys otherwise. */
    void update_pick_btn() {
        if (!picker) return;
        if (preq.mode == PICK_SAVE) okBtn.enabled = (nameFld.length() > 0 && dir_writable());
        else                        okBtn.enabled = (list.sel >= 0);
    }

    static void render_row(void *ctx, int i, ui::Rect cell, bool sel) {
        FilesApp *a = (FilesApp *)ctx;
        if (!sel && (i & 1)) ugfx_fill(cell.x, cell.y, cell.w, cell.h, C_ZEBRA);
        int ix = cell.x + 12, ty = cell.y + (cell.h - a->fh) / 2, iyy = cell.y + (cell.h - 20) / 2;
        const char *name; int type; unsigned size = 0; int idx = -1;
        if (a->has_up() && i == 0) { name = ".."; type = FT_DIR; }
        else { idx = a->ent_at(i); if (idx < 0) return; struct dirent *e = &a->ents[idx]; name = e->name; type = e->type; size = e->size; }
        char label[64]; disp_name(name, label, sizeof label);
        if (idx >= 0 && is_app_dir(type, name) && a->appicon[idx]) blit_scaled(ix, iyy, 20, 20, a->appicon[idx], a->appiw[idx], a->appih[idx]);
        else blit_scaled(ix, iyy, 20, 20, fileicons_argb[file_icon_for(type, name)], FILEICON_SZ, FILEICON_SZ);
        ugfx_text(ix + 30, ty, label, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
        if (type == FT_FILE) {
            char sz[20]; snprintf(sz, sizeof sz, "%u B", size);
            ugfx_text(cell.x + cell.w - ugfx_text_w(sz) - 14, ty, sz, sel ? RGB(230, 238, 250) : TH_MUTED, UGFX_TRANSPARENT);
        }
    }

    /* icon-view tile: a large icon centred in the cell with a centred, width-clamped
     * name below. Same item indexing as the rows (0 = ".." when present). */
    static void render_tile(void *ctx, int i, ui::Rect cell, bool sel, int icon_box) {
        FilesApp *a = (FilesApp *)ctx;
        const char *name; int type; int idx = -1;
        if (a->has_up() && i == 0) { name = ".."; type = FT_DIR; }
        else { idx = a->ent_at(i); if (idx < 0) return; struct dirent *e = &a->ents[idx]; name = e->name; type = e->type; }
        char label[64]; disp_name(name, label, sizeof label);
        int cx = cell.x + cell.w / 2, iy = cell.y + 8;
        if (idx >= 0 && is_app_dir(type, name) && a->appicon[idx])
            blit_scaled(cx - icon_box / 2, iy, icon_box, icon_box, a->appicon[idx], a->appiw[idx], a->appih[idx]);
        else
            blit_scaled(cx - icon_box / 2, iy, icon_box, icon_box, fileicons_argb[file_icon_for(type, name)], FILEICON_SZ, FILEICON_SZ);
        char trunc[64];                                  /* clamp the name to the tile width */
        int maxc = a->fw > 0 ? (cell.w - 8) / a->fw : 8; if (maxc < 1) maxc = 1; if (maxc > 63) maxc = 63;
        int ll = (int)strlen(label);
        if (ll <= maxc) { int j = 0; for (; label[j]; j++) trunc[j] = label[j]; trunc[j] = 0; }
        else { int k = maxc - 1; if (k < 1) k = 1; for (int j = 0; j < k; j++) trunc[j] = label[j]; trunc[k] = '.'; trunc[k + 1] = 0; }
        int tw = ugfx_text_w(trunc), ty = iy + icon_box + 5;
        ugfx_text(cx - tw / 2, ty, trunc, sel ? RGB(255, 255, 255) : TH_TEXT, UGFX_TRANSPARENT);
    }
    /* zoom presets: {tile_w, tile_h, icon_box} per level */
    void apply_zoom() {
        static const int Z[3][3] = { { 80, 78, 44 }, { 100, 92, 56 }, { 128, 110, 76 } };
        if (zoom < 0) zoom = 0; if (zoom > 2) zoom = 2;
        grid.tile_w = Z[zoom][0]; grid.tile_h = Z[zoom][1]; grid.icon_box = Z[zoom][2];
        grid.clamp_top();
    }
    /* Reflect view_mode + zoom in the widgets and menu checks WITHOUT persisting -- used
     * on navigate (where prefs were just restored from the registry) and by the
     * user-driven setters below (which persist on top). */
    void apply_view_state() {
        bool icons = (view_mode == 1) && !picker;
        grid.visible = icons; list.visible = !icons;
        grid.sel = list.sel; grid.count = list.count;    /* carry the selection across */
        focus = icons ? (ui::Widget *)&grid : (ui::Widget *)&list;
        sync_menus();
        if (icons) { apply_zoom(); grid.ensure_visible(grid.sel >= 0 ? grid.sel : 0); }
        layout_widgets();
    }
    /* Per-folder view memory (§2): one registry value per folder path. "view.default"
     * is a stable global fallback for never-visited folders (left at the built-in default
     * until a future "Use as defaults" action). The picker is transient -- no memory. */
    void persist_view() {
        if (picker) return;
        struct view_prefs v = { view_mode, sort_key, sort_desc, sort_ff, zoom };
        char val[32]; viewmem_encode(&v, val, sizeof val);
        char key[VIEWMEM_KEYMAX]; viewmem_key(path, key, sizeof key);
        reg_set(key, val); reg_save();
    }
    struct view_prefs load_view_prefs() {
        struct view_prefs def = viewmem_decode(reg_get("view.default", ""));
        char key[VIEWMEM_KEYMAX]; viewmem_key(path, key, sizeof key);
        const char *s = reg_get(key, "");
        return s[0] ? viewmem_decode(s) : def;
    }
    void set_view(int mode) {
        view_mode = mode ? 1 : 0;
        apply_view_state();
        persist_view();
        print("[files] view "); print(view_mode == 1 ? "icons" : "list"); print("\r\n");
        invalidate();
    }
    void set_zoom(int z) {
        zoom = z; apply_zoom();
        persist_view();
        print("[files] zoom "); printu((unsigned)zoom); print("\r\n");
        layout_widgets(); invalidate();
    }

    void layout_widgets() {
        fw = ugfx_font_w(); fh = ugfx_font_h();
        TBH = fh + 20;
        STH = fh + 12;
        SBW = 11 * fw + 34; if (SBW < 150) SBW = 150;
        DPW = 20 * fw + 28; if (DPW < 200) DPW = 200; if (DPW > 264) DPW = 264;
        int dp = (details_open && !picker && !split) ? DPW : 0;   /* details hidden in split (§4) */
        int mainx = SBW, mainw = w - SBW - dp;

        side.r = { 0, 0, SBW, h };
        side.row_h = fh + 10; side.head_h = fh + 18;

        bar.r = { SBW, 0, w - SBW, TBH };
        int sz = TBH - 12, bx = SBW + 8, by = 6;
        IconButton *b[5] = { &back, &fwd, &up, &newf, &del };
        int gl[5] = { G_BACK, G_FWD, G_UP, G_NEWF, G_TRASH };
        for (int i = 0; i < 5; i++) {
            b[i]->glyph = gl[i]; b[i]->r = { bx, by, sz, sz }; bx += sz + 5;
            if (i == 2) bx += 12;                            /* gap after the nav cluster */
        }
        int iw_ = ugfx_text_w("Info") + 18;
        info.text = "Info"; info.r = { w - iw_ - 8, by, iw_, sz };

        int locy = TBH + 4, loch = fh + 8;               /* the location-bar band          */
        crumbbar.r = { mainx + 6, locy, mainw - 12, loch };
        pathfld.r  = { mainx + 8, locy, mainw - 16, loch };
        crumbbar.visible = !editing_path;
        pathfld.visible  = editing_path;
        int loc_bottom = locy + loch + 2;                /* where the location bar ends   */
        int TSH = (!picker && ntabs > 1) ? fh + 12 : 0;  /* the tab strip band (§4); hidden w/ 1 tab */
        tabstrip.visible = TSH > 0;
        tabstrip.r = { mainx, loc_bottom, mainw, TSH };
        int tabs_bottom = loc_bottom + TSH;
        int FBH = filter_open ? fh + 16 : 0;             /* the live-filter bar band       */
        filterfld.visible = filter_open;
        filterfld.r = { mainx + 10, tabs_bottom + 3, mainw - 20, fh + 8 };
        FTH = picker ? fh + 26 : 0;                      /* the picker footer band         */
        listy = tabs_bottom + FBH;
        int lh = h - listy - STH - FTH; if (lh < fh) lh = fh;
        list.row_h = list2.row_h = fh + 12;
        if (split && !picker) {                          /* §4: two panes side by side */
            int half = mainw / 2;
            list.r  = { mainx, listy, half - 1, lh };
            list2.r = { mainx + half + 1, listy, mainw - half - 1, lh };
            list.visible = true; list2.visible = true; grid.visible = false;
            splitdecor.visible = true; splitdecor.on = true;
            splitdecor.pane[0] = list.r; splitdecor.pane[1] = list2.r;
        } else {
            list.r = { mainx, listy, mainw, lh };
            grid.r = list.r;                             /* the icon view shares the content area */
            bool icons = (view_mode == 1) && !picker;
            list.visible = !icons; grid.visible = icons;
            list2.visible = false; splitdecor.visible = false; splitdecor.on = false;
            grid.clamp_top();
        }
        renamefld.visible = renaming;                    /* the in-place rename overlay tracks the item */
        if (renaming) renamefld.r = rename_rect();
        status.r = { mainx, h - STH - FTH, mainw, STH };
        details.r = { w - DPW, TBH, DPW, h - TBH };
        details.visible = details_open && !picker && !split;

        if (picker) {                                    /* footer: [Name: ___]  Cancel  Open/Save */
            int fy = h - FTH, pad = 8, bh = FTH - 12, by2 = fy + 6;
            footer.r = { 0, fy, w, FTH };
            const char *oklbl = preq.mode == PICK_SAVE ? "Save" : "Open";
            int okw = ugfx_text_w(oklbl) + 30; if (okw < 72) okw = 72;
            int caw = ugfx_text_w("Cancel") + 24; if (caw < 72) caw = 72;
            okBtn.text = oklbl;     okBtn.r     = { w - okw - pad, by2, okw, bh };
            cancelBtn.text = "Cancel"; cancelBtn.r = { w - okw - caw - pad - 8, by2, caw, bh };
            if (preq.mode == PICK_SAVE) {
                int lblw = ugfx_text_w("Name:") + 10;
                nameLbl.r = { mainx + pad, fy + (FTH - fh) / 2, lblw, fh };
                int nfx = mainx + pad + lblw;
                int nfw = cancelBtn.r.x - nfx - 12; if (nfw < 80) nfw = 80;
                nameFld.r = { nfx, by2, nfw, bh };
            }
        }
    }

    bool build() {
        /* picker mode (#11): if a /tmp/.picker-req is pending we are the system Open/Save
         * dialog, not the normal manager. Detect it FIRST so the window is sized + titled
         * for a dialog. (Checked before any open-document arg, like the file-picker design.) */
        picker = (sys_pick_req(&preq) == 1);

        struct sysinfo si; sysinfo(&si);
        int cw, ch;
        if (picker) {
            cw = 560; ch = 380; details_open = false;       /* dialog-shaped; no Details pane.
                                                             * Kept short so the Cancel/Save footer
                                                             * clears the dock when twm centres it. */
        } else {
            cw = (int)si.fb_w - 150; ch = (int)si.fb_h - 130;
            if (cw < 640) cw = 640; if (cw > 1000) cw = 1000;
            if (ch < 420) ch = 420; if (ch > 720) ch = 720;
        }
        if (cw > (int)si.fb_w - 20) cw = (int)si.fb_w - 20;
        if (ch > (int)si.fb_h - 60) ch = (int)si.fb_h - 60;
        const char *title = "Files";
        if (picker) title = preq.title[0] ? preq.title : (preq.mode == PICK_SAVE ? "Save As" : "Open");
        modal = picker;                                  /* picker = a modal dialog: scrim + input-lock (#11 step 5) */
        if (!create(cw, ch, title)) return false;

        reg_load();
        load_apps();

        bar.color = C_TOOLBAR; bar.sep_bottom = true;
        list.bg = C_LIST; list.sel_bg = C_SELROW;
        crumbbar.ctx = pathfld.ctx = this;
        crumbbar.on_nav  = [](void *c, const char *p) { ((FilesApp *)c)->nav_to(p); };
        crumbbar.on_edit = [](void *c) { ((FilesApp *)c)->enter_path_edit(); };
        pathfld.on_submit = [](void *c) { ((FilesApp *)c)->commit_path_edit(); };
        renamefld.ctx = this; renamefld.on_submit = [](void *c) { ((FilesApp *)c)->commit_rename(); };
        footer.color = C_TOOLBAR; nameLbl.text = "Name:"; nameLbl.fg = TH_MUTED;

        side.ctx = this; side.on_pick = [](void *c, const char *p) { ((FilesApp *)c)->nav_to(p); };
        side.add_item("Home",         HOMEDIR,              0);
        side.add_item("Desktop",      HOMEDIR "/Desktop",   0);
        side.add_item("Documents",    HOMEDIR "/Documents", 0);
        side.add_item("Downloads",    HOMEDIR "/Downloads", 0);
        side.add_item("Pictures",     HOMEDIR "/Pictures",  0);
        side.add_item("Applications", "/Apps",              1);
        side.add_item("System",       "/System",            0);
        side.add_item("Computer",     "/",                  0);
        side.add_item("Trash",        TRASHDIR,             1);   /* §9: set apart at the bottom */
        if (!picker) ensure_trash();                              /* so the Trash place is always navigable */
        side.cur = path;

        list.ctx = this; list.render_row = render_row;
        list.on_select   = [](void *c, int i) { ((FilesApp *)c)->select_row(i); };
        list.on_activate = [](void *c, int i) { ((FilesApp *)c)->enter(i); };

        grid.ctx = this; grid.bg = C_LIST; grid.sel_bg = C_SELROW;
        grid.render_tile = render_tile;
        grid.on_select   = [](void *c, int i) { ((FilesApp *)c)->select_row(i); };
        grid.on_activate = [](void *c, int i) { ((FilesApp *)c)->enter(i); };

        list2.ctx = this; list2.render_row = render_row2; list2.bg = C_LIST; list2.sel_bg = C_SELROW;
        list2.on_select   = [](void *c, int i) { ((FilesApp *)c)->select2(i); };
        list2.on_activate = [](void *c, int i) { ((FilesApp *)c)->enter2(i); };
        apply_zoom();

        back.ctx = fwd.ctx = up.ctx = newf.ctx = del.ctx = info.ctx = this;
        back.on_click = [](void *c) { ((FilesApp *)c)->go_back(); };
        fwd.on_click  = [](void *c) { ((FilesApp *)c)->go_fwd(); };
        up.on_click   = [](void *c) { ((FilesApp *)c)->go_up(); };
        newf.on_click = [](void *c) { ((FilesApp *)c)->make_folder(); };
        del.on_click  = [](void *c) { ((FilesApp *)c)->do_delete(); };
        info.on_click = [](void *c) { ((FilesApp *)c)->toggle_info(); };

        tabstrip.ctx = this;
        tabstrip.on_select = [](void *c, int i) { ((FilesApp *)c)->switch_tab(i); };
        tabstrip.on_close  = [](void *c, int i) { ((FilesApp *)c)->close_tab(i); };
        tabstrip.on_new    = [](void *c) { ((FilesApp *)c)->new_tab_here(); };

        menu.ctx = this;
        menu.on_pick = [](void *c, int tag) { ((FilesApp *)c)->menu_pick(tag); };

        /* picker footer widgets (#11) */
        okBtn.ctx = cancelBtn.ctx = overwrite.ctx = this;
        okBtn.on_click = [](void *c) {
            auto *a = (FilesApp *)c;
            if (a->preq.mode == PICK_SAVE) a->do_save();
            else if (a->list.sel >= 0) a->enter(a->list.sel);   /* Open: navigate a dir / pick a file */
        };
        cancelBtn.on_click = [](void *c) { ((FilesApp *)c)->cancel_pick(); };
        nameFld.ctx = this;
        nameFld.on_change = [](void *c) { ((FilesApp *)c)->update_pick_btn(); };
        nameFld.on_submit = [](void *c) { ((FilesApp *)c)->do_save(); };  /* Enter in the name field = Save */
        overwrite.on_choice = [](void *c, int idx) {
            auto *a = (FilesApp *)c;
            if (idx == 0) a->finish_pick(a->pending_save);                /* Replace   */
            else if (idx == 1) { char dup[256]; a->dedup_path(dup, sizeof dup, a->pending_save); a->finish_pick(dup); }  /* Keep Both */
            else a->invalidate();                                        /* Cancel: stay */
        };

        /* picker-only widgets show only in picker mode; del/Info/Details hide there */
        footer.visible = okBtn.visible = cancelBtn.visible = picker;
        nameLbl.visible = nameFld.visible = (picker && preq.mode == PICK_SAVE);
        del.visible = info.visible = !picker;
        details.visible = details_open && !picker;

        layout_widgets();
        add(&side); add(&bar); add(&back); add(&fwd); add(&up); add(&newf); add(&del); add(&info); add(&tabstrip);
        add(&crumbbar); add(&pathfld); add(&list); add(&grid); add(&list2); add(&splitdecor); add(&renamefld); add(&details); add(&status);
        add(&footer); add(&nameLbl); add(&nameFld); add(&cancelBtn); add(&okBtn);
        add(&menu); add(&overwrite);                  /* menu + overwrite last = on top + modal */
        focus = picker && preq.mode == PICK_SAVE ? (ui::Widget *)&nameFld : (ui::Widget *)&list;

        if (!picker) {                                /* app menus #6: File/Edit/Go bar (normal mode only) */
            menu_begin();
            int mf = menu_add("File"); menu_item(mf, "New Folder", 'N'); menu_item(mf, "New File", 0);
                                       menu_item(mf, "Refresh", 0); menu_item(mf, "Empty Trash", 0);
                                       menu_item(mf, "New Tab", 'T'); menu_item(mf, "Close Tab", 'W');  /* §4 */
            int me = menu_add("Edit"); menu_item(me, "Copy", 'C'); menu_item(me, "Cut", 'X');
                                       menu_item(me, "Paste", 'V'); menu_item(me, "Duplicate", 'D');
                                       menu_item(me, "Delete", 0); menu_item(me, "Rename", 0);
                                       menu_item(me, "Copy to Other Pane", 0, split ? 0 : WMI_DISABLED);   /* §4 (item 6) */
                                       menu_item(me, "Move to Other Pane", 0, split ? 0 : WMI_DISABLED);   /* §4 (item 7) */
            int mg = menu_add("Go");   menu_item(mg, "Up", 0); menu_item(mg, "Back", 0); menu_item(mg, "Forward", 0);
            int mv = menu_add("View");                          /* view mode + zoom (§1) */
            menu_item(mv, "as Icons", 0, view_mode == 1 ? WMI_CHECKED : 0);
            menu_item(mv, "as List",  0, view_mode == 0 ? WMI_CHECKED : 0);
            menu_item(mv, "Zoom In",     0);
            menu_item(mv, "Zoom Out",    0);
            menu_item(mv, "Actual Size", 0);
            menu_item(mv, "Split View",  0, split ? WMI_CHECKED : 0);   /* §4 (item 5) */
            int ms = menu_add("Sort");                          /* Sort by … (§2) */
            menu_item(ms, "Sort by Name", 0, sort_key == FSORT_NAME ? WMI_CHECKED : 0);
            menu_item(ms, "Sort by Kind", 0, sort_key == FSORT_KIND ? WMI_CHECKED : 0);
            menu_item(ms, "Sort by Size", 0, sort_key == FSORT_SIZE ? WMI_CHECKED : 0);
            menu_item(ms, "Reversed",      0, sort_desc ? WMI_CHECKED : 0);
            menu_item(ms, "Folders First", 0, sort_ff   ? WMI_CHECKED : 0);
            menu_commit();
        }

        /* start folder: the request's dir (if a real folder) in picker mode, else Home */
        struct fstat st;
        const char *start = HOMEDIR;
        if (picker && preq.dir[0] && sys_exists(preq.dir, &st) && st.type == FT_DIR) start = preq.dir;
        else if (!sys_exists(HOMEDIR, &st)) start = "/";
        nav_to(start);

        grow_tabs(); ntabs = 1; curtab = 0; tab_save(0);   /* tab 0 mirrors the initial folder (§4) */
        sync_tabs();

        if (picker) {
            if (preq.mode == PICK_SAVE) {             /* pre-fill + select the suggested name */
                nameFld.set_text(preq.name[0] ? preq.name : "untitled.txt");
                nameFld.caret = nameFld.length();
                nameFld.on_key(0x01);                 /* ^A select-all: the first keystroke replaces it */
            }
            update_pick_btn();
            print("[files] picker "); print(preq.mode == PICK_SAVE ? "save " : "open "); print(path); print("\r\n");
        }
        return true;
    }

    void menu_pick(int tag) {
        if (tag == -1) { invalidate(); return; }              /* dismissed */
        if (menu_mode == 1) {                                 /* Open With */
            if (tag >= 0 && tag < napps) {
                if (menu.toggle && ow_ext[0]) {               /* remember the default for this extension */
                    char key[40]; snprintf(key, sizeof key, "open.default.%s", ow_ext);
                    reg_set(key, apps[tag].exec); reg_save();
                }
                sys_open_with(apps[tag].exec, ow_path);
            }
            invalidate(); return;
        }
        switch (tag) {                                        /* context actions */
        case 1: if (list.sel >= 0) enter(list.sel); break;    /* Open */
        case 2: if (list.sel >= 0) { int hu = has_up(); if (!(hu && list.sel == 0)) open_with_chooser(ents[list.sel - hu].name); } break;
        case 3: do_delete(); break;
        case 4: make_folder(); break;
        case 5: load_dir(); break;
        case 6: copy_sel(false); break;                   /* Copy a file's bytes to the clipboard */
        case 7: copy_sel(true);  break;                   /* Cut: copy + mark the source for a move */
        case 8: paste();         break;                   /* Paste the clipboard file here */
        case 9: start_rename();  break;                   /* in-place rename */
        case 10: restore_from_trash(); break;             /* Put Back a trashed item (§9) */
        case 11: empty_trash();        break;             /* Empty Trash (§9) */
        case 12: duplicate_sel();      break;             /* Duplicate (§12) */
        case 13: make_file();          break;             /* New File (§12)  */
        case 14: open_sel_new_tab();   break;             /* Open in New Tab (§4) */
        case 15: copy_to_other(false); break;             /* Copy to Other Pane (§4) */
        }
        invalidate();
    }

    /* right-click: select the row under the cursor (if any), then open a menu */
    void on_context(int x, int y) override {
        if (x >= list.r.x && x < list.r.x + list.r.w && y >= list.r.y && y < list.r.y + list.r.h) {
            int row = (view_mode == 1) ? grid.index_at(x, y)              /* hit-test the active view */
                                       : list.top + (y - list.r.y) / list.row_h;
            if (row >= 0 && row < list.count) select_row(row);           /* sets list.sel + grid.sel */
        }
        menu_mode = 0; menu.reset();
        int hu = has_up();
        int real = (list.sel >= 0 && !(hu && list.sel == 0)) ? list.sel - hu : -1;
        if (picker) {                                          /* picker mode: no Delete / Open With */
            menu.add("New Folder", PK_ACTION, 4, 0, 0, 0);
            menu.add("Refresh",    PK_ACTION, 5, 0, 0, 0);
            menu.show(x, y); invalidate(); return;
        }
        bool trash = in_trash();
        if (real >= 0) {
            struct dirent *e = &ents[real];
            if (trash) {                                       /* Trash items: restore or delete for good */
                menu.add("Put Back", PK_ACTION, 10, 0, 0, 0);
                menu.add("Delete Immediately", PK_ACTION, 3, 0, 0, 0);
            } else {
                if (e->type == FT_DIR || is_app_dir(e->type, e->name)) {
                    menu.add("Open", PK_ACTION, 1, 0, 0, 0);
                    if (e->type == FT_DIR && !is_app_dir(e->type, e->name))
                        menu.add("Open in New Tab", PK_ACTION, 14, 0, 0, 0);     /* §4 */
                }
                else { menu.add("Open", PK_ACTION, 1, 0, 0, 0); menu.add("Open With...", PK_ACTION, 2, 0, 0, 0);
                       menu.add("Copy", PK_ACTION, 6, 0, 0, 0); menu.add("Cut", PK_ACTION, 7, 0, 0, 0); }
                menu.add("Duplicate", PK_ACTION, 12, 0, 0, 0);        /* §12 */
                menu.add("Rename", PK_ACTION, 9, 0, 0, 0);
                menu.add("Delete", PK_ACTION, 3, 0, 0, 0);
                if (split) menu.add("Copy to Other Pane", PK_ACTION, 15, 0, 0, 0);   /* §4 */
            }
            menu.add("", PK_SEP, 0, 0, 0, 0);
        }
        if (clip_count() > 0 && !trash) { struct clipinfo ci;   /* offer Paste when a file is on the clipboard */
            if (clip_info(clip_active(-1), &ci) == 0 && ci.type == CLIP_FILE)
                menu.add("Paste", PK_ACTION, 8, 0, 0, 0); }
        if (trash) menu.add("Empty Trash", PK_ACTION, 11, 0, 0, 0);
        else { menu.add("New Folder", PK_ACTION,  4, 0, 0, 0);
               menu.add("New File",   PK_ACTION, 13, 0, 0, 0); }   /* §12 */
        menu.add("Refresh",    PK_ACTION, 5, 0, 0, 0);
        menu.show(x, y);
        invalidate();
    }
    void on_resize(int, int) override { layout_widgets(); if (menu.open && menu.win) menu.r = { 0, 0, w, h }; }
    /* Click-away dismisses an inline editor, the same way Esc/Enter do (#11): a click
     * outside the path bar reverts it (matching its Esc), and one outside an in-place
     * rename commits it (Finder behaviour). The click then routes on as usual, so e.g.
     * clicking a row both ends the edit and selects that row. */
    void dispatch_mouse(int x, int y, int btn) override {
        if (editing_path && !pathfld.r.has(x, y))      leave_path_edit();
        else if (renaming && !renamefld.r.has(x, y))   commit_rename();
        ui::Window::dispatch_mouse(x, y, btn);
    }
    /* the compositor close button: in picker mode, closing == Cancel (empty result). */
    bool on_close() override { if (picker) print("[files] pick cancel\r\n"); return true; }
    /* §8: toggle the Get Info / Details inspector (the toolbar Info button + Ctrl+I).
     * No-op in picker mode, where the inspector is hidden. */
    void toggle_info() {
        if (picker) return;
        details_open = !details_open; details.visible = details_open;
        print("[files] info "); printu(details_open ? 1u : 0u); print("\r\n");
        layout_widgets(); invalidate();
    }
    void on_key(int key) override {
        if (menu.open) { if (key == ui::UK_ESC) { menu.dismiss(); invalidate(); } return; }
        if (renaming) { if (key == ui::UK_ESC) cancel_rename(); return; }         /* Esc cancels the rename */
        if (editing_path) { if (key == ui::UK_ESC) leave_path_edit(); return; }  /* Esc reverts the path edit */
        if (key == 0x0c) { enter_path_edit(); return; }              /* Ctrl+L: edit the path literally */
        if (picker && key == ui::UK_ESC) { cancel_pick(); return; }   /* Esc cancels the picker */
        if (key == ui::UK_BACK) go_up();
        else if (key == 0x03) copy_sel(false);   /* Ctrl+C */
        else if (key == 0x18) copy_sel(true);    /* Ctrl+X */
        else if (key == 0x16) paste();           /* Ctrl+V */
        else if (key == 0x09) toggle_info();     /* Ctrl+I (== Tab): show/hide Get Info (§8) */
        else if (key == 'r') load_dir();
    }
    void on_nav(int dir) override { if (dir == 0) go_back(); else go_fwd(); }
    /* Menu bar (app menus #6): File / Edit / Go. The Edit accelerators ^C/^X/^V and
     * File's ^N are intercepted by the compositor for the focused Files window and
     * arrive here as WEV_MENU picks (the same actions the toolbar + right-click run). */
    void on_menu(int menu, int item) override {
        print("[files] menu "); printu((unsigned)menu); printc(' '); printu((unsigned)item); print("\r\n");
        if (menu == 0) { if (item == 0) make_folder(); else if (item == 1) make_file();
                         else if (item == 2) load_dir(); else if (item == 3) empty_trash();
                         else if (item == 4) new_tab_here(); else if (item == 5) close_cur_tab(); } /* File: New Folder / New File / Refresh / Empty Trash / New Tab / Close Tab */
        else if (menu == 1) {                                                                 /* Edit */
            if (item == 0) copy_sel(false); else if (item == 1) copy_sel(true);
            else if (item == 2) paste(); else if (item == 3) duplicate_sel();
            else if (item == 4) do_delete(); else if (item == 5) start_rename();
            else if (item == 6) copy_to_other(false); else if (item == 7) copy_to_other(true);  /* §4 */
        } else if (menu == 2) {                                                               /* Go */
            if (item == 0) go_up(); else if (item == 1) go_back(); else if (item == 2) go_fwd();
        } else if (menu == 3) {                                                               /* View: mode + zoom */
            if      (item == 0) set_view(1);                       /* as Icons   */
            else if (item == 1) set_view(0);                       /* as List    */
            else if (item == 2) set_zoom(zoom + 1);                /* Zoom In     */
            else if (item == 3) set_zoom(zoom - 1);                /* Zoom Out    */
            else if (item == 4) set_zoom(1);                       /* Actual Size */
            else if (item == 5) set_split(!split);                 /* Split View (§4) */
        } else if (menu == 4) {                                                               /* Sort by … */
            if      (item == 0) set_sort(FSORT_NAME, sort_desc, sort_ff);
            else if (item == 1) set_sort(FSORT_KIND, sort_desc, sort_ff);
            else if (item == 2) set_sort(FSORT_SIZE, sort_desc, sort_ff);
            else if (item == 3) set_sort(sort_key, !sort_desc, sort_ff);
            else if (item == 4) set_sort(sort_key, sort_desc, !sort_ff);
        }
        invalidate();
    }
};

int app_main() {
    FilesApp *app = new FilesApp();
    if (!app->build()) { print("[files] needs the desktop\r\n"); proc_exit(); }
    print("[files] file manager up\r\n");
    return app->run();
}
