/* Tables sizes */
#define V1_NR_DZONES       7	/* # direct zone numbers in a V1 inode */
#define V1_NR_TZONES       9	/* total # zone numbers in a V1 inode */
#define V2_NR_DZONES       7	/* # direct zone numbers in a V2 inode */
#define V2_NR_TZONES      10	/* total # zone numbers in a V2 inode */

#if (MACHINE == FLIPPER)	/* 16 processes, 3 file systems: small tables */
#define NR_FILPS          48	/* # slots in filp table */
#define NR_INODES         40	/* # slots in "in core" inode table */
#define NR_SUPERS          4	/* # slots in super block table */
#else
#define NR_FILPS         128	/* # slots in filp table */
#define NR_INODES         64	/* # slots in "in core" inode table */
#define NR_SUPERS          8	/* # slots in super block table */
#endif
#define NR_LOCKS           8	/* # slots in the file locking table */

/* The type of sizeof may be (unsigned) long.  Use the following macro for
 * taking the sizes of small objects so that there are no surprises like
 * (small) long constants being passed to routines expecting an int.
 */
#define usizeof(t) ((unsigned) sizeof(t))

/* File system types. */
#define SUPER_MAGIC   0x137F	/* magic number contained in super-block */
#define SUPER_REV     0x7F13	/* magic # when 68000 disk read on PC or vv */
#define SUPER_V2      0x2468	/* magic # for V2 file systems */
#define SUPER_V2_REV  0x6824	/* V2 magic written on PC, read on 68K or vv */

#define SUPER_V3      0x4D5A	/* magic # for V3 file systems (spec O.4) */

#define V1		   1	/* version number of V1 file systems */ 
#define V2		   2	/* version number of V2 file systems */ 
#define V3		   3	/* version number of V3 file systems (O.4) */

/* Directory entries (spec O.4): V1/V2 16 B (16-bit inode, 14 chars), V3
 * 64 B (32-bit inode, 60 chars).  The FS works on them as bytes. */
#define V2_DIRENT_SIZE    16
#define V3_DIRENT_SIZE    64
#define V2_DIRSIZ         14
#define V3_DIRSIZ         60

/* Path components in FS are up to the longest name of any format. */
#if NAME_MAX < V3_DIRSIZ
#undef NAME_MAX
#define NAME_MAX   V3_DIRSIZ
#endif

/* Miscellaneous constants */
#define SU_UID 	 ((uid_t) 0)	/* super_user's uid_t */
#define SYS_UID  ((uid_t) 0)	/* uid_t for processes MM and INIT */
#define SYS_GID  ((gid_t) 0)	/* gid_t for processes MM and INIT */
#define NORMAL	           0	/* forces get_block to do disk read */
#define NO_READ            1	/* prevents get_block from doing disk read */
#define PREFETCH           2	/* tells get_block not to read or mark dev */

#define XPIPE  (-NR_TASKS-1)	/* used in fp_task when susp'd on pipe */
#define XOPEN  (-NR_TASKS-2)	/* used in fp_task when susp'd on open */
#define XLOCK  (-NR_TASKS-3)	/* used in fp_task when susp'd on lock */
#define XPOPEN (-NR_TASKS-4)	/* used in fp_task when susp'd on pipe open */
#define XPOLL  (-NR_TASKS-5)	/* used in fp_task when susp'd in poll() */

#define NO_BIT   ((bit_t) 0)	/* returned by alloc_bit() to signal failure */

#define DUP_MASK        0100	/* mask to distinguish dup2 from dup */

#define LOOK_UP            0	/* tells search_dir to lookup string */
#define ENTER              1	/* tells search_dir to make dir entry */
#define DELETE             2	/* tells search_dir to delete entry */
#define IS_EMPTY           3	/* tells search_dir to ret. OK or ENOTEMPTY */  

#define CLEAN              0	/* disk and memory copies identical */
#define DIRTY              1	/* disk and memory copies differ */
#define PIPE_DIRTY         2	/* b_dirt: pipe data, in the cache only (K.6) */
#define ATIME            002	/* set if atime field needs updating */
#define CTIME            004	/* set if ctime field needs updating */
#define MTIME            010	/* set if mtime field needs updating */

#define BYTE_SWAP          0	/* tells conv2/conv4 to swap bytes */
#define DONT_SWAP          1	/* tells conv2/conv4 not to swap bytes */

#define END_OF_FILE   (-104)	/* eof detected */

#define ROOT_INODE         1	/* inode number for root directory */
#define BOOT_BLOCK  ((block_t) 0)	/* block number of boot block */
#define SUPER_BLOCK ((block_t) 1)	/* block number of super block */
#define BLOCK_SHIFT       10	/* log2(BLOCK_SIZE): 64-bit positions (R12.39) */

#define DIR_ENTRY_SIZE       usizeof (struct direct)  /* # bytes/dir entry   */
#define NR_DIR_ENTRIES   (BLOCK_SIZE/DIR_ENTRY_SIZE)  /* # dir entries/blk   */
#define SUPER_SIZE      usizeof (struct super_block)  /* super_block size    */
#define PIPE_SIZE          (V1_NR_DZONES*BLOCK_SIZE)  /* pipe size in bytes  */
#define BITMAP_CHUNKS (BLOCK_SIZE/usizeof (bitchunk_t))/* # map chunks/blk   */

/* Derived sizes pertaining to the V1 file system. */
#define V1_ZONE_NUM_SIZE           usizeof (zone1_t)  /* # bytes in V1 zone  */
#define V1_INODE_SIZE             usizeof (d1_inode)  /* bytes in V1 dsk ino */
#define V1_INDIRECTS   (BLOCK_SIZE/V1_ZONE_NUM_SIZE)  /* # zones/indir block */
#define V1_INODES_PER_BLOCK (BLOCK_SIZE/V1_INODE_SIZE)/* # V1 dsk inodes/blk */

/* Derived sizes pertaining to the V2 file system. */
#define V2_ZONE_NUM_SIZE            usizeof (zone_t)  /* # bytes in V2 zone  */
#define V2_INODE_SIZE             usizeof (d2_inode)  /* bytes in V2 dsk ino */
#define V2_INDIRECTS   (BLOCK_SIZE/V2_ZONE_NUM_SIZE)  /* # zones/indir block */
#define V2_INODES_PER_BLOCK (BLOCK_SIZE/V2_INODE_SIZE)/* # V2 dsk inodes/blk */
#define V2_MAX_FILE ((off_t) (V2_NR_DZONES + V2_INDIRECTS \
		+ (long) V2_INDIRECTS * V2_INDIRECTS) * BLOCK_SIZE)

#define printf printk
