/* If there were any type definitions local to the Memory Manager, they would
 * be here.  This file is included only for symmetry with the kernel and File
 * System, which do have some local type definitions.
 */


#if (CHIP == ARM_CM)
/* The one piece of flash work in progress (fcache.c owns it): a copy into
 * the run-time cache, or an install into the installed region (finst.c),
 * which borrows it so that an install costs no RAM of its own. */
enum { J_IDLE, J_RESERVE, J_ERASE, J_PROGRAM, J_RECORD, J_COMPACT,
	J_COMPACT_COPY, J_COMPACT_DROP,
	J_INST_ERASE, J_INST_PROGRAM, J_INST_RECORD };	/* finst.c */
#define J_INSTALLING(s)	((s) >= J_INST_ERASE)
struct mmjob {			/* packed: MM's static RAM (rule 0.1) */
  unsigned char state;
  unsigned char fd;		/* the program file, kept open */
  unsigned char unformatted:1;	/* cache: no valid directory yet */
  unsigned char dir:1;		/* cache: active directory page (0, 1) */
  unsigned char page;		/* first reserved page (of the cache or the
				 * installed region); cache compaction: next
				 * record (they are never needed together) */
  unsigned short toff, owner;	/* text offset in the file; cache: owner
				 * (key); install: the caller's slot */
  unsigned long size, crc;	/* the text; cache: key; install: ctime */
  unsigned long done;		/* bytes erased/programmed, records */
  unsigned long vcrc;		/* cache: CRC-32 of the copy so far;
				 * install: the caller's pid */
};
extern struct mmjob mjob;
#endif
