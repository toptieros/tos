/* tosfs -- a tiny hierarchical filesystem, shared between the host mkfs tool and
 * the kernel so the on-disk layout has a single definition.
 *
 * Layout (512-byte sectors):
 *   sector 0..D-1 : struct tosfs_super (magic + a flat slot table), D sectors
 *   sector D..    : each file's bytes, sector-aligned and contiguous
 *
 * The slot table holds both files and directories. Every entry stores the slot
 * index of its parent directory (TOSFS_ROOT for a top-level entry), so the tree
 * is encoded without nesting the on-disk layout: a directory is just an entry
 * with no data that other entries point at. Entries are never moved, so a parent
 * index stays valid for the life of the filesystem -- deleting an entry only
 * flips its type back to FREE. The directory spans TOSFS_DIR_SECTORS sectors, so
 * the entry count scales with the disk rather than being a fixed cap. */
#pragma once
#include <stdint.h>

#define TOSFS_MAGIC     0x33534F54u   /* "TOS3" little-endian (v3 = per-entry owner) */
#define TOSFS_NAME_MAX  32

#define TOSFS_DISK_SECTORS 8192u       /* image is padded to 4 MiB: the shipped tree is ~1.8 MiB
                                        * (hi-res 128px app icons), leaving ~2 MiB free for app/
                                        * package installs (tos app install -- design/packaging.md).
                                        * Kept in sync with FS_PART_CNT (boot/stage1.asm) and
                                        * UFS_SECTORS (Makefile). A growable fs is a later track. */

/* Entry types. FREE (0) is the zero value, so a zeroed slot is a free slot. */
#define TOSFS_FREE 0u
#define TOSFS_FILE 1u
#define TOSFS_DIR  2u

#define TOSFS_ROOT (-1)                /* parent index of a top-level entry      */

/* The FS lives in its own MBR partition on the boot disk (this type byte marks
 * it). The kernel scans the partition table at mount to find its base LBA, and
 * all on-disk LBAs in this header are relative to that base. */
#define TOSFS_PART_TYPE 0x7fu

struct tosfs_ent {
    char     name[TOSFS_NAME_MAX];     /* a single path component (NUL-padded)   */
    uint32_t start_lba;                /* file: first data sector; dir/free: 0   */
    uint32_t size;                     /* file: bytes; dir/free: 0               */
    int32_t  parent;                   /* slot index of parent dir, or TOSFS_ROOT */
    uint32_t type;                     /* TOSFS_FREE / TOSFS_FILE / TOSFS_DIR    */
    uint8_t  owner;                    /* owning uid (TOS_UID_SYSTEM / _USER -- perm.h) */
    uint8_t  mode;                     /* reserved for future mode bits (0 today)  */
    uint16_t _entrsv;                  /* pad to a 4-byte boundary                 */
    uint32_t mtime;                    /* packed modification time (fstime.h); 0 = unknown */
};                                     /* == 56 bytes */

/* Entry size drives the table geometry below; sizing off sizeof keeps the three
 * derived constants correct if the entry ever changes again. */
#define TOSFS_ENT_SZ       ((uint32_t)sizeof(struct tosfs_ent))

/* Size the directory so the disk runs out of *data* sectors before it runs out of
 * slots: D sectors hold (D*512 - 8)/E entries (E = entry size); data sectors =
 * DISK - D; want (D*512 - 8)/E >= DISK - D  <=>  D*(512+E) >= E*DISK + 8.
 * (Grow the disk and the directory grows with it.) D can exceed 255; fs_sread/
 * fs_swrite chunk the table into <=128-sector ATA transfers. */
#define TOSFS_DIR_SECTORS  ((TOSFS_ENT_SZ * TOSFS_DISK_SECTORS + 8u + (512u + TOSFS_ENT_SZ) - 1u) / (512u + TOSFS_ENT_SZ))
#define TOSFS_MAX_FILES    ((TOSFS_DIR_SECTORS * 512u - 8u) / TOSFS_ENT_SZ)

struct tosfs_super {
    uint32_t magic;
    uint32_t reserved;                 /* keeps the 8-byte header the entries assume */
    struct tosfs_ent ents[TOSFS_MAX_FILES];
    uint8_t  _pad[TOSFS_DIR_SECTORS * 512u - 8u - TOSFS_MAX_FILES * TOSFS_ENT_SZ];
};                                     /* exactly TOSFS_DIR_SECTORS sectors */
