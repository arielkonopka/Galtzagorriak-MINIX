/* This file is the counterpart of "read.c".  It contains the code for writing
 * insofar as this is not contained in read_write().
 *
 * The entry points into this file are
 *   do_write:     call read_write to perform the WRITE system call
 *   clear_zone:   erase a zone in the middle of a file
 *   new_block:    acquire a new block
 */

#include "fs.h"
#include <string.h>
#include "buf.h"
#include "file.h"
#include "fproc.h"
#include "inode.h"
#include "super.h"

FORWARD _PROTOTYPE( int write_map, (struct inode *rip, off_t position,
			zone_t new_zone)				);

FORWARD _PROTOTYPE( void wr_indir, (struct buf *bp, int index, zone_t zone) );
FORWARD _PROTOTYPE( zone_t pipe_zone, (struct inode *rip, off_t position) );

/*===========================================================================*
 *				do_write				     *
 *===========================================================================*/
PUBLIC int do_write()
{
/* Perform the write(fd, buffer, nbytes) system call. */

  return(read_write(WRITING));
}


/*===========================================================================*
 *				write_map				     *
 *===========================================================================*/
PRIVATE int write_map(rip, position, new_zone)
register struct inode *rip;	/* pointer to inode to be changed */
off_t position;			/* file address to be mapped */
zone_t new_zone;		/* zone # to be inserted */
{
/* Write a new zone into an inode. */
  int scale, new_ind, zones, nr_indirects, slot, zindex, ex;
  zone_t z;
  register block_t b;
  long excess, zone, span;
  struct buf *bp;

  rip->i_dirt = DIRTY;		/* inode will be changed */
  scale = rip->i_sp->s_log_zone_size;		/* for zone-block conversion */
  zone = (position/BLOCK_SIZE) >> scale;	/* relative zone # to insert */
  zones = rip->i_ndzones;	/* # direct zones in the inode */
  nr_indirects = rip->i_nindirs;/* # indirect zones per indirect block */

  /* Is 'position' to be found in the inode itself? */
  if (zone < zones) {
	zindex = (int) zone;	/* we need an integer here */
	rip->i_zone[zindex] = new_zone;
	return(OK);
  }

  /* It is not in the inode: single, double or (V3, spec O.4) triple
   * indirect.  Walk down from the inode, making missing indirect blocks.
   */
  excess = zone - zones;	/* first Vx_NR_DZONES don't count */
  span = 1;			/* zones under one entry at this level */
  slot = zones;
  if (excess >= nr_indirects) {
	excess -= nr_indirects;
	span = nr_indirects;
	slot = zones + 1;
	if (excess >= (long) nr_indirects * nr_indirects) {
		excess -= (long) nr_indirects * nr_indirects;
		span = (long) nr_indirects * nr_indirects;
		slot = zones + 2;
		if (!IS_V3(rip->i_sp) || excess / span >= nr_indirects)
			return(EFBIG);
	}
  }
  new_ind = FALSE;
  if ( (z = rip->i_zone[slot]) == NO_ZONE) {
	if ( (z = alloc_zone(rip->i_dev, rip->i_zone[0])) == NO_ZONE)
		return(err_code);
	rip->i_zone[slot] = z;
	new_ind = TRUE;
  }
  for (;;) {
	b = (block_t) z << scale;
	bp = get_block(rip->i_dev, b, (new_ind ? NO_READ : NORMAL));
	if (new_ind) {
		zero_block(bp);
		bp->b_dirt = DIRTY;
	}
	ex = (int) (excess / span);	/* entry at this level */
	excess %= span;
	if (span == 1) {
		wr_indir(bp, ex, new_zone);
		bp->b_dirt = DIRTY;
		put_block(bp, INDIRECT_BLOCK);
		return(OK);
	}
	new_ind = FALSE;
	if ( (z = rd_indir(bp, ex)) == NO_ZONE) {
		if ( (z = alloc_zone(rip->i_dev, rip->i_zone[0])) == NO_ZONE) {
			put_block(bp, INDIRECT_BLOCK);
			return(err_code);
		}
		wr_indir(bp, ex, z);
		bp->b_dirt = DIRTY;
		new_ind = TRUE;
	}
	put_block(bp, INDIRECT_BLOCK);
	span /= nr_indirects;
  }
}


/*===========================================================================*
 *				wr_indir				     *
 *===========================================================================*/
PRIVATE void wr_indir(bp, index, zone)
struct buf *bp;			/* pointer to indirect block */
int index;			/* index into *bp */
zone_t zone;			/* zone to write */
{
/* Given a pointer to an indirect block, write one entry. */

  struct super_block *sp;

  sp = get_super(bp->b_dev);	/* need super block to find file sys type */

  /* write a zone into an indirect block */
  if (sp->s_version == V1)
	bp->b_v1_ind[index] = (zone1_t) conv2(sp->s_native, (int)  zone);
  else
	bp->b_v2_ind[index] = (zone_t)  conv4(sp->s_native, (long) zone);
}


/*===========================================================================*
 *				clear_zone				     *
 *===========================================================================*/
PUBLIC void clear_zone(rip, pos, flag)
register struct inode *rip;	/* inode to clear */
off_t pos;			/* points to block to clear */
int flag;			/* 0 if called by read_write, 1 by new_block */
{
/* Zero a zone, possibly starting in the middle.  The parameter 'pos' gives
 * a byte in the first block to be zeroed.  Clearzone() is called from 
 * read_write and new_block().
 */

  register struct buf *bp;
  register block_t b, blo, bhi;
  register off_t next;
  register int scale;
  register zone_t zone_size;

  /* If the block size and zone size are the same, clear_zone() not needed. */
  scale = rip->i_sp->s_log_zone_size;
  if (scale == 0) return;

  zone_size = (zone_t) BLOCK_SIZE << scale;
  if (flag == 1) pos = (pos/zone_size) * zone_size;
  next = pos + BLOCK_SIZE - 1;

  /* If 'pos' is in the last block of a zone, do not clear the zone. */
  if (next/zone_size != pos/zone_size) return;
  if ( (blo = read_map(rip, next)) == NO_BLOCK) return;
  bhi = (  ((blo>>scale)+1) << scale)   - 1;

  /* Clear all the blocks between 'blo' and 'bhi'. */
  for (b = blo; b <= bhi; b++) {
	bp = get_block(rip->i_dev, b, NO_READ);
	zero_block(bp);
	put_block(bp, FULL_DATA_BLOCK);
  }
}


/*===========================================================================*
 *				new_block				     *
 *===========================================================================*/
PUBLIC struct buf *new_block(rip, position)
register struct inode *rip;	/* pointer to inode */
off_t position;			/* file pointer */
{
/* Acquire a new block and return a pointer to it.  Doing so may require
 * allocating a complete zone, and then returning the initial block.
 * On the other hand, the current zone may still have some unused blocks.
 */

  register struct buf *bp;
  block_t b, base_block;
  zone_t z;
  zone_t zone_size;
  int scale, r;
  struct super_block *sp;

  /* Is another block available in the current zone? */
  if ( (b = read_map(rip, position)) == NO_BLOCK
			&& (z = pipe_zone(rip, position)) != NO_ZONE) {
	/* Spec K.6: pipe data in the cache only.  No bit map, the inode is
	 * not dirtied; the zone is past the end of the file system. */
	rip->i_zone[(int) (position / BLOCK_SIZE)] = z;
	b = (block_t) z;
  } else if (b == NO_BLOCK) {
	/* Choose first zone if possible. */
	/* Lose if the file is nonempty but the first zone number is NO_ZONE
	 * corresponding to a zone full of zeros.  It would be better to
	 * search near the last real zone.
	 */
	if (rip->i_zone[0] == NO_ZONE) {
		sp = rip->i_sp;
		z = sp->s_firstdatazone;
	} else {
		z = rip->i_zone[0];	/* hunt near first zone */
	}
	if ( (z = alloc_zone(rip->i_dev, z)) == NO_ZONE) return(NIL_BUF);
	if ( (r = write_map(rip, position, z)) != OK) {
		free_zone(rip->i_dev, z);
		err_code = r;
		return(NIL_BUF);
	}

	/* If we are not writing at EOF, clear the zone, just to be safe. */
	if ( position != rip->i_size) clear_zone(rip, position, 1);
	scale = rip->i_sp->s_log_zone_size;
	base_block = (block_t) z << scale;
	zone_size = (zone_t) BLOCK_SIZE << scale;
	b = base_block + (block_t)((position % zone_size)/BLOCK_SIZE);
  }

  bp = get_block(rip->i_dev, b, NO_READ);
  zero_block(bp);
  return(bp);
}


/*===========================================================================*
 *				pipe_zone				     *
 *===========================================================================*/
PRIVATE zone_t pipe_zone(rip, position)
register struct inode *rip;	/* a pipe or FIFO, or another file */
off_t position;			/* the new block's position */
{
/* Spec K.6: a pipe's (or FIFO's) new block needs no zone on the disk: its
 * data never goes there.  It gets a number past the device's last zone,
 * one per inode slot and direct zone, so no other file has it.  A block
 * of it in the cache is PIPE_DIRTY: never written, never evicted while
 * pipes keep at most half of the cache; beyond that (or with zones of
 * more than a block) a real zone is used, as before (T9).
 */
  struct super_block *sp;

  if (rip->i_pipe != I_PIPE) return(NO_ZONE);
  sp = rip->i_sp;
  if (sp->s_log_zone_size != 0 || position >= PIPE_SIZE) return(NO_ZONE);
  if (pipe_bufs() >= NR_BUFS / 2) return(NO_ZONE);
  return(sp->s_zones + (zone_t) (rip - &inode[0]) * V1_NR_DZONES
					+ (zone_t) (position / BLOCK_SIZE));
}


/*===========================================================================*
 *				zero_block				     *
 *===========================================================================*/
PUBLIC void zero_block(bp)
register struct buf *bp;	/* pointer to buffer to zero */
{
/* Zero a block. */

  memset(bp->b_data, 0, BLOCK_SIZE);
  bp->b_dirt = DIRTY;
}
