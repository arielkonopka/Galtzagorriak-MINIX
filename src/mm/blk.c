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
 * counter wraps (blk_wrap), so a reused pid never sees old blocks; a run
 * that was live at the wrap also accepts the stamp before it (bk.old).
 *
 * The entry points into this file are:
 *   blk_init:	 MM starts: the boot stamp
 *   blk_on:	 MMSWAPON: cut the partition into extents (0: blocks off)
 *   blk_busy:	 does any process hold a reservation (swap device change)
 *   blk_end:	 exit or exec: the reservation ends (fork: the child's slot
 *		 was cleared by the last exit in it)
 *   blk_wrap:	 the pid counter wrapped
 *   blk_req:	 MEMINFO MI_BLK
 *   blk_step:	 at idle: one waiting request or one DISCARD step
 *   blk_nwipe, blk_wipe: MMSWAPON's DISCARD of the whole space, by steps
 *
 * R12.40: a run whose process PUT a block (BLKUSED) is not free when it
 * ends: its bit in bk.disc is set and the entry stays, so nobody gets the
 * extents until blk_step() has told the card they hold nothing (DISCARD,
 * at idle, at most DISC_MAX sectors a step).  Until then the slot's next
 * process has no reservation, and its OPEN waits in BLKWAIT.  A card
 * without DISCARD (ENOTTY once) frees runs at once from then on.
 */

#include "mm.h"
#include <minix/com.h>
#include <minix/blk.h>
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
#define DISC_MAX	(256UL * 2048)	/* sectors discarded a step: 256 MB */
#define PEND(slot)	(bk.disc & (1U << (slot)))	/* run not yet free */
#define SUB_MINOR	128		/* hd1a: MM swaps to a subpartition */
#define EPS		(NEXT / 4)	/* extents per subpartition (>= 32 GB) */
#define BIG		(sw_minor() >= SUB_MINOR)	/* hd3a ... hd3d */
#define SUBOK(e)	(!BIG || (e) < EPS || (bk.old & (1U << ((e) / EPS - 1))))

/* All of MM's block state (R12.34, R12.35; spec budget MM +39 B). */
PRIVATE struct {
  unsigned long stamp;		/* boot stamp (R12.35) */
  unsigned short ext;		/* blocks per extent, 0: blocks off */
  unsigned char run[NSLOT][2];	/* first extent, number of extents */
  unsigned short disc;		/* bit i: slot i's ended run waits for its
				 * DISCARD at idle (R12.40) */
  unsigned short old;		/* bit i: slot i's run began before the last
				 * pid wrap, so the stamp before it is its
				 * own too (R12.35).  Bits 0-2 (MM, FS and
				 * INIT hold no run): hd3b, hd3c, hd3d are
				 * there (cards >= 32 GB, R12.34) */
} bk;

/* Bit 0 of bk.disc (slot 0 is MM, which never holds a run): the card has
 * no DISCARD, runs end at once (R12.40). */
#define NODISC		(bk.disc & 1U)
#define SET_NODISC()	(bk.disc |= 1U)
#define DISC_PEND	(bk.disc & ~1U)	/* some run waits for its DISCARD */

/* The header sector on the card (R12.35).  It goes through the last
 * sector of exec's mbuf, as the swap header does (R12.26): 0 B. */
extern char mbuf[];
#define HDR	((struct bkhdr *) (mbuf + ARG_MAX - BK_SECT))
#define BK_MAGIC	0x4B425A47UL	/* "GZBK" */
#define BK_SCR		((ARG_MAX - BK_SECT) / BK_SECT)	/* mbuf in front */

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

FORWARD _PROTOTYPE( int bk_at, (unsigned e, unsigned long *sec)	);
FORWARD _PROTOTYPE( unsigned bk_gap, (unsigned pos)			);
FORWARD _PROTOTYPE( unsigned bk_fit, (unsigned n, unsigned *len)	);
FORWARD _PROTOTYPE( int bk_open, (unsigned long bytes, int flags)	);
FORWARD _PROTOTYPE( int bk_valid, (struct bkhdr *h, unsigned long id)	);
FORWARD _PROTOTYPE( int bk_buf, (vir_bytes v, vir_bytes n)		);
FORWARD _PROTOTYPE( int bk_user, (vir_bytes v, unsigned n,
						unsigned long *crc)	);
FORWARD _PROTOTYPE( int bk_io, (struct blkreq *rq, int bg)		);
FORWARD _PROTOTYPE( int bk_do, (char *desc, int bg)			);
FORWARD _PROTOTYPE( int bk_disc, (struct blkreq *rq)			);


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
/* next_pid went from 30000 back to 2: pids are used again (R12.35).
 * A new run's blocks need the new stamp, so a reused pid never sees an
 * old owner's blocks.  A run that is live now keeps its data: it also
 * accepts the stamp before the wrap (bk.old).  Its old owner with the same
 * pid in that slot ended before the run began, and so before the
 * previous wrap, with a stamp at most bk.stamp - 2. */
  int i;

  bk.stamp++;
  bk.old &= 7;				/* (hd3b-hd3d stay) */
  for (i = INIT_PROC_NR + 1; i < NR_PROCS; i++)
	if (RUN(i)[1] != 0 && !PEND(i)) bk.old |= 1U << i;
}


/*===========================================================================*
 *				blk_on					     *
 *===========================================================================*/
PUBLIC int blk_on(sz, nsz)
unsigned long *sz;		/* the swap partition's size, sectors */
int nsz;			/* 0: blocks off; 1: a primary partition;
				 * 4: hd3a-hd3d, sz[i] 0 if not swap */
{
/* MMSWAPON: the extents behind the swap slots, from the partition's size
 * in its MBR entry (swap.c reads the MBR anyway; WINCHESTER takes no ioctl
 * from MM).  A partition too small for 256 blocks after the slots has no
 * block space.  Cards >= 32 GB (R12.34): hd3a holds the slots, and each of
 * hd3a ... hd3d has 64 extents behind the same offset, the size of hd3a's;
 * one that is missing or short only removes its extents.  The caller
 * checked blk_busy(). */
  unsigned long n;
  int i;

  /* Runs still waiting for their DISCARD are dropped with the device. */
  for (i = INIT_PROC_NR + 1; i < NR_PROCS; i++)
	if (PEND(i)) RUN(i)[1] = 0;
  bk.disc = 0;			/* (and NODISC: a new device) */
  bk.old &= ~7U;
  bk.ext = 0;
  if (nsz == 0) return(OK);
  if (sz[0] <= BK_SKIP) return(ENOSPC);
  n = (sz[0] - BK_SKIP) / BK_BLOCK / (nsz == 1 ? NEXT : EPS);
  if (n == 0) return(ENOSPC);
  /* R12.39: a swap partition stays below 4 GiB, so this fits 16 bits. */
  bk.ext = n > 0xFFFF ? 0xFFFF : (unsigned short) n;
  for (i = 1; i < nsz; i++)
	if (sz[i] >= BK_SKIP + (unsigned long) EPS * bk.ext * BK_BLOCK)
		bk.old |= 1U << (i - 1);
  return(OK);
}


/*===========================================================================*
 *				blk_nwipe				     *
 *===========================================================================*/
PUBLIC unsigned blk_nwipe()
{
/* R12.40 at MMSWAPON: the number of steps of blk_wipe, the swap slots
 * first, then the extents at most DISC_MAX sectors a step. */
  unsigned long g;

  if (bk.ext == 0) return(1);
  g = DISC_MAX / ((unsigned long) bk.ext * BK_BLOCK);
  if (g == 0) g = 1;
  return(1 + (unsigned) ((NEXT + g - 1) / g));
}


/*===========================================================================*
 *				blk_wipe				     *
 *===========================================================================*/
PUBLIC int blk_wipe(step)
unsigned step;			/* 0: the 16 slots; then groups of extents */
{
/* R12.40: one step of the DISCARD of the whole swap space after MMSWAPON
 * (sw_step, at idle; sw.off counts the steps left, 0 B).  Whatever is
 * live by now is left alone: the slot of a process on the card, every
 * extent of a reservation.  ENOTTY: the card has no DISCARD. */
  unsigned long usz, g, u, u0, u1, s;
  struct mproc *rmp;
  unsigned char *e;
  int r, i, live;

  if (step == 0) {
	usz = BK_SKIP / NR_PROCS;
	u0 = 0;
	u1 = NR_PROCS;
  } else {
	if (bk.ext == 0) return(OK);
	usz = (unsigned long) bk.ext * BK_BLOCK;
	if ((g = DISC_MAX / usz) == 0) g = 1;
	u0 = (step - 1) * g;
	if (u0 >= NEXT) return(OK);
	u1 = u0 + g > NEXT ? NEXT : u0 + g;
  }
  for (u = u0; u < u1; u = s) {
	for (s = u; s < u1; s++) {
		if (step == 0) {
			rmp = &mproc[s];
			live = (rmp->mp_flags & (IN_USE | SWAPPED))
						== (IN_USE | SWAPPED);
			if (!live) rmp->mp_flags &= ~SWAPHDR;
		} else {
			/* (a run stays in one subpartition; one that is
			 * not there is left alone) */
			if (s > u && BIG && s % EPS == 0) break;
			live = !SUBOK(s);
			for (i = 0; i < NSLOT; i++) {
				e = bk.run[i];
				if (e[1] != 0 && s >= e[0] && s < e[0] + e[1])
					live = TRUE;
			}
		}
		if (live) break;
	}
	if (step == 0) {
		i = sw_minor();		/* the slots: hd3a */
		g = u * usz;
	} else {
		i = bk_at((unsigned) u, &g);
	}
	if (s > u && (r = sw_discard(i, g, (s - u) * usz)) != OK) {
		if (r == ENOTTY) SET_NODISC();
		return(r);
	}
	if (s == u) s++;		/* a live one: skip it */
  }
  return(OK);
}


/*===========================================================================*
 *				blk_busy				     *
 *===========================================================================*/
PUBLIC int blk_busy()
{
  int i;

  /* (a run that only waits for its DISCARD does not count) */
  for (i = 0; i < NSLOT; i++)
	if (bk.run[i][1] != 0 && !PEND(i + INIT_PROC_NR + 1)) return(TRUE);
  return(FALSE);
}


/*===========================================================================*
 *				blk_end					     *
 *===========================================================================*/
PUBLIC void blk_end(proc_nr)
int proc_nr;
{
/* Exit, exec or CLOSE (R12.35): the run is free again, nothing is written.
 * Who reserves those extents next has another pid, slot or program, and
 * reads the old blocks as zeros.  R12.40: if a block was PUT, the run is
 * free only after its DISCARD at idle (blk_step). */
  struct mproc *rmp = &mproc[proc_nr];

  if (proc_nr <= INIT_PROC_NR || proc_nr >= NR_PROCS) return;
  if ((rmp->mp_flags & BLKUSED) && RUN(proc_nr)[1] != 0 && !NODISC
						&& !PEND(proc_nr)) {
	bk.disc |= 1U << proc_nr;	/* R12.40: free after the DISCARD */
	sw_kick();
  } else if (!PEND(proc_nr)) {
	RUN(proc_nr)[1] = 0;
  }
  rmp->mp_flags &= ~BLKUSED;
}


/*===========================================================================*
 *				bk_at					     *
 *===========================================================================*/
PRIVATE int bk_at(e, sec)
unsigned e;			/* an extent */
unsigned long *sec;		/* its first sector on that partition */
{
/* Where extent e is: the minor of its partition (hd3a ... hd3d on cards
 * >= 32 GB), and its first sector there, behind the 16 swap slots. */
  int m = sw_minor();

  if (m >= SUB_MINOR) {
	m += e / EPS;
	e %= EPS;
  }
  *sec = BK_SKIP + (unsigned long) e * bk.ext * BK_BLOCK;
  return(m);
}


/*===========================================================================*
 *				bk_gap					     *
 *===========================================================================*/
PRIVATE unsigned bk_gap(pos)
unsigned pos;			/* an extent */
{
/* How many free extents start at pos (0: pos is taken). */
  unsigned end = NEXT, f, i;

  /* Cards >= 32 GB: a run stays in one subpartition, which is there. */
  if (BIG) {
	if (!SUBOK(pos)) return(0);
	end = (pos / EPS + 1) * EPS;
  }
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
 * there is none.  A free run starts at 0 or right after a taken one (or,
 * on cards >= 32 GB, at a subpartition's first extent). */
  unsigned pos, g, best = NEXT, i;

  *len = 0;
  for (i = 0; i < NSLOT + 4; i++) {
	if (i >= NSLOT) {
		pos = (i - NSLOT) * EPS;
		if (pos != 0 && !BIG) continue;
	} else if (bk.run[i][1] == 0) continue;
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
	blk_end(who);
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
  bk.old &= ~(1U << who);		/* a new run: only the new stamp */
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
	&& h->pid == (unsigned long) mp->mp_pid
	&& (h->stamp == bk.stamp
	    || ((bk.old & (1U << who)) && h->stamp == bk.stamp - 1))
	&& h->dev == (unsigned long) mp->mp_dev
	&& h->ino == (unsigned long) mp->mp_ino
	&& h->ctime == (unsigned long) mp->mp_ctime && h->id == id);
}


/*===========================================================================*
 *				bk_buf					     *
 *===========================================================================*/
PRIVATE int bk_buf(v, n)
vir_bytes v, n;			/* the caller's buffer, bytes */
{
/* R12.36: is the buffer wholly inside the caller's data or stack segment?
 * (WINCHESTER checks again with numap.) */
  struct mem_map *s;
  vir_bytes lo, hi;

  for (s = &mp->mp_seg[D]; s <= &mp->mp_seg[S]; s++) {
	lo = (vir_bytes) s->mem_vir << CLICK_SHIFT;
	hi = lo + ((vir_bytes) s->mem_len << CLICK_SHIFT);
	if (v >= lo && v < hi && n <= hi - v) return(TRUE);
  }
  return(FALSE);
}


/*===========================================================================*
 *				bk_user					     *
 *===========================================================================*/
PRIVATE int bk_user(v, n, crc)
vir_bytes v;			/* the caller's buffer, checked by bk_buf */
unsigned n;			/* sectors */
unsigned long *crc;		/* the CRC of each sector; 0: zero them */
{
/* The caller's sectors, only through the kernel (sys_copy), never at the
 * addresses in mp_seg: after a fork the caller shares them with its
 * shadow group, and the clock task may have put another member's image
 * there while MM waited for the card (arm/shadow.c).  Through the free
 * part of exec's mbuf, in front of HDR: 0 B. */
  unsigned i, j, k;
  int r;

  for (i = 0; i < n; i += k) {
	k = n - i < BK_SCR ? n - i : BK_SCR;
	if (crc == (unsigned long *) 0) {
		memset(mbuf, 0, (size_t) k * BK_SECT);
		r = sys_copy(MM_PROC_NR, D, (phys_bytes) mbuf, who, D,
			(phys_bytes) v, (phys_bytes) k * BK_SECT);
	} else {
		r = sys_copy(who, D, (phys_bytes) v, MM_PROC_NR, D,
			(phys_bytes) mbuf, (phys_bytes) k * BK_SECT);
		for (j = 0; r == OK && j < k; j++)
			*crc++ = ~crc32_more(0xFFFFFFFFUL, (unsigned char *)
				mbuf + j * BK_SECT, BK_SECT);
	}
	if (r != OK) return(EFAULT);
	v += (vir_bytes) k * BK_SECT;
  }
  return(OK);
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
  unsigned long pos, bits, c[BLK_NSECT];
  int valid, e, m;

  if (r[1] == 0 || PEND(who)) return(EBADF);
  if (rq->id >= (unsigned long) r[1] * bk.ext) return(ERANGE);
  if (first >= BLK_NSECT || n > BLK_NSECT - first) return(ERANGE);
  if (!bk_buf((vir_bytes) rq->buf, (vir_bytes) n * BK_SECT)) return(EFAULT);
  m = bk_at(r[0], &pos);		/* (a run is in one subpartition) */
  pos = (pos + rq->id * BK_BLOCK) * BK_SECT;
  bits = ((1UL << n) - 1) << first;

  /* The header first: a GET of a block that is not the caller's reads
   * nothing more; a PUT of the whole block without BLK_IFCHANGED needs
   * nothing from the card. */
  valid = FALSE;
  if (rq->op == BLK_GET || n != BLK_NSECT || (rq->flags & BLK_IFCHANGED)) {
	if ((e = sw_dio(m, DEV_READ, MM_PROC_NR, pos, (phys_bytes) h,
				(phys_bytes) BK_SECT)) != OK) return(e);
	valid = bk_valid(h, rq->id);
  }

  if (rq->op == BLK_GET) {
	if (!valid || (h->live & bits) == 0)
		return(bk_user((vir_bytes) rq->buf, n, (unsigned long *) 0));
	if ((e = sw_dio(m, DEV_READ, who, pos + (1UL + first) * BK_SECT,
		(phys_bytes) rq->buf, (phys_bytes) n * BK_SECT)) != OK)
		return(e);
	if ((e = bk_user((vir_bytes) rq->buf, n, c)) != OK) return(e);
	for (i = 0; i < n; i++) {
		if (!(h->live & (1UL << (first + i)))) {
			if ((e = bk_user((vir_bytes) rq->buf + i * BK_SECT, 1,
				(unsigned long *) 0)) != OK) return(e);
		} else if (c[i] != h->crc[first + i]) {
			return(EIO);
		}
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
  if ((e = bk_user((vir_bytes) rq->buf, n, c)) != OK) return(e);
  e = valid && (h->live & bits) == bits;	/* all the same so far */
  for (i = 0; i < n; i++) {
	if (c[i] != h->crc[first + i]) e = FALSE;
	h->crc[first + i] = c[i];
  }
  if (e && (rq->flags & BLK_IFCHANGED)) return(OK);
  /* R12.37: a background caller's writes count in the reserve's budget. */
  if (bg && !sw_wbudget((n + 2) / 2)) return(EAGAIN);
  h->live |= bits;
  h->count++;
  mp->mp_flags |= BLKUSED;		/* R12.40: discard it when it ends */
  if ((e = sw_dio(m, DEV_WRITE, MM_PROC_NR, pos, (phys_bytes) h,
				(phys_bytes) BK_SECT)) != OK) return(e);
  return(sw_dio(m, DEV_WRITE, who, pos + (1UL + first) * BK_SECT,
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
  unsigned len, i;
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
	/* R12.40: the last run in this slot is not discarded yet. */
	if (PEND(who)) return(SUSPEND);
	r = bk_open(rq.id, rq.flags);
	break;
  case BLK_DISCARD:
	/* R12.40: always in the idle class (from blk_step), never while
	 * keys arrive. */
	if (!(mp->mp_flags & BLKWAIT)
		|| sys_flash(FL_INFO | FL_BG, 0UL, NULL, 0, &b, &z) != OK)
		return(SUSPEND);
	if ((r = bk_disc(&rq)) == ENXIO) return(SUSPEND);
	break;
  case BLK_CLOSE:
	blk_end(who);
	r = OK;
	break;
  case BLK_INFO:
	memset((char *) &bi, 0, sizeof(bi));
	bi.blocks = PEND(who) ? 0 : (unsigned long) RUN(who)[1] * bk.ext;
	(void) bk_fit(NEXT, &len);
	bi.largest = (unsigned long) len * bk.ext;
	for (len = 0, i = 0; i < NEXT; i += EPS)
		if (SUBOK(i)) len += EPS;
	bi.space = (unsigned long) len * bk.ext;
	bi.extent = bk.ext;
	bi.run = RUN(who)[0];
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
 *				bk_disc					     *
 *===========================================================================*/
PRIVATE int bk_disc(rq)
struct blkreq *rq;
{
/* blk_discard (R12.40): the card is told that blocks rq->id .. id + n - 1
 * of the caller's run hold nothing, headers too, so they read as zeros or
 * old data.  A card without DISCARD: nothing is done. */
  unsigned char *r = RUN(who);
  unsigned long n = (unsigned long) rq->buf, all, sec;
  int e, m;

  if (r[1] == 0 || PEND(who)) return(EBADF);
  all = (unsigned long) r[1] * bk.ext;
  if (n == 0 || rq->id >= all || n > all - rq->id) return(ERANGE);
  if (NODISC) return(OK);
  m = bk_at(r[0], &sec);
  e = sw_discard(m, sec + rq->id * BK_BLOCK, n * BK_BLOCK);
  if (e == ENOTTY) SET_NODISC();
  return(e == ENOTTY ? OK : e);
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
/* From sw_step(), at idle (R12.37): one DISCARD step of an ended run
 * (R12.40), or else one waiting request, the descriptor read again from
 * the caller.  TRUE if more waits after this. */
  struct mproc *rmp, *caller = mp;
  int caller_who = who, r, more = FALSE, i;
  unsigned char *e;
  unsigned long b, z, n;

  if (DISC_PEND) {
	/* Not while keys arrive or a process waits for the CPU. */
	if (sys_flash(FL_INFO | FL_BG, 0UL, NULL, 0, &b, &z) != OK)
		return(TRUE);
	for (i = INIT_PROC_NR + 1; !PEND(i); i++) ;
	e = RUN(i);
	n = DISC_MAX / ((unsigned long) bk.ext * BK_BLOCK);
	if (n == 0) n = 1;
	if (n > e[1]) n = e[1];
	r = bk.ext == 0 ? OK : sw_discard(bk_at(e[0], &b), b,
		n * bk.ext * BK_BLOCK);
	if (r == ENXIO) return(TRUE);		/* card out: later */
	if (r == ENOTTY) {			/* no DISCARD: all free now */
		SET_NODISC();
		n = e[1];
	}
	e[0] += (unsigned char) n;		/* (EIO: freed all the same) */
	e[1] -= (unsigned char) n;
	if (e[1] == 0) bk.disc &= ~(1U << i);
	if (NODISC) {
		for (i = INIT_PROC_NR + 1; i < NR_PROCS; i++)
			if (PEND(i)) RUN(i)[1] = 0;
		bk.disc = 1U;		/* (NODISC stays) */
	}
	return(TRUE);				/* the OPENs waiting: next */
  }

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
