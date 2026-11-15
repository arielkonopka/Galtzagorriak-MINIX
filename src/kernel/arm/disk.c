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
 */
#include "../kernel.h"
#include "../driver.h"
#include "../drvlib.h"
#include "../proc.h"
#include "hal.h"

#define MAX_DRIVES	1
#define NR_DEVICES	(MAX_DRIVES * DEV_PER_DRIVE)
#define SUB_PER_DRIVE	(NR_PARTITIONS * NR_PARTITIONS)
#define NR_SUBDEVS	(MAX_DRIVES * SUB_PER_DRIVE)
#define BUF_SECTORS	(BLOCK_SIZE / SECTOR_SIZE)

PRIVATE u32_t w_errors;		/* hal_disk_errors last reported */
PRIVATE struct wini {
  int present;
  unsigned open_ct;
  struct device part[DEV_PER_DRIVE];	/* hd0 = whole disk, hd1-4 */
  struct device subpart[SUB_PER_DRIVE];	/* hd1a ... hd4d */
} wini[MAX_DRIVES];

PRIVATE struct device *w_dv;		/* current device */

FORWARD _PROTOTYPE( struct device *w_prepare, (int device)		);
FORWARD _PROTOTYPE( char *w_name, (void)				);
FORWARD _PROTOTYPE( int w_schedule, (int proc_nr, struct iorequest_s *iop) );
FORWARD _PROTOTYPE( int w_do_open, (struct driver *dp, message *m_ptr)	);
FORWARD _PROTOTYPE( int w_do_close, (struct driver *dp, message *m_ptr)	);
FORWARD _PROTOTYPE( void w_geometry, (struct partition *entry)		);
FORWARD _PROTOTYPE( int w_init, (void)					);

PRIVATE struct driver w_dtab = {
  w_name,	/* current device's name */
  w_do_open,	/* open or mount request, initialize device */
  w_do_close,	/* release device */
  do_diocntl,	/* get or set a partition's geometry */
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
		printf("%s: I/O error at sector %lu\n", w_name(), sector);
		rp->p_physio = FALSE;
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

  receive(HARDWARE, &m);
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
  wn->present = TRUE;
  printf("%s: %lu MB\n", w_name(), hal_disk_sectors() >> 11);
  return(OK);
}


/*===========================================================================*
 *				w_do_open				     *
 *===========================================================================*/
PRIVATE int w_do_open(dp, m_ptr)
struct driver *dp;
message *m_ptr;
{
  struct wini *wn = &wini[0];

  if (!wn->present && w_init() != OK) return(ENXIO);
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
