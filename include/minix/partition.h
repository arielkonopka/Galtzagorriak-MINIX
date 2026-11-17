/*	minix/partition.h				Author: Kees J. Bot
 *								7 Dec 1995
 * Place of a partition on disk and the disk geometry,
 * for use with the DIOCGETP and DIOCSETP ioctl's.
 */
#ifndef _MINIX__PARTITION_H
#define _MINIX__PARTITION_H

struct partition {
  u32_t base;		/* byte offset to the partition start */
  u32_t size;		/* number of bytes in the partition */
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
#endif /* _MINIX__PARTITION_H */
