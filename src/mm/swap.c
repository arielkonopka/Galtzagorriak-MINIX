/* Swapping idle processes to the SD card, without encryption (spec Phase 12:
 * 12a, 12e, R12.28 "plain swap first").
 *
 * A user process that waits in MM (wait, pause, sigsuspend) may give its
 * RAM back: MM writes [D.start, brk) and [sp, S.end) to the process's slot
 * on the swap partition, the kernel marks it P_SWAPPED (it is never run,
 * nothing is copied into its range) and MM frees [D.start, S.end).  Its
 * text stays where it is (flash), its mproc entry too.  Before MM wakes it
 * up (its wait ends, a signal it catches arrives) the image comes back to
 * the same addresses: without an MMU nothing can be relocated, so MM places
 * processes to keep that range free as often as it can (R12.22: the
 * foreground app from the bottom, everything else from the top).
 *
 * Slot i (192 KB) belongs to mproc[i].  Sector 0 is the header (magic,
 * pid, where the image goes, and in plain mode a CRC-32 of every 4 KB
 * chunk of the image, R12.29); the image follows from sector 1.  A wrong
 * header, a CRC mismatch or a read error kills the process on swap-in: it
 * never runs with a damaged image.
 *
 * Less wear on the card (12g):
 *   - R12.29: a swap-out writes only the chunks whose CRC differs from the
 *     one in the slot's header, and nothing at all if none does.  The
 *     header is trusted only while SWAPHDR says it describes the chunks on
 *     the card (set by a whole swap-out and a good swap-in, cleared by
 *     fork, a write of a chunk that is not yet covered by a new header,
 *     and a change of the swap device);
 *   - R12.30: a process woken from the card (SWAPHOT) is left alone by the
 *     reserve until it has waited 120 s once; on demand it goes at once;
 *   - R12.31: on demand, the CRCs first, then the header, then the changed
 *     chunks in ascending order.  The reserve's job writes its chunks in
 *     ascending order and the header last (the job may be voided between
 *     steps, and the header must never claim a chunk not on the card).
 *
 * When:
 *   - on demand (R12.5 a): exec or fork cannot get memory.  Victims have
 *     waited for at least 1 s; the one that makes the largest hole first;
 *   - the reserve (R12.5 b): at idle (only while no user process waits for
 *     the CPU and no key came in the last second, as the flash cache's
 *     work), when free RAM is below the reserve (MMSWAPRES, 16 KB).
 *     Victims have waited for at least 10 s, the longest-waiting first.
 *     4 KB a step, at most 256 KB a minute.
 * A wake that cannot get its memory back stays where MM keeps it anyway
 * (the child stays a zombie, the signal pending) and is tried again after
 * every MM call; nothing is stored for it.
 *
 * Never swapped: MM, FS, INIT; traced, vfork and shadowed processes; text
 * in RAM; set-uid and set-gid images and Secret: yes packages (R12.28:
 * their RAM never reaches the card in clear); the foreground app and its
 * children (AC12.R3).
 *
 * The swap device is a primary partition of the SD card (WINCHESTER major)
 * whose MBR type is 0x82 (R12.1 amended): never internal flash, so swap
 * costs 0 flash writes.  An I/O error or a lost card turns swap off; every
 * swapped process is then killed ("lost"), never run with a partial image
 * (R12.25).
 *
 * The entry points into this file are:
 *   sw_init:	 MM starts
 *   sw_ctl:	 MEMINFO swap requests (MMSWAPON, MMSWAPRES, info, tests)
 *   sw_room:	 swap out until a hole of that size exists (exec, fork)
 *   sw_in:	 bring a process back before MM touches or wakes it
 *   sw_touch:	 MM is about to change a process: drop a reserve job on it
 *   sw_after:	 after every MM call: note waits, retry wakes, kill lost
 *   sw_step:	 the reserve's idle work (synchronous alarm)
 */

#include "mm.h"
#include <minix/callnr.h>
#include <minix/com.h>
#include <signal.h>
#include <string.h>
#include "mproc.h"

#if (CHIP == ARM_CM)

#define SW_SECT		512		/* sector */
#define SW_SLOT		(192UL * 1024)	/* R12.2: one per mproc slot */
#define SW_CHUNK	8		/* sectors per transfer (4 KB, R12.23) */
#define SW_NCHUNK	48		/* chunks in a slot: 383 sectors at most */
#define SW_MAGIC	0x57535A47UL	/* "GZSW" */
#define SW_MAJOR	3		/* WINCHESTER: /dev/hd* (FS dmap) */
#define SW_TYPE		0x82		/* MBR partition type: swap */
#define RES_KB		16		/* default reserve (R12.5 b) */
#define RATE_KB		256		/* reserve: at most this a minute */
#define WAIT_DEMAND	1		/* s waited before on-demand swap */
#define WAIT_RESERVE	10		/* s waited before reserve swap */
#define WAIT_HOT	120		/* ... after a swap-in (R12.30) */
#define MAX_RES_KB	192

#define LOST		0x80		/* sw.mode: card gone or failing */

/* The header goes through the last sector of exec's mbuf: exec needs the
 * rest for the stack image (sw_room is only called if that fits), the
 * flash cache and installs use mbuf only inside one call, never across a
 * swap.  No buffer of its own (R12.26). */
extern char mbuf[];
#define HDR	((struct swhdr *) (mbuf + ARG_MAX - SW_SECT))

struct swhdr {
  unsigned long magic, pid;
  unsigned long dstart, dlen;		/* data part, bytes */
  unsigned long sstart, slen;		/* stack part */
  unsigned long count;			/* write counter (12b) */
  unsigned long tag[4];			/* 12b: GCM (R12.10a: per chunk) */
  unsigned long crc[SW_NCHUNK];		/* plain: CRC-32 of each chunk */
};

struct layout {			/* where a swapped image lives in RAM */
  phys_bytes d, dl;		/* data part: [d, d + dl) */
  phys_bytes s, sl;		/* stack part: [s, s + sl) */
  phys_clicks base, clicks;	/* [D.start, S.end) */
  phys_bytes glo, ghi;		/* the gap [brk, sp): zeroed on swap-in */
};

/* All of MM's swap state (R12.26: at most 96 B). */
PRIVATE struct {
  unsigned short since[NR_PROCS];	/* when it began to wait, s */
  dev_t dev;			/* swap partition, NO_DEV: off */
  unsigned short res_kb;	/* reserve, KB (0: off) */
  unsigned short win;		/* reserve: its minute began, s */
  unsigned short wkb;		/* reserve: KB written in that minute */
  unsigned short off;		/* reserve job: chunks done */
  unsigned short nout, nin;	/* swap-outs, swap-ins since boot */
  unsigned char busy;		/* reserve job: slot + 1, 0: none */
  unsigned char mode;		/* SW_OFF, SW_PLAIN; | LOST */
  unsigned char armed;		/* a synchronous alarm is set */
  unsigned char trust;		/* reserve job: the header was SWAPHDR's */
} sw;

FORWARD _PROTOTYPE( unsigned short sw_now, (void)			);
FORWARD _PROTOTYPE( int sw_layout, (struct mproc *rmp, struct layout *l));
FORWARD _PROTOTYPE( unsigned long sw_crc, (struct layout *l,
			unsigned long v, phys_bytes a, phys_bytes n)	);
FORWARD _PROTOTYPE( int sw_chunk, (struct mproc *rmp, struct layout *l,
				int op, unsigned c, unsigned long *crc)	);
FORWARD _PROTOTYPE( int sw_hdrread, (struct mproc *rmp, struct layout *l));
FORWARD _PROTOTYPE( void sw_mkhdr, (struct mproc *rmp, struct layout *l));
FORWARD _PROTOTYPE( int sw_eligible, (struct mproc *rmp, unsigned wait)	);
FORWARD _PROTOTYPE( int sw_out, (struct mproc *rmp)			);
FORWARD _PROTOTYPE( int sw_finish, (struct mproc *rmp, struct layout *l));
FORWARD _PROTOTYPE( void sw_fail, (char *why)				);
FORWARD _PROTOTYPE( int sw_pending, (struct mproc *rmp)			);
FORWARD _PROTOTYPE( void sw_kill, (struct mproc *rmp, char *why)	);
FORWARD _PROTOTYPE( void sw_alarm, (int ticks)				);
FORWARD _PROTOTYPE( int sw_on, (int dev, int mode)			);
FORWARD _PROTOTYPE( int sw_room_at, (phys_clicks base, phys_clicks n,
							int evict)	);

#define nsect(l)	((unsigned) (((l)->dl + (l)->sl) / SW_SECT))
#define nchunk(l)	((nsect(l) + SW_CHUNK - 1) / SW_CHUNK)
#define slotpos(rmp)	((unsigned long) ((rmp) - mproc) * SW_SLOT)


/*===========================================================================*
 *				sw_init					     *
 *===========================================================================*/
PUBLIC void sw_init()
{
  sw.res_kb = RES_KB;
  blk_init();
}


/*===========================================================================*
 *				sw_now					     *
 *===========================================================================*/
PRIVATE unsigned short sw_now()
{
/* Seconds since boot, modulo 65536 (the waits compared are short). */
  message m;

  m.m_type = GET_UPTIME;
  if (sendrec(CLOCK, &m) != OK) return(0);
  return((unsigned short) ((unsigned long) m.NEW_TIME / HZ));
}


/*===========================================================================*
 *				sw_layout				     *
 *===========================================================================*/
PRIVATE int sw_layout(rmp, l)
struct mproc *rmp;
struct layout *l;
{
/* Which parts of the image are written: the data up to brk and the stack
 * from sp, both whole sectors inside [D.start, S.end).  The kernel keeps
 * sp while the process is swapped (R12.26), so this gives the same answer
 * on swap-in.  Parts may overlap when the gap is under a sector.
 */
  phys_bytes end, dend, lo;
  vir_bytes sp;

  l->base = rmp->mp_seg[D].mem_phys;
  l->clicks = rmp->mp_seg[S].mem_phys + rmp->mp_seg[S].mem_len - l->base;
  l->d = (phys_bytes) l->base << CLICK_SHIFT;
  end = l->d + ((phys_bytes) l->clicks << CLICK_SHIFT);
  dend = l->d + ((phys_bytes) rmp->mp_seg[D].mem_len << CLICK_SHIFT);
  if (sys_getsp((int) (rmp - mproc), &sp) != OK) return(EINVAL);
  lo = sp;
  if (lo < l->d || lo > end) return(EINVAL);

  l->dl = (dend - l->d + SW_SECT - 1) & ~(phys_bytes) (SW_SECT - 1);
  if (l->d + l->dl > end) l->dl -= SW_SECT;
  if (l->d + l->dl < dend && lo > l->d + l->dl) lo = l->d + l->dl;
  l->sl = (end - lo + SW_SECT - 1) & ~(phys_bytes) (SW_SECT - 1);
  if (end - l->sl < l->d) l->sl -= SW_SECT;
  l->s = end - l->sl;
  /* Both parts must cover [D.start, brk) and [sp, S.end). */
  if (l->s > sp || (l->s > l->d + l->dl && l->d + l->dl < dend))
	return(EINVAL);
  if (SW_SECT * (1 + nsect(l)) > SW_SLOT) return(EINVAL);
  l->glo = dend;
  l->ghi = sp > dend ? (phys_bytes) sp : dend;
  return(OK);
}


/*===========================================================================*
 *				sw_io					     *
 *===========================================================================*/
PUBLIC int sw_io(op, proc, pos, addr, n)
int op;				/* DEV_READ or DEV_WRITE */
int proc;			/* whose memory */
unsigned long pos;		/* byte offset on the partition */
phys_bytes addr;		/* address (flat: virtual = physical) */
phys_bytes n;			/* bytes, whole sectors */
{
/* One transfer between the swap partition and memory, straight through the
 * WINCHESTER task as FS does it (no FS, no buffer cache, R12.1). */
  message m;

  m.m_type = op;
  m.DEVICE = (int) (sw.dev & BYTE);
  m.POSITION = (long) pos;
  m.HIGHPOS = 0;		/* R12.39: swap stays below 4 GiB */
  m.PROC_NR = proc;
  m.ADDRESS = (char *) addr;
  m.COUNT = (int) n;
  if (sendrec(WINCHESTER, &m) != OK) return(EIO);
  if (m.REP_STATUS == ENXIO) return(ENXIO);	/* card out (N.6) */
  return(m.REP_STATUS == (int) n ? OK : EIO);
}


/*===========================================================================*
 *				sw_state				     *
 *===========================================================================*/
PUBLIC int sw_state()
{
/* For the blocks (blk.c): SW_OFF or SW_PLAIN, | 0x80 if the card is gone. */
  return(sw.mode);
}


/*===========================================================================*
 *				sw_kick					     *
 *===========================================================================*/
PUBLIC void sw_kick()
{
/* A block request waits for idle (blk.c, R12.37): the next step soon. */
  sw_alarm(HZ / 10 + 1);		/* replaces a later one */
}


/*===========================================================================*
 *				sw_wbudget				     *
 *===========================================================================*/
PUBLIC int sw_wbudget(kb)
unsigned kb;
{
/* R12.37: a background block PUT counts in the reserve's write budget
 * (256 KB a minute, shared).  May kb more be written now? */
  unsigned short now = sw_now();

  if ((unsigned short) (now - sw.win) >= 60) {
	sw.win = now;
	sw.wkb = 0;
  }
  if (sw.wkb >= RATE_KB) return(FALSE);
  sw.wkb += kb;
  return(TRUE);
}


/*===========================================================================*
 *				sw_crc					     *
 *===========================================================================*/
PRIVATE unsigned long sw_crc(l, v, a, n)
struct layout *l;
unsigned long v;		/* CRC so far */
phys_bytes a, n;		/* [a, a + n) */
{
/* CRC-32 of [a, a + n) without the gap: whole sectors reach past brk and
 * below sp, and what lies there (a signal handler's frame below sp) is
 * zeroed on swap-in, never used, so it is no reason to write a chunk. */
  phys_bytes e = a + n, lo = l->glo, hi = l->ghi;

  if (lo < a) lo = a;
  if (hi > e) hi = e;
  if (lo >= hi) return(crc32_more(v, (unsigned char *) a, n));
  v = crc32_more(v, (unsigned char *) a, lo - a);
  return(crc32_more(v, (unsigned char *) hi, e - hi));
}


/*===========================================================================*
 *				sw_chunk				     *
 *===========================================================================*/
PRIVATE int sw_chunk(rmp, l, op, c, crc)
struct mproc *rmp;
struct layout *l;
int op;				/* 0: CRC only, DEV_WRITE: write only,
				 * DEV_READ: read, then its CRC */
unsigned c;			/* chunk: image sectors 8c .. 8c + 7 */
unsigned long *crc;		/* its CRC-32 (not for DEV_WRITE) */
{
/* One 4 KB chunk of the image (the last one may be shorter).  A chunk that
 * spans the end of the data part and the start of the stack part is
 * contiguous on the card but not in RAM: two transfers.  OK or EIO. */
  unsigned ds = (unsigned) (l->dl / SW_SECT), i = c * SW_CHUNK, e, n;
  unsigned long v = 0xFFFFFFFFUL;
  phys_bytes addr;

  e = i + SW_CHUNK;
  if (e > nsect(l)) e = nsect(l);
  for (; i < e; i += n) {
	if (i < ds) {
		addr = l->d + (phys_bytes) i * SW_SECT;
		n = (ds < e ? ds : e) - i;
	} else {
		addr = l->s + (phys_bytes) (i - ds) * SW_SECT;
		n = e - i;
	}
	if (op != 0 && sw_io(op, (int) (rmp - mproc),
			slotpos(rmp) + (1UL + i) * SW_SECT, addr,
			(phys_bytes) n * SW_SECT) != OK) return(EIO);
	if (op != DEV_WRITE)
		v = sw_crc(l, v, addr, (phys_bytes) n * SW_SECT);
  }
  *crc = ~v;
  return(OK);
}


/*===========================================================================*
 *				sw_hdrread				     *
 *===========================================================================*/
PRIVATE int sw_hdrread(rmp, l)
struct mproc *rmp;
struct layout *l;
{
/* Read the slot's header into HDR (a read does not wear the card).  Is it
 * this process's, for this layout? */
  struct swhdr *h = HDR;

  if (sw_io(DEV_READ, MM_PROC_NR, slotpos(rmp), (phys_bytes) h,
					(phys_bytes) SW_SECT) != OK)
	return(FALSE);
  return(h->magic == SW_MAGIC && h->pid == (unsigned long) rmp->mp_pid
		&& h->dstart == l->d && h->dlen == l->dl
		&& h->sstart == l->s && h->slen == l->sl);
}


/*===========================================================================*
 *				sw_mkhdr				     *
 *===========================================================================*/
PRIVATE void sw_mkhdr(rmp, l)
struct mproc *rmp;
struct layout *l;
{
/* A new header in HDR, without the chunk CRCs. */
  struct swhdr *h = HDR;

  memset((char *) h, 0, SW_SECT);
  h->magic = SW_MAGIC;
  h->pid = (unsigned long) rmp->mp_pid;
  h->dstart = l->d;
  h->dlen = l->dl;
  h->sstart = l->s;
  h->slen = l->sl;
}


/*===========================================================================*
 *				sw_eligible				     *
 *===========================================================================*/
PRIVATE int sw_eligible(rmp, wait)
struct mproc *rmp;
unsigned wait;			/* it must have waited this long, s */
{
/* R12.3 with R12.28: may this process be swapped out now? */
  struct mproc *q;
  int slot = (int) (rmp - mproc);

  if (sw.mode != SW_PLAIN || slot <= INIT_PROC_NR) return(FALSE);
  if ((rmp->mp_flags & (IN_USE | HANGING)) != IN_USE) return(FALSE);
  if (!(rmp->mp_flags & (WAITING | PAUSED | SIGSUSPENDED))) return(FALSE);
  if (rmp->mp_flags & (TRACED | STOPPED | VFORKED | VFORKING | SWAPPED
					| NOSWAP | FGAPP | SWAPBAD)) return(FALSE);
  /* Text in RAM lies just below the data (exec): it would stay behind. */
  if (rmp->mp_seg[T].mem_len != 0 && rmp->mp_seg[T].mem_phys
		+ rmp->mp_seg[T].mem_len == rmp->mp_seg[D].mem_phys)
	return(FALSE);
  /* A shadow group shares the addresses (fork): never. */
  for (q = &mproc[INIT_PROC_NR + 1]; q < &mproc[NR_PROCS]; q++)
	if (q != rmp && (q->mp_flags & IN_USE) && !(q->mp_flags & SWAPPED)
		    && q->mp_seg[D].mem_phys == rmp->mp_seg[D].mem_phys)
		return(FALSE);
  if (sw_pending(rmp)) return(FALSE);	/* about to wake anyway */
  return((unsigned short) (sw_now() - sw.since[slot]) >= wait);
}


/*===========================================================================*
 *				sw_finish				     *
 *===========================================================================*/
PRIVATE int sw_finish(rmp, l)
struct mproc *rmp;
struct layout *l;
{
/* The image and its header are on the card: tell the kernel, free the
 * memory (R12.4). */
  int slot = (int) (rmp - mproc);

  rmp->mp_flags |= SWAPHDR;
  if (sys_swap(slot, 1) != OK) return(EBUSY);
  rmp->mp_flags |= SWAPPED;
  free_mem(l->base, l->clicks);
  sw.nout++;
  if (mem_trace)
	printf("swap: out %-8.8s pid %d, %lu bytes freed\n", rmp->mp_name,
		rmp->mp_pid, (unsigned long) l->clicks << CLICK_SHIFT);
  return(OK);
}


/*===========================================================================*
 *				sw_out					     *
 *===========================================================================*/
PRIVATE int sw_out(rmp)
struct mproc *rmp;
{
/* Swap one eligible process out now.  R12.29: only the chunks that changed
 * since the header on the card was written; R12.31: CRCs first, then the
 * header, then those chunks in ascending order, 4 KB a transfer (other
 * requests to the card get in between). */
  struct layout l;
  struct swhdr *h = HDR;
  unsigned long crc, chg[2];
  unsigned c, nc;
  int same;

  if (sw.busy == (rmp - mproc) + 1) sw.busy = 0;
  if (sw_layout(rmp, &l) != OK) return(EINVAL);
  nc = nchunk(&l);
  same = (rmp->mp_flags & SWAPHDR) && sw_hdrread(rmp, &l);
  if (!same) sw_mkhdr(rmp, &l);
  chg[0] = chg[1] = 0;			/* on MM's stack: 0 B static */
  for (c = 0; c < nc; c++) {
	(void) sw_chunk(rmp, &l, 0, c, &crc);
	if (!same || crc != h->crc[c]) {
		chg[c >> 5] |= 1UL << (c & 31);
		h->crc[c] = crc;
	}
  }
  if (chg[0] | chg[1]) {
	/* Until the last chunk is on the card the header claims too much. */
	rmp->mp_flags &= ~SWAPHDR;
	if (sw_io(DEV_WRITE, MM_PROC_NR, slotpos(rmp), (phys_bytes) h,
					(phys_bytes) SW_SECT) != OK) {
		sw_fail("write error");
		return(EIO);
	}
	for (c = 0; c < nc; c++) {
		if (!(chg[c >> 5] & (1UL << (c & 31)))) continue;
		if (sw_chunk(rmp, &l, DEV_WRITE, c, &crc) != OK) {
			sw_fail("write error");
			return(EIO);
		}
	}
  }
  return(sw_finish(rmp, &l));
}


/*===========================================================================*
 *				sw_fail					     *
 *===========================================================================*/
PRIVATE void sw_fail(why)
char *why;
{
/* R12.25: the card is gone or failing.  No more swap-outs; the swapped
 * processes are killed by sw_after(), never run. */
  if (!(sw.mode & LOST))
	printf("swap: %s, swap is off\n", why);
  sw.mode |= LOST;
  sw.busy = 0;
}


/*===========================================================================*
 *				sw_room_at				     *
 *===========================================================================*/
PRIVATE int sw_room_at(base, n, evict)
phys_clicks base, n;
int evict;			/* swap out the eligible occupants */
{
/* Can [base, base + n) be made free: is every click in a hole or in the
 * range of a process that may be swapped out?  If evict, do it. */
  struct mproc *q;
  phys_clicks pos = base, e;
  int found;

  while (pos < base + n) {
	if ((e = hole_end(pos)) != 0) {
		pos = e;
		continue;
	}
	found = FALSE;
	for (q = &mproc[INIT_PROC_NR + 1]; q < &mproc[NR_PROCS]; q++) {
		if (!(q->mp_flags & IN_USE) || (q->mp_flags & SWAPPED))
			continue;
		e = q->mp_seg[S].mem_phys + q->mp_seg[S].mem_len;
		if (pos < q->mp_seg[D].mem_phys || pos >= e) continue;
		if (!sw_eligible(q, WAIT_DEMAND)) return(FALSE);
		if (evict && sw_out(q) != OK) return(FALSE);
		pos = e;
		found = TRUE;
		break;
	}
	if (!found) return(FALSE);	/* a shadow copy, the kernel ... */
  }
  return(TRUE);
}


/*===========================================================================*
 *				sw_room					     *
 *===========================================================================*/
PUBLIC void sw_room(clicks)
phys_clicks clicks;
{
/* R12.5 a: exec or fork needs a hole of that size.  Swap out processes
 * that have waited for 1 s, each time the one that makes the largest hole,
 * until the hole is there or no victim makes it larger.  The caller then
 * tries once more. */
  struct mproc *q, *best;
  phys_clicks have, n, bestn;
  unsigned short now = sw_now();

  while ((have = max_hole()) < clicks && sw.mode == SW_PLAIN) {
	best = NULL;
	bestn = have;
	for (q = &mproc[INIT_PROC_NR + 1]; q < &mproc[NR_PROCS]; q++) {
		if (!sw_eligible(q, WAIT_DEMAND)) continue;
		n = q->mp_seg[S].mem_phys + q->mp_seg[S].mem_len
						- q->mp_seg[D].mem_phys;
		n = hole_with(q->mp_seg[D].mem_phys, n);
		if (n > bestn || (n == bestn && best != NULL
			  && (unsigned short) (now - sw.since[q - mproc])
			   > (unsigned short) (now - sw.since[best - mproc]))) {
			best = q;
			bestn = n;
		}
	}
	if (best == NULL || sw_out(best) != OK) return;
  }
}


/*===========================================================================*
 *				sw_in					     *
 *===========================================================================*/
PUBLIC int sw_in(rmp)
struct mproc *rmp;
{
/* R12.6: bring rmp's image back to its own addresses, swapping out eligible
 * occupants if needed.  OK when it is resident (also if it never left),
 * else the wake has to wait. */
  struct layout l;
  struct swhdr *h = HDR;
  unsigned long crc;
  unsigned c;
  int slot = (int) (rmp - mproc);

  if (!(rmp->mp_flags & SWAPPED)) return(OK);
  if ((sw.mode & LOST) || (rmp->mp_flags & SWAPBAD)) return(ENXIO);
  if (sw_layout(rmp, &l) != OK) {
	rmp->mp_flags |= SWAPBAD;
	return(EIO);
  }
  if (alloc_mem_at(l.base, l.clicks) != OK) {
	if (!sw_room_at(l.base, l.clicks, FALSE)
			|| !sw_room_at(l.base, l.clicks, TRUE)
			|| alloc_mem_at(l.base, l.clicks) != OK)
		return(EAGAIN);
  }
  /* The process waits for MM's reply, so it cannot run before that even
   * with P_SWAPPED off; the reads need its map in the kernel. */
  sys_swap(slot, 0);
  if (sw_io(DEV_READ, MM_PROC_NR, slotpos(rmp), (phys_bytes) h,
					(phys_bytes) SW_SECT) != OK) {
	sw_fail("read error");
	goto bad;
  }
  if (h->magic != SW_MAGIC || h->pid != (unsigned long) rmp->mp_pid
		|| h->dstart != l.d || h->dlen != l.dl || h->sstart != l.s
		|| h->slen != l.sl) {
	printf("swap: %s (pid %d): wrong header\n", rmp->mp_name,
								rmp->mp_pid);
	goto bad;
  }
  /* R12.29: each chunk against its own CRC, also the ones a swap-out
   * skipped because they had not changed. */
  for (c = 0; c < nchunk(&l); c++) {
	if (sw_chunk(rmp, &l, DEV_READ, c, &crc) != OK) {
		sw_fail("read error");
		goto bad;
	}
	if (crc != h->crc[c]) {
		printf("swap: %s (pid %d): CRC mismatch\n", rmp->mp_name,
								rmp->mp_pid);
		goto bad;
	}
  }
  /* R12.6: the gap gets zeroes, never what the RAM held meanwhile (nor
   * what the card holds there: the CRCs leave it out). */
  if (l.ghi > l.glo) memset((char *) l.glo, 0, (size_t) (l.ghi - l.glo));
  /* The card holds what is in RAM now (SWAPHDR stays); R12.30: woken
   * from the card, so the reserve leaves it alone for a while. */
  rmp->mp_flags = (rmp->mp_flags & ~SWAPPED) | SWAPHOT;
  sw.since[slot] = sw_now();
  sw.nin++;
  if (mem_trace)
	printf("swap: in  %-8.8s pid %d\n", rmp->mp_name, rmp->mp_pid);
  return(OK);

bad:
  /* Never run it: killed by sw_after(). */
  sys_swap(slot, 1);
  free_mem(l.base, l.clicks);
  rmp->mp_flags |= SWAPBAD;
  return(EIO);
}


/*===========================================================================*
 *				sw_touch				     *
 *===========================================================================*/
PUBLIC void sw_touch(proc_nr)
int proc_nr;
{
/* MM replies to the process or changes it: a reserve job on it is void. */
  if (sw.busy == proc_nr + 1) sw.busy = 0;
}


/*===========================================================================*
 *				sw_pending				     *
 *===========================================================================*/
PRIVATE int sw_pending(rmp)
struct mproc *rmp;
{
/* Does a wake wait for this process: a signal that may be delivered now,
 * or (in wait) a child that has exited? */
  struct mproc *c;
  pid_t w;

  if (rmp->mp_sigpending & ~rmp->mp_sigmask) return(TRUE);
  if (!(rmp->mp_flags & WAITING)) return(FALSE);
  w = rmp->mp_wpid;
  for (c = &mproc[0]; c < &mproc[NR_PROCS]; c++) {
	if ((c->mp_flags & (IN_USE | HANGING)) != (IN_USE | HANGING)
				|| c->mp_parent != (int) (rmp - mproc))
		continue;
	if (w == -1 || w == c->mp_pid || -w == c->mp_procgrp) return(TRUE);
  }
  return(FALSE);
}


/*===========================================================================*
 *				sw_kill					     *
 *===========================================================================*/
PRIVATE void sw_kill(rmp, why)
struct mproc *rmp;
char *why;
{
/* R12.25: SIGKILL without a swap-in. */
  struct mproc *caller = mp;

  printf("swap: %s (pid %d) lost: %s\n", rmp->mp_name, rmp->mp_pid, why);
  mp = rmp;
  rmp->mp_sigstatus = (char) SIGKILL;
  mm_exit(rmp, 0);
  mp = caller;
}


/*===========================================================================*
 *				sw_after				     *
 *===========================================================================*/
PUBLIC void sw_after()
{
/* After every MM call.  Note when the caller began to wait (R12.3); kill
 * lost images (R12.25); retry the wakes that waited for memory (R12.6);
 * set the reserve going (R12.5 b). */
  struct mproc *rmp;
  int i;

  if (sw.mode == SW_OFF) return;
  if (who > INIT_PROC_NR && who < NR_PROCS && dont_reply
		&& (mp->mp_flags & (IN_USE | HANGING)) == IN_USE
		&& (mp->mp_flags & (WAITING | PAUSED | SIGSUSPENDED)))
	sw.since[who] = sw_now();

  for (rmp = &mproc[INIT_PROC_NR + 1]; rmp < &mproc[NR_PROCS]; rmp++) {
	if ((rmp->mp_flags & (IN_USE | SWAPPED)) != (IN_USE | SWAPPED))
		continue;
	if (sw.mode & LOST) {
		sw_kill(rmp, "SD card removed");
		continue;
	}
	if (rmp->mp_flags & SWAPBAD) {
		sw_kill(rmp, "swap image damaged");
		continue;
	}
	if (!sw_pending(rmp) || sw_in(rmp) != OK) continue;
	/* Back in RAM: deliver what waited. */
	if (rmp->mp_flags & WAITING) wait_retry(rmp);
	for (i = 1; i < _NSIG; i++) {
		if (!(rmp->mp_flags & IN_USE) || (rmp->mp_flags & HANGING))
			break;
		if (sigismember(&rmp->mp_sigpending, i)
				&& !sigismember(&rmp->mp_sigmask, i)) {
			sigdelset(&rmp->mp_sigpending, i);
			sig_proc(rmp, i);
			break;
		}
	}
  }

  if (sw.mode == SW_PLAIN && !sw.armed && sw.res_kb != 0
	    && ((phys_bytes) mem_free() << CLICK_SHIFT)
					< (phys_bytes) sw.res_kb * 1024)
	sw_alarm(HZ);
}


/*===========================================================================*
 *				sw_alarm				     *
 *===========================================================================*/
PRIVATE void sw_alarm(ticks)
int ticks;
{
/* The reserve's next step.  MM has one synchronous alarm, shared with the
 * flash cache (main() calls both steps); while the cache works, never
 * later than its next step. */
  message m;

  if (fc_busy()) ticks = 1;	/* its steps are a tick apart */
  m.m_type = SET_SYNC_AL;
  m.CLOCK_PROC_NR = MM_PROC_NR;
  m.DELTA_TICKS = (long) ticks;
  if (sendrec(CLOCK, &m) == OK) sw.armed = TRUE;
}


/*===========================================================================*
 *				sw_step					     *
 *===========================================================================*/
PUBLIC void sw_step()
{
/* R12.5 b: one step of the reserve, at idle priority (R0.8d).  Start a job
 * on the process that has waited longest (at least 10 s), or write the
 * next 4 KB of the current one.  Nothing while a user process waits for
 * the CPU or a key came in the last second (the kernel's FL_BG test, as
 * for the flash cache's work), and at most 256 KB a minute. */
  struct mproc *rmp, *best;
  struct layout l;
  unsigned long b, z, crc;
  unsigned short now;
  unsigned c, nc;
  int slot, same;

  sw.armed = FALSE;
  /* R12.37: waiting block requests first (one a step), then the reserve;
   * swap-ins the user waits for were done in sw_after() already. */
  if (blk_step()) {
	sw_alarm(HZ / 10 + 1);
	return;
  }
  if (sw.mode != SW_PLAIN || sw.res_kb == 0) {
	sw.busy = 0;
	return;
  }
  if (sw.busy == 0) {
	if (((phys_bytes) mem_free() << CLICK_SHIFT)
				>= (phys_bytes) sw.res_kb * 1024)
		return;				/* enough: no alarm */
	now = sw_now();
	if ((unsigned short) (now - sw.win) >= 60) {
		sw.win = now;
		sw.wkb = 0;
	}
	if (sw.wkb >= RATE_KB) {		/* until the minute is over */
		sw_alarm((int) (60 - (unsigned short) (now - sw.win)) * HZ);
		return;
	}
	best = NULL;
	for (rmp = &mproc[INIT_PROC_NR + 1]; rmp < &mproc[NR_PROCS]; rmp++) {
		/* R12.30: back from the card by its own wake: 120 s, once. */
		if (!sw_eligible(rmp, (rmp->mp_flags & SWAPHOT)
					? WAIT_HOT : WAIT_RESERVE)) continue;
		rmp->mp_flags &= ~SWAPHOT;
		slot = (int) (rmp - mproc);
		if (best == NULL || (unsigned short) (now - sw.since[slot])
			    > (unsigned short) (now - sw.since[best - mproc]))
			best = rmp;
	}
	if (best == NULL) {
		sw_alarm(5 * HZ);	/* someone may become eligible */
		return;
	}
	sw.busy = (unsigned char) (best - mproc + 1);
	sw.off = 0;
	sw.trust = (best->mp_flags & SWAPHDR) != 0;
  }

  /* One chunk, if the system is idle. */
  if (sys_flash(FL_INFO | FL_BG, 0UL, NULL, 0, &b, &z) != OK) {
	sw_alarm(1);
	return;
  }
  rmp = &mproc[sw.busy - 1];
  if (sw_layout(rmp, &l) != OK) {
	sw.busy = 0;
	sw_alarm(HZ);
	return;
  }
  /* R12.29: the next chunk that differs from the header on the card (read
   * again: mbuf does not live between steps); the ones before it that do
   * not cost CRCs only.  The header still describes the chunks after the
   * one this step writes, and it is no longer SWAPHDR's once a chunk is
   * written. */
  nc = nchunk(&l);
  if (sw.off < nc) {
	same = sw.trust && sw_hdrread(rmp, &l);
	while (sw.off < nc) {
		c = sw.off++;
		(void) sw_chunk(rmp, &l, 0, c, &crc);
		if (same && crc == HDR->crc[c]) continue;
		rmp->mp_flags &= ~SWAPHDR;
		if (sw_chunk(rmp, &l, DEV_WRITE, c, &crc) != OK) {
			sw_fail("write error");
			return;
		}
		sw.wkb += SW_CHUNK / 2;
		break;
	}
  }
  if (sw.off >= nc) {
	sw.busy = 0;
	/* The header last, from CRCs taken now: the process is blocked, its
	 * RAM is what the job wrote (sw_touch voids the job otherwise).
	 * Nothing changed: nothing written, not even the header. */
	if (!(rmp->mp_flags & SWAPHDR)) {
		sw_mkhdr(rmp, &l);
		for (c = 0; c < nc; c++)
			(void) sw_chunk(rmp, &l, 0, c, &HDR->crc[c]);
		if (sw_io(DEV_WRITE, MM_PROC_NR, slotpos(rmp),
			    (phys_bytes) HDR, (phys_bytes) SW_SECT) != OK) {
			sw_fail("write error");
			return;
		}
	}
	if (sw_finish(rmp, &l) != OK) return;
  }
  sw_alarm(2);			/* the next chunk, or the next victim */
}


/*===========================================================================*
 *				sw_on					     *
 *===========================================================================*/
PRIVATE int sw_on(dev, mode)
int dev;			/* the partition, NO_DEV: off */
int mode;			/* SW_OFF, SW_PLAIN, SW_CRYPT */
{
/* MMSWAPON (R12.1 amended): only a primary partition of the SD card with
 * the MBR type 0x82, big enough for 16 slots.  Off, or another device,
 * only while nothing is swapped. */
  struct mproc *rmp;
  unsigned char *mbr = (unsigned char *) HDR;
  int minor = dev & BYTE;
  message m;

  int blk = mode & SW_BLK;		/* R12.38: blocks too (or only) */
  int r;

  mode &= ~SW_BLK;
  if (mode == SW_CRYPT) return(ENOSYS);		/* 12b, later */
  if (mode != SW_OFF && mode != SW_PLAIN) return(EINVAL);
  for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++)
	if ((rmp->mp_flags & (IN_USE | SWAPPED)) == (IN_USE | SWAPPED))
		return(EBUSY);
  if (blk_busy()) return(EBUSY);	/* a reservation is on this device */
  sw.busy = 0;
  for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++)
	rmp->mp_flags &= ~SWAPHDR;	/* the headers were another device's */
  (void) blk_on(0);
  if (mode == SW_OFF && !blk) {
	sw.mode = SW_OFF;
	sw.dev = NO_DEV;
	return(OK);
  }
  if (((dev >> MAJOR) & BYTE) != SW_MAJOR || minor < 1 || minor > 4)
	return(EINVAL);
  m.m_type = DEV_OPEN;
  m.DEVICE = minor;
  m.PROC_NR = MM_PROC_NR;
  m.COUNT = R_BIT | W_BIT;
  if (sendrec(WINCHESTER, &m) != OK || m.REP_STATUS != OK) return(ENXIO);
  sw.dev = (dev_t) (SW_MAJOR << MAJOR);		/* hd0: the MBR */
  if (sw_io(DEV_READ, MM_PROC_NR, 0UL, (phys_bytes) mbr, SW_SECT) != OK
		|| mbr[510] != 0x55 || mbr[511] != 0xAA
		|| mbr[0x1BE + 16 * (minor - 1) + 4] != SW_TYPE) {
	sw.dev = NO_DEV;
	return(EINVAL);
  }
  sw.dev = (dev_t) dev;				/* big enough? */
  if (sw_io(DEV_READ, MM_PROC_NR, NR_PROCS * SW_SLOT - SW_SECT,
			(phys_bytes) mbr, SW_SECT) != OK) {
	sw.dev = NO_DEV;
	return(ENOSPC);
  }
  /* Blocks behind the slots (R12.34); with SW_OFF the partition serves
   * only them, nothing is swapped. */
  if (blk && (r = blk_on(minor)) != OK) {
	sw.dev = NO_DEV;
	return(r);
  }
  sw.mode = mode;
  return(OK);
}


/*===========================================================================*
 *				sw_ctl					     *
 *===========================================================================*/
PUBLIC int sw_ctl()
{
/* MEMINFO with m1_i2 = MI_SWAPON (m1_i1 the partition, 0: off; m1_i3 the
 * mode), MI_SWAPRES (m1_i1 KB), MI_SWAPINFO (struct swinfo to m1_p1),
 * MI_SWAPTEST (tests: m1_i1 1 = swap out pid m1_i3 now, 2 = act as if the
 * card were pulled), MI_FGEXEC (the caller's next child execs the
 * foreground app, R12.22). */
  struct swinfo *si = (struct swinfo *) HDR;
  struct mproc *rmp;
  int n;

  if (mm_in.m1_i2 == MI_SWAPINFO) {
	memset((char *) si, 0, sizeof(*si));
	si->mode = sw.mode;
	si->dev = sw.dev;
	si->res_kb = sw.res_kb;
	si->nout = sw.nout;
	si->nin = sw.nin;
	n = 0;
	for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++) {
		if ((rmp->mp_flags & (IN_USE | SWAPPED)) != (IN_USE | SWAPPED))
			continue;
		si->p[n].pid = rmp->mp_pid;
		memcpy(si->p[n].name, rmp->mp_name, sizeof(si->p[n].name));
		si->p[n].bytes = (unsigned long) (rmp->mp_seg[S].mem_phys
			+ rmp->mp_seg[S].mem_len - rmp->mp_seg[D].mem_phys)
							<< CLICK_SHIFT;
		si->p[n].slot = (int) (rmp - mproc);
		n++;
	}
	si->nproc = n;
	return(sys_copy(MM_PROC_NR, D, (phys_bytes) si, who, D,
		(phys_bytes) mm_in.m1_p1, (phys_bytes) sizeof(*si)));
  }
  if (mp->mp_effuid != SUPER_USER) return(EPERM);
  switch (mm_in.m1_i2) {
  case MI_SWAPON:
	return(sw_on(mm_in.m1_i1, mm_in.m1_i1 == 0 ? SW_OFF : mm_in.m1_i3));
  case MI_SWAPRES:
	if (mm_in.m1_i1 < 0 || mm_in.m1_i1 > MAX_RES_KB) return(EINVAL);
	sw.res_kb = (unsigned short) mm_in.m1_i1;
	return(OK);
  case MI_FGEXEC:
	mp->mp_flags |= FGEXEC;
	return(OK);
  case MI_SWAPTEST:
	if (mm_in.m1_i1 == 2) {			/* R12.25, AC12.12 */
		if (sw.mode == SW_OFF) return(EINVAL);
		sw_fail("SD card removed (test)");
		return(OK);
	}
	if (mm_in.m1_i1 != 1) return(EINVAL);
	for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++)
		if ((rmp->mp_flags & IN_USE) && rmp->mp_pid == mm_in.m1_i3)
			break;
	if (rmp == &mproc[NR_PROCS]) return(ESRCH);
	if (!sw_eligible(rmp, 0)) return(EBUSY);
	return(sw_out(rmp));
  }
  return(EINVAL);
}

#endif /* CHIP == ARM_CM */
