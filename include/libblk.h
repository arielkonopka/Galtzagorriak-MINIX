/* <libblk.h> - frames over swappable memory blocks (spec 12h, R12.38).
 *
 * A pool in the app's own memory holds frames of 1, 2 or 4 KB; each
 * frame caches one frame-sized piece of the app's block space (<blk.h>).
 * vm_get() gives a pointer into a frame, loading it first (and writing
 * back the least recently used dirty frame if it needs the room).  The
 * pointer stays valid until the next vm_get() unless the frame is
 * pinned.  8 B of metadata per frame, taken from the pool.  See libblk(3).
 * Static library: link with -lblk (flipper/minix.mk: $(MB)/blk/libblk.o).
 */
#ifndef _LIBBLK_H
#define _LIBBLK_H

#include <blk.h>

/* vm_get() modes. */
#define VM_RD	0x01		/* read the data */
#define VM_RW	0x02		/* read and change: the frame is written back */
#define VM_PIN	0x04		/* keep the frame (and the pointer) until
				 * vm_unpin() */

#define VM_NONE	0xFFFFFFFFU	/* struct vm_frame unit: an empty frame */

struct vm_frame {		/* 8 B per frame */
  unsigned int unit;		/* which frame-sized piece; VM_NONE: empty */
  unsigned short age;		/* LRU clock at the last use */
  unsigned char flags;		/* VF_DIRTY: changed since the last write */
  unsigned char pins;		/* vm_get(..., VM_PIN) without vm_unpin() */
};

struct vm {
  char *pool;			/* nframes frames of fsize bytes */
  struct vm_frame *f;		/* behind the frames, in the pool */
  unsigned long size;		/* bytes of the block space reserved */
  unsigned short fsize;		/* 1024, 2048 or 4096 */
  unsigned short nframes;
  unsigned short clock;		/* LRU clock */
  short err;			/* the last error, as errno */
};

/* Reserve size bytes of blocks (flags: BLK_ANY) and split pool (poolsize
 * bytes, at least one frame and its 8 B) into frames of framesize bytes.
 * 0, or -1 with errno.  v->size tells what was reserved. */
_PROTOTYPE( int vm_open, (struct vm *_v, void *_pool, unsigned _poolsize,
		unsigned long _size, unsigned _framesize, int _flags)	);
_PROTOTYPE( void *vm_get, (struct vm *_v, unsigned long _off, int _mode));
_PROTOTYPE( void vm_unpin, (struct vm *_v, unsigned long _off)		);
/* Write every changed frame back.  0, or -1 (the frames stay dirty, so a
 * later vm_flush() tries again, e.g. after ENXIO). */
_PROTOTYPE( int vm_flush, (struct vm *_v)				);
/* Forget the frames of [off, off + len) without writing them: their data
 * is undefined afterwards (zeros or the old contents). */
_PROTOTYPE( void vm_drop, (struct vm *_v, unsigned long _off,
						unsigned long _len)	);
/* vm_flush(), then blk_close(). */
_PROTOTYPE( int vm_close, (struct vm *_v)				);

/* Arrays of fixed-size records that never cross a frame: record i of
 * type T.  A frame holds fsize / sizeof(T) of them; the rest of a frame
 * stays unused. */
#define VA_OFF(v, sz, i) \
	((unsigned long) ((i) / ((v)->fsize / (sz))) * (v)->fsize \
	+ (unsigned long) ((i) % ((v)->fsize / (sz))) * (sz))
#define VA_GET(v, T, i)	((T *) vm_get((v), VA_OFF((v), sizeof(T), (i)), VM_RD))
#define VA_SET(v, T, i)	((T *) vm_get((v), VA_OFF((v), sizeof(T), (i)), VM_RW))

#endif /* _LIBBLK_H */
