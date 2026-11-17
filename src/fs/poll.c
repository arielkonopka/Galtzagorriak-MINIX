/* The POLL system call (spec R4b.6; the FS <-> device protocol is #131).
 *
 * poll(fds, nfds, timeout) looks at every descriptor once (a "scan"):
 * pipes and FIFOs are judged here from the inode, character devices are
 * asked with DEV_SELECT, everything else is always ready.  If nothing is
 * ready and the caller may wait, it is suspended (XPOLL) and its scan runs
 * again ("re-run", through revive() and get_work(), like a pipe reader)
 * when something it waits for may have changed:
 *   - a pipe or FIFO was read, written, opened or closed (poll_pipe),
 *   - a device task sent DEV_SELECTED for one of its minors (poll_selected),
 *   - its timeout came (poll_timer, from SYN_ALRM_TASK's CLOCK_INT).
 * A re-run that finds nothing ready suspends again, so a wake-up too many
 * costs a scan, never a wrong answer.  A signal ends the wait with EINTR
 * (do_unpause).
 *
 * Devices: a scan that may wait adds SEL_NOTIFY.  The task then remembers
 * only the ops bits per minor; the pollers are here, in pollrec[]: up to
 * POLL_DEVS devices per process.  More than that: those are asked without
 * SEL_NOTIFY and the process re-runs every clock tick instead.  When the
 * call ends, a device that no other waiting poller has gets DEV_SELECT
 * with ops 0 (forget it).  A device that does not know DEV_SELECT (it
 * answers with an error) is always ready, as POSIX has it for files.
 *
 * Timeouts are in clock ticks (rounded up, plus one: never early), through
 * one synchronous alarm for all pollers (SET_SYNC_AL, as MM's flash cache
 * does).  In Stop2 the clock task's wake-up is the R4b.2 ms timer.
 *
 * The entry points into this file are
 *   do_poll:	    perform the POLL system call (also the re-run)
 *   do_selected:   DEV_SELECTED from a device task
 *   poll_selected: a DEV_SELECTED arrived (also inside call_task)
 *   poll_pipe:	    a pipe changed: re-run the pollers that have one
 *   poll_timer:    CLOCK_INT: re-run the pollers whose time has come
 *   poll_cancel:   a waiting poller gets a signal or exits (do_unpause)
 */

#include "fs.h"
#include <poll.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include "dev.h"
#include "file.h"
#include "fproc.h"
#include "inode.h"
/* Not param.h: its 'fd' would rename struct pollfd's member. */
#define buffer		m.m1_p1		/* fds */
#define nbytes		m.m1_i2		/* nfds */
#define poll_timeout	m.m1_i3		/* ms; < 0: none */

#define POLL_DEVS	4	/* devices registered per process */
#define TDIFF(a, b)	((long) ((a) - (b)))

/* One record per process: 16 B (R4b.6 budget: about 16 B x 16). */
PRIVATE struct pollrec {
  clock_t pr_due;		/* uptime tick of the timeout; 0: none */
  dev_t pr_dev[POLL_DEVS];	/* devices told SEL_NOTIFY; NO_DEV: free */
  char pr_wait;			/* suspended in poll() */
  char pr_pipes;		/* it polls a pipe or FIFO */
  char pr_full;			/* more devices than pr_dev: re-run each tick */
  char pr_pad;
} pollrec[NR_PROCS];

PRIVATE clock_t poll_alarm;	/* the alarm asked for; 0: none */
PRIVATE unsigned char sel_events;	/* DEV_SELECTED seen (count) */

FORWARD _PROTOTYPE( int poll_fd, (struct pollfd *p, int notify)		);
FORWARD _PROTOTYPE( int poll_dev, (Dev_t dev, int events, int notify)	);
FORWARD _PROTOTYPE( void poll_done, (int proc_nr)			);
FORWARD _PROTOTYPE( void poll_unwait, (struct pollrec *pr)		);
FORWARD _PROTOTYPE( void poll_arm, (clock_t now)			);
FORWARD _PROTOTYPE( void poll_rerun, (int proc_nr)			);
FORWARD _PROTOTYPE( clock_t uptime, (void)				);

/*===========================================================================*
 *				do_poll					     *
 *===========================================================================*/
PUBLIC int do_poll()
{
/* poll(fds, nfds, timeout): fds in 'buffer', nfds in 'nbytes', the timeout
 * (ms, < 0: none) in 'poll_timeout'.  A re-run has only 'buffer' and
 * 'nbytes' (get_work); its due time is in the record. */
  struct pollfd pfd[OPEN_MAX];
  struct pollrec *pr = &pollrec[who];
  int n = nbytes, i, ready, block, r;
  unsigned char ev;
  long ms;
  clock_t now = 0;
  phys_bytes size;

  if (who < 0 || who >= NR_PROCS) return(EPERM);
  if (pr->pr_wait) {			/* a re-run: wait on */
	poll_unwait(pr);
	block = TRUE;
  } else {				/* a new call */
	ms = poll_timeout;
	block = (ms != 0);
	pr->pr_due = 0;
	if (ms > 0) {
		now = uptime();
		pr->pr_due = now + 1 + ms / 1000 * HZ
					+ ((ms % 1000) * HZ + 999) / 1000;
		if (pr->pr_due == 0) pr->pr_due = 1;
	}
  }
  if (n < 0 || n > OPEN_MAX) {
	poll_done(who);
	return(EINVAL);
  }
  size = (phys_bytes) n * sizeof(pfd[0]);
  if (n > 0 && sys_copy(who, D, (phys_bytes) buffer,
			FS_PROC_NR, D, (phys_bytes) pfd, size) != OK) {
	poll_done(who);
	return(EFAULT);
  }

  for (;;) {
	ev = sel_events;
	ready = 0;
	for (i = 0; i < POLL_DEVS; i++) pr->pr_dev[i] = NO_DEV;
	pr->pr_pipes = pr->pr_full = FALSE;
	for (i = 0; i < n; i++) {
		pfd[i].revents = poll_fd(&pfd[i], block && ready == 0);
		if (pfd[i].revents != 0) ready++;
	}
	if (ready != 0 || !block) break;
	if (pr->pr_due != 0 && TDIFF(now = uptime(), pr->pr_due) >= 0)
		break;				/* timed out */

	/* A DEV_SELECTED during the scan may have been ours: again. */
	if (sel_events == ev) {
		m.m1_i1 = 0;		/* suspend() saves it as 'fd' */
		suspend(XPOLL);
		pr->pr_wait = TRUE;
		if (pr->pr_pipes) poll_npipe++;
		if (pr->pr_due != 0 || pr->pr_full) {
			if (now == 0) now = uptime();
			poll_arm(now);
		}
		return(OK);			/* no reply: suspended */
	}
  }

  poll_done(who);
  if (n > 0 && (r = sys_copy(FS_PROC_NR, D, (phys_bytes) pfd,
			who, D, (phys_bytes) buffer, size)) != OK)
	return(EFAULT);
  return(ready);
}


/*===========================================================================*
 *				poll_fd					     *
 *===========================================================================*/
PRIVATE int poll_fd(p, notify)
struct pollfd *p;
int notify;			/* nothing ready yet: register devices */
{
/* The revents of one entry. */
  struct filp *f;
  struct inode *rip;
  int ev = p->events, r = 0;
  dev_t dev;

  if (p->fd < 0) return(0);
  if (p->fd >= OPEN_MAX || (f = fp->fp_filp[p->fd]) == NIL_FILP)
	return(POLLNVAL);
  if (f->filp_mode == FILP_CLOSED) return(POLLERR);	/* read: EIO */
  rip = f->filp_ino;

  if (rip->i_pipe == I_PIPE) {
	pollrec[who].pr_pipes = TRUE;
	if (f->filp_mode & R_BIT) {
		if (rip->i_size > f->filp_pos) r |= POLLIN | POLLRDNORM;
		if (find_filp(rip, W_BIT) == NIL_FILP) r |= POLLHUP;
	}
	if (f->filp_mode & W_BIT) {
		if (find_filp(rip, R_BIT) == NIL_FILP)
			r |= POLLERR;		/* write: EPIPE */
		else if (f->filp_pos < PIPE_SIZE)
			r |= POLLOUT | POLLWRNORM;
	}
  } else if ((rip->i_mode & I_TYPE) == I_CHAR_SPECIAL) {
	dev = (dev_t) rip->i_zone[0];
	if (dmap[(dev >> MAJOR) & BYTE].dmap_rw == call_ctty) {
		if (fp->fp_tty == 0) return(POLLERR);	/* read: EIO */
		dev = fp->fp_tty;	/* /dev/tty: the real one */
	}
	r = poll_dev(dev, ev, notify);
  } else {
	r = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
  }
  return(r & (ev | POLLERR | POLLHUP | POLLNVAL));
}


/*===========================================================================*
 *				poll_dev				     *
 *===========================================================================*/
PRIVATE int poll_dev(dev, events, notify)
dev_t dev;
int events;
int notify;
{
/* Ask a device task which of the ops are ready (DEV_SELECT). */
  struct pollrec *pr = &pollrec[who];
  int ops = SEL_ERR, r, i, slot = -1;

  if (events & (POLLIN | POLLRDNORM)) ops |= SEL_RD;
  if (events & (POLLOUT | POLLWRNORM)) ops |= SEL_WR;
  if (notify) {
	for (i = 0; i < POLL_DEVS; i++) {
		if (pr->pr_dev[i] == dev) break;
		if (pr->pr_dev[i] == NO_DEV && slot < 0) slot = i;
	}
	if (i < POLL_DEVS || slot >= 0) {
		if (i == POLL_DEVS) pr->pr_dev[slot] = dev;
		ops |= SEL_NOTIFY;
	} else {
		pr->pr_full = TRUE;	/* no room: re-run each tick */
	}
  }
  r = dev_select(dev, ops, who);
  if (r < 0) {
	/* The task does not do DEV_SELECT: always ready, as a file is. */
	if (ops & SEL_NOTIFY)
		for (i = 0; i < POLL_DEVS; i++)
			if (pr->pr_dev[i] == dev) pr->pr_dev[i] = NO_DEV;
	return(POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM);
  }
  return((r & SEL_RD ? POLLIN | POLLRDNORM : 0)
	| (r & SEL_WR ? POLLOUT | POLLWRNORM : 0)
	| (r & SEL_ERR ? POLLERR : 0));
}


/*===========================================================================*
 *				poll_done				     *
 *===========================================================================*/
PRIVATE void poll_done(proc_nr)
int proc_nr;
{
/* The call ends: cancel the registrations no other waiting poller has. */
  struct pollrec *pr = &pollrec[proc_nr], *q;
  dev_t dev;
  int i, j;

  poll_unwait(pr);
  for (i = 0; i < POLL_DEVS; i++) {
	if ((dev = pr->pr_dev[i]) == NO_DEV) continue;
	pr->pr_dev[i] = NO_DEV;
	for (q = &pollrec[0]; q < &pollrec[NR_PROCS]; q++) {
		if (!q->pr_wait) continue;
		for (j = 0; j < POLL_DEVS; j++)
			if (q->pr_dev[j] == dev) break;
		if (j < POLL_DEVS) break;
	}
	if (q == &pollrec[NR_PROCS]) (void) dev_select(dev, 0, proc_nr);
  }
  pr->pr_due = 0;
  pr->pr_full = FALSE;
}


/*===========================================================================*
 *				poll_unwait				     *
 *===========================================================================*/
PRIVATE void poll_unwait(pr)
struct pollrec *pr;
{
  if (!pr->pr_wait) return;
  pr->pr_wait = FALSE;
  if (pr->pr_pipes) poll_npipe--;
}


/*===========================================================================*
 *				poll_cancel				     *
 *===========================================================================*/
PUBLIC void poll_cancel(proc_nr)
int proc_nr;
{
/* do_unpause: a waiting poller got a signal (EINTR) or exits.  It may be
 * marked for a re-run already: that must not happen any more. */
  struct fproc *rfp = &fproc[proc_nr];

  if (rfp->fp_revived == REVIVING) {
	rfp->fp_revived = NOT_REVIVING;
	reviving--;
  }
  poll_done(proc_nr);
}


/*===========================================================================*
 *				poll_rerun				     *
 *===========================================================================*/
PRIVATE void poll_rerun(proc_nr)
int proc_nr;
{
  if (pollrec[proc_nr].pr_wait) revive(proc_nr, 0);	/* XPOLL: re-run */
}


/*===========================================================================*
 *				do_selected				     *
 *===========================================================================*/
PUBLIC int do_selected()
{
/* DEV_SELECTED from a device task: ops REP_STATUS are ready on minor
 * DEVICE.  No reply to the task. */
  if (who >= 0) return(ENOSYS);		/* a user process: not a call */
  poll_selected(who, m.DEVICE);
  dont_reply = TRUE;
  return(OK);
}


/*===========================================================================*
 *				poll_selected				     *
 *===========================================================================*/
PUBLIC void poll_selected(task, minor)
int task;			/* who sent DEV_SELECTED */
int minor;
{
/* Re-run every poller waiting on that minor of that task.  The task has
 * forgotten the registration; those that still wait register again. */
  struct pollrec *pr;
  dev_t dev;
  int i;

  sel_events++;
  for (pr = &pollrec[0]; pr < &pollrec[NR_PROCS]; pr++) {
	if (!pr->pr_wait) continue;
	for (i = 0; i < POLL_DEVS; i++) {
		dev = pr->pr_dev[i];
		if (dev != NO_DEV && ((dev >> MINOR) & BYTE) == (minor & BYTE)
				&& dmap[(dev >> MAJOR) & BYTE].dmap_task == task) {
			poll_rerun((int) (pr - pollrec));
			break;
		}
	}
  }
}


/*===========================================================================*
 *				poll_pipe				     *
 *===========================================================================*/
PUBLIC void poll_pipe()
{
/* A pipe or FIFO was read, written, opened or closed: re-run the pollers
 * that poll one (only called while poll_npipe != 0). */
  struct pollrec *pr;

  for (pr = &pollrec[0]; pr < &pollrec[NR_PROCS]; pr++)
	if (pr->pr_wait && pr->pr_pipes) poll_rerun((int) (pr - pollrec));
}


/*===========================================================================*
 *				poll_timer				     *
 *===========================================================================*/
PUBLIC void poll_timer()
{
/* CLOCK_INT from SYN_ALRM_TASK: re-run the pollers whose time has come
 * (the re-run sees the timeout) or that re-run every tick; then ask for
 * the next alarm. */
  struct pollrec *pr;
  clock_t now = uptime();

  poll_alarm = 0;
  for (pr = &pollrec[0]; pr < &pollrec[NR_PROCS]; pr++) {
	if (!pr->pr_wait) continue;
	if (pr->pr_full || (pr->pr_due != 0 && TDIFF(now, pr->pr_due) >= 0))
		poll_rerun((int) (pr - pollrec));
  }
  poll_arm(now);
}


/*===========================================================================*
 *				poll_arm				     *
 *===========================================================================*/
PRIVATE void poll_arm(now)
clock_t now;
{
/* Ask for a CLOCK_INT at the earliest due time of the waiting pollers,
 * unless one is asked for at that time or before (FS has one alarm). */
  struct pollrec *pr;
  clock_t due, need = 0;
  long delta;
  message cm;

  for (pr = &pollrec[0]; pr < &pollrec[NR_PROCS]; pr++) {
	if (!pr->pr_wait || fproc[pr - pollrec].fp_revived == REVIVING)
		continue;
	due = pr->pr_full ? now + 1 : pr->pr_due;
	if (due == 0) continue;
	if (need == 0 || TDIFF(due, need) < 0) need = due;
  }
  if (need == 0) return;
  if (poll_alarm != 0 && TDIFF(poll_alarm, need) <= 0) return;
  delta = TDIFF(need, now);
  if (delta < 1) delta = 1;
  poll_alarm = now + delta;
  if (poll_alarm == 0) poll_alarm = 1;
  cm.m_type = SET_SYNC_AL;
  cm.CLOCK_PROC_NR = FS_PROC_NR;
  cm.DELTA_TICKS = delta;
  (void) sendrec(CLOCK, &cm);
}


/*===========================================================================*
 *				uptime					     *
 *===========================================================================*/
PRIVATE clock_t uptime()
{
  message cm;
  int k;

  cm.m_type = GET_UPTIME;
  if ((k = sendrec(CLOCK, &cm)) != OK) panic("poll uptime err", k);
  return((clock_t) cm.NEW_TIME);
}
