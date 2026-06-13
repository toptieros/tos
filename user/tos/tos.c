/* tos -- the tOS package manager front-end (design/packaging.md). Phase 1: install /
 * uninstall / list / info for local `.app` bundles. One binary, subcommand-dispatched
 * (`tos app install <bundle>`); the shared copy/remove engine lives in copytree.h so a
 * future repository client and the OS installer reuse it. Packages (`.tpkg`) + the
 * receipts DB are Phase 2. Now that argv passing landed (shell band 2), this is a real
 * /System/bin program rather than a shell built-in. */
#include "ulib.h"
#include "manifest.h"      /* manifest_get: the shared key=value reader */
#include "copytree.h"      /* copytree / rmtree / ct_join / ct_basename */

static int streql(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Does `s` end in ".app"? (a bundle directory) */
static int ends_app(const char *s) {
    int n = 0; while (s[n]) n++;
    return n >= 4 && s[n-4] == '.' && s[n-3] == 'a' && s[n-2] == 'p' && s[n-1] == 'p';
}

/* Read a whole file into buf[cap] (NUL-terminated). -> bytes, or -1. */
static int slurp(const char *path, char *buf, int cap) {
    int fd = fopen(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = fread_(fd, buf, cap - 1);
    fclose_(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

/* Resolve an app reference (a bare "Hello", "Hello.app", or an absolute path) to its
 * installed bundle directory under /Apps. */
static void bundle_path(char *out, int cap, const char *name) {
    int n = 0;
    if (name[0] == '/') { for (; name[n] && n < cap - 1; n++) out[n] = name[n]; out[n] = 0; return; }
    for (const char *p = "/Apps/"; *p && n < cap - 1; p++) out[n++] = *p;
    for (int i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = 0;
    if (!ends_app(out)) { for (const char *e = ".app"; *e && n < cap - 1; e++) out[n++] = *e; out[n] = 0; }
}

static void usage(void) {
    print("usage: tos <command>\r\n");
    print("  tos app install <bundle.app>   install a local .app bundle into /Apps\r\n");
    print("  tos app uninstall <name>       remove an installed (user) app\r\n");
    print("  tos app list                   list installed apps (name, version, owner)\r\n");
    print("  tos app info <name>            show an app's manifest\r\n");
    print("  tos package ...                packages (.tpkg) -- Phase 2, not yet built\r\n");
}

/* tos app install <src.app> */
static int app_install(const char *src) {
    if (!src || !src[0]) { print("tos: install needs a bundle path\r\n"); return 1; }
    char mf[300]; ct_join(mf, sizeof mf, src, "manifest");
    char buf[1024];
    if (slurp(mf, buf, sizeof buf) < 0) { print("tos: not an app bundle (no manifest): "); print(src); print("\r\n"); return 1; }
    char name[64];
    if (!manifest_get(buf, "name", name, sizeof name)) { print("tos: bundle manifest has no 'name'\r\n"); return 1; }

    char base[96]; ct_basename(base, sizeof base, src);
    char dst[160]; ct_join(dst, sizeof dst, "/Apps", base);
    struct fstat st;
    if (stat_(dst, &st) == 0) { print("tos: already installed: "); print(name); print(" ("); print(dst); print(")\r\n"); return 1; }

    if (copytree(src, dst) < 0) {
        print("tos: install failed (cannot write /Apps -- permission?). "); print(name); print("\r\n");
        rmtree(dst);                                  /* clean up a partial copy */
        return 1;
    }
    apps_refresh(1);                                  /* nudge twm to rescan the dock */
    print("installed "); print(name); print(" -> "); print(dst); print("\r\n");
    return 0;
}

/* tos app uninstall <name> */
static int app_uninstall(const char *name) {
    if (!name || !name[0]) { print("tos: uninstall needs an app name\r\n"); return 1; }
    char dst[160]; bundle_path(dst, sizeof dst, name);
    struct fstat st;
    if (stat_(dst, &st) < 0) { print("tos: not installed: "); print(name); print("\r\n"); return 1; }
    if (st.owner == 0) { print("tos: cannot uninstall a system app: "); print(name); print(" (shipped with the OS)\r\n"); return 1; }
    if (rmtree(dst) < 0) { print("tos: uninstall failed: "); print(name); print("\r\n"); return 1; }
    apps_refresh(1);
    print("uninstalled "); print(name); print("\r\n");
    return 0;
}

/* tos app list */
static int app_list(void) {
    struct dirent ents[64];
    int n = readdir("/Apps", ents, 64);
    if (n <= 0) { print("no apps installed\r\n"); return 0; }
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].type != FT_DIR || !ends_app(ents[i].name)) continue;
        char bp[160]; ct_join(bp, sizeof bp, "/Apps", ents[i].name);
        char mf[200]; ct_join(mf, sizeof mf, bp, "manifest");
        char buf[1024];
        if (slurp(mf, buf, sizeof buf) < 0) continue;
        char nm[64], ver[32];
        if (!manifest_get(buf, "name", nm, sizeof nm)) continue;
        if (!manifest_get(buf, "version", ver, sizeof ver)) { ver[0] = '?'; ver[1] = 0; }
        print(nm); print("\t"); print(ver);
        print(ents[i].owner == 0 ? "\t[system]\r\n" : "\t[user]\r\n");
        shown++;
    }
    if (!shown) print("no apps installed\r\n");
    return 0;
}

/* tos app info <name> */
static int app_info(const char *name) {
    if (!name || !name[0]) { print("tos: info needs an app name\r\n"); return 1; }
    char bp[160]; bundle_path(bp, sizeof bp, name);
    char mf[200]; ct_join(mf, sizeof mf, bp, "manifest");
    char buf[1024];
    if (slurp(mf, buf, sizeof buf) < 0) { print("tos: not installed: "); print(name); print("\r\n"); return 1; }
    struct fstat st;
    print("bundle: "); print(bp);
    if (stat_(bp, &st) == 0) print(st.owner == 0 ? "  [system]\r\n" : "  [user]\r\n"); else print("\r\n");
    print(buf);
    print("\r\n");
    return 0;
}

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) {
    char *av[16];
    int ac = getargs(av, 16);
    int rc = 0;
    if (ac < 2) { usage(); rc = 1; }
    else if (streql(av[1], "help") || streql(av[1], "--help") || streql(av[1], "-h")) { usage(); }
    else if (streql(av[1], "app")) {
        const char *sub = ac >= 3 ? av[2] : "";
        const char *arg = ac >= 4 ? av[3] : "";
        if      (streql(sub, "install"))   rc = app_install(arg);
        else if (streql(sub, "uninstall") || streql(sub, "remove")) rc = app_uninstall(arg);
        else if (streql(sub, "list"))      rc = app_list();
        else if (streql(sub, "info"))      rc = app_info(arg);
        else { print("tos app: unknown subcommand. Try: install uninstall list info\r\n"); rc = 1; }
    }
    else if (streql(av[1], "package") || streql(av[1], "pkg")) {
        print("tos: package (.tpkg) management is Phase 2 -- not yet built (design/packaging.md)\r\n");
        rc = 1;
    }
    else { print("tos: unknown command: "); print(av[1]); print("\r\n"); usage(); rc = 1; }

    (void)rc;
    proc_exit();
    for (;;) { }
}
