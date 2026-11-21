/* This table has one slot per process.  It contains all the memory management
 * information for each process.  Among other things, it defines the text, data
 * and stack segments, uids and gids, and various flags.  The kernel and file
 * systems have tables that are also indexed by process, with the contents
 * of corresponding slots referring to the same process in all three.
 */

EXTERN struct mproc {
  struct mem_map mp_seg[NR_SEGS];/* points to text, data, stack */
#if (CHIP == ARM_CM)
  char mp_name[8];		/* program name, for the memory trace */
#endif
  char mp_exitstatus;		/* storage for status when process exits */
  char mp_sigstatus;		/* storage for signal # for killed procs */
  pid_t mp_pid;			/* process id */
  pid_t mp_procgrp;		/* pid of process group (used for signals) */
  pid_t mp_wpid;		/* pid this process is waiting for */
  int mp_parent;		/* index of parent process */

  /* Real and effective uids and gids. */
  uid_t mp_realuid;		/* process' real uid */
  uid_t mp_effuid;		/* process' effective uid */
  gid_t mp_realgid;		/* process' real gid */
  gid_t mp_effgid;		/* process' effective gid */
  dev_t mp_dev;			/* device number of the program's file system;
				 * here, beside the 16-bit fields, so the
				 * 32-bit mp_ino costs no padding (spec O.4) */

  /* File identification for sharing. */
  ino_t mp_ino;			/* inode number of file */
  ftime32_t mp_ctime;		/* inode changed time (as stored, R2d.3) */

  /* Signal handling information. */
  sigset_t mp_ignore;		/* 1 means ignore the signal, 0 means don't */
  sigset_t mp_catch;		/* 1 means catch the signal, 0 means don't */
  sigset_t mp_sigmask;		/* signals to be blocked */
  sigset_t mp_sigmask2;		/* saved copy of mp_sigmask */
  sigset_t mp_sigpending;	/* signals being blocked */
  struct sigaction mp_sigact[_NSIG + 1]; /* as in sigaction(2) */
  vir_bytes mp_sigreturn; 	/* address of C library __sigreturn function */

  /* Backwards compatibility for signals. */
  sighandler_t mp_func;		/* all sigs vectored to a single user fcn */

  unsigned mp_flags;		/* flag bits */
  vir_bytes mp_procargs;        /* ptr to proc's initial stack arguments */
} mproc[NR_PROCS];

/* Flag values */
#define IN_USE           001	/* set when 'mproc' slot in use */
#define WAITING          002	/* set by WAIT system call */
#define HANGING          004	/* set by EXIT system call */
#define PAUSED           010	/* set by PAUSE system call */
#define ALARM_ON         020	/* set when SIGALRM timer started */
#define SEPARATE	 040	/* set if file is separate I & D space */
#define	TRACED		0100	/* set if process is to be traced */
#define STOPPED		0200	/* set if process stopped for tracing */
#define SIGSUSPENDED 	0400	/* set by SIGSUSPEND system call */
#define VFORKED		01000	/* a vfork child: runs in its parent's memory
				 * until exec or exit (R2c.1) */
#define VFORKING	02000	/* waits in vfork for that child; signals to
				 * it wait too (its stack is in use) */
#define SWAPPED		04000	/* image on the swap device (R12.4) */
#define NOSWAP	       010000	/* set-uid/set-gid or Secret: yes image: never
				 * swapped in plain mode (R12.28) */
#define FGEXEC	       020000	/* its next child's exec is the foreground app
				 * (launcher, R12.22); passed on by fork */
#define FGAPP	       040000	/* the foreground app or a child of it: placed
				 * low, never swapped (R12.22, AC12.R3) */
#define SWAPBAD	      0100000	/* swap-in failed: to be killed (R12.25) */
#define SWAPHOT	      0200000	/* woken from the card: the reserve waits
				 * 120 s, not 10 s, once (R12.30) */
#define SWAPHDR	      0400000	/* its slot's header on the card describes
				 * the chunks there: compare, write only the
				 * changed ones (R12.29) */

#define BLKWAIT	     01000000	/* a background MI_BLK request waits for idle
				 * (sw_step, R12.37); no reply yet */

#define NIL_MPROC ((struct mproc *) 0)
