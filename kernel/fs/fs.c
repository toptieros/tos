/* tosfs: a small hierarchical filesystem on the IDE disk.
 *
 * The on-disk directory (tosfs.h) is a flat slot table; each entry names its
 * parent directory by slot index, which encodes the tree without nesting the
 * layout. Files are still stored contiguously: a new file is placed at the first
 * free sector (first-fit over an in-memory free-sector bitmap), write() streams
 * it out claiming the next contiguous sector as it grows, and close() records it
 * in the table and flushes the superblock back to disk. Directories hold no data
 * -- they exist only as table entries that other entries point at.
 *
 * Paths are resolved component by component against a per-task current working
 * directory (fs_chdir/fs_getcwd). Entries are never relocated, so a parent index
 * is stable for the life of the FS; delete just flips an entry's type to FREE
 * and (for files) frees its sectors back to the bitmap. The bitmap lives only in
 * RAM and is rebuilt from the table at mount, so nothing extra goes on disk. */
#include "fs.h"
#include "ata.h"
#include "console.h"
#include "sched.h"        /* MAX_TASKS, sched_current()/sched_uid() -> fd tables, cwd, identity */
#include "syscall.h"      /* O_CREATE / O_TRUNC / O_RDONLY, struct dirent / fstat */
#include "spinlock.h"
#include "perm.h"         /* tos_may_write() ownership check */
#include "rtc.h"          /* rtc_now(): CMOS wall-clock for file mtimes (§8) */
#include "fstime.h"       /* fstime_pack(): the on-disk packed mtime format */

#define NOFILE 16

/* the current wall-clock packed for an entry's mtime (fstime.h); 0 if the RTC is
 * unreadable so the field stays "unknown" rather than a bogus date */
static uint32_t fs_now(void) {
    struct rtctime t;
    rtc_now(&t);
    if (t.year < 1970) return 0;
    return fstime_pack(t.year, t.month, t.day, t.hour, t.min);
}

/* Guards the shared FS metadata (superblock, free-sector map, open-file tables,
 * per-task cwd) so file I/O from different CPUs doesn't corrupt it. */
static spinlock_t fs_lock = SPINLOCK_INIT;

struct ofile {
    int      used;
    int      writing;
    int      slot;              /* committed entry slot (readers); -1 for a writer */
    int      parent;            /* writer: dir slot the new file will live in       */
    uint32_t base_lba;          /* first data sector                                */
    uint32_t nsect;             /* sectors currently reserved for a writer          */
    uint32_t size;              /* file size in bytes                               */
    uint32_t pos;               /* current byte offset                              */
    char     name[TOSFS_NAME_MAX];
};

static struct tosfs_super super;
static int      mounted = 0;
static uint32_t base_lba = 0;    /* first sector of our partition on the disk */
/* One open-file table per task: an fd is an index into the *caller's* table, so
 * fds are private to a task. The single shared write buffer (wbuf) still means
 * only one writer system-wide at a time. cwd[] is each task's working directory
 * (a slot index, or TOSFS_ROOT). */
static struct ofile oft[MAX_TASKS][NOFILE];
static int32_t  cwd[MAX_TASKS];
static uint8_t  rbuf[512];       /* scratch for reads  */
static uint8_t  wbuf[512];       /* the single in-progress write sector */

/* In-memory free-sector map: one bit per disk sector, 1 = in use. Rebuilt from
 * the table at mount, kept in sync by alloc/free below. */
static uint8_t  used_map[(TOSFS_DISK_SECTORS + 7) / 8];

static int  bit_get(uint32_t s) { return used_map[s >> 3] & (1u << (s & 7)); }
static void bit_set(uint32_t s) { used_map[s >> 3] |=  (1u << (s & 7)); }
static void bit_clr(uint32_t s) { used_map[s >> 3] &= ~(1u << (s & 7)); }

static int names_equal(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void copy_name(char *dst, const char *src) {
    int j = 0;
    for (; src[j] && j < TOSFS_NAME_MAX - 1; j++) dst[j] = src[j];
    for (; j < TOSFS_NAME_MAX; j++) dst[j] = 0;
}

static uint32_t sectors_for(uint32_t bytes) { return (bytes + 511) / 512; }

/* All FS sector numbers are relative to our partition; these add the base. The
 * count is chunked into <=128-sector ATA transfers (the ATA count register is only
 * 8-bit) so multi-sector I/O like the directory table -- now >256 sectors on a
 * larger disk -- isn't capped to a single command. Lets TOSFS_DISK_SECTORS scale. */
static int fs_sread(uint32_t lba, uint32_t n, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    while (n) { uint8_t c = n > 128 ? 128 : (uint8_t)n;
        if (ata_read(base_lba + lba, c, p) < 0) return -1;
        lba += c; p += (uint32_t)c * 512; n -= c; }
    return 0;
}
static int fs_swrite(uint32_t lba, uint32_t n, const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n) { uint8_t c = n > 128 ? 128 : (uint8_t)n;
        if (ata_write(base_lba + lba, c, p) < 0) return -1;
        lba += c; p += (uint32_t)c * 512; n -= c; }
    return 0;
}

uint32_t fs_base_lba(void) { return base_lba; }

/* Scan the MBR partition table (sector 0) for our tosfs partition and return its
 * first LBA, or 0 if there is no partition table / no such partition (in which
 * case the FS is assumed to sit at LBA 0, as on a bare, unpartitioned image). */
static uint32_t find_partition(void) {
    uint8_t mbr[512];
    if (ata_read(0, 1, mbr) < 0) return 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;     /* no MBR signature */
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = &mbr[446 + i * 16];              /* partition entry */
        if (e[4] != TOSFS_PART_TYPE) continue;
        return (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
               ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);  /* LBA start (LE) */
    }
    return 0;
}

/* Mark every sector each file occupies as in use (sectors 0..D-1 are the table). */
static void rebuild_used_map(void) {
    for (uint32_t i = 0; i < sizeof used_map; i++) used_map[i] = 0;
    for (uint32_t s = 0; s < TOSFS_DIR_SECTORS; s++) bit_set(s);
    for (uint32_t i = 0; i < TOSFS_MAX_FILES; i++) {
        if (super.ents[i].type != TOSFS_FILE) continue;
        uint32_t s = super.ents[i].start_lba;
        uint32_t n = sectors_for(super.ents[i].size);
        for (uint32_t k = 0; k < n; k++) bit_set(s + k);
    }
}

/* First-fit: the lowest free data sector, or -1 if the disk is full. */
static int first_free(void) {
    for (uint32_t s = TOSFS_DIR_SECTORS; s < TOSFS_DISK_SECTORS; s++)
        if (!bit_get(s)) return (int)s;
    return -1;
}

/* Lowest free slot in the entry table, or -1 if the directory is full. */
static int alloc_slot(void) {
    for (uint32_t i = 0; i < TOSFS_MAX_FILES; i++)
        if (super.ents[i].type == TOSFS_FREE) return (int)i;
    return -1;
}

/* Persist just the directory sector(s) holding entry `slot`. Every mutating op
 * (create/delete/rename/mkdir) changes exactly one entry and flushes it right
 * away, so there is never a cross-sector batch of unpersisted changes -- writing
 * only this entry's 1-2 sectors is byte-identical on disk to rewriting the whole
 * table, but moves ~189 KB (TOSFS_DIR_SECTORS) of PIO per save down to ~1 KB.
 * On a single core that polled-PIO write is what froze the desktop on every
 * notepad autosave; bounding it to a sector or two makes a save imperceptible.
 * (Neighbouring entries that share the sector are rewritten from the in-memory
 * super, which is authoritative, so they stay consistent.) */
static int flush_super_ent(int slot) {
    uint32_t off0 = 8u + (uint32_t)slot * TOSFS_ENT_SZ;     /* entries start past the 8-byte header */
    uint32_t s0   = off0 / 512u;
    uint32_t s1   = (off0 + TOSFS_ENT_SZ - 1u) / 512u;      /* an entry spans at most two sectors */
    return fs_swrite(s0, s1 - s0 + 1u, (const uint8_t *)&super + s0 * 512u);
}

int fs_mount(void) {
    base_lba = find_partition();             /* locate our partition on the disk */
    if (fs_sread(0, TOSFS_DIR_SECTORS, &super) < 0) return -1;
    if (super.magic != TOSFS_MAGIC) return -1;
    mounted = 1;
    rebuild_used_map();
    for (int i = 0; i < MAX_TASKS; i++) cwd[i] = TOSFS_ROOT;
    return 0;
}

/* --- path resolution ------------------------------------------------------- *
 * A slot index >= 0 names a real entry; -1 (TOSFS_ROOT) is the root directory
 * (which has no entry); -2 means "not found". Resolution starts at `from` (the
 * caller's cwd) unless the path is absolute. */
#define R_NONE (-2)

static int child_named(int parent, const char *name) {
    for (uint32_t i = 0; i < TOSFS_MAX_FILES; i++)
        if (super.ents[i].type != TOSFS_FREE &&
            super.ents[i].parent == parent &&
            names_equal(super.ents[i].name, name)) return (int)i;
    return R_NONE;
}

static int resolve(const char *path, int from) {
    int cur = (path[0] == '/') ? TOSFS_ROOT : from;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[TOSFS_NAME_MAX];
        int j = 0;
        while (*p && *p != '/' && j < TOSFS_NAME_MAX - 1) comp[j++] = *p++;
        comp[j] = 0;
        while (*p && *p != '/') p++;     /* skip an over-long component's tail */
        while (*p == '/') p++;
        if (comp[0] == 0 || names_equal(comp, ".")) continue;
        if (names_equal(comp, "..")) { if (cur != TOSFS_ROOT) cur = super.ents[cur].parent; continue; }
        int next = child_named(cur, comp);
        if (next == R_NONE) return R_NONE;
        cur = next;
    }
    return cur;
}

/* Resolve everything but the final component: returns the parent directory slot
 * (>=0 or TOSFS_ROOT) and copies the leaf name into `leaf`, or R_NONE on error. */
static int resolve_parent(const char *path, int from, char *leaf) {
    char buf[256];
    int n = 0;
    while (path[n] && n < 255) { buf[n] = path[n]; n++; }
    buf[n] = 0;
    while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;     /* strip trailing slashes */

    int last = -1;
    for (int i = 0; buf[i]; i++) if (buf[i] == '/') last = i;
    if (last < 0) { copy_name(leaf, buf); return from; }  /* bare name in `from` */
    copy_name(leaf, buf + last + 1);
    if (leaf[0] == 0) return R_NONE;
    if (last == 0) return TOSFS_ROOT;                     /* "/leaf" */
    buf[last] = 0;
    return resolve(buf, from);
}

static int is_dir(int slot) { return slot == TOSFS_ROOT || (slot >= 0 && super.ents[slot].type == TOSFS_DIR); }

/* The owner uid of a slot; the root directory (no entry) is system-owned, so a
 * user can't create top-level entries. */
static int owner_of(int slot) { return slot == TOSFS_ROOT ? TOS_UID_SYSTEM : super.ents[slot].owner; }
/* May the running task write (create in / delete / modify) the entry at `slot`? */
static int can_write(int slot) { return tos_may_write(sched_uid(), owner_of(slot)); }

static const struct tosfs_ent *find_ent(const char *name) {
    if (!mounted) return 0;
    /* Resolve from root: an absolute path (e.g. an app bundle's "/Apps/X.app/bin/x")
     * loads directly. A bare program name ("shell", "twm", ...) is a system binary,
     * so fall back to /System/bin -- this keeps exec("shell") working from anywhere
     * after the boot chain moved out of the flat root into the system tree. */
    int s = resolve(name, TOSFS_ROOT);
    if (s < 0 && name[0] != '/') {
        char path[16 + TOSFS_NAME_MAX];
        const char *pre = "/System/bin/";
        int j = 0;
        while (pre[j]) { path[j] = pre[j]; j++; }
        for (int k = 0; name[k] && j < (int)sizeof(path) - 1; k++) path[j++] = name[k];
        path[j] = 0;
        s = resolve(path, TOSFS_ROOT);
    }
    if (s < 0 || super.ents[s].type != TOSFS_FILE) return 0;
    return &super.ents[s];
}

const struct tosfs_ent *fs_find(const char *name) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    const struct tosfs_ent *e = find_ent(name);
    spin_unlock_preempt(&fs_lock, f);
    return e;
}

uint32_t fs_nfiles(void) {
    if (!mounted) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < TOSFS_MAX_FILES; i++) if (super.ents[i].type == TOSFS_FILE) n++;
    return n;
}

void fs_ls(void) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    if (!mounted) { spin_unlock_preempt(&fs_lock, f); console_puts("fs: not mounted\r\n"); return; }
    int dir = cwd[sched_current()];
    for (uint32_t i = 0; i < TOSFS_MAX_FILES; i++) {
        if (super.ents[i].type == TOSFS_FREE || super.ents[i].parent != dir) continue;
        console_puts(super.ents[i].name);
        if (super.ents[i].type == TOSFS_DIR) { console_puts("/\r\n"); continue; }
        console_puts("\t");
        console_putdec(super.ents[i].size);
        console_puts(" bytes\r\n");
    }
    spin_unlock_preempt(&fs_lock, f);
}

/* --- directory operations -------------------------------------------------- */

static int mkdir_l(const char *path) {
    if (!mounted) return -1;
    char leaf[TOSFS_NAME_MAX];
    int parent = resolve_parent(path, cwd[sched_current()], leaf);
    if (parent == R_NONE || !is_dir(parent)) return -1;
    if (!can_write(parent)) return -1;                    /* may not create in a system dir */
    if (names_equal(leaf, ".") || names_equal(leaf, "..")) return -1;
    if (child_named(parent, leaf) != R_NONE) return -1;   /* already exists */
    int s = alloc_slot();
    if (s < 0) return -1;
    copy_name(super.ents[s].name, leaf);
    super.ents[s].start_lba = 0;
    super.ents[s].size      = 0;
    super.ents[s].parent    = parent;
    super.ents[s].type      = TOSFS_DIR;
    super.ents[s].owner     = (uint8_t)sched_uid();       /* a new entry is owned by its creator */
    super.ents[s].mode      = 0;
    super.ents[s].mtime     = fs_now();
    return flush_super_ent(s);
}

int fs_mkdir(const char *path) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = mkdir_l(path);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int has_children(int slot) {
    for (uint32_t i = 0; i < TOSFS_MAX_FILES; i++)
        if (super.ents[i].type != TOSFS_FREE && super.ents[i].parent == slot) return 1;
    return 0;
}

static int rmdir_l(const char *path) {
    if (!mounted) return -1;
    int s = resolve(path, cwd[sched_current()]);
    if (s < 0 || super.ents[s].type != TOSFS_DIR) return -1;   /* not a dir / root */
    if (!can_write(s)) return -1;                              /* may not remove a system dir */
    if (has_children(s)) return -1;                            /* not empty */
    /* if any task's cwd sits on it, refuse so it can't dangle */
    for (int t = 0; t < MAX_TASKS; t++) if (cwd[t] == s) return -1;
    super.ents[s].type   = TOSFS_FREE;
    super.ents[s].name[0] = 0;
    return flush_super_ent(s);
}

int fs_rmdir(const char *path) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = rmdir_l(path);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int chdir_l(const char *path) {
    if (!mounted) return -1;
    int s = resolve(path, cwd[sched_current()]);
    if (!is_dir(s)) return -1;          /* R_NONE or a file */
    cwd[sched_current()] = s;
    return 0;
}

int fs_chdir(const char *path) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = chdir_l(path);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int getcwd_l(char *buf, int len) {
    if (len <= 0) return -1;
    int chain[64], d = 0, cur = cwd[sched_current()];
    while (cur >= 0 && d < 64) { chain[d++] = cur; cur = super.ents[cur].parent; }
    int pos = 0;
    if (d == 0) { if (len > 1) buf[pos++] = '/'; buf[pos] = 0; return 0; }
    for (int i = d - 1; i >= 0; i--) {
        if (pos < len - 1) buf[pos++] = '/';
        const char *nm = super.ents[chain[i]].name;
        for (int j = 0; nm[j] && pos < len - 1; j++) buf[pos++] = nm[j];
    }
    buf[pos] = 0;
    return 0;
}

int fs_getcwd(char *buf, int len) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = getcwd_l(buf, len);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

/* Is `to` the same as, or a descendant of, `slot`? (used to reject moving a dir
 * into itself, which would orphan a subtree into a cycle.) */
static int is_within(int to, int slot) {
    for (int cur = to; cur >= 0; cur = super.ents[cur].parent)
        if (cur == slot) return 1;
    return 0;
}

static int rename_l(const char *oldp, const char *newp) {
    if (!mounted) return -1;
    int s = resolve(oldp, cwd[sched_current()]);
    if (s < 0) return -1;                               /* can't move the root */
    if (!can_write(s)) return -1;                       /* may not move a system entry */
    char leaf[TOSFS_NAME_MAX];
    int parent = resolve_parent(newp, cwd[sched_current()], leaf);
    if (parent == R_NONE || !is_dir(parent)) return -1;
    if (!can_write(parent)) return -1;                  /* may not drop it into a system dir */
    if (names_equal(leaf, ".") || names_equal(leaf, "..")) return -1;
    if (child_named(parent, leaf) != R_NONE) return -1; /* destination exists */
    if (super.ents[s].type == TOSFS_DIR && is_within(parent, s)) return -1; /* cycle */
    super.ents[s].parent = parent;
    copy_name(super.ents[s].name, leaf);
    return flush_super_ent(s);
}

int fs_rename(const char *oldp, const char *newp) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = rename_l(oldp, newp);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int stat_l(const char *path, struct fstat *st) {
    if (!mounted) return -1;
    int s = resolve(path, cwd[sched_current()]);
    if (s == R_NONE) return -1;
    if (s == TOSFS_ROOT) { st->type = TOSFS_DIR; st->size = 0; st->owner = TOS_UID_SYSTEM; st->mtime = 0; return 0; }
    st->type  = super.ents[s].type;
    st->size  = super.ents[s].size;
    st->owner = super.ents[s].owner;
    st->mtime = super.ents[s].mtime;
    return 0;
}

int fs_stat(const char *path, struct fstat *st) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = stat_l(path, st);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

/* Volume usage: the directory table (sectors 0..D-1) is fixed overhead, so the data
 * capacity is DISK-D sectors; free is however many of those the bitmap has clear. */
static int statfs_l(struct statfs *st) {
    if (!mounted) return -1;
    uint32_t freecnt = 0;
    for (uint32_t s = TOSFS_DIR_SECTORS; s < TOSFS_DISK_SECTORS; s++)
        if (!bit_get(s)) freecnt++;
    st->block_size  = 512;
    st->total_bytes = (TOSFS_DISK_SECTORS - TOSFS_DIR_SECTORS) * 512u;
    st->free_bytes  = freecnt * 512u;
    return 0;
}
int fs_statfs(struct statfs *st) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = statfs_l(st);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int readdir_l(const char *path, struct dirent *out, int max) {
    if (!mounted) return -1;
    int dir = (path && path[0]) ? resolve(path, cwd[sched_current()]) : cwd[sched_current()];
    if (!is_dir(dir)) return -1;
    int n = 0;
    for (uint32_t i = 0; i < TOSFS_MAX_FILES && n < max; i++) {
        if (super.ents[i].type == TOSFS_FREE || super.ents[i].parent != dir) continue;
        copy_name(out[n].name, super.ents[i].name);
        out[n].type  = super.ents[i].type;
        out[n].size  = super.ents[i].size;
        out[n].mtime = super.ents[i].mtime;
        out[n].owner = super.ents[i].owner;       /* lock badges read this without a per-row SYS_STAT */
        n++;
    }
    return n;
}

int fs_readdir(const char *path, struct dirent *out, int max) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = readdir_l(path, out, max);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

/* --- files: open / read / write / close / unlink --------------------------- */

static int alloc_fd(struct ofile *t) {
    for (int i = 0; i < NOFILE; i++) if (!t[i].used) return i;
    return -1;
}

/* Is slot `s` open by any reader? (delete cares.) */
static int slot_open(int s) {
    for (int t = 0; t < MAX_TASKS; t++)
        for (int i = 0; i < NOFILE; i++)
            if (oft[t][i].used && !oft[t][i].writing && oft[t][i].slot == s) return 1;
    return 0;
}

/* The write buffer is shared, so only one writer may be open across all tasks. */
static int writer_active(void) {
    for (int t = 0; t < MAX_TASKS; t++)
        for (int i = 0; i < NOFILE; i++)
            if (oft[t][i].used && oft[t][i].writing) return 1;
    return 0;
}

/* Delete the file in slot `s`: free its sectors, flip the entry to FREE, persist.
 * Refuses if a reader holds it open. */
static int unlink_slot(int s) {
    if (slot_open(s)) return -1;
    uint32_t start = super.ents[s].start_lba;
    uint32_t nsec  = sectors_for(super.ents[s].size);
    for (uint32_t k = 0; k < nsec; k++) bit_clr(start + k);
    super.ents[s].type    = TOSFS_FREE;
    super.ents[s].name[0] = 0;
    super.ents[s].start_lba = super.ents[s].size = 0;
    return flush_super_ent(s);
}

static int unlink_l(const char *path) {
    if (!mounted) return -1;
    int s = resolve(path, cwd[sched_current()]);
    if (s < 0 || super.ents[s].type != TOSFS_FILE) return -1;   /* not a file */
    if (!can_write(s)) return -1;                               /* may not delete a system file */
    return unlink_slot(s);
}

int fs_unlink(const char *name) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = unlink_l(name);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int open_l(const char *name, int flags) {
    if (!mounted) return -1;
    struct ofile *tbl = oft[sched_current()];

    if (flags & O_CREATE) {
        char leaf[TOSFS_NAME_MAX];
        int parent = resolve_parent(name, cwd[sched_current()], leaf);
        if (parent == R_NONE || !is_dir(parent)) return -1;
        if (!can_write(parent)) return -1;        /* may not create in a system dir */
        if (leaf[0] == 0 || names_equal(leaf, ".") || names_equal(leaf, "..")) return -1;
        int ex = child_named(parent, leaf);
        if (ex != R_NONE) {                       /* name already in use */
            if (super.ents[ex].type != TOSFS_FILE) return -1;   /* it's a directory */
            if (!can_write(ex)) return -1;         /* may not overwrite a system file */
            if (!(flags & O_TRUNC)) return -1;     /* no overwrite without O_TRUNC */
        }
        /* Validate EVERY resource BEFORE destroying the existing file, so a failed
         * open(O_CREATE|O_TRUNC) can never lose the user's data (the old order
         * unlinked first, so any later -1 here -- another writer, a full fd table,
         * a full disk -- left the file already gone). A brand-new entry needs a free
         * dir slot; an overwrite reuses ex's slot, freed by the unlink below. */
        if (ex == R_NONE && alloc_slot() < 0) return -1;        /* directory full */
        if (writer_active()) return -1;                         /* one writer only */
        int fd = alloc_fd(tbl);
        if (fd < 0) return -1;
        if (ex != R_NONE && unlink_slot(ex) < 0) return -1;     /* in-place rewrite = delete + recreate */
        int base = first_free();                                /* after reclaiming ex's sectors */
        if (base < 0) return -1;                                /* disk full */
        bit_set((uint32_t)base);
        struct ofile *f = &tbl[fd];
        f->used = 1; f->writing = 1; f->slot = -1; f->parent = parent;
        f->base_lba = (uint32_t)base;
        f->nsect = 1; f->size = 0; f->pos = 0;
        copy_name(f->name, leaf);
        return fd;
    }

    int s = resolve(name, cwd[sched_current()]);
    if (s < 0 || super.ents[s].type != TOSFS_FILE) return -1;
    int fd = alloc_fd(tbl);
    if (fd < 0) return -1;
    struct ofile *f = &tbl[fd];
    f->used = 1; f->writing = 0; f->slot = s; f->parent = -1;
    f->base_lba = super.ents[s].start_lba;
    f->nsect = sectors_for(super.ents[s].size);
    f->size = super.ents[s].size; f->pos = 0;
    copy_name(f->name, super.ents[s].name);
    return fd;
}

int fs_open(const char *name, int flags) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = open_l(name, flags);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int read_l(int fd, void *buf, uint32_t len) {
    struct ofile *tbl = oft[sched_current()];
    if (fd < 0 || fd >= NOFILE || !tbl[fd].used || tbl[fd].writing) return -1;
    struct ofile *f = &tbl[fd];
    uint8_t *out = (uint8_t *)buf;
    uint32_t n = 0;
    while (n < len && f->pos < f->size) {
        if (fs_sread(f->base_lba + f->pos / 512, 1, rbuf) < 0) return -1;
        uint32_t off   = f->pos % 512;
        uint32_t chunk = 512 - off;
        if (chunk > len - n)        chunk = len - n;
        if (chunk > f->size - f->pos) chunk = f->size - f->pos;
        for (uint32_t i = 0; i < chunk; i++) out[n + i] = rbuf[off + i];
        n += chunk; f->pos += chunk;
    }
    return (int)n;
}

int fs_read(int fd, void *buf, uint32_t len) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = read_l(fd, buf, len);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int write_l(int fd, const void *buf, uint32_t len) {
    struct ofile *tbl = oft[sched_current()];
    if (fd < 0 || fd >= NOFILE || !tbl[fd].used || !tbl[fd].writing) return -1;
    struct ofile *f = &tbl[fd];
    const uint8_t *in = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t sec_idx = f->pos / 512;
        uint32_t sec     = f->base_lba + sec_idx;
        if (sec_idx >= f->nsect) {              /* grow into the next contiguous sector */
            if (sec >= TOSFS_DISK_SECTORS || bit_get(sec)) { f->size = f->pos; return (int)i; }
            bit_set(sec);
            f->nsect++;
        }
        wbuf[f->pos % 512] = in[i];
        f->pos++;
        if (f->pos % 512 == 0)                  /* sector filled -> flush it */
            if (fs_swrite(sec, 1, wbuf) < 0) return -1;
    }
    f->size = f->pos;
    return (int)len;
}

int fs_write(int fd, const void *buf, uint32_t len) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = write_l(fd, buf, len);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

static int close_l(int fd) {
    struct ofile *tbl = oft[sched_current()];
    if (fd < 0 || fd >= NOFILE || !tbl[fd].used) return -1;
    struct ofile *f = &tbl[fd];
    if (f->writing) {
        if (f->pos == 0) {                               /* empty file: no data sectors, but
                                                          * still a real 0-byte entry (like a
                                                          * dir: start_lba 0, size 0) so New File
                                                          * / truncate-to-empty / copying an empty
                                                          * file persist instead of vanishing */
            bit_clr(f->base_lba);                        /* return the speculatively-reserved sector */
            int s = alloc_slot();
            if (s >= 0) {
                copy_name(super.ents[s].name, f->name);
                super.ents[s].start_lba = 0;
                super.ents[s].size      = 0;
                super.ents[s].parent    = f->parent;
                super.ents[s].type      = TOSFS_FILE;
                super.ents[s].owner     = (uint8_t)sched_uid();
                super.ents[s].mode      = 0;
                super.ents[s].mtime     = fs_now();
                if (flush_super_ent(s) < 0) return -1;
            }
        } else {
            if (f->pos % 512 != 0) {                     /* flush partial sector */
                for (uint32_t j = f->pos % 512; j < 512; j++) wbuf[j] = 0;
                if (fs_swrite(f->base_lba + f->pos / 512, 1, wbuf) < 0) return -1;
            }
            int s = alloc_slot();                        /* record in the table */
            if (s >= 0) {
                copy_name(super.ents[s].name, f->name);
                super.ents[s].start_lba = f->base_lba;
                super.ents[s].size      = f->pos;
                super.ents[s].parent    = f->parent;
                super.ents[s].type      = TOSFS_FILE;
                super.ents[s].owner     = (uint8_t)sched_uid();   /* owned by the writer */
                super.ents[s].mode      = 0;
                super.ents[s].mtime     = fs_now();
                if (flush_super_ent(s) < 0) return -1;   /* persist just this entry's sector */
            } else {
                for (uint32_t k = 0; k < f->nsect; k++) bit_clr(f->base_lba + k);
            }
        }
    }
    f->used = 0;
    return 0;
}

int fs_close(int fd) {
    uint64_t f = spin_lock_preempt(&fs_lock);
    int r = close_l(fd);
    spin_unlock_preempt(&fs_lock, f);
    return r;
}

/* Release every fd a task left open and reset its cwd (called when it exits,
 * under the scheduler lock). A half-written file is discarded -- its reserved
 * sectors go back to the free map and it is never recorded. */
void fs_close_all(int task) {
    if (task < 0 || task >= MAX_TASKS) return;
    uint64_t fl = spin_lock_preempt(&fs_lock);
    for (int i = 0; i < NOFILE; i++) {
        struct ofile *f = &oft[task][i];
        if (!f->used) continue;
        if (f->writing)
            for (uint32_t k = 0; k < f->nsect; k++) bit_clr(f->base_lba + k);
        f->used = 0;
    }
    cwd[task] = TOSFS_ROOT;
    spin_unlock_preempt(&fs_lock, fl);
}

/* A forked child inherits the parent's working directory. */
void fs_fork(int parent, int child) {
    if (parent < 0 || parent >= MAX_TASKS || child < 0 || child >= MAX_TASKS) return;
    uint64_t fl = spin_lock_preempt(&fs_lock);
    cwd[child] = cwd[parent];
    spin_unlock_preempt(&fs_lock, fl);
}
