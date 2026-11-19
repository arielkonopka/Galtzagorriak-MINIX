/*	minix/partition.h				Author: Kees J. Bot
 *								7 Dec 1995
 * Place of a partition on disk and the disk geometry,
 * for use with the DIOCGETP and DIOCSETP ioctl's.
 */
#ifndef _MINIX__PARTITION_H
#define _MINIX__PARTITION_H

struct partition {
  u32_t base;		/* byte offset to the partition start (the SD card:
			 * EOVERFLOW past 4 GiB, use DIOCGETPS) */
  u32_t size;		/* number of bytes in the partition (the SD card:
			 * at most 4 GiB - 512, DIOCGETPS has the rest) */
  unsigned cylinders;	/* disk geometry */
  unsigned heads;
  unsigned sectors;
};

/* DIOCDISCARD (R12.40): sectors that hold nothing any more, counted from
 * the start of the partition the ioctl is made on. */
struct part_range {
  u32_t first;		/* first sector */
  u32_t count;		/* number of sectors */
};
/* DIOCGETPS (R12.39) answers with a part_range too: the partition's first
 * sector on the disk and its number of sectors (any size, up to 2 TiB). */

/* DIOCPRW (R12.39): raw sectors of the partition, also past 4 GiB where
 * read() and lseek() of /dev/hdN cannot go.  Straight into the caller's
 * buffer.  FS refuses a write on a mounted device or through a descriptor
 * not open for writing. */
#define PRW_MAX		64	/* sectors per call at most (32 KB) */
struct part_rw {
  u32_t sector;		/* first sector, counted from the partition start */
  u16_t count;		/* number of sectors, 1 to PRW_MAX */
  u8_t write;		/* 0 read, 1 write */
  char *buf;		/* count * 512 bytes */
};

/* libc: DIOCPRW on fd, in pieces of PRW_MAX; 0 or -1 with errno. */
_PROTOTYPE( int bread, (int _fd, unsigned long _sector, void *_buf,
							unsigned _count) );
_PROTOTYPE( int bwrite, (int _fd, unsigned long _sector, const void *_buf,
							unsigned _count) );
#endif /* _MINIX__PARTITION_H */
