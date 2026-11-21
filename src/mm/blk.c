/* Swappable memory blocks for apps (spec 12h, R12.34-R12.38).
 *
 * An app that asks for it keeps working data in numbered 4 KB blocks on
 * the swap partition, behind the 16 swap slots (the "block space").  It
 * reads a block into a buffer of its own and writes it back from one; no
 * MMU, so nothing is mapped, MM only moves sectors between the card and
 * the caller's memory (straight through WINCHESTER as swap does, sw_io).
 *
 * R12.34: the block space is cut into 256 extents of equal size.  A
 * process reserves one contiguous run of them with OPEN (first fit,
 * rounded up to whole extents); its blocks 0 .. N-1 count from the start
 * of that run.  The only table: two bytes per user process slot (first
 * extent, number of extents; 0: none), 26 B for slots 3 .. 15 (MM, FS and
 * INIT never use blocks).  A second OPEN shrinks the run at once or grows
 * it in place if the extents after it are free; CLOSE, exec and exit give
 * it back.  A non-root process holds at most half of the block space.
 *
 * R12.35: nothing is written when a reservation ends.  Every block has a
 * header sector that names its owner (slot, pid, boot stamp, the
 * program's identity); a block whose header does not match the caller
 * reads as zeros.  The boot stamp changes at every boot and when MM's pid
 * counter wraps (blk_wrap), so a reused pid never sees old blocks.
 *
 * The entry points into this file are:
 *   blk_init:	 MM starts: the boot stamp
 *   blk_on:	 MMSWAPON: cut the partition into extents (0: blocks off)
 *   blk_busy:	 does any process hold a reservation (swap device change)
 *   blk_end:	 exit or exec: the reservation ends (fork: the child's slot
 *		 was cleared by the last exit in it)
 *   blk_wrap:	 the pid counter wrapped
 *   blk_req:	 MEMINFO MI_BLK
 */

#include "mm.h"
#include <minix/com.h>
#include <minix/blk.h>
#include <minix/partition.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string.h>
#include "mproc.h"

#if (CHIP == ARM_CM)

#define NEXT		256		/* extents (R12.34) */
#define BK_SECT		512
#define BK_BLOCK	(1 + BLK_NSECT)	/* sectors on the card: header + data */
#define BK_SKIP		(NR_PROCS * 384UL)	/* sectors of the 16 swap slots */
#define NSLOT		(NR_PROCS - INIT_PROC_NR - 1)	/* user slots */
#define MAXRUN		255		/* a byte holds the run's length */

/* All of MM's block state (R12.34, R12.35; spec budget MM +39 B). */
PRIVATE struct {
  unsigned long stamp;		/* boot stamp (R12.35) */
  unsigned short ext;		/* blocks per extent, 0: blocks off */
  unsigned char run[NSLOT][2];	/* first extent, number of extents */
} bk;

extern char mbuf[];		/* exec's; its last sector as in swap.c */

#define RUN(slot)	(bk.run[(slot) - INIT_PROC_NR - 1])

FORWARD _PROTOTYPE( unsigned bk_gap, (unsigned pos)			);
FORWARD _PROTOTYPE( unsigned bk_fit, (unsigned n, unsigned *len)	);
FORWARD _PROTOTYPE( int bk_open, (unsigned long bytes, int flags)	);


/*===========================================================================*
 *				blk_init				     *
 *===========================================================================*/
PUBLIC void blk_init()
{
/* R12.35: the boot stamp, the RTC's seconds mixed with the uptime ticks,
 * so two boots within the same second still differ. */
  message m;

  m.m_type = GET_TIME;
  if (sendrec(CLOCK, &m) == OK) bk.stamp = (unsigned long) m.NEW_TIME;
  m.m_type = GET_UPTIME;
  if (sendrec(CLOCK, &m) == OK) bk.stamp ^= (unsigned long) m.NEW_TIME << 20;
}


/*===========================================================================*
 *				blk_wrap				     *
 *===========================================================================*/
PUBLIC void blk_wrap()
{
/* next_pid went from 30000 back to 2: pids are used again (R12.35). */
  bk.stamp++;
}


/*===========================================================================*
 *				blk_on					     *
 *===========================================================================*/
PUBLIC int blk_on(minor)
int minor;			/* the swap partition, 0: blocks off */
{
/* MMSWAPON: the partition's size from WINCHESTER (DIOCGETPS, R12.39), the
 * extents behind the swap slots.  A partition too small for 256 blocks
 * after the slots has no block space.  The caller checked blk_busy(). */
  struct part_range *rg = (struct part_range *) (mbuf + ARG_MAX - BK_SECT);
  unsigned long n;
  message m;

  bk.ext = 0;
  if (minor == 0) return(OK);
  m.m_type = DEV_IOCTL;
  m.DEVICE = minor;
  m.PROC_NR = MM_PROC_NR;
  m.REQUEST = DIOCGETPS;
  m.ADDRESS = (char *) rg;
  if (sendrec(WINCHESTER, &m) != OK || m.REP_STATUS != OK) return(ENXIO);
  if (rg->count <= BK_SKIP) return(ENOSPC);
  n = (rg->count - BK_SKIP) / BK_BLOCK / NEXT;
  if (n == 0) return(ENOSPC);
  /* R12.39: a swap partition stays below 4 GiB, so this fits 16 bits. */
  bk.ext = n > 0xFFFF ? 0xFFFF : (unsigned short) n;
  return(OK);
}


/*===========================================================================*
 *				blk_busy				     *
 *===========================================================================*/
PUBLIC int blk_busy()
{
  int i;

  for (i = 0; i < NSLOT; i++) if (bk.run[i][1] != 0) return(TRUE);
  return(FALSE);
}


/*===========================================================================*
 *				blk_end					     *
 *===========================================================================*/
PUBLIC void blk_end(proc_nr)
int proc_nr;
{
/* Exit or exec (R12.35): the run is free again, nothing is written.  Who
 * reserves those extents next has another pid, slot or program, and reads
 * the old blocks as zeros.  (R12.40 adds the DISCARD at idle.) */
  if (proc_nr > INIT_PROC_NR && proc_nr < NR_PROCS) RUN(proc_nr)[1] = 0;
}


/*===========================================================================*
 *				bk_gap					     *
 *===========================================================================*/
PRIVATE unsigned bk_gap(pos)
unsigned pos;			/* an extent */
{
/* How many free extents start at pos (0: pos is taken). */
  unsigned end = NEXT, f, i;

  for (i = 0; i < NSLOT; i++) {
	if (bk.run[i][1] == 0) continue;
	f = bk.run[i][0];
	if (pos >= f && pos < f + bk.run[i][1]) return(0);
	if (f > pos && f < end) end = f;
  }
  return(end - pos);
}


/*===========================================================================*
 *				bk_fit					     *
 *===========================================================================*/
PRIVATE unsigned bk_fit(n, len)
unsigned n;			/* extents wanted */
unsigned *len;			/* the largest free run, extents */
{
/* First fit (R12.34): the lowest free run of at least n extents; NEXT if
 * there is none.  A free run starts at 0 or right after a taken one. */
  unsigned pos, g, best = NEXT, i;

  *len = 0;
  for (i = 0; i <= NSLOT; i++) {
	if (i == NSLOT) pos = 0;
	else if (bk.run[i][1] == 0) continue;
	else pos = bk.run[i][0] + bk.run[i][1];
	if (pos >= NEXT || (g = bk_gap(pos)) == 0) continue;
	if (g > *len) *len = g;
	if (g >= n && pos < best) best = pos;
  }
  return(best);
}


/*===========================================================================*
 *				bk_open					     *
 *===========================================================================*/
PRIVATE int bk_open(bytes, flags)
unsigned long bytes;
int flags;
{
/* OPEN (R12.34): reserve, grow or shrink the caller's run.  The reply is
 * the number of blocks it holds now. */
  unsigned char *r = RUN(who);
  unsigned long eb = (unsigned long) bk.ext * BLK_SIZE;	/* bytes/extent */
  unsigned long n, lim;
  unsigned pos, len, g;

  n = bytes / eb + (bytes % eb != 0);
  lim = mp->mp_effuid == SUPER_USER ? MAXRUN : NEXT / 2;
  if (n > lim) {
	if (!(flags & BLK_ANY)) return(ENOSPC);
	n = lim;
  }
  if (n == 0) {
	r[1] = 0;
	return(0);
  }
  if (r[1] != 0) {
	if (n > r[1]) {			/* grow in place, or not at all */
		pos = r[0] + r[1];
		g = pos < NEXT ? bk_gap(pos) : 0;
		if (r[1] + (unsigned long) g < n) {
			if (!(flags & BLK_ANY)) return(ENOSPC);
			n = r[1] + g;
		}
	}
	r[1] = (unsigned char) n;	/* (R12.40: the tail is discarded) */
	return((int) (n * bk.ext));
  }
  pos = bk_fit((unsigned) n, &len);
  if (pos == NEXT) {
	if (!(flags & BLK_ANY) || len == 0) return(ENOSPC);
	n = len;
	pos = bk_fit((unsigned) n, &len);
  }
  r[0] = (unsigned char) pos;
  r[1] = (unsigned char) n;
  return((int) (n * bk.ext));
}


/*===========================================================================*
 *				blk_req					     *
 *===========================================================================*/
PUBLIC int blk_req()
{
/* MEMINFO MI_BLK (R12.36): the descriptor from the caller's memory. */
  struct blkreq rq;
  struct blkinfo bi;
  unsigned len;
  int r;

  if (bk.ext == 0) return(ENOSYS);		/* blocks off, no partition */
  if (who <= INIT_PROC_NR) return(EPERM);	/* servers: never */
  if (sys_copy(who, D, (phys_bytes) mm_in.m1_p1, MM_PROC_NR, D,
		(phys_bytes) &rq, (phys_bytes) sizeof(rq)) != OK)
	return(EFAULT);
  if (sw_state() & 0x80) r = ENXIO;		/* card gone (N.6) */
  else switch (rq.op) {
  case BLK_OPEN:
	r = bk_open(rq.id, rq.flags);
	break;
  case BLK_CLOSE:
	RUN(who)[1] = 0;
	r = OK;
	break;
  case BLK_INFO:
	memset((char *) &bi, 0, sizeof(bi));
	bi.blocks = (unsigned long) RUN(who)[1] * bk.ext;
	(void) bk_fit(NEXT, &len);
	bi.largest = (unsigned long) len * bk.ext;
	bi.space = (unsigned long) NEXT * bk.ext;
	bi.extent = bk.ext;
	bi.size = BLK_SIZE;
	bi.mode = SW_PLAIN;
	bi.present = 1;
	r = sys_copy(MM_PROC_NR, D, (phys_bytes) &bi, who, D,
			(phys_bytes) rq.buf, (phys_bytes) sizeof(bi));
	break;
  default:
	r = EINVAL;
  }
  rq.status = r;
  (void) sys_copy(MM_PROC_NR, D, (phys_bytes) &rq.status, who, D,
	(phys_bytes) &((struct blkreq *) mm_in.m1_p1)->status,
	(phys_bytes) sizeof(rq.status));
  return(r);
}

#endif /* CHIP == ARM_CM */
