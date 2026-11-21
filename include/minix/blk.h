/* Swappable memory blocks for apps (spec 12h, R12.34-R12.38).
 *
 * MEMINFO with m1_i2 = MI_BLK and m1_p1 pointing to a struct blkreq in the
 * caller's own memory.  MM reads the descriptor with sys_copy and keeps no
 * request state.  The reply is OK, a count (OPEN: blocks reserved) or a
 * negative error; the status field gets the same value.
 */
#ifndef _MINIX_BLK_H
#define _MINIX_BLK_H

#define BLK_SIZE	4096	/* bytes of data in a block */
#define BLK_SECT	512	/* a GET or PUT moves whole sectors */
#define BLK_NSECT	8	/* data sectors in a block */

/* Operations (op). */
#define BLK_GET		1	/* block id (or its sectors) to buf */
#define BLK_PUT		2	/* buf to block id (or its sectors) */
#define BLK_OPEN	3	/* reserve id bytes (R12.34); reply: blocks */
#define BLK_CLOSE	4	/* give the reservation back */
#define BLK_INFO	5	/* struct blkinfo to buf */
#define BLK_DISCARD	6	/* blocks id .. id + (long) buf - 1 hold nothing
				 * any more (R12.40; at idle) */

/* Flags. */
#define BLK_ANY		0x01	/* OPEN: the largest free run if smaller */
#define BLK_IFCHANGED	0x02	/* PUT: write nothing if every sector's CRC
				 * on the card matches */

struct blkreq {
  unsigned char op;		/* BLK_GET ... BLK_INFO */
  unsigned char flags;		/* BLK_ANY, BLK_IFCHANGED */
  unsigned char first;		/* GET/PUT: first data sector, 0..7 */
  unsigned char count;		/* GET/PUT: sectors, 1..8 (0: all 8) */
  unsigned long id;		/* GET/PUT: block number; OPEN: bytes */
  char *buf;			/* GET/PUT: the data; INFO: struct blkinfo */
  long status;			/* MM's answer, as the reply */
};

struct blkinfo {
  unsigned long blocks;		/* the caller's reservation, blocks */
  unsigned long largest;	/* the largest free run, blocks */
  unsigned long space;		/* the whole block space, blocks */
  unsigned long extent;		/* blocks per extent (the unit of OPEN) */
  unsigned long run;		/* the caller's first extent (free -v, tests) */
  unsigned short size;		/* BLK_SIZE */
  unsigned char mode;		/* SW_PLAIN, SW_CRYPT (12b) */
  unsigned char present;	/* the card is there */
};

#endif /* _MINIX_BLK_H */
