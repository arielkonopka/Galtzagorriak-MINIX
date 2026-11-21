/* <blk.h> - swappable memory blocks for apps (spec 12h, R12.38).
 *
 * Numbered 4 KB blocks on the SD card's swap partition, read into and
 * written from the caller's own buffers.  blk_open() reserves them; every
 * block 0..N-1 then exists and starts zeroed.  The reservation ends with
 * blk_close(), exec or exit; nothing is written then.  See blk(3), and
 * libblk(3) for frames in the app's memory (<libblk.h>).
 *
 * All return -1 with errno on an error: ENOSYS (blocks off), EBADF (no
 * reservation), ERANGE (block or sector range), ENOSPC (open), EIO (a CRC
 * mismatch or read error), ENXIO (card out: try again later), EPERM
 * (set-uid, set-gid or Secret: yes image, plain mode), EINVAL.
 */
#ifndef _BLK_H
#define _BLK_H

#ifndef _ANSI_H
#include <ansi.h>
#endif
#include <minix/blk.h>

/* blk_open: the number of blocks reserved (with BLK_ANY maybe fewer than
 * asked for).  A second call shrinks the run or grows it in place. */
_PROTOTYPE( int blk_open, (unsigned long _bytes, int _flags)		);
_PROTOTYPE( int blk_close, (void)					);
_PROTOTYPE( int blk_info, (struct blkinfo *_bi)				);
/* Whole blocks: BLK_SIZE bytes at buf. */
_PROTOTYPE( int blk_get, (unsigned long _id, void *_buf)		);
_PROTOTYPE( int blk_put, (unsigned long _id, const void *_buf)		);
_PROTOTYPE( int blk_putf, (unsigned long _id, const void *_buf, int _flags));
/* A part of a block: off and len multiples of BLK_SECT, off + len at most
 * BLK_SIZE. */
_PROTOTYPE( int blk_read, (unsigned long _id, unsigned _off, void *_buf,
							unsigned _len)	);
_PROTOTYPE( int blk_write, (unsigned long _id, unsigned _off,
					const void *_buf, unsigned _len)	);
/* R12.40: blocks id .. id + n - 1 are not needed any more; their contents
 * are undefined from now on (zeros or old data).  Done at idle. */
_PROTOTYPE( int blk_discard, (unsigned long _id, unsigned long _n)	);

#endif /* _BLK_H */
