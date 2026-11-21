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

/* The header sector on the card (R12.35).  It goes through the last
 * sector of exec's mbuf, as the swap header does (R12.26): 0 B. */
extern char mbuf[];
#define HDR	((struct bkhdr *) (mbuf + ARG_MAX - BK_SECT))
#define BK_MAGIC	0x4B425A47UL	/* "GZBK" */

struct bkhdr {
  unsigned long magic, slot, pid, stamp;	/* whose block it is */
  unsigned long dev, ino, ctime;	/* the program's identity */
  unsigned long id;			/* the app's block number */
  unsigned long live;			/* data sectors written (bit i) */
  unsigned long count;			/* write counter (12b) */
  unsigned long tag[4];			/* 12b: GCM */
  unsigned long crc[BLK_NSECT];		/* CRC-32 of each data sector */
};

#define RUN(slot)	(bk.run[(slot) - INIT_PROC_NR - 1])

FORWARD _PROTOTYPE( unsigned bk_gap, (unsigned pos)			);
FORWARD _PROTOTYPE( unsigned bk_fit, (unsigned n, unsigned *len)	);
FORWARD _PROTOTYPE( int bk_open, (unsigned long bytes, int flags)	);
FORWARD _PROTOTYPE( int bk_valid, (struct bkhdr *h, unsigned long id)	);
FORWARD _PROTOTYPE( phys_bytes bk_buf, (vir_bytes v, vir_bytes n)	);
FORWARD _PROTOTYPE( int bk_io, (struct blkreq *rq, int bg)		);
FORWARD _PROTOTYPE( int bk_do, (char *desc, int bg)			);


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
 *				bk_valid				     *
 *===========================================================================*/
PRIVATE int bk_valid(h, id)
struct bkhdr *h;
unsigned long id;
{
/* R12.35: is the header in HDR the caller's block id?  Anything else is
 * someone's leftover and reads as zeros. */
  return(h->magic == BK_MAGIC && h->slot == (unsigned long) who
	&& h->pid == (unsigned long) mp->mp_pid && h->stamp == bk.stamp
	&& h->dev == (unsigned long) mp->mp_dev
	&& h->ino == (unsigned long) mp->mp_ino
	&& h->ctime == (unsigned long) mp->mp_ctime && h->id == id);
}


/*===========================================================================*
 *				bk_buf					     *
 *===========================================================================*/
PRIVATE phys_bytes bk_buf(v, n)
vir_bytes v, n;			/* the caller's buffer, bytes */
{
/* R12.36: the buffer wholly inside the caller's data or stack segment
 * (WINCHESTER checks again with numap).  Its physical address, 0: no. */
  struct mem_map *s;
  vir_bytes lo, hi;

  for (s = &mp->mp_seg[D]; s <= &mp->mp_seg[S]; s++) {
	lo = (vir_bytes) s->mem_vir << CLICK_SHIFT;
	hi = lo + ((vir_bytes) s->mem_len << CLICK_SHIFT);
	if (v >= lo && v < hi && n <= hi - v)
		return(((phys_bytes) s->mem_phys << CLICK_SHIFT) + (v - lo));
  }
  return(0);
}


/*===========================================================================*
 *				bk_io					     *
 *===========================================================================*/
PRIVATE int bk_io(rq, bg)
struct blkreq *rq;
int bg;				/* a background caller: the write budget */
{
/* GET or PUT of block rq->id, its sectors first .. first + count - 1,
 * straight between the card and the caller's buffer (R12.36).  The block
 * is the caller's only if its header says so (R12.35); a data sector that
 * was never written reads as zeros (h->live), so a PUT of a sector range
 * needs no write of the rest. */
  struct bkhdr *h = HDR;
  unsigned char *r = RUN(who);
  unsigned first = rq->first, n = rq->count ? rq->count : BLK_NSECT, i;
  unsigned long pos, crc, bits;
  phys_bytes buf, a;
  int valid, e;

  if (r[1] == 0) return(EBADF);
  if (rq->id >= (unsigned long) r[1] * bk.ext) return(ERANGE);
  if (first >= BLK_NSECT || n > BLK_NSECT - first) return(ERANGE);
  if ((buf = bk_buf((vir_bytes) rq->buf, (vir_bytes) n * BK_SECT)) == 0)
	return(EFAULT);
  pos = (BK_SKIP + ((unsigned long) r[0] * bk.ext + rq->id) * BK_BLOCK)
								* BK_SECT;
  bits = ((1UL << n) - 1) << first;

  /* The header first: a GET of a block that is not the caller's reads
   * nothing more; a PUT of the whole block without BLK_IFCHANGED needs
   * nothing from the card. */
  valid = FALSE;
  if (rq->op == BLK_GET || n != BLK_NSECT || (rq->flags & BLK_IFCHANGED)) {
	if ((e = sw_io(DEV_READ, MM_PROC_NR, pos, (phys_bytes) h,
				(phys_bytes) BK_SECT)) != OK) return(e);
	valid = bk_valid(h, rq->id);
  }

  if (rq->op == BLK_GET) {
	if (!valid || (h->live & bits) == 0) {
		memset((char *) buf, 0, (size_t) n * BK_SECT);
		return(OK);
	}
	if ((e = sw_io(DEV_READ, who, pos + (1UL + first) * BK_SECT,
		(phys_bytes) rq->buf, (phys_bytes) n * BK_SECT)) != OK)
		return(e);
	for (i = 0, a = buf; i < n; i++, a += BK_SECT) {
		if (!(h->live & (1UL << (first + i)))) {
			memset((char *) a, 0, BK_SECT);
			continue;
		}
		crc = ~crc32_more(0xFFFFFFFFUL, (unsigned char *) a, BK_SECT);
		if (crc != h->crc[first + i]) return(EIO);
	}
	return(OK);
  }

  /* PUT: the CRCs from the buffer; with BLK_IFCHANGED nothing is written
   * if the card holds the same (R12.36, AC12.20). */
  if (!valid) {
	memset((char *) h, 0, BK_SECT);
	h->magic = BK_MAGIC;
	h->slot = (unsigned long) who;
	h->pid = (unsigned long) mp->mp_pid;
	h->stamp = bk.stamp;
	h->dev = (unsigned long) mp->mp_dev;
	h->ino = (unsigned long) mp->mp_ino;
	h->ctime = (unsigned long) mp->mp_ctime;
	h->id = rq->id;
  }
  e = valid && (h->live & bits) == bits;	/* all the same so far */
  for (i = 0, a = buf; i < n; i++, a += BK_SECT) {
	crc = ~crc32_more(0xFFFFFFFFUL, (unsigned char *) a, BK_SECT);
	if (crc != h->crc[first + i]) e = FALSE;
	h->crc[first + i] = crc;
  }
  if (e && (rq->flags & BLK_IFCHANGED)) return(OK);
  /* R12.37: a background caller's writes count in the reserve's budget. */
  if (bg && !sw_wbudget((n + 2) / 2)) return(EAGAIN);
  h->live |= bits;
  h->count++;
  if ((e = sw_io(DEV_WRITE, MM_PROC_NR, pos, (phys_bytes) h,
				(phys_bytes) BK_SECT)) != OK) return(e);
  return(sw_io(DEV_WRITE, who, pos + (1UL + first) * BK_SECT,
		(phys_bytes) rq->buf, (phys_bytes) n * BK_SECT));
}


/*===========================================================================*
 *				bk_do					     *
 *===========================================================================*/
PRIVATE int bk_do(desc, bg)
char *desc;			/* the caller's struct blkreq */
int bg;				/* a background caller (R12.37) */
{
/* One MI_BLK request of process 'who' ('mp').  SUSPEND: not now, the
 * caller waits in BLKWAIT for sw_step(). */
  struct blkreq rq;
  struct blkinfo bi;
  unsigned long b, z;
  unsigned len;
  int r;

  if (sys_copy(who, D, (phys_bytes) desc, MM_PROC_NR, D,
		(phys_bytes) &rq, (phys_bytes) sizeof(rq)) != OK)
	return(EFAULT);
  /* R12.37: in plain mode the blocks are in clear, as swap images. */
  if (mp->mp_flags & NOSWAP) r = EPERM;
  else switch (rq.op) {
  case BLK_GET:
  case BLK_PUT:
	/* A background caller waits while the system is busy: a key in
	 * the last second or a process waiting for the CPU (FL_BG). */
	if (bg && sys_flash(FL_INFO | FL_BG, 0UL, NULL, 0, &b, &z) != OK)
		return(SUSPEND);
	r = bk_io(&rq, bg);
	/* Card out (N.6) or the minute's budget spent: a background
	 * request waits and is tried again; the foreground gets the error
	 * at once, so the screen never hangs on the card. */
	if (bg && (r == ENXIO || r == EAGAIN)) return(SUSPEND);
	if (r == EAGAIN) r = OK;
	break;
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
	bi.present = !(sw_state() & 0x80);
	r = sys_copy(MM_PROC_NR, D, (phys_bytes) &bi, who, D,
			(phys_bytes) rq.buf, (phys_bytes) sizeof(bi));
	break;
  default:
	r = EINVAL;
  }
  rq.status = r;
  (void) sys_copy(MM_PROC_NR, D, (phys_bytes) &rq.status, who, D,
	(phys_bytes) &((struct blkreq *) desc)->status,
	(phys_bytes) sizeof(rq.status));
  return(r);
}


/*===========================================================================*
 *				blk_req					     *
 *===========================================================================*/
PUBLIC int blk_req()
{
/* MEMINFO MI_BLK (R12.36).  The foreground app is served at once; anyone
 * else may have to wait for idle (R12.37). */
  int r;

  if (bk.ext == 0) return(ENOSYS);		/* blocks off, no partition */
  if (who <= INIT_PROC_NR) return(EPERM);	/* servers: never */
  r = bk_do(mm_in.m1_p1, !(mp->mp_flags & FGAPP));
  if (r != SUSPEND) return(r);
  /* No reply now.  mp_wpid is free while the process is in this call
   * (only wait uses it): it keeps where the descriptor is. */
  mp->mp_flags |= BLKWAIT;
  mp->mp_wpid = (pid_t) mm_in.m1_p1;
  dont_reply = TRUE;
  sw_kick();
  return(OK);
}


/*===========================================================================*
 *				blk_step				     *
 *===========================================================================*/
PUBLIC int blk_step()
{
/* From sw_step(), at idle (R12.37): one waiting request, the descriptor
 * read again from the caller.  TRUE if one still waits after this. */
  struct mproc *rmp, *caller = mp;
  int caller_who = who, r, more = FALSE;

  for (rmp = &mproc[INIT_PROC_NR + 1]; rmp < &mproc[NR_PROCS]; rmp++) {
	if (!(rmp->mp_flags & BLKWAIT)) continue;
	if ((rmp->mp_flags & (IN_USE | HANGING)) != IN_USE || bk.ext == 0) {
		rmp->mp_flags &= ~BLKWAIT;	/* gone, or blocks off */
		if ((rmp->mp_flags & (IN_USE | HANGING)) == IN_USE)
			reply((int) (rmp - mproc), ENOSYS, 0, NIL_PTR);
		continue;
	}
	if (more) return(TRUE);			/* one request a step */
	who = (int) (rmp - mproc);
	mp = rmp;
	r = bk_do((char *) rmp->mp_wpid, TRUE);
	if (r != SUSPEND) {
		rmp->mp_flags &= ~BLKWAIT;
		reply(who, r, 0, NIL_PTR);
	}
	more = TRUE;
	who = caller_who;
	mp = caller;
	if (r == SUSPEND) return(TRUE);		/* still busy: later */
  }
  return(FALSE);
}

#endif /* CHIP == ARM_CM */
