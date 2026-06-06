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
#define PICKER_REQ "/tmp/.picker-req"
#define PICKER_RES "/tmp/.picker-res"
#define FILES_BIN  "/Apps/Files.app/bin/files"

int sys_pick_begin(const struct pick_req *r) {
    char blob[512];
    int n = pickreq_encode(r, blob, sizeof blob);
    if (sys_spit(PICKER_REQ, blob, n) < 0) return -1;
    funlink(PICKER_RES);             /* so a previous run's result can't be read as this one's */
    return sys_launch(FILES_BIN);    /* fork+exec Files; it sees the request via sys_pick_req() */
}

int sys_pick_poll(int pid, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    if (trywait() != pid) return 0;  /* the picker (child `pid`) hasn't exited yet */
    int n = 0; char *res = sys_slurp(PICKER_RES, &n);
    funlink(PICKER_RES);             /* consume the result either way */
    if (!res) return -1;             /* no result file: Cancel / close / crash */
    int i = 0; for (; i < n && i < cap - 1 && res[i] && res[i] != '\n' && res[i] != '\r'; i++) out[i] = res[i];
    out[i] = 0;
    free(res);
    return out[0] ? 1 : -1;          /* empty result also means cancelled */
}

int sys_pick_req(struct pick_req *out) {
    int n = 0; char *b = sys_slurp(PICKER_REQ, &n);
    if (!b) return 0;
    funlink(PICKER_REQ);             /* consume it so a later launch won't re-enter picker mode */
    int ok = pickreq_parse(b, out);
    free(b);
    return ok;
}
