#pragma once
#include <stdint.h>
#include "tosfs.h"
#include "syscall.h"     /* struct dirent / struct fstat (shared with userspace) */

int  fs_mount(void);                        /* read+validate the table; 0 ok    */
uint32_t fs_base_lba(void);                 /* disk LBA of our partition's start */
const struct tosfs_ent *fs_find(const char *name);   /* root-level program lookup */
void fs_ls(void);                           /* print the cwd's directory to console */
uint32_t fs_nfiles(void);                   /* number of files (for sysinfo)   */

/* A tiny file API. Descriptors index a small per-task open-file table. Names are
 * paths resolved against the caller's working directory (absolute if they start
 * with '/'). Writing is sequential: O_CREATE makes a new file committed to the
 * table on close; only one writer may be open at a time. */
int fs_open(const char *name, int flags);   /* -> fd, or -1                    */
int fs_read(int fd, void *buf, uint32_t len);        /* -> bytes (0=EOF) or -1 */
int fs_write(int fd, const void *buf, uint32_t len); /* -> bytes, or -1        */
int fs_close(int fd);                                /* -> 0, or -1            */
int fs_unlink(const char *name);                     /* delete a file; 0 or -1 */
void fs_close_all(int task);                  /* close a task's fds when it exits */
void fs_fork(int parent, int child);          /* child inherits the parent's cwd  */

/* directory / path operations */
int fs_mkdir(const char *path);                          /* make a directory; 0/-1 */
int fs_rmdir(const char *path);                          /* remove an empty dir    */
int fs_chdir(const char *path);                          /* set the caller's cwd   */
int fs_getcwd(char *buf, int len);                       /* absolute cwd path      */
int fs_rename(const char *oldp, const char *newp);       /* move/rename            */
int fs_stat(const char *path, struct fstat *st);         /* type + size            */
int fs_readdir(const char *path, struct dirent *out, int max);  /* list a dir -> n */
