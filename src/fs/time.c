/* This file takes care of those system calls that deal with time.
 *
 * The entry points into this file are
 *   do_utime:	perform the UTIME system call
 *   do_time:	perform the TIME system call
 *   do_stime:	perform the STIME system call
 *   do_tims:	perform the TIMES system call
 */

#include "fs.h"
#include <minix/callnr.h>
#include <minix/com.h>
#include "file.h"
#include "fproc.h"
#include "inode.h"
#include "param.h"

/*===========================================================================*
 *				do_utime				     *
 *===========================================================================*/
PUBLIC int do_utime()
{
/* Perform the utime(name, timep) system call. */

  register struct inode *rip;
  register int len, r;

  /* Adjust for case of NULL 'timep'. */
  len = utime_length;
  if (len == 0) len = m.m2_i2;

  /* Temporarily open the file. */
  if (fetch_name(utime_file, len, M1) != OK) return(err_code);
  if ( (rip = eat_path(user_path)) == NIL_INODE) return(err_code);

  /* Only the owner of a file or the super_user can change its time. */
  r = OK;
  if (rip->i_uid != fp->fp_effuid && !super_user) r = EPERM;
  if (utime_length == 0 && r != OK) r = forbidden(rip, W_BIT);
  if (read_only(rip) != OK) r = EROFS;	/* not even su can touch if R/O */
  if (r == OK) {
	if (utime_length == 0) {
		rip->i_atime = clock_time();
		rip->i_mtime = rip->i_atime;
	} else {
		/* libc checked the range (R2d.2): stored as is */
		rip->i_atime = (ftime32_t) utime_actime;
		rip->i_mtime = (ftime32_t) utime_modtime;
	}
	rip->i_update = CTIME;	/* discard any stale ATIME and MTIME flags */
	rip->i_dirt = DIRTY;
  }

  put_inode(rip);
  return(r);
}


/*===========================================================================*
 *				do_time					     *
 *===========================================================================*/
PUBLIC int do_time()

{
/* Perform the time(tp) system call: the 64-bit time in two halves (R2d.2;
 * no 64-bit member in the message union). */

  time_t t = wall_time();

  reply_l1 = (long) (unsigned long) t;		/* low 32 bits */
  reply_l2 = (long) (unsigned long) (t >> 32);	/* high 32 bits */
  return(OK);
}


/*===========================================================================*
 *				do_stime				     *
 *===========================================================================*/
PUBLIC int do_stime()
{
/* Perform the stime(tp) system call. */

  message m;
  register int k;

  if (!super_user) return(EPERM);
  /* 64-bit in two halves (R2d.2).  The clock keeps 1970-2106 until the
   * RTC driver (R6.2): anything else is EINVAL, not a wrapped clock. */
  if (tp_high != 0) return(EINVAL);
  m.m_type = SET_TIME;
  m.NEW_TIME = tp;
  if ( (k = sendrec(CLOCK, &m)) != OK) panic("do_stime error", k);
  return(OK);
}


/*===========================================================================*
 *				do_tims					     *
 *===========================================================================*/
PUBLIC int do_tims()
{
/* Perform the times(buffer) system call. */

  clock_t t[5];

  sys_times(who, t);
  reply_t1 = t[0];
  reply_t2 = t[1];
  reply_t3 = t[2];
  reply_t4 = t[3];
  reply_t5 = t[4];
  return(OK);
}
