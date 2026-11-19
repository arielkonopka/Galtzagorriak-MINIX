/* The WINCHESTER task for the Flipper Zero port: /dev/hd0 is the microSD
 * card (QEMU: a disk image file), /dev/hd1-4 its MBR partitions, /dev/hd1a..
 * MINIX subpartitions.  Uses the generic driver.c/drvlib.c framework.
 *
 * Transfers are polled and done directly in w_schedule(), a block at a
 * time through a bounce buffer, so nothing is held across messages.
 *
 * Positions: io_position is a signed long and dv_base/dv_size are byte
 * counts, so the original drivers break past 2 GB.  Here the position is
 * taken as unsigned and the sector is computed as base sector + offset
 * sector, which works for partitions up to 4 GB anywhere on the first 2 TB.
 *
 * The card pulled out (spec N.6): before every request the card-detect
 * line is read.  While the card is out, every request fails with ENXIO
 * (FS keeps a dirty buffer dirty on it, T17a), and the console says so
 * with a short buzz.  When a card is back it is initialised and its CID
 * compared with the one read at the first open: the same card carries on
 * (nothing lost); another card is never written: ENXIO until it is taken
 * out again and the original goes in.  A transfer that fails with the line
 * saying "no card" is ENXIO too, not EIO.
 */
#include "../kernel.h"
#include "../driver.h"
#include "../drvlib.h"
#include <sys/ioctl.h>
#include "../proc.h"
#include "hal.h"

#define MAX_DRIVES	1
#define NR_DEVICES	(MAX_DRIVES * DEV_PER_DRIVE)
#define SUB_PER_DRIVE	(NR_PARTITIONS * NR_PARTITIONS)
#define NR_SUBDEVS	(MAX_DRIVES * SUB_PER_DRIVE)
#define BUF_SECTORS	(BLOCK_SIZE / SECTOR_SIZE)

PRIVATE u32_t w_errors;		/* hal_disk_errors last reported */
PRIVATE u32_t w_id;		/* N.6: the card's id at the first open */
PRIVATE struct wini {
  int present;			/* W_* bits */
  unsigned open_ct;
  struct device part[DEV_PER_DRIVE];	/* hd0 = whole disk, hd1-4 */
  struct device subpart[SUB_PER_DRIVE];	/* hd1a ... hd4d */
} wini[MAX_DRIVES];

PRIVATE struct device *w_dv;		/* current device */
#define W_IN		1	/* initialised */
#define W_OUT		2	/* N.6: pulled out (or a failed return) */
#define W_OTHER		4	/* N.6: another card is in: never touch it */
#define W_NODET		8	/* N.6: the detect line is not to be trusted */

FORWARD _PROTOTYPE( struct device *w_prepare, (int device)		);
FORWARD _PROTOTYPE( char *w_name, (void)				);
FORWARD _PROTOTYPE( int w_schedule, (int proc_nr, struct iorequest_s *iop) );
FORWARD _PROTOTYPE( int w_do_open, (struct driver *dp, message *m_ptr)	);
FORWARD _PROTOTYPE( int w_do_close, (struct driver *dp, message *m_ptr)	);
FORWARD _PROTOTYPE( void w_geometry, (struct partition *entry)		);
FORWARD _PROTOTYPE( int w_init, (void)					);
FORWARD _PROTOTYPE( int w_card, (void)					);
FORWARD _PROTOTYPE( void w_tell, (char *what)				);
FORWARD _PROTOTYPE( int w_ioctl, (struct driver *dp, message *m_ptr)	);
FORWARD _PROTOTYPE( int w_discard, (message *m_ptr, int erase)		);
FORWARD _PROTOTYPE( void w_nap_end, (void)				);
#define DISCARD_STEP	0x20000UL	/* sectors per DISCARD: 64 MB */

PRIVATE struct driver w_dtab = {
  w_name,	/* current device's name */
  w_do_open,	/* open or mount request, initialize device */
  w_do_close,	/* release device */
  w_ioctl,	/* get or set a partition's geometry; QEMU's card hook */
  w_prepare,	/* prepare for I/O on a given minor device */
  w_schedule,	/* do the I/O */
  nop_finish,	/* nothing left to do */
  nop_cleanup,	/* no cleanup needed */
  w_geometry	/* tell the geometry of the disk */
};


/*===========================================================================*
 *				winchester_task				     *
 *===========================================================================*/
PUBLIC void winchester_task()
{
  driver_task(&w_dtab);
}


/*===========================================================================*
 *				w_prepare				     *
 *===========================================================================*/
PRIVATE struct device *w_prepare(device)
int device;
{
  if (device < NR_DEVICES) {
	w_dv = &wini[device / DEV_PER_DRIVE].part[device % DEV_PER_DRIVE];
  } else
  if ((unsigned) (device -= MINOR_hd1a) < NR_SUBDEVS) {
	w_dv = &wini[device / SUB_PER_DRIVE].subpart[device % SUB_PER_DRIVE];
  } else {
	return(NIL_DEV);
  }
  return(w_dv);
}


/*===========================================================================*
 *				w_name					     *
 *===========================================================================*/
PRIVATE char *w_name()
{
  return "sd";
}


/*===========================================================================*
 *				w_schedule				     *
 *===========================================================================*/
PRIVATE int w_schedule(proc_nr, iop)
int proc_nr;			/* process doing the request */
struct iorequest_s *iop;	/* pointer to read or write request */
{
  unsigned long pos, nbytes, sector, count;
  phys_bytes user_phys;
  struct proc *rp;
  int opcode, r;

  nbytes = iop->io_nbytes;
  pos = (unsigned long) iop->io_position;
  if ((nbytes & SECTOR_MASK) != 0 || (pos & SECTOR_MASK) != 0)
	return(iop->io_nbytes = EINVAL);

  opcode = iop->io_request & ~OPTIONAL_IO;

  /* A buffer that is only read (a write to the disk) may be constant. */
  user_phys = opcode == DEV_WRITE
	? numap_src(proc_nr, (vir_bytes) iop->io_buf, nbytes)
	: numap(proc_nr, (vir_bytes) iop->io_buf, nbytes);
  if (user_phys == 0) return(iop->io_nbytes = EFAULT);	/* not the caller's memory */
  if ((r = w_card()) != OK) return(iop->io_nbytes = r);	/* N.6 */

  if (pos >= w_dv->dv_size) return(OK);			/* at EOF */
  if (nbytes > w_dv->dv_size - pos) nbytes = w_dv->dv_size - pos;
  sector = (w_dv->dv_base >> SECTOR_SHIFT) + (pos >> SECTOR_SHIFT);

  /* Straight between the card and the caller's memory (R2a.3: no bounce
   * buffer; the SPI moves bytes, so any alignment does).  The caller must
   * stay where it is meanwhile: no shadow swap (p_physio). */
  rp = proc_addr(proc_nr);
  rp->p_physio = TRUE;
  while (nbytes > 0) {
	count = nbytes < BLOCK_SIZE ? nbytes : BLOCK_SIZE;
	if (opcode == DEV_WRITE)
		r = hal_disk_write(sector, (void *) user_phys,
						count >> SECTOR_SHIFT);
	else
		r = hal_disk_read(sector, (void *) user_phys,
						count >> SECTOR_SHIFT);
	if (hal_disk_errors != w_errors) {	/* retried: CRC, timeout */
		w_errors = hal_disk_errors;
		printf("%s: %lu transfer(s) retried in all, bus at %u MHz\n",
			w_name(), (unsigned long) w_errors, hal_disk_mhz);
	}
	if (r != 0) {
		rp->p_physio = FALSE;
		if (!(wini[0].present & W_NODET)
		    && !hal_disk_present()) {	/* N.6: pulled out meanwhile */
			(void) w_card();
			return(iop->io_nbytes = ENXIO);
		}
		printf("%s: I/O error at sector %lu\n", w_name(), sector);
		return(iop->io_nbytes = EIO);
	}
	sector += count >> SECTOR_SHIFT;
	user_phys += count;
	nbytes -= count;
	iop->io_nbytes -= count;
	/* The card is deselected: other tasks first (R0.8b), also between
	 * the requests of a scatter (an exec's text read in one piece
	 * starved the infrared ring) */
	task_yield();
  }
  rp->p_physio = FALSE;
  return(OK);
}


/*===========================================================================*
 *				hal_disk_yield				     *
 *===========================================================================*/
PUBLIC void hal_disk_yield()
{
/* The HAL waits for a busy card (R0.8b): the other tasks first. */
  task_yield();
}

/*===========================================================================*
 *				hal_disk_wait				     *
 *===========================================================================*/
PUBLIC void hal_disk_wait()
{
/* A DMA transfer runs (T4b): sleep until its interrupt, so the CPU goes
 * to whoever is ready, user processes too.  The caller (the HAL) checks
 * the hardware afterwards: a stale wakeup costs a little polling. */
  message m;

  pm_busy |= PM_DISK;		/* DMA needs the clocks: no Stop2 */
  receive(HARDWARE, &m);
  pm_busy &= ~PM_DISK;
}


PRIVATE int w_dma_int(irq)
int irq;
{
  hal_disk_dma_ack();
  interrupt(WINCHESTER);
  return(1);
}


/*===========================================================================*
 *				w_init					     *
 *===========================================================================*/
FORWARD _PROTOTYPE( int w_dma_int, (int irq) );

PRIVATE int w_init()
{
  struct wini *wn = &wini[0];
  unsigned long sectors;

  {	/* SD data by DMA, the task asleep meanwhile (T4b step 2) */
	int irq = hal_disk_dma_irq();

	if (irq >= 0) {
		put_irq_handler(irq, w_dma_int);
		enable_irq(irq);
	}
  }
  if (hal_disk_init() != 0) {
	printf("%s: no card\n", w_name());
	return(ENXIO);
  }
  sectors = hal_disk_sectors();
  /* dv_size is a 32-bit byte count: clip whole-disk access to 4 GB. */
  if (sectors > 0xFFFFFFFFUL >> SECTOR_SHIFT)
	sectors = 0xFFFFFFFFUL >> SECTOR_SHIFT;
  wn->part[0].dv_base = 0;
  wn->part[0].dv_size = sectors << SECTOR_SHIFT;
  wn->present = W_IN;
  if (hal_disk_id(&w_id) != 0) w_id = 0;	/* N.6: 0 matches no card */
  if (!hal_disk_present()) {	/* a card answers, the switch says none */
	wn->present |= W_NODET;
	printf("%s: card detect says no card: not used\n", w_name());
  }
  printf("%s: %lu MB%s\n", w_name(), hal_disk_sectors() >> 11,
	hal_disk_discard(1, 0) == 0 ? ", DISCARD" : "");	/* R12.40 */
  return(OK);
}


/*===========================================================================*
 *				w_card					     *
 *===========================================================================*/
PRIVATE int w_card()
{
/* N.6: OK if the card we started with is in and ready, else ENXIO.  One
 * GPIO read when all is well.  While it is out, every request looks at
 * the slot again (requests are rare then: FS's update, a failing program),
 * so a quick swap between two requests is never missed; W_OTHER only
 * keeps the message from repeating. */
  struct wini *wn = &wini[0];
  u32_t id;

  if (wn->present & W_NODET) return(OK);
  if (!hal_disk_present()) {
	if (!(wn->present & W_OUT)) {
		wn->present |= W_OUT;
		w_tell("SD card removed: put it back");
	}
	wn->present &= ~W_OTHER;	/* a next other card: say it again */
	return(ENXIO);
  }
  if (!(wn->present & W_OUT)) return(OK);
  if (hal_disk_init() != 0 || hal_disk_id(&id) != 0 || id != w_id) {
	/* another card, or one that does not answer (a card in another
	 * format, half in): never touched; said once until it goes */
	if (!(wn->present & W_OTHER)) {
		w_tell(hal_disk_sectors() == 0
			? "the card in the slot does not answer: put the original back"
			: "Different card: put the original back or power off");
		flipdev_notice(NOTICE_OTHER);
	}
	wn->present |= W_OTHER;
	return(ENXIO);
  }
  wn->present &= ~(W_OUT | W_OTHER);
  printf("%s: the card is back, carrying on\n", w_name());
  flipdev_notice(NOTICE_BACK);
  return(OK);
}


/*===========================================================================*
 *				w_tell					     *
 *===========================================================================*/
PRIVATE void w_tell(what)
char *what;
{
/* N.6: on the console.  The screen and the buzz are flipdev's: it reads
 * the card-detect switch every tick and keeps the notice over every
 * frame (R-N6.2: over any app; the launcher's files are on the card). */
  printf("%s: %s\n", w_name(), what);
}


/*===========================================================================*
 *				w_ioctl					     *
 *===========================================================================*/
PRIVATE int w_ioctl(dp, m_ptr)
struct driver *dp;
message *m_ptr;
{
/* DIOCFAKECARD (QEMU only, tests of N.6): 0 card in, 1 out, 2 another;
 * 3 puts the number of sectors written since boot in its int (K.7 ACK7.2).
 * DIOCDISCARD (R12.40): tell the card a range of the partition is unused.
 * DIOCERASE (R12.33): erase a range of the partition (install time). */
  int state, r;
  phys_bytes user;

  if (m_ptr->REQUEST == DIOCDISCARD) return(w_discard(m_ptr, FALSE));
  if (m_ptr->REQUEST == DIOCERASE) return(w_discard(m_ptr, TRUE));
  if (m_ptr->REQUEST != DIOCFAKECARD) return(do_diocntl(dp, m_ptr));
  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, sizeof(int));
  if (user == 0) return(EFAULT);
  phys_copy(user, vir2phys(&state), (phys_bytes) sizeof(int));
  r = hal_disk_fake(state);
  if (r < 0 || (state != 3 && r != 0)) return(EINVAL);
  if (state == 3) phys_copy(vir2phys(&r), user, (phys_bytes) sizeof(int));
  return(OK);
}


/*===========================================================================*
 *				w_discard				     *
 *===========================================================================*/
PRIVATE int w_discard(m_ptr, erase)
message *m_ptr;
int erase;			/* ERASE (R12.33) instead of DISCARD */
{
/* R12.40: DISCARD (SD 5.1) a sector range of the partition: the card marks
 * it unused, nothing is erased or written.  ENOTTY if the card has none.
 * R12.33, erase: ERASE the range instead (first-boot setup, through the
 * erase command; never at run time).  ENOTTY while the HAL has none
 * (the Flipper until T18b).
 * Done in steps of DISCARD_STEP sectors with the other tasks between them;
 * the HAL waits for the busy card by naps (hal_disk_nap), never a loop.
 * Only MM (swap, at idle) and root reach it: /dev/hd* are root's. */
  struct part_range rg;
  phys_bytes user;
  unsigned long size, sector, n;
  int r;

  if (w_prepare(m_ptr->DEVICE) == NIL_DEV) return(ENXIO);
  user = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS, sizeof(rg));
  if (user == 0) return(EFAULT);
  phys_copy(user, vir2phys(&rg), (phys_bytes) sizeof(rg));
  size = w_dv->dv_size >> SECTOR_SHIFT;
  if (rg.count == 0 || rg.first >= size || rg.count > size - rg.first)
	return(EINVAL);
  if ((r = w_card()) != OK) return(r);			/* N.6 */
  sector = (w_dv->dv_base >> SECTOR_SHIFT) + rg.first;
  while (rg.count > 0) {
	n = rg.count < DISCARD_STEP ? rg.count : DISCARD_STEP;
	r = erase ? hal_disk_erase(sector, sector + n - 1)
		  : hal_disk_discard(sector, sector + n - 1);
	if (r == -2) return(ENOTTY);
	if (r != 0) {
		if (!(wini[0].present & W_NODET) && !hal_disk_present()) {
			(void) w_card();
			return(ENXIO);
		}
		printf("%s: %s failed at sector %lu\n", w_name(),
				erase ? "erase" : "discard", sector);
		return(EIO);
	}
	sector += n;
	rg.count -= n;
	task_yield();
  }
  return(OK);
}


/*===========================================================================*
 *				hal_disk_nap				     *
 *===========================================================================*/
PUBLIC void hal_disk_nap()
{
/* The HAL waits for a card that is busy for long (DISCARD, R12.40): sleep
 * one clock tick, so user processes run and the CPU may sleep (spec 0.8).
 * The clock's watchdog wakes us as an interrupt would; a stale wakeup (the
 * DMA's) only shortens the nap. */
  message m;

  clock_mess(1, w_nap_end);
  receive(HARDWARE, &m);
}

PRIVATE void w_nap_end()
{
  interrupt(WINCHESTER);
}


/*===========================================================================*
 *				w_do_open				     *
 *===========================================================================*/
PRIVATE int w_do_open(dp, m_ptr)
struct driver *dp;
message *m_ptr;
{
  struct wini *wn = &wini[0];

  if (!(wn->present & W_IN) && w_init() != OK) return(ENXIO);
  if (w_prepare(m_ptr->DEVICE) == NIL_DEV) return(ENXIO);
  if (wn->open_ct++ == 0) partition(&w_dtab, 0, P_PRIMARY);
  return(OK);
}


/*===========================================================================*
 *				w_do_close				     *
 *===========================================================================*/
PRIVATE int w_do_close(dp, m_ptr)
struct driver *dp;
message *m_ptr;
{
  if (w_prepare(m_ptr->DEVICE) == NIL_DEV) return(ENXIO);
  wini[0].open_ct--;
  return(OK);
}


/*===========================================================================*
 *				w_geometry				     *
 *===========================================================================*/
PRIVATE void w_geometry(entry)
struct partition *entry;
{
/* Fake a geometry: 64 heads, 32 sectors, like most card readers report. */
  unsigned long sectors = wini[0].part[0].dv_size >> SECTOR_SHIFT;

  entry->heads = 64;
  entry->sectors = 32;
  entry->cylinders = sectors / (64 * 32);
}
