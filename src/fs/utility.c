/* This file contains a few general purpose utility routines.
 *
 * The entry points into this file are
 *   clock_time:  ask the clock task for the real time
 *   copy:	  copy a block of data
 *   fetch_name:  go get a path name from user space
 *   no_sys:      reject a system call that FS does not handle
 *   panic:       something awful has occurred;  MINIX cannot continue
 *   conv2:	  do byte swapping on a 16-bit int
 *   conv4:	  do byte swapping on a 32-bit long
 */

#include "fs.h"
#include <minix/com.h>
#include <minix/boot.h>
#include <unistd.h>
#include "buf.h"
#include "file.h"
#include "fproc.h"
#include "inode.h"
#include "param.h"

PRIVATE char panicking;		/* inhibits recursive panics during sync */
PRIVATE char time_full;		/* clock_time() saturated: said so once */

/*===========================================================================*
 *				clock_time				     *
 *===========================================================================*/
PUBLIC time_t wall_time()
{
/* This routine returns the time in seconds since 1.1.1970 UTC.  MINIX is an
 * astrophysically naive system that assumes the earth rotates at a constant
 * rate and that such things as leap seconds do not exist.  The clock task
 * keeps it as an unsigned 32-bit value until the RTC driver (R6.2) gives
 * more.  The message is on the stack: no static RAM for it.
 */

  message cm;			/* not m: that is the request (param.h) */
  register int k;

  cm.m_type = GET_TIME;
  if ( (k = sendrec(CLOCK, &cm)) != OK) panic("clock_time err", k);

  return( (time_t) (ftime32_t) cm.NEW_TIME);
}


/*===========================================================================*
 *				clock_time				     *
 *===========================================================================*/
PUBLIC ftime32_t clock_time()
{
/* The time to stamp a file with: as stored, unsigned 32-bit (R2d.3).  Past
 * 2106-02-07 06:28:14 UTC it stays there instead of wrapping to 1970, and
 * the console says so once. */

  time_t t = wall_time();

  if (t < 0) return(0);
  if (t > (time_t) 0xFFFFFFFEUL) {
	if (!time_full) {
		time_full = TRUE;
		printf("FS: the time is past 2106, file times stay at 2106\n");
	}
	return(0xFFFFFFFEUL);
  }
  return((ftime32_t) t);
}


/*===========================================================================*
 *				fetch_name				     *
 *===========================================================================*/
PUBLIC int fetch_name(path, len, flag)
char *path;			/* pointer to the path in user space */
int len;			/* path length, including 0 byte */
int flag;			/* M3 means path may be in message */
{
/* Go get path and put it in 'user_path'.
 * If 'flag' = M3 and 'len' <= M3_STRING, the path is present in 'message'.
 * If it is not, go copy it from user space.
 */

  register char *rpu, *rpm;
  int r;

  /* Check name length for validity. */
  if (len <= 0) {
	err_code = EINVAL;
	return(EGENERIC);
  }

  if (len > PATH_MAX) {
	err_code = ENAMETOOLONG;
	return(EGENERIC);
  }

  if (flag == M3 && len <= M3_STRING) {
	/* Just copy the path from the message to 'user_path'. */
	rpu = &user_path[0];
	rpm = pathname;		/* contained in input message */
	do { *rpu++ = *rpm++; } while (--len);
	r = OK;
  } else {
	/* String is not contained in the message.  Get it from user space. */
	r = sys_copy(who, D, (phys_bytes) path,
		FS_PROC_NR, D, (phys_bytes) user_path, (phys_bytes) len);
  }
  return(r);
}


/*===========================================================================*
 *				no_sys					     *
 *===========================================================================*/
PUBLIC int no_sys()
{
/* Somebody has used an illegal system call number */

  return(EINVAL);
}


/*===========================================================================*
 *				panic					     *
 *===========================================================================*/
PUBLIC void panic(format, num)
char *format;			/* format string */
int num;			/* number to go with format string */
{
/* Something awful has happened.  Panics are caused when an internal
 * inconsistency is detected, e.g., a programming error or illegal value of a
 * defined constant.
 */

  if (panicking) return;	/* do not panic during a sync */
  panicking = TRUE;		/* prevent another panic during the sync */
  printf("File system panic: %s ", format);
  if (num != NO_NUM) printf("%d",num); 
  printf("\n");
  (void) do_sync();		/* flush everything to the disk */
  sys_abort(RBT_PANIC);
}


/*===========================================================================*
 *				conv2					     *
 *===========================================================================*/
PUBLIC unsigned conv2(norm, w)
int norm;			/* TRUE if no swap, FALSE for byte swap */
int w;				/* promotion of 16-bit word to be swapped */
{
/* Possibly swap a 16-bit word between 8086 and 68000 byte order. */

  if (norm) return( (unsigned) w & 0xFFFF);
  return( ((w&BYTE) << 8) | ( (w>>8) & BYTE));
}


/*===========================================================================*
 *				conv4					     *
 *===========================================================================*/
PUBLIC long conv4(norm, x)
int norm;			/* TRUE if no swap, FALSE for byte swap */
long x;				/* 32-bit long to be byte swapped */
{
/* Possibly swap a 32-bit long between 8086 and 68000 byte order. */

  unsigned lo, hi;
  long l;
  
  if (norm) return(x);			/* byte order was already ok */
  lo = conv2(FALSE, (int) x & 0xFFFF);	/* low-order half, byte swapped */
  hi = conv2(FALSE, (int) (x>>16) & 0xFFFF);	/* high-order half, swapped */
  l = ( (long) lo <<16) | hi;
  return(l);
}
