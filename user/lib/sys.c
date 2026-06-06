/* tOS SDK conveniences (see sys.h). */
#include "ulib.h"
#include "sys.h"

char *sys_slurp(const char *path, int *len_out) {
    if (len_out) *len_out = 0;
    struct fstat st;
    if (stat_(path, &st) != 0 || st.type != FT_FILE) return 0;
    int fd = fopen(path, O_RDONLY);
    if (fd < 0) return 0;
    int cap = (int)st.size;
    char *buf = (char *)malloc((size_t)cap + 1);
    if (!buf) { fclose_(fd); return 0; }
    int n = 0;
    while (n < cap) {
        int r = fread_(fd, buf + n, cap - n);
        if (r <= 0) break;
        n += r;
    }
    fclose_(fd);
    buf[n] = 0;
    if (len_out) *len_out = n;
    return buf;
}

int sys_spit(const char *path, const void *buf, int len) {
    int fd = fopen(path, O_CREATE | O_TRUNC);
    if (fd < 0) return -1;
    int n = fwrite_(fd, buf, len);
    fclose_(fd);
    return n;
}

int sys_launch(const char *prog) {
    /* Copy the program path onto the STACK before forking. fork() copies the
     * task's code/stack/data window but NOT the anonymous mmap heap, so a `prog`
     * that lives on the heap (e.g. a C++ app's std::string-style buffer) would be
     * unmapped in the child -- exec'ing it would then fail (and, before the kernel
     * validated user pointers, fault the kernel). A stack copy is always carried
     * across the fork. */
    char buf[256];
    int i = 0; for (; prog[i] && i < (int)sizeof(buf) - 1; i++) buf[i] = prog[i];
    buf[i] = 0;
    int pid = fork();
    if (pid == 0) { exec(buf); proc_exit(); }   /* child: never returns on success */
    return pid;
}

int sys_exists(const char *path, struct fstat *st) {
    struct fstat tmp;
    if (!st) st = &tmp;
    return stat_(path, st) == 0;
}

#define OPEN_DOC_PATH "/tmp/.open-doc"

int sys_open_with(const char *prog, const char *path) {
    int n = 0; while (path[n]) n++;
    sys_spit(OPEN_DOC_PATH, path, n);             /* hand the document to the next app */
    return sys_launch(prog);
}

int sys_open_arg(char *out, int cap) {
    int n = 0; char *d = sys_slurp(OPEN_DOC_PATH, &n);
    if (!d) { if (cap > 0) out[0] = 0; return 0; }
    funlink(OPEN_DOC_PATH);                        /* consume it so a later launch won't reuse it */
    int i = 0; for (; i < n && i < cap - 1 && d[i] && d[i] != '\n' && d[i] != '\r'; i++) out[i] = d[i];
    out[i] = 0;
    free(d);
    return out[0] ? 1 : 0;
}

/* ---- the system file picker (design/file-picker.md) ------------------------- */
#define FILES_BIN  "/Apps/Files.app/bin/files"

/* The request/result temp files are namespaced by the *caller's* pid so two apps can
 * have a picker open at once without clobbering each other (#11 step 6). The caller
 * derives the pid from getpid(); the picker -- a fork+exec child of the caller -- derives
 * the same number from getppid(). Builds "/tmp/.picker-<pid>.req" / ".res". */
static void picker_path(char *out, int cap, int pid, const char *suffix) {
    char num[16]; int n = 0;
    if (pid < 0) pid = 0;
    if (pid == 0) num[n++] = '0';
    else { char t[16]; int m = 0; while (pid && m < 15) { t[m++] = (char)('0' + pid % 10); pid /= 10; } while (m) num[n++] = t[--m]; }
    num[n] = 0;
    int o = 0; const char *pfx = "/tmp/.picker-";
    for (int i = 0; pfx[i] && o < cap - 1; i++) out[o++] = pfx[i];
    for (int i = 0; num[i]  && o < cap - 1; i++) out[o++] = num[i];
    out[o < cap - 1 ? o++ : o] = '.';
    for (int i = 0; suffix[i] && o < cap - 1; i++) out[o++] = suffix[i];
    out[o] = 0;
}

int sys_pick_begin(const struct pick_req *r) {
    int me = getpid();
    char req[64], res[64];
    picker_path(req, sizeof req, me, "req");
    picker_path(res, sizeof res, me, "res");
    char blob[512];
    int n = pickreq_encode(r, blob, sizeof blob);
    if (sys_spit(req, blob, n) < 0) return -1;
    funlink(res);                    /* so a previous run's result can't be read as this one's */
    return sys_launch(FILES_BIN);    /* fork+exec Files; it sees the request via sys_pick_req() */
}

int sys_pick_poll(int pid, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    if (trywait() != pid) return 0;  /* the picker (child `pid`) hasn't exited yet */
    char res[64]; picker_path(res, sizeof res, getpid(), "res");
    int n = 0; char *r = sys_slurp(res, &n);
    funlink(res);                    /* consume the result either way */
    if (!r) return -1;               /* no result file: Cancel / close / crash */
    int i = 0; for (; i < n && i < cap - 1 && r[i] && r[i] != '\n' && r[i] != '\r'; i++) out[i] = r[i];
    out[i] = 0;
    free(r);
    return out[0] ? 1 : -1;          /* empty result also means cancelled */
}

int sys_pick_req(struct pick_req *out) {
    char req[64]; picker_path(req, sizeof req, getppid(), "req");
    int n = 0; char *b = sys_slurp(req, &n);
    if (!b) return 0;
    funlink(req);                    /* consume it so a later launch won't re-enter picker mode */
    int ok = pickreq_parse(b, out);
    free(b);
    return ok;
}

int sys_pick_result(const char *path) {
    char res[64]; picker_path(res, sizeof res, getppid(), "res");
    return sys_spit(res, path, (int)strlen(path));
}
