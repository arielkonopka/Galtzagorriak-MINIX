/* The file system maintains a buffer cache to reduce the number of disk
 * accesses needed.  Whenever a read or write to the disk is done, a check is
 * first made to see if the block is in the cache.  This file manages the
 * cache.
 *
 * The entry points into this file are:
 *   get_block:	  request to fetch a block for reading or writing from cache
 *   put_block:	  return a block previously requested with get_block
 *   alloc_zone:  allocate a new zone (to increase the length of a file)
 *   free_zone:	  release a zone (when a file is removed)
 *   rw_block:	  read or write a block from the disk itself
 *   invalidate:  remove all the cache blocks on some device
 *   pipe_bufs:   count the buffers that hold pipe data (K.6)
 */

#include "fs.h"
#include <minix/com.h>
#include <minix/boot.h>
#include "buf.h"
#include "file.h"
#include "fproc.h"
#include "super.h"

FORWARD _PROTOTYPE( void rm_lru, (struct buf *bp) );
FORWARD _PROTOTYPE( struct super_block *rw_super, (Dev_t dev) );
FORWARD _PROTOTYPE( int sb_room, (Dev_t dev) );
FORWARD _PROTOTYPE( int clean_off, (struct super_block *sp) );
FORWARD _PROTOTYPE( int before_write, (Dev_t dev) );

/*===========================================================================*
 *				get_block				     *
 *===========================================================================*/
PUBLIC struct buf *get_block(dev, block, only_search)
register dev_t dev;		/* on which device is the block? */
register block_t block;		/* which block is wanted? */
int only_search;		/* if NO_READ, don't read, else act normal */
{
/* Check to see if the requested block is in the block cache.  If so, return
 * a pointer to it.  If not, evict some other block and fetch it (unless
 * 'only_search' is 1).  All the blocks in the cache that are not in use
 * are linked together in a chain, with 'front' pointing to the least recently
 * used block and 'rear' to the most recently used block.  If 'only_search' is
 * 1, the block being requested will be overwritten in its entirety, so it is
 * only necessary to see if it is in the cache; if it is not, any free buffer
 * will do.  It is not necessary to actually read the block in from disk.
 * If 'only_search' is PREFETCH, the block need not be read from the disk,
 * and the device is not to be marked on the block, so callers can tell if
 * the block returned is valid.
 * In addition to the LRU chain, there is also a hash chain to link together
 * blocks whose block numbers end with the same bit strings, for fast lookup.
 */

  int b;
  register struct buf *bp, *prev_ptr;

  /* Search the hash chain for (dev, block). Do_read() can use 
   * get_block(NO_DEV ...) to get an unnamed block to fill with zeros when
   * someone wants to read from a hole in a file, in which case this search
   * is skipped
   */
  if (dev != NO_DEV) {
	b = (int) block & HASH_MASK;
	bp = buf_hash[b];
	while (bp != NIL_BUF) {
		if (bp->b_blocknr == block && bp->b_dev == dev) {
			/* Block needed has been found. */
			if (bp->b_count == 0) rm_lru(bp);
			bp->b_count++;	/* record that block is in use */
			return(bp);
		} else {
			/* This block is not the one sought. */
			bp = bp->b_hash; /* move to next block on hash chain */
		}
	}
  }

  /* Desired block is not on available chain.  Take the oldest clean block,
   * else the oldest dirty one: it costs a write - and flushall() writes
   * every dirty block.  Pipe data (PIPE_DIRTY, spec K.6) has no place on
   * the disk: it is taken last (never while pipes keep at most half of
   * the cache, write.c), and then lost. */
  if ((bp = front) == NIL_BUF) panic("all buffers in use", NR_BUFS);
  while (bp != NIL_BUF && bp->b_dirt != CLEAN) bp = bp->b_next;
  if (bp == NIL_BUF)
	for (bp = front; bp->b_dirt == PIPE_DIRTY && bp->b_next != NIL_BUF; )
		bp = bp->b_next;
  rm_lru(bp);

  /* Remove the block that was just taken from its hash chain. */
  b = (int) bp->b_blocknr & HASH_MASK;
  prev_ptr = buf_hash[b];
  if (prev_ptr == bp) {
	buf_hash[b] = bp->b_hash;
  } else {
	/* The block just taken is not on the front of its hash chain. */
	while (prev_ptr->b_hash != NIL_BUF)
		if (prev_ptr->b_hash == bp) {
			prev_ptr->b_hash = bp->b_hash;	/* found it */
			break;
		} else {
			prev_ptr = prev_ptr->b_hash;	/* keep looking */
		}
  }

  /* If the block taken is dirty, make it clean by writing it to the disk.
   * Avoid hysteresis by flushing all other dirty blocks for the same device.
   */
  if (bp->b_dev != NO_DEV) {
	if (bp->b_dirt == DIRTY) flushall(bp->b_dev);
	if (bp->b_dirt != CLEAN) {	/* N.6: card out, every buffer dirty */
		printf("Lost block %ld of device %d/%d: no room to keep it\n",
			(long) bp->b_blocknr, (bp->b_dev>>MAJOR)&BYTE,
			(bp->b_dev>>MINOR)&BYTE);
		bp->b_dirt = CLEAN;
	}
#if ENABLE_CACHE2
	put_block2(bp);
#endif
  }

  /* Fill in block's parameters and add it to the hash chain where it goes. */
  bp->b_dev = dev;		/* fill in device number */
  bp->b_blocknr = block;	/* fill in block number */
  bp->b_count++;		/* record that block is being used */
  b = (int) bp->b_blocknr & HASH_MASK;
  bp->b_hash = buf_hash[b];
  buf_hash[b] = bp;		/* add to hash list */

  /* Go get the requested block unless searching or prefetching. */
  if (dev != NO_DEV) {
#if ENABLE_CACHE2
	if (get_block2(bp, only_search)) /* in 2nd level cache */;
	else
#endif
	if (only_search == PREFETCH) bp->b_dev = NO_DEV;
	else
	if (only_search == NORMAL) rw_block(bp, READING);
  }
  return(bp);			/* return the newly acquired block */
}


/*===========================================================================*
 *				put_block				     *
 *===========================================================================*/
PUBLIC void put_block(bp, block_type)
register struct buf *bp;	/* pointer to the buffer to be released */
int block_type;			/* INODE_BLOCK, DIRECTORY_BLOCK, or whatever */
{
/* Return a block to the list of available blocks.   Depending on 'block_type'
 * it may be put on the front or rear of the LRU chain.  Blocks that are
 * expected to be needed again shortly (e.g., partially full data blocks)
 * go on the rear; blocks that are unlikely to be needed again shortly
 * (e.g., full data blocks) go on the front.  Blocks whose loss can hurt
 * the integrity of the file system (e.g., inode blocks) are written to
 * disk immediately if they are dirty.
 */

  if (bp == NIL_BUF) return;	/* it is easier to check here than in caller */

  bp->b_count--;		/* there is one use fewer now */
  if (bp->b_count != 0) return;	/* block is still in use */

  bufs_in_use--;		/* one fewer block buffers in use */

  /* Spec N.1: the first dirty block of a file system that is CLEAN on the
   * disk clears CLEAN there now, while it is the only dirty block of that
   * device and outside the LRU chain: getting block 1 for that then never
   * has to write a block of this device first. */
  if (bp->b_dirt == DIRTY && bp->b_blocknr != SUPER_BLOCK) {
	struct super_block *sp = rw_super(bp->b_dev);

	if (sp != NIL_SUPER && (sp->s_flags & SF_DCLEAN)) (void) clean_off(sp);
  }

  /* Put this block back on the LRU chain.  If the ONE_SHOT bit is set in
   * 'block_type', the block is not likely to be needed again shortly, so put
   * it on the front of the LRU chain where it will be the first one to be
   * taken when a free buffer is needed later.
   */
  if (block_type & ONE_SHOT) {
	/* Block probably won't be needed quickly. Put it on front of chain.
  	 * It will be the next block to be evicted from the cache.
  	 */
	bp->b_prev = NIL_BUF;
	bp->b_next = front;
	if (front == NIL_BUF)
		rear = bp;	/* LRU chain was empty */
	else
		front->b_prev = bp;
	front = bp;
  } else {
	/* Block probably will be needed quickly.  Put it on rear of chain.
  	 * It will not be evicted from the cache for a long time.
  	 */
	bp->b_prev = rear;
	bp->b_next = NIL_BUF;
	if (rear == NIL_BUF)
		front = bp;
	else
		rear->b_next = bp;
	rear = bp;
  }

  /* Some blocks are so important (e.g., inodes, indirect blocks) that they
   * should be written to the disk immediately to avoid messing up the file
   * system in the event of a crash.
   */
  if ((block_type & WRITE_IMMED) && bp->b_dirt==DIRTY && bp->b_dev != NO_DEV)
	rw_block(bp, WRITING);
}


/*===========================================================================*
 *				alloc_zone				     *
 *===========================================================================*/
PUBLIC zone_t alloc_zone(dev, z)
dev_t dev;			/* device where zone wanted */
zone_t z;			/* try to allocate new zone near this one */
{
/* Allocate a new zone on the indicated device and return its number. */

  int major, minor;
  bit_t b, bit;
  struct super_block *sp;

  /* Note that the routine alloc_bit() returns 1 for the lowest possible
   * zone, which corresponds to sp->s_firstdatazone.  To convert a value
   * between the bit number, 'b', used by alloc_bit() and the zone number, 'z',
   * stored in the inode, use the formula:
   *     z = b + sp->s_firstdatazone - 1
   * Alloc_bit() never returns 0, since this is used for NO_BIT (failure).
   */
  sp = get_super(dev);		/* find the super_block for this device */

  /* If z is 0, skip initial part of the map known to be fully in use. */
  if (z == sp->s_firstdatazone) {
	bit = sp->s_zsearch;
  } else {
	bit = (bit_t) z - (sp->s_firstdatazone - 1);
  }
  b = alloc_bit(sp, ZMAP, bit);
  if (b == NO_BIT) {
	err_code = ENOSPC;
	major = (int) (sp->s_dev >> MAJOR) & BYTE;
	minor = (int) (sp->s_dev >> MINOR) & BYTE;
	printf("No space on %sdevice %d/%d\n",
		sp->s_dev == ROOT_DEV ? "root " : "", major, minor);
	return(NO_ZONE);
  }
  if (z == sp->s_firstdatazone) sp->s_zsearch = b;	/* for next time */
  return(sp->s_firstdatazone - 1 + (zone_t) b);
}


/*===========================================================================*
 *				free_zone				     *
 *===========================================================================*/
PUBLIC void free_zone(dev, numb)
dev_t dev;				/* device where zone located */
zone_t numb;				/* zone to be returned */
{
/* Return a zone. */

  register struct super_block *sp;
  bit_t bit;

  /* Locate the appropriate super_block and return bit. */
  sp = get_super(dev);
  if (numb < sp->s_firstdatazone) return;
  if (numb >= sp->s_zones) {	/* a pipe's zone, in core only (K.6) */
	forget_zone(dev, numb, sp->s_log_zone_size);
	return;
  }
  bit = (bit_t) (numb - (sp->s_firstdatazone - 1));
  free_bit(sp, ZMAP, bit);
  if (bit < sp->s_zsearch) sp->s_zsearch = bit;
  forget_zone(dev, numb, sp->s_log_zone_size);	/* its data is not needed */
}


/*===========================================================================*
 *				forget_zone				     *
 *===========================================================================*/
PUBLIC void forget_zone(dev, z, scale)
dev_t dev;
zone_t z;
int scale;			/* log2 blocks per zone */
{
/* The contents of zone z are of no use any more (freed, or pipe data that
 * has been read): blocks of it in the cache need not go to the disk (T9:
 * pipes never write their data to the SD card; deleted files neither).
 */
  register struct buf *bp;
  block_t b = (block_t) z << scale, e = b + ((block_t) 1 << scale);

  if (z == NO_ZONE) return;
  for (; b < e; b++)
	for (bp = buf_hash[(int) b & HASH_MASK]; bp != NIL_BUF; bp = bp->b_hash)
		if (bp->b_blocknr == b && bp->b_dev == dev) {
			bp->b_dirt = CLEAN;
			break;
		}
}


/*===========================================================================*
 *				pipe_bufs				     *
 *===========================================================================*/
PUBLIC int pipe_bufs()
{
/* How many buffers hold pipe data that has no place on the disk (K.6)?
 * Write.c keeps it at most half of the cache, rahead() off it. */
  register int i, n;

  for (i = 0, n = 0; i < NR_BUFS; i++)
	if (BUF(i)->b_dirt == PIPE_DIRTY) n++;
  return(n);
}


/*===========================================================================*
 *				rw_block				     *
 *===========================================================================*/
PUBLIC void rw_block(bp, rw_flag)
register struct buf *bp;	/* buffer pointer */
int rw_flag;			/* READING or WRITING */
{
/* Read or write a disk block. This is the only routine in which actual disk
 * I/O is invoked. If an error occurs, a message is printed here, but the error
 * is not reported to the caller.  If the error occurred while purging a block
 * from the cache, it is not clear what the caller could do about it anyway.
 */

  int r, op;
  off_t pos;
  dev_t dev;

  if ( (dev = bp->b_dev) != NO_DEV) {
	/* R12.39: the byte position in 64 bits, the high half as HIGHPOS
	 * (dev_io's nonblock field): partitions past 4 GiB (spec O.3). */
	pos = (off_t) (bp->b_blocknr << BLOCK_SHIFT);
	op = (rw_flag == READING ? DEV_READ : DEV_WRITE);
	r = BLOCK_SIZE;
	if (rw_flag == WRITING) {
		if (bp->b_blocknr == SUPER_BLOCK) sb_written(bp);
		else r = before_write(dev);
	}
	if (r != ENXIO)
	   r = dev_io(op, (int) (bp->b_blocknr >> (32 - BLOCK_SHIFT)), dev,
				pos, BLOCK_SIZE, FS_PROC_NR, bp->b_data);
	if (r == ENXIO && rw_flag == WRITING) {
		/* N.6: the card is out.  The block stays dirty in the cache
		 * until the same card is back (the driver checks), so
		 * nothing is lost and nothing goes to another card. */
		return;
	}
	if (r != BLOCK_SIZE) {
	    if (r >= 0) r = END_OF_FILE;
	    if (r == ENXIO) r = EIO;	/* N.6: a read with the card out */
	    if (r != END_OF_FILE)
	      printf("Unrecoverable disk error on device %d/%d, block %ld\n",
			(dev>>MAJOR)&BYTE, (dev>>MINOR)&BYTE, bp->b_blocknr);
		bp->b_dev = NO_DEV;	/* invalidate block */

		/* Report read errors to interested parties. */
		if (rw_flag == READING) rdwt_err = r;
	}
  }

  bp->b_dirt = CLEAN;
}


/*===========================================================================*
 *				invalidate				     *
 *===========================================================================*/
PUBLIC void invalidate(device)
dev_t device;			/* device whose blocks are to be purged */
{
/* Remove all the blocks belonging to some device from the cache. */

  register struct buf *bp;
  int i;

  for (i = 0; i < NR_BUFS; i++)
	if ((bp = BUF(i))->b_dev == device) {
		bp->b_dev = NO_DEV;
		if (bp->b_dirt == PIPE_DIRTY) bp->b_dirt = CLEAN;  /* K.6 */
	}

#if ENABLE_CACHE2
  invalidate2(device);
#endif
}


/*==========================================================================*
 *				flushall				    *
 *==========================================================================*/
PUBLIC void flushall(dev)
dev_t dev;			/* device to flush */
{
/* Flush all dirty blocks for one device. */

  register struct buf *bp;
  static struct buf *dirty[NR_BUFS];	/* static so it isn't on stack */
  int ndirty, i;

  for (i = 0, ndirty = 0; i < NR_BUFS; i++)
	if ((bp = BUF(i))->b_dirt == DIRTY && bp->b_dev == dev)
		dirty[ndirty++] = bp;
  rw_scattered(dev, dirty, ndirty, WRITING);
}


/*===========================================================================*
 *				rw_scattered				     *
 *===========================================================================*/
PUBLIC void rw_scattered(dev, bufq, bufqsize, rw_flag)
dev_t dev;			/* major-minor device number */
struct buf **bufq;		/* pointer to array of buffers */
int bufqsize;			/* number of buffers */
int rw_flag;			/* READING or WRITING */
{
/* Read or write scattered data from a device. */

  register struct buf *bp;
  int gap;
  register int i;
  register struct iorequest_s *iop;
  static struct iorequest_s iovec[NR_IOREQS];  /* static so it isn't on stack */
  int j, r, out;

  /* (Shell) sort buffers on b_blocknr. */
  gap = 1;
  do
	gap = 3 * gap + 1;
  while (gap <= bufqsize);
  while (gap != 1) {
	gap /= 3;
	for (j = gap; j < bufqsize; j++) {
		for (i = j - gap;
		     i >= 0 && bufq[i]->b_blocknr > bufq[i + gap]->b_blocknr;
		     i -= gap) {
			bp = bufq[i];
			bufq[i] = bufq[i + gap];
			bufq[i + gap] = bp;
		}
	}
  }

  /* Spec N.1: CLEAN off on the disk before any of these blocks goes there.
   * N.6: with the card out, everything stays dirty. */
  out = FALSE;
  if (rw_flag == WRITING) {
	for (i = 0; i < bufqsize; i++)
		if (bufq[i]->b_blocknr == SUPER_BLOCK) sb_written(bufq[i]);
	out = (before_write(dev) == ENXIO);
  }

  /* Set up i/o vector and do i/o.  The result of dev_io is discarded because
   * all results are returned in the vector.  If dev_io fails completely, the
   * vector is unchanged and all results are taken as errors.
   */  
  while (bufqsize > 0 && !out) {
	for (j = 0, iop = iovec; j < NR_IOREQS && j < bufqsize; j++, iop++) {
		bp = bufq[j];
		iop->io_position = (off_t) (bp->b_blocknr << BLOCK_SHIFT);
		iop->io_high = bp->b_blocknr >> (32 - BLOCK_SHIFT); /* R12.39 */
		iop->io_buf = bp->b_data;
		iop->io_nbytes = BLOCK_SIZE;
		iop->io_request = rw_flag == WRITING ?
				  DEV_WRITE : DEV_READ | OPTIONAL_IO;
	}
	r = dev_io(SCATTERED_IO, 0, dev, (off_t) 0, j, FS_PROC_NR,
							(char *) iovec);
	if (r == ENXIO) out = TRUE;	/* N.6: the card is out */

	/* Harvest the results.  Leave read errors for rw_block() to complain. */
	for (i = 0, iop = iovec; i < j; i++, iop++) {
		bp = bufq[i];
		if (rw_flag == READING) {
		    if (iop->io_nbytes == 0)
			bp->b_dev = dev;	/* validate block */
		    put_block(bp, PARTIAL_DATA_BLOCK);
		} else {
		    /* N.6: the card went out at this request (ENXIO): it and
		     * the ones not tried after it stay dirty, not dropped. */
		    if (iop->io_nbytes == ENXIO) out = TRUE;
		    if (iop->io_nbytes == 0) {
			bp->b_dirt = CLEAN;
		    } else if (!out) {
		     printf("Unrecoverable write error on device %d/%d, block %ld\n",
				(dev>>MAJOR)&BYTE, (dev>>MINOR)&BYTE, bp->b_blocknr);
			bp->b_dev = NO_DEV;	/* invalidate block */
			bp->b_dirt = CLEAN;
		    }
		}
	}
	bufq += j;
	bufqsize -= j;
  }
}


/*===========================================================================*
 *				rm_lru					     *
 *===========================================================================*/
PRIVATE void rm_lru(bp)
struct buf *bp;
{
/* Remove a block from its LRU chain. */

  struct buf *next_ptr, *prev_ptr;

  bufs_in_use++;
  next_ptr = bp->b_next;	/* successor on LRU chain */
  prev_ptr = bp->b_prev;	/* predecessor on LRU chain */
  if (prev_ptr != NIL_BUF)
	prev_ptr->b_next = next_ptr;
  else
	front = next_ptr;	/* this block was at front of chain */

  if (next_ptr != NIL_BUF)
	next_ptr->b_prev = prev_ptr;
  else
	rear = prev_ptr;	/* this block was at rear of chain */
}


/*===========================================================================*
 *				rw_super				     *
 *===========================================================================*/
PRIVATE struct super_block *rw_super(dev)
dev_t dev;
{
/* The super block of a file system mounted read-write on 'dev', or none. */
  register struct super_block *sp;

  if (dev == NO_DEV) return(NIL_SUPER);
  for (sp = &super_block[0]; sp < &super_block[NR_SUPERS]; sp++)
	if (sp->s_dev == dev) return(sp->s_rd_only ? NIL_SUPER : sp);
  return(NIL_SUPER);
}


/*===========================================================================*
 *				sb_room					     *
 *===========================================================================*/
PRIVATE int sb_room(dev)
dev_t dev;
{
/* Spec N.1: can block 1 of 'dev' be had (get_block) without writing a block
 * of a device that is still CLEAN on the disk first?  It can if it is in
 * the cache or a clean buffer is free.  Else write one free dirty block of
 * a device that is not CLEAN on the disk (nothing to keep in order there).
 */
  register struct buf *bp;
  struct super_block *sp;

  for (bp = buf_hash[SUPER_BLOCK & HASH_MASK]; bp != NIL_BUF; bp = bp->b_hash)
	if (bp->b_blocknr == SUPER_BLOCK && bp->b_dev == dev) return(TRUE);
  for (bp = front; bp != NIL_BUF; bp = bp->b_next)
	if (bp->b_dirt == CLEAN) return(TRUE);
  for (bp = front; bp != NIL_BUF; bp = bp->b_next) {
	if (bp->b_dirt != DIRTY) continue;	/* pipe data: never (K.6) */
	sp = rw_super(bp->b_dev);
	if (sp != NIL_SUPER && (sp->s_flags & SF_DCLEAN)) continue;
	rw_block(bp, WRITING);
	if (bp->b_dirt == CLEAN) return(TRUE);
  }
  return(FALSE);
}


/*===========================================================================*
 *				clean_off				     *
 *===========================================================================*/
PRIVATE int clean_off(sp)
struct super_block *sp;		/* read-write, CLEAN on the disk */
{
/* Spec N.1: write CLEAN = 0 to the disk, synchronously.  OK, ENXIO (the card
 * is out: write nothing else either), or another error (go on: a card that
 * fails this write fails the others too).  With no room for block 1 (every
 * free buffer dirty with blocks of CLEAN file systems: put_block's hook
 * failed with the card out, and now it is back) go on too: losing a block
 * would be worse than the order.  CLEAN stays set in core, so the next
 * block retries.
 */
  if (!sb_room(sp->s_dev)) return(OK);
  return(put_sflags(sp, 0, SF_CLEAN));
}


/*===========================================================================*
 *				before_write				     *
 *===========================================================================*/
PRIVATE int before_write(dev)
dev_t dev;
{
/* A block other than the super block goes to 'dev' now (spec N.1, N.2). */
  struct super_block *sp;

  if ((sp = rw_super(dev)) == NIL_SUPER) return(OK);
  sp->s_flags |= SF_WRITTEN;
  if (!(sp->s_flags & SF_DCLEAN)) return(OK);
  return(clean_off(sp));
}


/*===========================================================================*
 *				sb_written				     *
 *===========================================================================*/
PUBLIC void sb_written(bp)
struct buf *bp;			/* block 1 of its device goes to the disk */
{
/* Block 1 written through the cache (fsck writing the device) carries the
 * flag bits: the in-core "CLEAN on the disk" follows them (spec N.1). */
  struct super_block *sp, *dsp;

  if ((sp = rw_super(bp->b_dev)) == NIL_SUPER) return;
  dsp = (struct super_block *) bp->b_data;
  if (conv2(sp->s_native, (int) dsp->s_flags) & SF_CLEAN)
	sp->s_flags |= SF_DCLEAN;
  else
	sp->s_flags &= ~SF_DCLEAN;
}
