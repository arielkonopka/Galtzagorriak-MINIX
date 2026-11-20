/* <sys/statfs.h> - free space of a mounted file system (spec O.5)
 *
 * fstatfs(fd, &buf) and statfs(path, &buf) answer from the FS server's
 * counters, without reading the bit maps.  Right after a mount the counts
 * come from a hint in the super block (ST_ESTIMATE); the idle recount that
 * update drives makes them exact.
 */
#ifndef _STATFS_H
#define _STATFS_H

#ifndef _TYPES_H
#include <sys/types.h>
#endif

struct statfs {
  long f_bsize;			/* block size: 1024 */
  long f_blocks;		/* data zones on the file system */
  long f_bfree;			/* free zones */
  long f_bavail;		/* free zones for ordinary users (= f_bfree) */
  long f_files;			/* inodes */
  long f_ffree;			/* free inodes */
  dev_t f_dev;			/* the device */
  short f_namemax;		/* longest file name: 14 (V1/V2) or 60 (V3) */
  short f_version;		/* 1, 2 or 3 */
  short f_flag;			/* ST_* below */
};

#define ST_RDONLY	0x0001	/* mounted read-only */
#define ST_ESTIMATE	0x0002	/* counts from the hint; recount not done */

/* m1_i1 of FSTATFS: >= 0 a file descriptor; FSTATFS_PATH a path (m1_p1,
 * m1_i2 its length); FSTATFS_RECOUNT one step of the idle recount (update
 * only, superuser; returns 1 while work is left, 0 when none). */
#define FSTATFS_PATH	(-1)
#define FSTATFS_RECOUNT	(-2)

#include <ansi.h>
_PROTOTYPE( int fstatfs, (int _fd, struct statfs *_buf)			);
_PROTOTYPE( int statfs, (const char *_path, struct statfs *_buf)	);

#endif /* _STATFS_H */
