/* This file handles the EXEC system call.  It performs the work as follows:
 *    - see if the permissions allow the file to be executed
 *    - read the header and extract the sizes
 *    - fetch the initial args and environment from the user space
 *    - allocate the memory for the new process
 *    - copy the initial stack from MM to the process
 *    - read in the text and data segments and copy to the process
 *    - take care of setuid and setgid bits
 *    - fix up 'mproc' table
 *    - tell kernel about EXEC
 *    - save offset to initial argc (for ps)
 *
 * The entry points into this file are:
 *   do_exec:	 perform the EXEC system call
 *   find_share: find a process whose text segment can be shared
 */

#include "mm.h"
#include <sys/stat.h>
#include <minix/gzversion.h>
#include <minix/shlibc.h>
#include <minix/callnr.h>
#include <a.out.h>
#include <signal.h>
#include <string.h>
#include "mproc.h"
#include "param.h"
#if (CHIP == ARM_CM)
#include <minix/mpucover.h>
#endif

FORWARD _PROTOTYPE( void load_seg, (int fd, int seg, vir_bytes seg_bytes) );
FORWARD _PROTOTYPE( int new_mem, (struct mproc *sh_mp, vir_bytes text_bytes,
		vir_bytes data_bytes, vir_bytes bss_bytes,
		vir_bytes stk_bytes, phys_bytes tot_bytes,
		unsigned long fc_text)					);
FORWARD _PROTOTYPE( void patch_ptr, (char stack [ARG_MAX ], vir_bytes base) );
FORWARD _PROTOTYPE( int read_header, (int fd, int *ft, vir_bytes *text_bytes,
		vir_bytes *data_bytes, vir_bytes *bss_bytes,
		phys_bytes *tot_bytes, long *sym_bytes, vir_clicks sc,
		vir_bytes *pc)						);

#if (SHADOWING == 1)
FORWARD _PROTOTYPE( int relocate, (int fd, unsigned char *buf, int sep,
			vir_bytes text_bytes, vir_bytes data_bytes)	);
FORWARD _PROTOTYPE( int gather, (char *buf, vir_bytes *bytes)		);
FORWARD _PROTOTYPE( int fetch_ptrs, (vir_bytes src, char *buf, int *n,
							vir_bytes room)	);

#if (CHIP == ARM_CM)
PUBLIC char mbuf[ARG_MAX];	/* stack image; fcache.c, finst.c between execs */
#endif
#endif

/*===========================================================================*
 *				do_exec					     *
 *===========================================================================*/
PUBLIC int do_exec()
{
/* Perform the execve(name, argv, envp) call.  The stack image (argc,
 * pointers, args, environ) is put together in a buffer inside MM, then
 * copied to the new core image.  Old programs build the image themselves
 * and pass it (stack_ptr); the current library passes argv and envp
 * (stack_ptr NULL) and MM gathers the strings from the process, so the
 * process needs no free memory to exec (a shell after a big glob).
 */

  register struct mproc *rmp;
  struct mproc *sh_mp;
  int m, r, fd, ft, sn;
  static char name_buf[PATH_MAX]; /* the name of the file to exec */
  char *new_sp, *basename;
  vir_bytes src, dst, text_bytes, data_bytes, bss_bytes, stk_bytes, vsp;
  phys_bytes tot_bytes;		/* total space for program, including gap */
  long sym_bytes;
  vir_clicks sc;
  struct stat s_buf;
  vir_bytes pc;
  int reloc_bad = FALSE;
#if (CHIP == ARM_CM)
  int fc_new;			/* not cached yet: copy it after the exec */
  unsigned long fc_text;	/* flash address of the text, 0: in RAM */
  off_t text_off;
  int text_read;		/* fc_sync() has read the text already */
  int secret;			/* A_NOSWAP: a Secret: yes package */
#endif

  /* Do some validity checks. */
  rmp = mp;
  stk_bytes = (vir_bytes) stack_bytes;
  if (stk_bytes > ARG_MAX) return(ENOMEM);	/* stack too big */
  if (exec_len <= 0 || exec_len > PATH_MAX) return(EINVAL);
  if (stack_ptr == NULL) {
	/* Gather the arguments now, while the old image still exists. */
	if ((r = gather(mbuf, &stk_bytes)) != OK) return(r);
  }

  /* Get the exec file name and see if the file is executable. */
  src = (vir_bytes) exec_name;
  dst = (vir_bytes) name_buf;
  r = sys_copy(who, D, (phys_bytes) src,
		MM_PROC_NR, D, (phys_bytes) dst, (phys_bytes) exec_len);
  if (r != OK) return(r);	/* file name not in user data segment */
  tell_fs(CHDIR, who, FALSE, 0);	/* switch to the user's FS environ. */
  fd = allowed(name_buf, &s_buf, X_BIT);	/* is file executable? */
  if (fd < 0) return(fd);	/* file was not executable */

  /* Read the file header and extract the segment sizes. */
  sc = (stk_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  m = read_header(fd, &ft, &text_bytes, &data_bytes, &bss_bytes, 
					&tot_bytes, &sym_bytes, sc, &pc);
  if (m < 0) {
	close(fd);		/* something wrong with header */
	return(ENOEXEC);
  }
#if (CHIP == ARM_CM)
  secret = (m == 1);		/* never swapped in plain mode (R12.28) */
#endif
#if (CHIP == ARM_CM)
  text_off = lseek(fd, 0L, SEEK_CUR);	/* where the text is in the file */
#endif

  /* Fetch the stack from the user before destroying the old core image. */
  if (stack_ptr != NULL) {
	src = (vir_bytes) stack_ptr;
	dst = (vir_bytes) mbuf;
	r = sys_copy(who, D, (phys_bytes) src,
			MM_PROC_NR, D, (phys_bytes) dst, (phys_bytes)stk_bytes);
	if (r != OK) {
		close(fd);	/* can't fetch stack (e.g. bad virtual addr) */
		return(EACCES);
	}
  }

  /* Can the process' text be shared with that of one already running? */
#if (CHIP == ARM_CM)
  /* Not with shadowing on ARM: freeing and MPU cover of shared text in
   * RAM are not done.  Text is shared through the flash cache instead.
   */
  sh_mp = NULL;
  /* Position-independent text: run it from the flash text cache
   * (fcache.c) if it is there.  If not, it runs from RAM this time and
   * MM copies it to the cache afterwards, as idle work (R0.8c): exec never
   * waits for a flash write.
   */
  fc_text = fc_new = 0;
  /* Only files that nobody but their owner can have written, looked up
   * under their owner (S12, R1.4).  Others run with their text in RAM. */
  if (ft == SEPARATE && !(s_buf.st_mode & (S_IWGRP | S_IWOTH))) {
	/* Installed by root first (R2b.4), then the run-time cache. */
	fc_text = in_lookup(&s_buf, (unsigned long) text_bytes, fd,
						(unsigned long) text_off);
	if (fc_text == 0) fc_text = fc_lookup(text_crc, (unsigned long) text_bytes,
					(unsigned long) s_buf.st_uid);
	fc_new = (fc_text == 0);	/* to be cached once it runs */
  }
#else
  sh_mp = find_share(rmp, s_buf.st_ino, s_buf.st_dev,
						(ftime32_t) s_buf.st_ctime);
#endif

  /* Allocate new memory and release old memory.  Fix map and tell kernel. */
  r = new_mem(sh_mp, text_bytes, data_bytes, bss_bytes, stk_bytes, tot_bytes,
								fc_text);
#if (CHIP == ARM_CM)
  text_read = FALSE;
  if (r != OK && fc_new) {
	/* No room for its text in RAM (right after an update, nothing is
	 * cached yet): put the text in the cache now, the old way, and run
	 * it from flash.  The only case where exec waits for a flash write. */
	/* mbuf holds the new stack image (gather(), patched below): read
	 * the text through what is left after it, never over it (#108: the
	 * argv pointers came out as text bytes plus the stack base). */
	vir_bytes used = (stk_bytes + 7) & ~(vir_bytes) 7;

	fc_text = ARG_MAX - used < 256 ? 0 :
		fc_sync(fd, (unsigned long) text_bytes, text_crc,
			(unsigned long) s_buf.st_uid, mbuf + used,
			(int) ((ARG_MAX - used) & ~7));
	if (fc_text != 0) {
		fc_new = FALSE;
		text_read = TRUE;		/* fd is past the text now */
		r = new_mem(sh_mp, text_bytes, data_bytes, bss_bytes,
						stk_bytes, tot_bytes, fc_text);
	}
  }
#endif
  if (r != OK) {
	close(fd);		/* insufficient core or program too big */
	return(r);
  }

  /* Save file identification to allow it to be shared. */
  rmp->mp_ino = s_buf.st_ino;
  rmp->mp_dev = s_buf.st_dev;
  rmp->mp_ctime = (ftime32_t) s_buf.st_ctime;

  /* Patch up stack and copy it from MM to new core image. */
  vsp = (vir_bytes) rmp->mp_seg[S].mem_vir << CLICK_SHIFT;
  vsp += (vir_bytes) rmp->mp_seg[S].mem_len << CLICK_SHIFT;
  vsp -= stk_bytes;
  patch_ptr(mbuf, vsp);
  src = (vir_bytes) mbuf;
  r = sys_copy(MM_PROC_NR, D, (phys_bytes) src,
  			who, D, (phys_bytes) vsp, (phys_bytes)stk_bytes);
  if (r != OK) panic("do_exec stack copy err", NO_NUM);

  /* Read in text and data segments. */
#if (CHIP == ARM_CM)
  if (fc_text != 0) {
	if (!text_read)
		lseek(fd, (off_t) text_bytes, SEEK_CUR);  /* cached: skip */
  } else
#endif
  if (sh_mp != NULL) {
	lseek(fd, (off_t) text_bytes, SEEK_CUR);  /* shared: skip text */
  } else {
	load_seg(fd, T, text_bytes);
  }
  load_seg(fd, D, data_bytes);

#if (SHADOWING == 1)
  if (lseek(fd, (off_t)sym_bytes, SEEK_CUR) == (off_t) -1) ;	/* error */
  /* A bad relocation table (out of the image, unaligned, malformed) is
   * past the point of no return: the image is killed below (S2). */
  reloc_bad = relocate(fd, (unsigned char *)mbuf, ft == SEPARATE,
					text_bytes, data_bytes) < 0;
  pc += (vir_bytes) rmp->mp_seg[T].mem_vir << CLICK_SHIFT;	/* was "rp": never compiled */
#endif

#if (CHIP == ARM_CM)
  /* Run from RAM often enough: MM copies its text from this file to the
   * cache later, keeping the file open meanwhile (the program itself may
   * be long gone by then). */
  if (reloc_bad || !fc_new || !fc_ran(text_crc, (unsigned long) s_buf.st_uid)
	|| !fc_queue(fd, (unsigned long) text_off, (unsigned long) text_bytes,
				text_crc, (unsigned long) s_buf.st_uid))
#endif
  close(fd);			/* don't need exec file any more */

#if (CHIP == ARM_CM)
  /* Swap (R12.28, R12.22): set-uid, set-gid and secret images never go to
   * the card in clear; the launcher's child is the foreground app. */
  rmp->mp_flags &= ~NOSWAP;
  if (secret || (s_buf.st_mode & (I_SET_UID_BIT | I_SET_GID_BIT)))
	rmp->mp_flags |= NOSWAP;
  if (rmp->mp_flags & FGEXEC) rmp->mp_flags = (rmp->mp_flags & ~FGEXEC) | FGAPP;
#endif

  /* Take care of setuid/setgid bits. */
  if ((rmp->mp_flags & TRACED) == 0 && !reloc_bad) {
	if (s_buf.st_mode & I_SET_UID_BIT) {
		rmp->mp_effuid = s_buf.st_uid;
		tell_fs(SETUID,who, (int)rmp->mp_realuid, (int)rmp->mp_effuid);
	}
	if (s_buf.st_mode & I_SET_GID_BIT) {
		rmp->mp_effgid = s_buf.st_gid;
		tell_fs(SETGID,who, (int)rmp->mp_realgid, (int)rmp->mp_effgid);
	}
  }

  /* Save offset to initial argc (for ps) */
  rmp->mp_procargs = vsp;

  /* Fix 'mproc' fields, tell kernel that exec is done,  reset caught sigs. */
  for (sn = 1; sn <= _NSIG; sn++) {
	if (sigismember(&rmp->mp_catch, sn)) {
		sigdelset(&rmp->mp_catch, sn);
		rmp->mp_sigact[sn].sa_handler = SIG_DFL;
		sigemptyset(&rmp->mp_sigact[sn].sa_mask);
	}
  }

  rmp->mp_flags &= ~SEPARATE;	/* turn off SEPARATE bit */
  rmp->mp_flags |= ft;		/* turn it on for separate I & D files */
  new_sp = (char *) vsp;

  tell_fs(EXEC, who, 0, 0);	/* allow FS to handle FD_CLOEXEC files */

  /* System will save command line for debugging, ps(1) output, etc. */
  basename = strrchr(name_buf, '/');
  if (basename == NULL) basename = name_buf; else basename++;
#if (CHIP == ARM_CM)
  strncpy(rmp->mp_name, basename, sizeof(rmp->mp_name));
#endif
  sys_exec(who, new_sp, rmp->mp_flags & TRACED, basename, pc);
#if (CHIP == ARM_CM)
  vfork_done(rmp);		/* a vfork parent goes on (R2c.1) */
  if (reloc_bad) {		/* relocation refused: never run it */
	printf("exec: %s: bad relocation, killed\n", basename);
	sig_proc(rmp, SIGKILL);
  }
#endif
  return(OK);
}


/*===========================================================================*
 *				read_header				     *
 *===========================================================================*/
PRIVATE int read_header(fd, ft, text_bytes, data_bytes, bss_bytes, 
						tot_bytes, sym_bytes, sc, pc)
int fd;				/* file descriptor for reading exec file */
int *ft;			/* place to return ft number */
vir_bytes *text_bytes;		/* place to return text size */
vir_bytes *data_bytes;		/* place to return initialized data size */
vir_bytes *bss_bytes;		/* place to return bss size */
phys_bytes *tot_bytes;		/* place to return total size */
long *sym_bytes;		/* place to return symbol table size */
vir_clicks sc;			/* stack size in clicks */
vir_bytes *pc;			/* program entry point (initial PC) */
{
/* Read the header and extract the text, data, bss and total sizes from it. */

  int m, ct;
  vir_clicks tc, dc, s_vir, dvir;
  phys_clicks totc;
  struct exec hdr;		/* a.out header is read in here */

  /* Read the header and check the magic number.  The standard MINIX header 
   * is defined in <a.out.h>.  It consists of 8 chars followed by 6 longs.
   * Then come 4 more longs that are not used here.
   *	Byte 0: magic number 0x01
   *	Byte 1: magic number 0x03
   *	Byte 2: normal = 0x10 (not checked, 0 is OK), separate I/D = 0x20
   *	Byte 3: CPU type, Intel 16 bit = 0x04, Intel 32 bit = 0x10, 
   *            Motorola = 0x0B, Sun SPARC = 0x17
   *	Byte 4: Header length = 0x20
   *	Bytes 5-7 are not used.
   *
   *	Now come the 6 longs
   *	Bytes  8-11: size of text segments in bytes
   *	Bytes 12-15: size of initialized data segment in bytes
   *	Bytes 16-19: size of bss in bytes
   *	Bytes 20-23: program entry point
   *	Bytes 24-27: total memory allocated to program (text, data + stack)
   *	Bytes 28-31: size of symbol table in bytes
   * The longs are represented in a machine dependent order,
   * little-endian on the 8088, big-endian on the 68000.
   * The header is followed directly by the text and data segments, and the 
   * symbol table (if any). The sizes are given in the header. Only the 
   * text and data segments are copied into memory by exec. The header is 
   * used here only. The symbol table is for the benefit of a debugger and 
   * is ignored here.
   */

  if (read(fd, (char *) &hdr, A_MINHDR) != A_MINHDR) return(ENOEXEC);

  /* Check magic number, cpu type, and flags. */
  if (BADMAG(hdr)) return(ENOEXEC);
#if (CHIP == INTEL && _WORD_SIZE == 2)
  if (hdr.a_cpu != A_I8086) return(ENOEXEC);
#endif
#if (CHIP == INTEL && _WORD_SIZE == 4)
  if (hdr.a_cpu != A_I80386) return(ENOEXEC);
#endif
#if (CHIP == ARM_CM)
  if (hdr.a_cpu != A_ARMCM) return(ENOEXEC);
  /* Built for another ABI (spec J.3): 0 is a program from before the
   * stamp, ABI 1, whose 32-bit time_t ABI 2 dropped (R2d.1).  Refused
   * before anything is allocated. */
  if (hdr.a_version != GZ_ABI) return(ENOEXEC);
  /* Linked against the shared libc in flash (R2b.9): its data area is in
   * the program, so only the very build it was linked with will do.  The
   * id is in a_tbase of the long header; the library's header is read in
   * place (XIP).  Refused before anything is allocated. */
  if (hdr.a_flags & A_LIBC) {
	if ((hdr.a_hdrlen & BYTE) != sizeof(hdr)
	    || read(fd, (char *) &hdr.a_trsize, sizeof(hdr) - A_MINHDR)
					!= sizeof(hdr) - A_MINHDR
	    || SHLIBC_HDR->sh_magic != SHLIBC_MAGIC
	    || SHLIBC_HDR->sh_id != (unsigned long) hdr.a_tbase
	    || !(hdr.a_flags & A_SEP))
		return(ENOEXEC);
  }
  if ((hdr.a_flags & ~(A_NSYM | A_EXEC | A_SEP | A_LIBC | A_NOSWAP)) != 0)
	return(ENOEXEC);
#else
  if ((hdr.a_flags & ~(A_NSYM | A_EXEC | A_SEP)) != 0) return(ENOEXEC);
#endif

  *ft = ( (hdr.a_flags & A_SEP) ? SEPARATE : 0);    /* separate I & D or not */

  /* Get text and data sizes. */
  *text_bytes = (vir_bytes) hdr.a_text;	/* text size in bytes */
  *data_bytes = (vir_bytes) hdr.a_data;	/* data size in bytes */
  *bss_bytes  = (vir_bytes) hdr.a_bss;	/* bss size in bytes */
  *tot_bytes  = hdr.a_total;		/* total bytes to allocate for prog */
  *sym_bytes  = hdr.a_syms;		/* symbol table size in bytes */
#if (CHIP == ARM_CM)
  if (hdr.a_flags & A_SEP) {	/* a_syms is the CRC of the text */
	text_crc = hdr.a_syms;
	*sym_bytes = 0;
  }
#endif
  if (*tot_bytes == 0) return(ENOEXEC);

  if (*ft != SEPARATE) {

#if (SHADOWING == 0)
	/* If I & D space is not separated, it is all considered data. Text=0*/
	*data_bytes += *text_bytes;
	*text_bytes = 0;
#else
	/*
	 * Treating text as data increases the shadowing overhead.
	 * Under the assumption that programs DO NOT MODIFY TEXT
	 * we can share the text between father and child processes.
	 * This is similar to the UNIX V7 -n option of ld(1).
	 * However, for MINIX the linker did not provide alignment
	 * to click boundaries, so an incomplete text click at the end
	 * must be treated as data.
	 * Correct tot_bytes, since it excludes the text segment.
	 */
	*data_bytes += *text_bytes;
	*text_bytes = (*text_bytes >> CLICK_SHIFT) << CLICK_SHIFT;
	*data_bytes -= *text_bytes;
	*tot_bytes -= *text_bytes;
#endif

  }
  *pc = hdr.a_entry;	/* initial address to start execution */

  /* Check to see if segment sizes are feasible. */
  tc = ((unsigned long) *text_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  dc = (*data_bytes + *bss_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  totc = (*tot_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  if (dc >= totc) return(ENOEXEC);	/* stack must be at least 1 click */
  dvir = (*ft == SEPARATE ? 0 : tc);
  s_vir = dvir + (totc - sc);
  m = size_ok(*ft, tc, dc, sc, dvir, s_vir);
  ct = hdr.a_hdrlen & BYTE;		/* header length */
  if (ct > A_MINHDR) lseek(fd, (off_t) ct, SEEK_SET); /* skip unused hdr */
#if (CHIP == ARM_CM)
  if (m == OK && (hdr.a_flags & A_NOSWAP)) m = 1;	/* Secret: yes */
#endif
  return(m);
}


/*===========================================================================*
 *				new_mem					     *
 *===========================================================================*/
PRIVATE int new_mem(sh_mp, text_bytes, data_bytes, bss_bytes, stk_bytes,
							tot_bytes, fc_text)
struct mproc *sh_mp;		/* text can be shared with this process */
vir_bytes text_bytes;		/* text segment size in bytes */
vir_bytes data_bytes;		/* size of initialized data in bytes */
vir_bytes bss_bytes;		/* size of bss in bytes */
vir_bytes stk_bytes;		/* size of initial stack segment in bytes */
phys_bytes tot_bytes;		/* total memory to allocate, including gap */
unsigned long fc_text;		/* the text in the flash cache, or 0 */
{
  vir_clicks tflash_clicks;	/* text size when in flash */
/* Allocate new memory and release the old memory.  Change the map and report
 * the new map to the kernel.  Zero the new core image's bss, gap and stack.
 */

  register struct mproc *rmp;
  vir_clicks text_clicks, data_clicks, gap_clicks, stack_clicks, tot_clicks;
  phys_clicks new_base;

#if (SHADOWING == 1)
  phys_clicks base, size;
#else
  static char zero[1024];		/* used to zero bss */
  phys_bytes bytes, base, count, bss_offset;
#endif

  /* No need to allocate text if it can be shared. */
  if (sh_mp != NULL) text_bytes = 0;
#if (CHIP == ARM_CM)
  tflash_clicks = ((unsigned long) text_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  if (fc_text != 0) text_bytes = 0;	/* in the flash cache */
#endif

  /* Acquire the new memory.  Each of the 4 parts: text, (data+bss), gap,
   * and stack occupies an integral number of clicks, starting at click
   * boundary.  The data and bss parts are run together with no space.
   */

  text_clicks = ((unsigned long) text_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  data_clicks = (data_bytes + bss_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  stack_clicks = (stk_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  tot_clicks = (tot_bytes + CLICK_SIZE - 1) >> CLICK_SHIFT;
  gap_clicks = tot_clicks - data_clicks - stack_clicks;
  if ( (int) gap_clicks < 0) return(ENOMEM);

  /* Check to see if there is a hole big enough.  If so, we can risk first
   * releasing the old core image before allocating the new one, since we
   * know it will succeed.  If there is not enough, return failure.
   */
#if (CHIP == ARM_CM)
  /* R12.5: swap idle processes out to make the hole, if the header buffer
   * (mbuf's last sector, swap.c) is not part of the stack image. */
  if (text_clicks + tot_clicks > max_hole() && stk_bytes <= ARG_MAX - 512)
	sw_room(text_clicks + tot_clicks);
#endif
  if (text_clicks + tot_clicks > max_hole()) return(EAGAIN);

  /* There is enough memory for the new core image.  Release the old one. */
  rmp = mp;

#if (SHADOWING == 0)
  if (find_share(rmp, rmp->mp_ino, rmp->mp_dev, rmp->mp_ctime) == NULL) {
	/* No other process shares the text segment, so free it. */
	free_mem(rmp->mp_seg[T].mem_phys, rmp->mp_seg[T].mem_len);
  }
  /* Free the data and stack segments. */
  free_mem(rmp->mp_seg[D].mem_phys,
      rmp->mp_seg[S].mem_vir + rmp->mp_seg[S].mem_len - rmp->mp_seg[D].mem_vir);
#endif

  /* We have now passed the point of no return.  The old core image has been
   * forever lost.  The call must go through now.  Set up and report new map.
   */
#if (CHIP == ARM_CM)
  /* Program images from the top, fork copies from the bottom: see
   * alloc_mem_high() - this keeps free memory in one piece. */
  /* R12.22: the foreground app from the bottom, the rest from the top, so
   * that a swapped image usually finds its addresses free again. */
  new_base = (mp->mp_flags & FGEXEC) ? alloc_mem(text_clicks + tot_clicks)
				: alloc_mem_high(text_clicks + tot_clicks);
#else
  new_base = alloc_mem(text_clicks + tot_clicks);	/* new core image */
#endif
  if (new_base == NO_MEM) panic("MM hole list is inconsistent", NO_NUM);
#if (CHIP == ARM_CM) && (SHADOWING == 1)
  {
	/* The kernel gives the process exactly [text in RAM or data, end of
	 * stack) through the MPU; refuse a layout it cannot express (R1.6)
	 * while the old image is still there. */
	phys_clicks top = new_base + text_clicks + tot_clicks;

	if (mpu_cover((unsigned long) new_base << CLICK_SHIFT,
			(unsigned long) top << CLICK_SHIFT, NULL, NULL,
			MPUC_REGIONS) < 0) {
		free_mem(new_base, text_clicks + tot_clicks);
		return(ENOMEM);
	}
  }
#endif

#if (CHIP == ARM_CM)
  if (fc_text != 0) {
	rmp->mp_seg[T].mem_phys = (phys_clicks) (fc_text >> CLICK_SHIFT);
	rmp->mp_seg[T].mem_vir = 0;
	rmp->mp_seg[T].mem_len = tflash_clicks;
  } else
#endif
  if (sh_mp != NULL) {
	/* Share the text segment. */
	rmp->mp_seg[T] = sh_mp->mp_seg[T];
  } else {
	rmp->mp_seg[T].mem_phys = new_base;
	rmp->mp_seg[T].mem_vir = 0;
	rmp->mp_seg[T].mem_len = text_clicks;
  }
  rmp->mp_seg[D].mem_phys = new_base + text_clicks;
  rmp->mp_seg[D].mem_vir = 0;
  rmp->mp_seg[D].mem_len = data_clicks;
  rmp->mp_seg[S].mem_phys = rmp->mp_seg[D].mem_phys + data_clicks + gap_clicks;
  rmp->mp_seg[S].mem_vir = rmp->mp_seg[D].mem_vir + data_clicks + gap_clicks;
  rmp->mp_seg[S].mem_len = stack_clicks;

#if (CHIP == M68000) || (CHIP == ARM_CM)
#if (SHADOWING == 0)
  rmp->mp_seg[T].mem_vir = 0;
  rmp->mp_seg[D].mem_vir = rmp->mp_seg[T].mem_len;
  rmp->mp_seg[S].mem_vir = rmp->mp_seg[D].mem_vir + rmp->mp_seg[D].mem_len + gap_clicks;
#else
  rmp->mp_seg[T].mem_vir = rmp->mp_seg[T].mem_phys;
  rmp->mp_seg[D].mem_vir = rmp->mp_seg[D].mem_phys;
  rmp->mp_seg[S].mem_vir = rmp->mp_seg[S].mem_phys;
#endif
#endif

#if (SHADOWING == 0)
  sys_newmap(who, rmp->mp_seg);   /* report new map to the kernel */

  /* Zero the bss, gap, and stack segment. */
  bytes = (phys_bytes)(data_clicks + gap_clicks + stack_clicks) << CLICK_SHIFT;
  base = (phys_bytes) rmp->mp_seg[D].mem_phys << CLICK_SHIFT;
  bss_offset = (data_bytes >> CLICK_SHIFT) << CLICK_SHIFT;
  base += bss_offset;
  bytes -= bss_offset;

  while (bytes > 0) {
	count = MIN(bytes, (phys_bytes) sizeof(zero));
	if (sys_copy(MM_PROC_NR, D, (phys_bytes) zero,
						ABS, 0, base, count) != OK) {
		panic("new_mem can't zero", NO_NUM);
	}
	base += count;
	bytes -= count;
  }
#endif

#if (SHADOWING == 1)
  sys_fresh(who, rmp->mp_seg, (phys_clicks)(data_bytes >> CLICK_SHIFT),
			&base, &size);
  if (!(rmp->mp_flags & VFORKED)) free_mem(base, size);	/* parent's */
#endif
#if (CHIP == ARM_CM)
  if (mem_trace) mem_paint(rmp);
#endif

  return(OK);
}


/*===========================================================================*
 *				patch_ptr				     *
 *===========================================================================*/
PRIVATE void patch_ptr(stack, base)
char stack[ARG_MAX];	/* pointer to stack image within MM */
vir_bytes base;			/* virtual address of stack base inside user */
{
/* When doing an exec(name, argv, envp) call, the user builds up a stack
 * image with arg and env pointers relative to the start of the stack.  Now
 * these pointers must be relocated, since the stack is not positioned at
 * address 0 in the user's address space.
 */

  char **ap, flag;
  vir_bytes v;

  flag = 0;			/* counts number of 0-pointers seen */
  ap = (char **) stack;		/* points initially to 'nargs' */
  ap++;				/* now points to argv[0] */
  while (flag < 2) {
	if (ap >= (char **) &stack[ARG_MAX]) return;	/* too bad */
	if (*ap != NIL_PTR) {
		v = (vir_bytes) *ap;	/* v is relative pointer */
		v += base;		/* relocate it */
		*ap = (char *) v;	/* put it back */
	} else {
		flag++;
	}
	ap++;
  }
}


/*===========================================================================*
 *				load_seg				     *
 *===========================================================================*/
PRIVATE void load_seg(fd, seg, seg_bytes)
int fd;				/* file descriptor to read from */
int seg;			/* T or D */
vir_bytes seg_bytes;		/* how big is the segment */
{
/* Read in text or data from the exec file and copy to the new core image.
 * This procedure is a little bit tricky.  The logical way to load a segment
 * would be to read it block by block and copy each block to the user space
 * one at a time.  This is too slow, so we do something dirty here, namely
 * send the user space and virtual address to the file system in the upper
 * 10 bits of the file descriptor, and pass it the user virtual address
 * instead of a MM address.  The file system extracts these parameters when 
 * gets a read call from the memory manager, which is the only process that
 * is permitted to use this trick.  The file system then copies the whole 
 * segment directly to user space, bypassing MM completely.
 */

  int new_fd, bytes;
  char *ubuf_ptr;

  new_fd = (who << 8) | (seg << 6) | fd;
  ubuf_ptr = (char *) ((vir_bytes)mp->mp_seg[seg].mem_vir << CLICK_SHIFT);
  while (seg_bytes != 0) {
	bytes = (INT_MAX / BLOCK_SIZE) * BLOCK_SIZE;
	if (seg_bytes < bytes)
		bytes = (int)seg_bytes;
	if (read(new_fd, ubuf_ptr, bytes) != bytes)
		break;		/* error */
	ubuf_ptr += bytes;
	seg_bytes -= bytes;
  }
}


/*===========================================================================*
 *				find_share				     *
 *===========================================================================*/
PUBLIC struct mproc *find_share(mp_ign, ino, dev, ctime)
struct mproc *mp_ign;		/* process that should not be looked at */
ino_t ino;			/* parameters that uniquely identify a file */
dev_t dev;
ftime32_t ctime;
{
/* Look for a process that is the file <ino, dev, ctime> in execution.  Don't
 * accidentally "find" mp_ign, because it is the process on whose behalf this
 * call is made.
 */
  struct mproc *sh_mp;

  for (sh_mp = &mproc[INIT_PROC_NR]; sh_mp < &mproc[NR_PROCS]; sh_mp++) {
	if ((sh_mp->mp_flags & (IN_USE | HANGING | SEPARATE))
					!= (IN_USE | SEPARATE)) continue;
	if (sh_mp == mp_ign) continue;
	if (sh_mp->mp_ino != ino) continue;
	if (sh_mp->mp_dev != dev) continue;
	if (sh_mp->mp_ctime != ctime) continue;
	return sh_mp;
  }
  return(NULL);
}


#if (SHADOWING == 1)
/*===========================================================================*
 *				relocate				     *
 *===========================================================================*/
PRIVATE int relocate(fd, buf, sep, text_bytes, data_bytes)
int fd;				/* file descriptor to read from */
unsigned char *buf;		/* borrowed from do_exec() */
int sep;			/* separate I&D: position-independent code */
vir_bytes text_bytes;		/* the image, to check every address in */
vir_bytes data_bytes;		/* initialized data */
{
  register int n;
  register unsigned char *p, c;
  register phys_bytes off;
  register phys_bytes adr;

  /* Read in relocation info from the exec file and relocate.
   * Relocation info is in GEMDOS format. Only longs can be relocated.
   *
   * The GEMDOS format starts with a long L: the offset to the
   * beginning of text for the first long to be relocated.
   * If L==0 then no relocations have to be made.
   *
   * The long is followed by zero or more bytes. Each byte B is
   * processed separately, in one of the following ways:
   *
   * B==0:
   *	end of relocation
   * B==1:
   *	no relocation, but add 254 to the current offset
   * B==0bWWWWWWW0:
   *	B is added to the current offset and the long addressed
   *	is relocated. Note that 00000010 means 1 word distance.
   * B==0bXXXXXXX1:
   *	illegal
   */
  off = (phys_bytes)mp->mp_seg[T].mem_phys << CLICK_SHIFT;
#if (CHIP == ARM_CM)
  /* Separate I&D (position-independent code, tools/elf2mnx.py): only data
   * is relocated - the GOT and initialized pointers.  Offsets count from
   * the start of data.  Each word holds a link-time address: data was
   * linked at A_DATA_LINK, text at 0, so the value says which base to add.
   */
  if (sep) {
	phys_bytes tbase = off;
	phys_bytes dbase = (phys_bytes)mp->mp_seg[D].mem_phys << CLICK_SHIFT;
	long first;

	p = buf;
	n = read(fd, (char *)p, ARG_MAX);
	if (n < sizeof(long)) return(-1);
	first = *((long *)p);		/* first offset + 1; 0 = none */
	if (first == 0) return(0);
	if ((unsigned long) first - 1 >= data_bytes) return(-1);
	adr = dbase + first - 1;
	n -= sizeof(long);
	p += sizeof(long);
	for (;;) {
		unsigned long *w = (unsigned long *) adr;

		/* Only aligned words inside the new data (S2). */
		if ((adr & 3) != 0 || adr < dbase
				|| adr + 4 > dbase + data_bytes) return(-1);
		*w = *w >= A_DATA_LINK ? *w - A_DATA_LINK + dbase : *w + tbase;
		do {
			if (--n < 0) {
				p = buf;
				n = read(fd, (char *)p, ARG_MAX);
				if (--n < 0) return(-1);
			}
			c = *p++;
			if (c == 0) return(0);
			if (c == 1) adr += 254;
			else if (c & 1) return(-1);
			else adr += c;
		} while (c == 1);
	}
  }
#endif
  p = buf;
  n = read(fd, (char *)p, ARG_MAX);
  if (n < sizeof(long)) return(-1);	/* error */
  if (*((long *)p) == 0) return(0);	/* ok */
  if ((unsigned long) *((long *)p) >= text_bytes + data_bytes) return(-1);
  adr = off + *((long *)p);
  n -= sizeof(long);
  p += sizeof(long);
  /* Only aligned words inside the new text and data (S2). */
#define RELOC_OK(a)	(((a) & 3) == 0 && (a) >= off \
			 && (a) + 4 <= off + text_bytes + data_bytes)
  if (!RELOC_OK(adr)) return(-1);
  *((long *)adr) += off;
  while (1) {			/* once per relocation byte */
	if (--n < 0) {
		p = buf;
		n = read(fd, (char *)p, ARG_MAX);
		if (--n < 0)
			return(-1);	/* error */
	}
	c = *p++;
	if (c == 1)
		adr += 254;
	else if (c == 0)
		return(0);	/* ok */
	else if (c & 1)
		return(-1);	/* error */
	else {
		adr += c;
		if (!RELOC_OK(adr)) return(-1);
		*((long *)adr) += off;
	}
  }
}
#endif


/*===========================================================================*
 *				gather					     *
 *===========================================================================*/
PRIVATE int gather(buf, bytes)
char *buf;			/* the stack image goes here (ARG_MAX bytes) */
vir_bytes *bytes;		/* its size, rounded up to a word */
{
/* Build the initial stack image from the caller's argv and envp, the same
 * layout the library used to build: argc, argv offsets, 0, envp offsets, 0,
 * the strings.  Offsets are relative to the image (patch_ptr relocates
 * them).  Everything is fetched with sys_copy, so a bad pointer only fails
 * this exec (E2BIG if it does not fit, EFAULT if unreadable).
 */
  char **slot;
  char *hp, *end = buf + ARG_MAX;
  vir_bytes p, room;
  int nargs, nenv, i, r, n, k;

  /* The pointer arrays, each up to and including its NULL. */
  room = ARG_MAX - sizeof(char *);
  nargs = 0;
  if (exec_argv != NULL) {
	r = fetch_ptrs((vir_bytes) exec_argv, buf + sizeof(char *), &nargs,
									room);
	if (r != OK) return(r);
  }
  ((char **) buf)[1 + nargs] = NULL;
  room -= (nargs + 1) * sizeof(char *);
  if (room < sizeof(char *)) return(E2BIG);
  nenv = 0;
  if (exec_envp != NULL) {
	r = fetch_ptrs((vir_bytes) exec_envp,
		buf + (2 + nargs) * sizeof(char *), &nenv, room);
	if (r != OK) return(r);
  }
  ((char **) buf)[2 + nargs + nenv] = NULL;
  ((char **) buf)[0] = (char *) nargs;

  /* The strings, after the pointers. */
  hp = buf + (3 + nargs + nenv) * sizeof(char *);
  slot = (char **) buf + 1;
  for (i = 0; i < nargs + 1 + nenv; i++, slot++) {
	if (i == nargs) continue;		/* the NULL after argv */
	p = (vir_bytes) *slot;
	*slot = (char *) (hp - buf);
	for (;;) {				/* in chunks, to the NUL */
		n = end - hp < 64 ? end - hp : 64;
		if (n <= 0) return(E2BIG);
		r = sys_copy(who, D, (phys_bytes) p,
				MM_PROC_NR, D, (phys_bytes) hp, (phys_bytes) n);
		if (r != OK) {			/* near the segment end */
			n = 1;
			r = sys_copy(who, D, (phys_bytes) p, MM_PROC_NR, D,
					(phys_bytes) hp, (phys_bytes) 1);
			if (r != OK) return(EFAULT);
		}
		for (k = 0; k < n && hp[k] != 0; k++) ;
		if (k < n) {			/* found the end */
			hp += k + 1;
			break;
		}
		hp += n;
		p += n;
	}
  }
  *bytes = ((hp - buf) + sizeof(char *) - 1) & ~(sizeof(char *) - 1);
  if (*bytes > ARG_MAX) return(E2BIG);
  while (hp < buf + *bytes) *hp++ = 0;
  return(OK);
}


/*===========================================================================*
 *				fetch_ptrs				     *
 *===========================================================================*/
PRIVATE int fetch_ptrs(src, buf, n, room)
vir_bytes src;			/* a NULL-terminated pointer array */
char *buf;			/* where to put it */
int *n;				/* number of pointers before the NULL */
vir_bytes room;			/* bytes available at buf */
{
  char **ptr = (char **) buf;
  int got = 0, k, chunk, r;

  for (;;) {
	chunk = 16;
	if ((got + chunk) * sizeof(char *) > room)
		chunk = room / sizeof(char *) - got;
	if (chunk <= 0) return(E2BIG);
	r = sys_copy(who, D, (phys_bytes) (src + got * sizeof(char *)),
			MM_PROC_NR, D, (phys_bytes) (ptr + got),
			(phys_bytes) (chunk * sizeof(char *)));
	if (r != OK) {				/* near the segment end */
		chunk = 1;
		r = sys_copy(who, D, (phys_bytes) (src + got * sizeof(char *)),
			MM_PROC_NR, D, (phys_bytes) (ptr + got),
			(phys_bytes) sizeof(char *));
		if (r != OK) return(EFAULT);
	}
	for (k = 0; k < chunk; k++) {
		if (ptr[got + k] == NULL) {
			*n = got + k;
			return(OK);
		}
	}
	got += chunk;
  }
}
