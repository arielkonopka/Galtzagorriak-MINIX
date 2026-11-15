#ifndef _TYPE_H
#define _TYPE_H
#ifndef _MINIX_TYPE_H
#define _MINIX_TYPE_H

/* Type definitions. */
typedef unsigned int vir_clicks; /* virtual  addresses and lengths in clicks */
typedef unsigned long phys_bytes;/* physical addresses and lengths in bytes */
typedef unsigned int phys_clicks;/* physical addresses and lengths in clicks */

#if (CHIP == INTEL)
typedef unsigned int vir_bytes;	/* virtual addresses and lengths in bytes */
#endif

#if (CHIP == M68000)
typedef unsigned long vir_bytes;/* virtual addresses and lengths in bytes */
#endif

#if (CHIP == SPARC)
typedef unsigned long vir_bytes;/* virtual addresses and lengths in bytes */
#endif

#if (CHIP == ARM_CM)
typedef unsigned long vir_bytes;/* virtual addresses and lengths in bytes */
#endif

/* Types relating to messages. */
#define M1                 1
#define M3                 3
#define M4                 4
#define M3_STRING         14

typedef struct {int m1i1, m1i2, m1i3; char *m1p1, *m1p2, *m1p3;} mess_1;
typedef struct {int m2i1, m2i2, m2i3; long m2l1, m2l2; char *m2p1;} mess_2;
typedef struct {int m3i1, m3i2; char *m3p1; char m3ca1[M3_STRING];} mess_3;
typedef struct {long m4l1, m4l2, m4l3, m4l4, m4l5;} mess_4;
typedef struct {char m5c1, m5c2; int m5i1, m5i2; long m5l1, m5l2, m5l3;}mess_5;
typedef struct {int m6i1, m6i2, m6i3; long m6l1; sighandler_t m6f1;} mess_6;

typedef struct {
  int m_source;			/* who sent the message */
  int m_type;			/* what kind of message is it */
  union {
	mess_1 m_m1;
	mess_2 m_m2;
	mess_3 m_m3;
	mess_4 m_m4;
	mess_5 m_m5;
	mess_6 m_m6;
  } m_u;
} message;

/* The following defines provide names for useful members. */
#define m1_i1  m_u.m_m1.m1i1
#define m1_i2  m_u.m_m1.m1i2
#define m1_i3  m_u.m_m1.m1i3
#define m1_p1  m_u.m_m1.m1p1
#define m1_p2  m_u.m_m1.m1p2
#define m1_p3  m_u.m_m1.m1p3

#define m2_i1  m_u.m_m2.m2i1
#define m2_i2  m_u.m_m2.m2i2
#define m2_i3  m_u.m_m2.m2i3
#define m2_l1  m_u.m_m2.m2l1
#define m2_l2  m_u.m_m2.m2l2
#define m2_p1  m_u.m_m2.m2p1

#define m3_i1  m_u.m_m3.m3i1
#define m3_i2  m_u.m_m3.m3i2
#define m3_p1  m_u.m_m3.m3p1
#define m3_ca1 m_u.m_m3.m3ca1

#define m4_l1  m_u.m_m4.m4l1
#define m4_l2  m_u.m_m4.m4l2
#define m4_l3  m_u.m_m4.m4l3
#define m4_l4  m_u.m_m4.m4l4
#define m4_l5  m_u.m_m4.m4l5

#define m5_c1  m_u.m_m5.m5c1
#define m5_c2  m_u.m_m5.m5c2
#define m5_i1  m_u.m_m5.m5i1
#define m5_i2  m_u.m_m5.m5i2
#define m5_l1  m_u.m_m5.m5l1
#define m5_l2  m_u.m_m5.m5l2
#define m5_l3  m_u.m_m5.m5l3

#define m6_i1  m_u.m_m6.m6i1
#define m6_i2  m_u.m_m6.m6i2
#define m6_i3  m_u.m_m6.m6i3
#define m6_l1  m_u.m_m6.m6l1
#define m6_f1  m_u.m_m6.m6f1

struct mem_map {
  vir_clicks mem_vir;		/* virtual address */
  phys_clicks mem_phys;		/* physical address */
  vir_clicks mem_len;		/* length */
};

struct iorequest_s {
  long io_position;		/* position in device file (really off_t) */
  char *io_buf;			/* buffer in user space */
  int io_nbytes;		/* size of request */
  unsigned short io_request;	/* read, write (optionally) */
};
#endif /* _TYPE_H */

typedef struct {
  vir_bytes iov_addr;		/* address of an I/O buffer */
  vir_bytes iov_size;		/* sizeof an I/O buffer */
} iovec_t;

typedef struct {
  vir_bytes cpv_src;		/* src address of data */
  vir_bytes cpv_dst;		/* dst address of data */
  vir_bytes cpv_size;		/* size of data */
} cpvec_t;

/* MM passes the address of a structure of this type to KERNEL when
 * do_sendsig() is invoked as part of the signal catching mechanism.
 * The structure contain all the information that KERNEL needs to build
 * the signal stack.
 */
struct sigmsg {
  int sm_signo;			/* signal number being caught */
  unsigned long sm_mask;	/* mask to restore when handler returns */
  vir_bytes sm_sighandler;	/* address of handler */
  vir_bytes sm_sigreturn;	/* address of _sigreturn in C library */
  vir_bytes sm_stkptr;		/* user stack pointer */
};

#define MESS_SIZE (sizeof(message))	/* might need usizeof from fs here */
#define NIL_MESS ((message *) 0)

struct psinfo {		/* information for the ps(1) program */
  u16_t nr_tasks, nr_procs;	/* NR_TASKS and NR_PROCS constants. */
  vir_bytes proc, mproc, fproc;	/* addresses of the main process tables. */
};

#if (CHIP == ARM_CM)
/* Stack use of one kernel task, from MEMINFO with m1_i2 = MI_KSTACKS. */
struct kstack {
  char name[8];			/* task name */
  u16_t size;			/* stack size in bytes */
  u16_t used;			/* high-water mark in bytes */
};
#define MI_KSTACKS	1
#define MI_TRACE	2	/* m1_i1 = 1/0: report memory use at exit */
#define MI_FCACHE	3	/* flash text cache: struct fcinfo to m1_p1 */
#define MI_RESET	5	/* reply: cause of the last reset (RCC_CSR >> 24),
			 * 0x100: dev mode (a watchdog reset goes to DFU) */
#define MI_FCBUDGET	6	/* struct fccount to m1_p1; m1_i1 >= 0 (root):
			 * the run-time cache's erase budget, pages a
			 * day (R2b.11; 0: no caching) */
#define MI_INSTALL	4	/* root: m1_p1 path, m1_i1 its length, m1_i3 0:
			 * install its text in flash (reply 1: call again
			 * with m1_i3 4, one flash step each, until not 1),
			 * 1: remove it, 2: bind, 3: count unbound */

/* sys_flash() operations. */
#define FL_INFO		0	/* cache region: base, size; with addr: */
#define FL_LAYOUT_INST	1	/*   the installed-programs region */
#define FL_LAYOUT_SFSA	2	/*   where CPU2's flash starts */
#define FL_COUNTS	3	/*   pages erased in all: cache, installed */
#define FL_LAYOUT_ROM	5	/*   the settings image (/dev/rom, L.2) */
#define FL_BUDGET	4	/*   cache: budget, erased today; len = new
				 *   budget + 1 sets it (R2b.11) */
#define FL_ERASE	1	/* erase the page at addr */
#define FL_WRITE	2	/* program len bytes at addr */
#define FL_BG		0x10	/* or'ed in: background work, EAGAIN while a
				 * user process waits for the CPU or a key or
				 * button came in the last second (T4) */

/* Flash text cache summary (MEMINFO, MI_FCACHE). */
struct fcinfo {
  unsigned long size;		/* bytes in the cache's data pages */
  unsigned long used;		/* bytes in pages of live entries */
  int entries;			/* programs in the cache */
  int running;			/* of them in use by a process now */
  unsigned long cache_base;	/* where the cache is */
  unsigned long inst_base, inst_size;	/* installed programs (Phase 2b) */
  unsigned long inst_used;	/* bytes in pages of installed programs */
  int inst_entries;		/* programs installed */
  unsigned long sfsa;		/* CPU2's flash starts here */
};

struct fccount {		/* MI_FCBUDGET (R2b.11) */
  unsigned long erased;		/* run-time cache pages erased in all */
  unsigned long inst_erased;	/* installed-region pages erased in all */
  unsigned long today;		/* cache pages erased today */
  unsigned long budget;		/* the cache's daily budget, pages */
};
#endif

#endif /* _MINIX_TYPE_H */
