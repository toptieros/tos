/* tOS system registry implementation -- see registry.h + design/settings.md. */
#include "ulib.h"
#include "registry.h"

struct rentry { char key[REG_KEYMAX]; char val[REG_VALMAX]; uint8_t user; };
static struct rentry ents[REG_MAX];
static int nent;

static void rcopy(char *d, const char *s, int max) {
    int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0;
}
static struct rentry *find(const char *key) {
    for (int i = 0; i < nent; i++) if (streq(ents[i].key, key)) return &ents[i];
    return 0;
}
/* insert or update a key; mark it as a user override when `user`. */
static void put(const char *key, const char *val, int user) {
    struct rentry *e = find(key);
    if (!e) {
        if (nent >= REG_MAX) return;
        e = &ents[nent++];
        rcopy(e->key, key, REG_KEYMAX);
        e->user = 0;
    }
    rcopy(e->val, val, REG_VALMAX);
    if (user) e->user = 1;
}

/* parse a "key = value" file (blank lines and '#' comments skipped) into entries */
static void load_file(const char *path, int user) {
    int fd = fopen(path, O_RDONLY);
    if (fd < 0) return;
    char buf[4096];
    int n = fread_(fd, buf, sizeof buf - 1);
    fclose_(fd);
    if (n <= 0) return;
    buf[n] = 0;
    int i = 0;
    while (i < n) {
        while (buf[i] == ' ' || buf[i] == '\t') i++;
        if (buf[i] == '#' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0) {
            while (buf[i] && buf[i] != '\n') i++;
            if (buf[i] == '\n') i++;
            continue;
        }
        char key[REG_KEYMAX]; int k = 0;
        while (buf[i] && buf[i] != '=' && buf[i] != '\n' && buf[i] != ' ' && buf[i] != '\t' && k < REG_KEYMAX - 1)
            key[k++] = buf[i++];
        key[k] = 0;
        while (buf[i] == ' ' || buf[i] == '\t') i++;
        if (buf[i] != '=') { while (buf[i] && buf[i] != '\n') i++; if (buf[i] == '\n') i++; continue; }
        i++;
        while (buf[i] == ' ' || buf[i] == '\t') i++;
        char val[REG_VALMAX]; int v = 0;
        while (buf[i] && buf[i] != '\n' && buf[i] != '\r' && v < REG_VALMAX - 1) val[v++] = buf[i++];
        while (v > 0 && (val[v - 1] == ' ' || val[v - 1] == '\t')) v--;
        val[v] = 0;
        if (key[0]) put(key, val, user);
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
    }
}

void reg_load(void) {
    nent = 0;
    load_file(REG_SYS_PATH, 0);     /* defaults first ... */
    load_file(REG_USER_PATH, 1);    /* ... then user overrides win */
}

const char *reg_get(const char *key, const char *fallback) {
    struct rentry *e = find(key);
    return e ? e->val : fallback;
}

int reg_int(const char *key, int fallback) {
    const char *s = reg_get(key, 0);
    if (!s) return fallback;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    if (*s < '0' || *s > '9') return fallback;
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sign;
}

int reg_bool(const char *key, int fallback) {
    const char *s = reg_get(key, 0);
    if (!s) return fallback;
    return streq(s, "true") || streq(s, "1") || streq(s, "yes") || streq(s, "on");
}

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
uint32_t reg_color(const char *key, uint32_t fallback) {
    const char *s = reg_get(key, 0);
    if (!s || *s != '#') return fallback;
    s++;
    int c[6];
    for (int i = 0; i < 6; i++) { c[i] = hex(s[i]); if (c[i] < 0) return fallback; }
    uint32_t r = c[0] * 16 + c[1], g = c[2] * 16 + c[3], b = c[4] * 16 + c[5];
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void reg_set(const char *key, const char *val) { put(key, val, 1); }

void reg_set_int(const char *key, int val) {
    char b[16]; snprintf(b, sizeof b, "%d", val);
    put(key, b, 1);
}

int reg_save(void) {
    int fd = fopen(REG_USER_PATH, O_CREATE | O_TRUNC);
    if (fd < 0) return -1;
    char line[REG_KEYMAX + REG_VALMAX + 8];
    for (int i = 0; i < nent; i++) {
        if (!ents[i].user) continue;
        int n = snprintf(line, sizeof line, "%s = %s\n", ents[i].key, ents[i].val);
        if (n > 0) fwrite_(fd, line, n);
    }
    fclose_(fd);
    return 0;
}

int reg_keys(const char *prefix, char out[][REG_KEYMAX], int max) {
    int n = 0, pl = 0;
    while (prefix[pl]) pl++;
    for (int i = 0; i < nent && n < max; i++) {
        int j = 0;
        while (j < pl && ents[i].key[j] == prefix[j]) j++;
        if (j == pl) rcopy(out[n++], ents[i].key, REG_KEYMAX);
    }
    return n;
}
