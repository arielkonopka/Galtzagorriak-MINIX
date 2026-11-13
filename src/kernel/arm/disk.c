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
#include "hal.h"

#define MAX_DRIVES	1
#define NR_DEVICES	(MAX_DRIVES * DEV_PER_DRIVE)
#define SUB_PER_DRIVE	(NR_PARTITIONS * NR_PARTITIONS)
#define NR_SUBDEVS	(MAX_DRIVES * SUB_PER_DRIVE)
#define BUF_SECTORS	(BLOCK_SIZE / SECTOR_SIZE)

PRIVATE struct wini {
  int present;
  unsigned open_ct;
  struct device part[DEV_PER_DRIVE];	/* hd0 = whole disk, hd1-4 */
  struct device subpart[SUB_PER_DRIVE];	/* hd1a ... hd4d */
} wini[MAX_DRIVES];

PRIVATE struct device *w_dv;		/* current device */
PRIVATE u8_t w_buf[BLOCK_SIZE];		/* bounce buffer */

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
  int opcode, r;

  nbytes = iop->io_nbytes;
  pos = (unsigned long) iop->io_position;
  if ((nbytes & SECTOR_MASK) != 0 || (pos & SECTOR_MASK) != 0)
	return(iop->io_nbytes = EINVAL);

  user_phys = numap(proc_nr, (vir_bytes) iop->io_buf, nbytes);
  if (user_phys == 0) return(iop->io_nbytes = EINVAL);

  opcode = iop->io_request & ~OPTIONAL_IO;

  if (pos >= w_dv->dv_size) return(OK);			/* at EOF */
  if (nbytes > w_dv->dv_size - pos) nbytes = w_dv->dv_size - pos;
  sector = (w_dv->dv_base >> SECTOR_SHIFT) + (pos >> SECTOR_SHIFT);

  while (nbytes > 0) {
	count = nbytes < sizeof(w_buf) ? nbytes : sizeof(w_buf);
	if (opcode == DEV_WRITE) {
		phys_copy(user_phys, vir2phys(w_buf), (phys_bytes) count);
		r = hal_disk_write(sector, w_buf, count >> SECTOR_SHIFT);
	} else {
		r = hal_disk_read(sector, w_buf, count >> SECTOR_SHIFT);
		if (r == 0)
			phys_copy(vir2phys(w_buf), user_phys,
							(phys_bytes) count);
	}
	if (r != 0) {
		printf("%s: I/O error at sector %lu\n", w_name(), sector);
		return(iop->io_nbytes = EIO);
	}
	sector += count >> SECTOR_SHIFT;
	user_phys += count;
	nbytes -= count;
	iop->io_nbytes -= count;
  }
  return(OK);
}


/*===========================================================================*
 *				w_init					     *
 *===========================================================================*/
PRIVATE int w_init()
{
  struct wini *wn = &wini[0];
  unsigned long sectors;

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
