/* This file contains the device dependent part of the drivers for the
 * following special files:
 *     /dev/null	- null device (data sink)
 *     /dev/mem		- absolute memory
 *     /dev/kmem	- kernel virtual memory
 *     /dev/ram		- RAM disk
 *     /dev/random	- hardware random numbers (ARM_CM: the STM32 RNG)
 *     /dev/zero	- zeroes (ARM_CM)
 *
 * The file contains one entry point:
 *
 *   mem_task:	main entry when system is brought up
 *
 *  Changes:
 *	20 Apr  1992 by Kees J. Bot: device dependent/independent split
 */

#include "kernel.h"
#include "driver.h"
#include <sys/ioctl.h>
#include <string.h>

#if (CHIP == ARM_CM)
#include "hal.h"
#define NR_RAMS            7	/* + /dev/random, /dev/zero, /dev/rom */
/* /dev/random and /dev/zero need no geometry (they have no end): five
 * entries, /dev/rom in the fifth (static RAM, rule 0.1). */
#define NR_GEOM            5
#define GEOM(d)	((d) == ROM_DEV ? 4 : (d) == RANDOM_DEV || (d) == ZERO_DEV \
							? NULL_DEV : (d))
#else
#define NR_RAMS            4	/* number of RAM-type devices */
#define NR_GEOM            NR_RAMS
#define GEOM(d)	(d)
#endif

PRIVATE struct device m_geom[NR_GEOM];	/* Base and size of each RAM disk */
PRIVATE int m_device;		/* current device */

FORWARD _PROTOTYPE( struct device *m_prepare, (int device) );
FORWARD _PROTOTYPE( int m_schedule, (int proc_nr, struct iorequest_s *iop) );
#if (CHIP == ARM_CM)
FORWARD _PROTOTYPE( int m_random, (int device, int opcode, phys_bytes user_phys,
						struct iorequest_s *iop) );
#endif
FORWARD _PROTOTYPE( int m_do_open, (struct driver *dp, message *m_ptr) );
FORWARD _PROTOTYPE( void m_init, (void) );
FORWARD _PROTOTYPE( int m_ioctl, (struct driver *dp, message *m_ptr) );
FORWARD _PROTOTYPE( void m_geometry, (struct partition *entry) );


/* Entry points to this driver. */
PRIVATE struct driver m_dtab = {
  no_name,	/* current device's name */
  m_do_open,	/* open or mount */
  do_nop,	/* nothing on a close */
  m_ioctl,	/* specify ram disk geometry */
  m_prepare,	/* prepare for I/O on a given minor device */
  m_schedule,	/* do the I/O */
  nop_finish,	/* schedule does the work, no need to be smart */
  nop_cleanup,	/* nothing's dirty */
  m_geometry,	/* memory device "geometry" */
};


/*===========================================================================*
 *				mem_task				     *
 *===========================================================================*/
PUBLIC void mem_task()
{
  m_init();
  driver_task(&m_dtab);
}


/*===========================================================================*
 *				m_prepare				     *
 *===========================================================================*/
PRIVATE struct device *m_prepare(device)
int device;
{
/* Prepare for I/O on a device. */

  if (device < 0 || device >= NR_RAMS) return(NIL_DEV);
  m_device = device;

  return(&m_geom[GEOM(device)]);
}


#if (CHIP == ARM_CM)
/*===========================================================================*
 *				m_random				     *
 *===========================================================================*/
PRIVATE int __attribute__((noinline)) m_random(device, opcode, user_phys, iop)
int device, opcode;
phys_bytes user_phys;
struct iorequest_s *iop;
{
/* /dev/random and /dev/zero.  A function of its own: its buffer stays off
 * the MEMORY task's small stack for every other device (/dev/rom is read
 * by FS as a block device: do_vrdwt + m_schedule). */
  int done = FALSE, count = iop->io_nbytes;

  if (opcode == DEV_WRITE) {	/* accepted and dropped */
	iop->io_nbytes -= count;
	return(OK);
  }
  if (device == ZERO_DEV) {
	memset((void *) user_phys, 0, (size_t) count);
	iop->io_nbytes -= count;
	return(OK);
  }
  while (count > 0) {		/* the hardware generator */
	u32_t w[16];
	int i, n = count < (int) sizeof(w) ? count : (int) sizeof(w);

	for (i = 0; i < (n + 3) / 4; i++)
		if (hal_random(&w[i]) < 0) {
			if (!done) return(iop->io_nbytes = EIO);
			return(OK);	/* a short read */
		}
	phys_copy(vir2phys(w), user_phys, (phys_bytes) n);
	user_phys += n;
	count -= n;
	iop->io_nbytes -= n;
	done = TRUE;
  }
  return(OK);
}
#endif


/*===========================================================================*
 *				m_schedule				     *
 *===========================================================================*/
PRIVATE int m_schedule(proc_nr, iop)
int proc_nr;			/* process doing the request */
struct iorequest_s *iop;	/* pointer to read or write request */
{
/* Read or write /dev/null, /dev/mem, /dev/kmem, or /dev/ram. */

  int device, count, opcode;
  phys_bytes mem_phys, user_phys;
  struct device *dv;

  /* Type of request */
  opcode = iop->io_request & ~OPTIONAL_IO;

  /* Get minor device number and check for /dev/null. */
  device = m_device;
  dv = &m_geom[GEOM(device)];

  /* Determine address where data is to go or to come from. */
  user_phys = opcode == DEV_WRITE	/* a source may be constant */
	? numap_src(proc_nr, (vir_bytes) iop->io_buf,
						(vir_bytes) iop->io_nbytes)
	: numap(proc_nr, (vir_bytes) iop->io_buf,
						(vir_bytes) iop->io_nbytes);
  if (user_phys == 0) return(iop->io_nbytes = EFAULT);	/* not the caller's memory */

#if (CHIP == ARM_CM)
  if (device == RANDOM_DEV || device == ZERO_DEV)	/* no end */
	return(m_random(device, opcode, user_phys, iop));
  /* /dev/rom: the default settings in flash (L.2), read only */
  if (device == ROM_DEV && opcode == DEV_WRITE)
	return(iop->io_nbytes = EROFS);
#endif

  if (device == NULL_DEV) {
	/* /dev/null: Black hole. */
	if (opcode == DEV_WRITE) iop->io_nbytes = 0;
	count = 0;
  } else {
	/* /dev/mem, /dev/kmem, /dev/ram or /dev/rom: Check for EOF */
	if (iop->io_position >= dv->dv_size) return(OK);
	count = iop->io_nbytes;
	if (iop->io_position + count > dv->dv_size)
		count = dv->dv_size - iop->io_position;
  }

  /* Set up 'mem_phys' for /dev/mem, /dev/kmem, or /dev/ram */
  mem_phys = dv->dv_base + iop->io_position;
#if (CHIP == ARM_CM)
  /* Only where the board has memory: a hole or a peripheral would be a
   * bus fault in this task, i.e. a panic (dd skip= reads from 0 on). */
  if ((device == MEM_DEV || device == KMEM_DEV) && count > 0) {
	count = (int) hal_mem_span((u32_t) mem_phys, (u32_t) count,
						opcode == DEV_WRITE);
	if (count == 0) return(iop->io_nbytes = EIO);
  }
#endif

  /* Book the number of bytes to be transferred in advance. */
  iop->io_nbytes -= count;

  if (count == 0) return(OK);

  /* Copy the data. */
  if (opcode == DEV_READ)
	phys_copy(mem_phys, user_phys, (phys_bytes) count);
  else
	phys_copy(user_phys, mem_phys, (phys_bytes) count);

  return(OK);
}


/*============================================================================*
 *				m_do_open				      *
 *============================================================================*/
PRIVATE int m_do_open(dp, m_ptr)
struct driver *dp;
message *m_ptr;
{
/* Check device number on open.  Give I/O privileges to a process opening
 * /dev/mem or /dev/kmem.
 */

  if (m_prepare(m_ptr->DEVICE) == NIL_DEV) return(ENXIO);
#if (CHIP == ARM_CM)
  /* /dev/rom is never opened or mounted for writing (L.2) */
  if (m_device == ROM_DEV && (m_ptr->COUNT & W_BIT)) return(EROFS);
#endif

#if (CHIP == INTEL)
  if (m_device == MEM_DEV || m_device == KMEM_DEV)
	enable_iop(proc_addr(m_ptr->PROC_NR));
#endif

  return(OK);
}


/*===========================================================================*
 *				m_init					     *
 *===========================================================================*/
PRIVATE void m_init()
{
  /* Initialize this task. */
  extern int _end;

  m_geom[KMEM_DEV].dv_base = vir2phys(0);
  m_geom[KMEM_DEV].dv_size = vir2phys(&_end);

#if (CHIP == INTEL)
  if (!protected_mode) {
	m_geom[MEM_DEV].dv_size =   0x100000;	/* 1M for 8086 systems */
  } else {
#if _WORD_SIZE == 2
	m_geom[MEM_DEV].dv_size =  0x1000000;	/* 16M for 286 systems */
#else
	m_geom[MEM_DEV].dv_size = 0xFFFFFFFF;	/* 4G-1 for 386 systems */
#endif
  }
#else /* !(CHIP == INTEL) */
#if (CHIP == M68000) || (CHIP == ARM_CM)
  m_geom[MEM_DEV].dv_size = MEM_BYTES;
#if (CHIP == ARM_CM)
  /* /dev/rom: where make base-flash put the settings image (L.2) */
  m_geom[GEOM(ROM_DEV)].dv_base = hal_rom_base();
  m_geom[GEOM(ROM_DEV)].dv_size = HAL_ROM_SIZE;
#endif
#else /* !(CHIP == M68000) */
#error /* memory limit not set up */
#endif /* !(CHIP == M68000) */
#endif /* !(CHIP == INTEL) */
}


/*===========================================================================*
 *				m_ioctl					     *
 *===========================================================================*/
PRIVATE int m_ioctl(dp, m_ptr)
struct driver *dp;
message *m_ptr;			/* pointer to read or write message */
{
/* Set parameters for one of the RAM disks. */

  unsigned long bytesize;
  unsigned base, size;
  struct memory *memp;
  static struct psinfo psinfo = { NR_TASKS, NR_PROCS, (vir_bytes) proc, 0, 0 };
  phys_bytes psinfo_phys;

  switch (m_ptr->REQUEST) {
  case MIOCRAMSIZE:
	/* FS sets the RAM disk size. */
	if (m_ptr->PROC_NR != FS_PROC_NR) return(EPERM);

	bytesize = m_ptr->POSITION * BLOCK_SIZE;
	size = (bytesize + CLICK_SHIFT-1) >> CLICK_SHIFT;

	/* Find a memory chunk big enough for the RAM disk. */
	memp= &mem[NR_MEMS];
	while ((--memp)->size < size) {
		if (memp == mem) panic("RAM disk is too big", NO_NUM);
	}
	base = memp->base;
	memp->base += size;
	memp->size -= size;

	m_geom[RAM_DEV].dv_base = (unsigned long) base << CLICK_SHIFT;
	m_geom[RAM_DEV].dv_size = bytesize;
	break;
  case MIOCSPSINFO:
	/* MM or FS set the address of their process table. */
	if (m_ptr->PROC_NR == MM_PROC_NR) {
		psinfo.mproc = (vir_bytes) m_ptr->ADDRESS;
	} else
	if (m_ptr->PROC_NR == FS_PROC_NR) {
		psinfo.fproc = (vir_bytes) m_ptr->ADDRESS;
	} else {
		return(EPERM);
	}
	break;
  case MIOCGPSINFO:
	/* The ps program wants the process table addresses. */
	psinfo_phys = numap(m_ptr->PROC_NR, (vir_bytes) m_ptr->ADDRESS,
							sizeof(psinfo));
	if (psinfo_phys == 0) return(EFAULT);
	phys_copy(vir2phys(&psinfo), psinfo_phys, (phys_bytes) sizeof(psinfo));
	break;
  default:
  	return(do_diocntl(&m_dtab, m_ptr));
  }
  return(OK);
}


/*============================================================================*
 *				m_geometry				      *
 *============================================================================*/
PRIVATE void m_geometry(entry)
struct partition *entry;
{
  /* Memory devices don't have a geometry, but the outside world insists. */
  entry->cylinders = (m_geom[GEOM(m_device)].dv_size >> SECTOR_SHIFT)
							/ (64 * 32);
  entry->heads = 64;
  entry->sectors = 32;
}
