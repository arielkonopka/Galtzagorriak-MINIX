/* This file manages the super block table and the related data structures,
 * namely, the bit maps that keep track of which zones and which inodes are
 * allocated and which are free.  When a new inode or zone is needed, the
 * appropriate bit map is searched for a free entry.
 *
 * The entry points into this file are
 *   alloc_bit:       somebody wants to allocate a zone or inode; find one
 *   free_bit:        indicate that a zone or inode is available for allocation
 *   get_super:       search the 'superblock' table for a device
 *   mounted:         tells if file inode is on mounted (or ROOT) file system
 *   read_super:      read a superblock
 *   put_sflags:      change the CLEAN/CHECK bits on the disk (spec N.1)
 *   sf_mounted:      a file system was mounted read-write (N.1)
 *   sf_clean:        mark a file system clean when idle or at shutdown (N.2)
 */

#include "fs.h"
#include <string.h>
#include <minix/boot.h>
#include <minix/com.h>
#include "buf.h"
#include "inode.h"
#include "super.h"

#define BITCHUNK_BITS	(usizeof(bitchunk_t) * CHAR_BIT)
#define BITS_PER_BLOCK	(BITMAP_CHUNKS * BITCHUNK_BITS)

/*===========================================================================*
 *				alloc_bit				     *
 *===========================================================================*/
PUBLIC bit_t alloc_bit(sp, map, origin)
struct super_block *sp;		/* the filesystem to allocate from */
int map;			/* IMAP (inode map) or ZMAP (zone map) */
bit_t origin;			/* number of bit to start searching at */
{
/* Allocate a bit from a bit map and return its bit number. */

  block_t start_block;		/* first bit block */
  bit_t map_bits;		/* how many bits are there in the bit map? */
  unsigned bit_blocks;		/* how many blocks are there in the bit map? */
  unsigned block, word, bcount, wcount;
  struct buf *bp;
  bitchunk_t *wptr, *wlim, k;
  bit_t i, b;

  if (sp->s_rd_only)
	panic("can't allocate bit on read-only filesys.", NO_NUM);

  if (map == IMAP) {
	start_block = SUPER_BLOCK + 1;
	map_bits = sp->s_ninodes + 1;
	bit_blocks = sp->s_imap_blocks;
  } else {
	start_block = SUPER_BLOCK + 1 + sp->s_imap_blocks;
	map_bits = sp->s_zones - (sp->s_firstdatazone - 1);
	bit_blocks = sp->s_zmap_blocks;
  }

  /* Figure out where to start the bit search (depends on 'origin'). */
  if (origin >= map_bits) origin = 0;	/* for robustness */

  /* Locate the starting place. */
  block = origin / BITS_PER_BLOCK;
  word = (origin % BITS_PER_BLOCK) / BITCHUNK_BITS;

  /* Iterate over all blocks plus one, because we start in the middle. */
  bcount = bit_blocks + 1;
  do {
	bp = get_block(sp->s_dev, start_block + block, NORMAL);
	wlim = &bp->b_bitmap[BITMAP_CHUNKS];

	/* Iterate over the words in block. */
	for (wptr = &bp->b_bitmap[word]; wptr < wlim; wptr++) {

		/* Does this word contain a free bit? */
		if (*wptr == (bitchunk_t) ~0) continue;

		/* Find and allocate the free bit. */
		k = conv2(sp->s_native, (int) *wptr);
		for (i = 0; (k & (1 << i)) != 0; ++i) {}

		/* Bit number from the start of the bit map. */
		b = ((bit_t) block * BITS_PER_BLOCK)
		    + (wptr - &bp->b_bitmap[0]) * BITCHUNK_BITS
		    + i;

		/* Don't allocate bits beyond the end of the map. */
		if (b >= map_bits) break;

		/* Allocate and return bit number. */
		k |= 1 << i;
		*wptr = conv2(sp->s_native, (int) k);
		bp->b_dirt = DIRTY;
		put_block(bp, MAP_BLOCK);
		return(b);
	}
	put_block(bp, MAP_BLOCK);
	if (++block >= bit_blocks) block = 0;	/* last block, wrap around */
	word = 0;
  } while (--bcount > 0);
  return(NO_BIT);		/* no bit could be allocated */
}


/*===========================================================================*
 *				free_bit				     *
 *===========================================================================*/
PUBLIC void free_bit(sp, map, bit_returned)
struct super_block *sp;		/* the filesystem to operate on */
int map;			/* IMAP (inode map) or ZMAP (zone map) */
bit_t bit_returned;		/* number of bit to insert into the map */
{
/* Return a zone or inode by turning off its bitmap bit. */

  unsigned block, word, bit;
  struct buf *bp;
  bitchunk_t k, mask;
  block_t start_block;

  if (sp->s_rd_only)
	panic("can't free bit on read-only filesys.", NO_NUM);

  if (map == IMAP) {
	start_block = SUPER_BLOCK + 1;
	if (bit_returned / BITS_PER_BLOCK >= sp->s_imap_blocks) return;
  } else {
	start_block = SUPER_BLOCK + 1 + sp->s_imap_blocks;
	if (bit_returned / BITS_PER_BLOCK >= sp->s_zmap_blocks) return;
  }
  block = bit_returned / BITS_PER_BLOCK;
  word = (bit_returned % BITS_PER_BLOCK) / BITCHUNK_BITS;
  bit = bit_returned % BITCHUNK_BITS;
  mask = 1 << bit;

  bp = get_block(sp->s_dev, start_block + block, NORMAL);

  k = conv2(sp->s_native, (int) bp->b_bitmap[word]);
  if (!(k & mask)) {
	/* A damaged file system (two files sharing a zone, a stale map):
	 * this halted the whole system (security review 2026-11-16, F2).
	 * Leave the map as it is and say so; fsck repairs it. */
	printf("FS: freeing unused %s %u on device %d/%d: run fsck\n",
		map == IMAP ? "inode" : "zone bit", (unsigned) bit_returned,
		(sp->s_dev >> MAJOR) & BYTE, (sp->s_dev >> MINOR) & BYTE);
	put_block(bp, MAP_BLOCK);
	return;
  }

  k &= ~mask;
  bp->b_bitmap[word] = conv2(sp->s_native, (int) k);
  bp->b_dirt = DIRTY;

  put_block(bp, MAP_BLOCK);
}


/*===========================================================================*
 *				get_super				     *
 *===========================================================================*/
PUBLIC struct super_block *get_super(dev)
dev_t dev;			/* device number whose super_block is sought */
{
/* Search the superblock table for this device.  It is supposed to be there. */

  register struct super_block *sp;

  for (sp = &super_block[0]; sp < &super_block[NR_SUPERS]; sp++)
	if (sp->s_dev == dev) return(sp);

  /* Search failed.  Something wrong. */
  panic("can't find superblock for device (in decimal)", (int) dev);

  return(NIL_SUPER);		/* to keep the compiler and lint quiet */
}


/*===========================================================================*
 *				mounted					     *
 *===========================================================================*/
PUBLIC int mounted(rip)
register struct inode *rip;	/* pointer to inode */
{
/* Report on whether the given inode is on a mounted (or ROOT) file system. */

  register struct super_block *sp;
  register dev_t dev;

  dev = (dev_t) rip->i_zone[0];
  if (dev == ROOT_DEV) return(TRUE);	/* inode is on root file system */

  for (sp = &super_block[0]; sp < &super_block[NR_SUPERS]; sp++)
	if (sp->s_dev == dev) return(TRUE);

  return(FALSE);
}


/*===========================================================================*
 *				read_super				     *
 *===========================================================================*/
PUBLIC int read_super(sp)
register struct super_block *sp; /* pointer to a superblock */
{
/* Read a superblock. */

  register struct buf *bp;
  dev_t dev;
  int magic;
  int version, native;
  u16_t *w;
  u32_t *l;

  dev = sp->s_dev;		/* save device (will be overwritten by copy) */
  bp = get_block(sp->s_dev, SUPER_BLOCK, NORMAL);
  w = (u16_t *) bp->b_data;
  l = (u32_t *) bp->b_data;
  /* The V1/V2 layout, field by field: the in-core s_ninodes is 32-bit
   * (spec O.4), so the block is no longer copied as it is. */
  sp->s_ninodes = (ino_t) w[0];
  sp->s_nzones = w[1];
  sp->s_imap_blocks = w[2];
  sp->s_zmap_blocks = w[3];
  sp->s_firstdatazone = w[4];
  sp->s_log_zone_size = w[5];
  sp->s_max_size = (off_t) l[3];
  sp->s_magic = w[8];
  sp->s_flags = w[9];
  sp->s_zones = (zone_t) l[5];
  sp->s_dev = NO_DEV;		/* restore later */
  magic = sp->s_magic;		/* determines file system type */

  /* Get file system version and type. */
  if (magic == SUPER_MAGIC || magic == conv2(BYTE_SWAP, SUPER_MAGIC)) {
	version = V1;
	native  = (magic == SUPER_MAGIC);
  } else if (magic == SUPER_V2 || magic == conv2(BYTE_SWAP, SUPER_V2)) {
	version = V2;
	native  = (magic == SUPER_V2);
  } else if (w[12] == SUPER_V3) {
	/* Spec O.4: MINIX V3 as util-linux mkfs.minix -3 writes it (native
	 * byte order only).  Its own layout: 32-bit inode count, 16-bit
	 * fields moved up by 2, the magic at 24, the block size at 28. */
	if (w[14] != BLOCK_SIZE || w[6] != 0) {
		printf("FS: V3 file system with %u-byte blocks, zone shift %u: "
			"only 1024 and 0 work, not mounted\n",
			(unsigned) w[14], (unsigned) w[6]);
		put_block(bp, ZUPER_BLOCK);
		return(EINVAL);
	}
	sp->s_ninodes = (ino_t) l[0];
	sp->s_nzones = 0;
	sp->s_imap_blocks = w[3];
	sp->s_zmap_blocks = w[4];
	sp->s_firstdatazone = w[5];
	sp->s_log_zone_size = 0;
	sp->s_max_size = (off_t) l[4];
	sp->s_zones = (zone_t) l[5];
	sp->s_flags = (w[7] & SF_CLEAN) ? SF_DCLEAN : 0;
	version = V3;
	native = 1;
  } else {
	put_block(bp, ZUPER_BLOCK);
	return(EINVAL);
  }
  put_block(bp, ZUPER_BLOCK);

  if (version != V3) {
  /* If the super block has the wrong byte order, swap the fields; the magic
   * number doesn't need conversion. */
  sp->s_ninodes =       conv2(native, (int) sp->s_ninodes);
  sp->s_nzones =        conv2(native, (int) sp->s_nzones);
  sp->s_imap_blocks =   conv2(native, (int) sp->s_imap_blocks);
  sp->s_zmap_blocks =   conv2(native, (int) sp->s_zmap_blocks);
  sp->s_firstdatazone = conv2(native, (int) sp->s_firstdatazone);
  sp->s_log_zone_size = conv2(native, (int) sp->s_log_zone_size);
  sp->s_max_size =      conv4(native, sp->s_max_size);
  sp->s_zones =         conv4(native, sp->s_zones);
  /* N.1: in core only whether CLEAN is on the disk; never the disk bits. */
  sp->s_flags = (conv2(native, (int) sp->s_flags) & SF_CLEAN) ? SF_DCLEAN : 0;
  }

  /* In V1, the device size was kept in a short, s_nzones, which limited
   * devices to 32K zones.  For V2, it was decided to keep the size as a
   * long.  However, just changing s_nzones to a long would not work, since
   * then the position of s_magic in the super block would not be the same
   * in V1 and V2 file systems, and there would be no way to tell whether
   * a newly mounted file system was V1 or V2.  The solution was to introduce
   * a new variable, s_zones, and copy the size there.
   *
   * Calculate some other numbers that depend on the version here too, to
   * hide some of the differences.
   */
  if (version == V1) {
	sp->s_zones = sp->s_nzones;	/* only V1 needs this copy */
	sp->s_inodes_per_block = V1_INODES_PER_BLOCK;
	sp->s_ndzones = V1_NR_DZONES;
	sp->s_nindirs = V1_INDIRECTS;
  } else {
	sp->s_inodes_per_block = V2_INODES_PER_BLOCK;	/* V3: same inode */
	sp->s_ndzones = V2_NR_DZONES;
	sp->s_nindirs = V2_INDIRECTS;
  }
  if (sp->s_max_size < 0)		/* V3: a u32; files stay below 2 GiB */
	sp->s_max_size = 0x7FFFFFFFL;

  sp->s_isearch = 0;		/* inode searches initially start at 0 */
  sp->s_zsearch = 0;		/* zone searches initially start at 0 */
  sp->s_version = version;
  sp->s_native  = native;

  /* Make a few basic checks to see if super block looks reasonable. */
  if (sp->s_imap_blocks < 1 || sp->s_zmap_blocks < 1
				|| sp->s_ninodes < 1 || sp->s_zones < 1
				|| (unsigned) sp->s_log_zone_size > 4) {
	return(EINVAL);
  }
  /* The layout must add up (security review 2026-11-16, F3): bit maps big
   * enough for every inode and zone (else freeing a bit cleared one in
   * the inode table), the data zones after the inodes and inside the
   * device's zone count.  A damaged card is refused, not mounted. */
  if ((unsigned long) sp->s_imap_blocks * BITS_PER_BLOCK
					< (unsigned long) sp->s_ninodes + 1
	|| sp->s_firstdatazone < 1
	|| (unsigned long) sp->s_firstdatazone >= (unsigned long) sp->s_zones
	|| (unsigned long) sp->s_zmap_blocks * BITS_PER_BLOCK
		< (unsigned long) sp->s_zones - (sp->s_firstdatazone - 1)
	|| ((unsigned long) sp->s_firstdatazone << sp->s_log_zone_size)
		< (unsigned long) SUPER_BLOCK + 1 + sp->s_imap_blocks
			+ sp->s_zmap_blocks + (sp->s_ninodes
			+ sp->s_inodes_per_block - 1) / sp->s_inodes_per_block) {
	printf("FS: inconsistent super block, not mounted\n");
	return(EINVAL);
  }
  sp->s_dev = dev;		/* restore device number */
  return(OK);
}


/*===========================================================================*
 *				put_sflags				     *
 *===========================================================================*/
PUBLIC int put_sflags(sp, set, clr)
register struct super_block *sp; /* a mounted file system */
int set, clr;			/* SF_CLEAN / SF_CHECK bits to set, clear */
{
/* Spec N.1: change the flag bits of the super block on the disk by read-
 * modify-write of block 1 through the cache, written through at once (one
 * 1 KB write).  On success the in-core SF_DCLEAN follows the disk.  A failed
 * write never leaves CLEAN = 1 in the cache (it could reach the disk after
 * other blocks later); CLEAN = 0 or CHECK = 1 stay dirty, the safe way.
 */
  register struct buf *bp;
  unsigned old, new;
  int r;

  bp = get_block(sp->s_dev, SUPER_BLOCK, NORMAL);
  if (bp->b_dev != sp->s_dev) {		/* the read failed */
	put_block(bp, ZUPER_BLOCK);
	return(EIO);
  }
  old = (unsigned) conv2(sp->s_native, (int) SB_FLAGS(sp, bp)) & 0xFFFF;
  new = ((old | set) & ~clr) & SF_DISK;
  SB_FLAGS(sp, bp) = conv2(sp->s_native, (int) new);
  r = dev_io(DEV_WRITE, FALSE, sp->s_dev, (off_t) SUPER_BLOCK * BLOCK_SIZE,
					BLOCK_SIZE, FS_PROC_NR, bp->b_data);
  if (r == BLOCK_SIZE) {
	bp->b_dirt = CLEAN;
	if (new & SF_CLEAN) sp->s_flags |= SF_DCLEAN;
	else sp->s_flags &= ~SF_DCLEAN;
	r = OK;
  } else {
	if ((new & SF_CLEAN) && !(old & SF_CLEAN))
		SB_FLAGS(sp, bp) = conv2(sp->s_native, (int) old);
	else
		bp->b_dirt = DIRTY;
	if (r >= 0) r = EIO;
  }
  put_block(bp, ZUPER_BLOCK & ~WRITE_IMMED);
  return(r);
}


/*===========================================================================*
 *				sf_mounted				     *
 *===========================================================================*/
PUBLIC void sf_mounted(sp)
register struct super_block *sp; /* just mounted read-write */
{
/* Spec N.1: a file system that was not left clean gets CHECK = 1 on the
 * disk at once, so that a later boot checks it even if it is clean by then
 * (one 1 KB write, only after an unclean end).
 */
  if (sp->s_rd_only || (sp->s_flags & SF_DCLEAN)) return;
  (void) put_sflags(sp, SF_CHECK, 0);
}


/*===========================================================================*
 *				sf_clean				     *
 *===========================================================================*/
PUBLIC void sf_clean(sp, force)
register struct super_block *sp; /* a mounted file system, after a sync */
int force;			/* shutdown or unmount: no idle period */
{
/* Spec N.2: called after a sync.  A read-write file system that is not
 * CLEAN on the disk becomes CLEAN when nothing was written to it since the
 * previous sync (unless 'force'), none of its buffers or in-core inodes is
 * dirty (a write that failed, e.g. the card out, keeps it unclean) and no
 * file of it is open after its last link went (unless 'force': every
 * process has gone).  Unnamed pipes never reach the disk (T9).
 */
  register struct buf *bp;
  register struct inode *rip;
  dev_t dev = sp->s_dev;
  int i;

  if (dev == NO_DEV || sp->s_rd_only || (sp->s_flags & SF_DCLEAN)) return;
  if (sp->s_flags & SF_WRITTEN) {
	sp->s_flags &= ~SF_WRITTEN;
	if (!force) return;
  }
  for (i = 0; i < NR_BUFS; i++)
	if ((bp = BUF(i))->b_dev == dev && bp->b_dirt == DIRTY) return;
  for (rip = &inode[0]; rip < &inode[NR_INODES]; rip++) {
	if (rip->i_count == 0 || rip->i_dev != dev) continue;
	if (rip->i_pipe == I_PIPE && (rip->i_nlinks & BYTE) == 0) continue;
	if (rip->i_dirt == DIRTY) return;
	if (!force && (rip->i_nlinks & BYTE) == 0) return;
  }
  (void) put_sflags(sp, SF_CLEAN, 0);
}
