/* Super block table.  The root file system and every mounted file system
 * has an entry here.  The entry holds information about the sizes of the bit
 * maps and inodes.  The s_ninodes field gives the number of inodes available
 * for files and directories, including the root directory.  Inode 0 is 
 * on the disk, but not used.  Thus s_ninodes = 4 means that 5 bits will be
 * used in the bit map, bit 0, which is always 1 and not used, and bits 1-4
 * for files and directories.  The disk layout is:
 *
 *      Item        # blocks
 *    boot block      1
 *    super block     1
 *    inode map     s_imap_blocks
 *    zone map      s_zmap_blocks
 *    inodes        (s_ninodes + 'inodes per block' - 1)/'inodes per block'
 *    unused        whatever is needed to fill out the current zone
 *    data zones    (s_zones - s_firstdatazone) << s_log_zone_size
 *
 * A super_block slot is free if s_dev == NO_DEV. 
 */


EXTERN struct super_block {
  ino_t s_ninodes;		/* # usable inodes on the minor device */
  zone1_t  s_nzones;		/* total device size, including bit maps etc */
  short s_imap_blocks;		/* # of blocks used by inode bit map */
  short s_zmap_blocks;		/* # of blocks used by zone bit map */
  zone1_t s_firstdatazone;	/* number of first data zone */
  short s_log_zone_size;	/* log2 of blocks/zone */
  off_t s_max_size;		/* maximum file size on this device */
  short s_magic;		/* magic number to recognize super-blocks */
  unsigned short s_flags;	/* ex s_pad: SF_* below (spec N.1) */
  zone_t s_zones;		/* number of zones (replaces s_nzones in V2) */

  /* The following items are only used when the super_block is in memory. */
  struct inode *s_isup;		/* inode for root dir of mounted file sys */
  struct inode *s_imount;	/* inode mounted on */
  unsigned s_inodes_per_block;	/* precalculated from magic number */
  dev_t s_dev;			/* whose super block is this? */
  int s_rd_only;		/* set to 1 iff file sys mounted read only */
  int s_native;			/* set to 1 iff not byte swapped file system */
  int s_version;		/* file system version, zero means bad magic */
  int s_ndzones;		/* # direct zones in an inode */
  int s_nindirs;		/* # indirect zones per indirect block */
  bit_t s_isearch;		/* inodes below this bit number are in use */
  bit_t s_zsearch;		/* all zones below this bit number are in use*/
} super_block[NR_SUPERS];

#define NIL_SUPER (struct super_block *) 0

/* Spec O.4: directory entry size, name offset and name length per file
 * system, and where the N.1 flag bits live in block 1 (V1/V2 s_pad at 18;
 * V3 at 14, the pad MINIX 3.3 uses for its s_flags; Linux ignores both). */
#define IS_V3(sp)	((sp)->s_version == V3)
#define DIR_ESIZE(sp)	(IS_V3(sp) ? V3_DIRENT_SIZE : V2_DIRENT_SIZE)
#define DIR_NOFF(sp)	(IS_V3(sp) ? 4 : 2)
#define DIR_NLEN(sp)	(IS_V3(sp) ? V3_DIRSIZ : V2_DIRSIZ)
#define SB_FLAGS(sp, bp) \
	(*(unsigned short *) &(bp)->b_data[IS_V3(sp) ? 14 : 18])

/* s_flags (spec N.1).  On disk only bits 0-1, the values of Linux's minix
 * driver (MINIX_VALID_FS, MINIX_ERROR_FS) and MINIX 3's MFSFLAG_CLEAN.  The
 * FS keeps no copy of them: it changes them by read-modify-write of block 1
 * (put_sflags).  In core only bits 14-15, never written to the disk. */
#define SF_CLEAN	0x0001	/* disk: nothing written since consistent */
#define SF_CHECK	0x0002	/* disk: found unclean, not checked since */
#define SF_DISK		0x00FF	/* the bits that live on the disk */
#define SF_DCLEAN	0x4000	/* core: CLEAN is set on the disk */
#define SF_WRITTEN	0x8000	/* core: written since the last sync */
#define IMAP		0	/* operating on the inode bit map */
#define ZMAP		1	/* operating on the zone bit map */
