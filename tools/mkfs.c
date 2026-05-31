/* mkfs for tosfs -- a HOST program (built with the normal gcc, not the kernel
 * toolchain). Packs files and directories into a tosfs (v2) image:
 *
 *   mkfs out.img  dest=hostfile  dest/  ...
 *
 *   dest=hostfile   a file at path `dest` in the image (any leading directories
 *                   in `dest` are created automatically), with the contents of
 *                   the host file `hostfile`.
 *   dest            (no '=')  an empty directory at path `dest`.
 *
 * Each file becomes a table entry plus sector-aligned, contiguous data; each
 * directory is just an entry that its children point at by slot index. The
 * kernel's fs.c reads the result back. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../kernel/fs/tosfs.h"

#define SECTOR 512

static struct tosfs_super super;
static char *hostpath[TOSFS_MAX_FILES];     /* host source for each FILE slot */
static int   nslots = 0;

static long file_size(FILE *f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    return n;
}

static int new_slot(unsigned type, const char *name, int parent) {
    if (nslots >= (int)TOSFS_MAX_FILES) { fprintf(stderr, "mkfs: directory full (max %u)\n", TOSFS_MAX_FILES); exit(1); }
    if (strlen(name) >= TOSFS_NAME_MAX) { fprintf(stderr, "mkfs: name too long: %s\n", name); exit(1); }
    int s = nslots++;
    memset(&super.ents[s], 0, sizeof super.ents[s]);
    strncpy(super.ents[s].name, name, TOSFS_NAME_MAX - 1);
    super.ents[s].type   = type;
    super.ents[s].parent = parent;
    return s;
}

static int find_child(int parent, const char *name) {
    for (int i = 0; i < nslots; i++)
        if (super.ents[i].type != TOSFS_FREE && super.ents[i].parent == parent &&
            !strcmp(super.ents[i].name, name)) return i;
    return -1;
}

/* Ensure every directory component of `path` exists; return the slot of the last
 * one (or TOSFS_ROOT for an empty path). */
static int ensure_dir(const char *path) {
    int cur = TOSFS_ROOT;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[TOSFS_NAME_MAX]; int j = 0;
        while (*p && *p != '/' && j < TOSFS_NAME_MAX - 1) comp[j++] = *p++;
        comp[j] = 0;
        while (*p && *p != '/') p++;
        while (*p == '/') p++;
        if (!comp[0]) continue;
        int c = find_child(cur, comp);
        if (c < 0) c = new_slot(TOSFS_DIR, comp, cur);
        else if (super.ents[c].type != TOSFS_DIR) { fprintf(stderr, "mkfs: %s is not a directory\n", comp); exit(1); }
        cur = c;
    }
    return cur;
}

/* Split `path` into its parent directory (created on demand) and leaf name. */
static int split_parent(const char *path, char *leaf) {
    char buf[1024];
    strncpy(buf, path, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;
    char *slash = strrchr(buf, '/');
    if (!slash) { strncpy(leaf, buf, TOSFS_NAME_MAX - 1); leaf[TOSFS_NAME_MAX - 1] = 0; return TOSFS_ROOT; }
    strncpy(leaf, slash + 1, TOSFS_NAME_MAX - 1); leaf[TOSFS_NAME_MAX - 1] = 0;
    *slash = 0;
    return ensure_dir(buf);          /* "" -> root */
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.img dest=hostfile | dest ...\n", argv[0]);
        return 1;
    }

    memset(&super, 0, sizeof super);   /* zeroed slots are TOSFS_FREE */
    super.magic = TOSFS_MAGIC;

    /* Build the entry table from the arguments. */
    for (int i = 2; i < argc; i++) {
        char *arg = argv[i];
        char *eq  = strchr(arg, '=');
        if (eq) {                                   /* dest=hostfile: a file */
            *eq = 0;
            const char *dest = arg, *host = eq + 1;
            char leaf[TOSFS_NAME_MAX];
            int parent = split_parent(dest, leaf);
            if (find_child(parent, leaf) >= 0) { fprintf(stderr, "mkfs: duplicate %s\n", dest); return 1; }
            int s = new_slot(TOSFS_FILE, leaf, parent);
            hostpath[s] = strdup(host);
        } else {                                    /* dest: an empty directory */
            ensure_dir(arg);
        }
    }

    /* Assign each file a sector-aligned, contiguous region after the table. */
    uint32_t lba = TOSFS_DIR_SECTORS;
    for (int s = 0; s < nslots; s++) {
        if (super.ents[s].type != TOSFS_FILE) continue;
        FILE *f = fopen(hostpath[s], "rb");
        if (!f) { fprintf(stderr, "mkfs: cannot open %s\n", hostpath[s]); return 1; }
        long sz = file_size(f); fclose(f);
        super.ents[s].start_lba = lba;
        super.ents[s].size      = (uint32_t)sz;
        lba += (uint32_t)((sz + SECTOR - 1) / SECTOR);
        if (lba > TOSFS_DISK_SECTORS) { fprintf(stderr, "mkfs: image overflow (need %u sectors)\n", lba); return 1; }
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) { fprintf(stderr, "mkfs: cannot create %s\n", argv[1]); return 1; }

    fwrite(&super, 1, sizeof super, out);           /* the table (TOSFS_DIR_SECTORS sectors) */

    static unsigned char sec[SECTOR];
    int nfiles = 0, ndirs = 0;
    for (int s = 0; s < nslots; s++) {
        if (super.ents[s].type == TOSFS_DIR) { ndirs++; continue; }
        if (super.ents[s].type != TOSFS_FILE) continue;
        FILE *f = fopen(hostpath[s], "rb");
        long remaining = super.ents[s].size;
        while (remaining > 0) {
            memset(sec, 0, SECTOR);
            long n = remaining < SECTOR ? remaining : SECTOR;
            fread(sec, 1, n, f);
            fwrite(sec, 1, SECTOR, out);
            remaining -= n;
        }
        fclose(f);
        nfiles++;
        printf("  %-20s %7u bytes @ lba %u (parent %d)\n",
               super.ents[s].name, super.ents[s].size, super.ents[s].start_lba, super.ents[s].parent);
    }

    /* Pad out to the fixed disk size so the kernel has free sectors to append into. */
    memset(sec, 0, SECTOR);
    while (lba < TOSFS_DISK_SECTORS) { fwrite(sec, 1, SECTOR, out); lba++; }

    fclose(out);
    printf("mkfs: wrote %s (%d files, %d dirs, %u-sector disk, %u-sector table)\n",
           argv[1], nfiles, ndirs, TOSFS_DISK_SECTORS, TOSFS_DIR_SECTORS);
    return 0;
}
