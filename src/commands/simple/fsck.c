/* Hacks for version 1.6 */					

#define INODES_PER_BLOCK V2_INODES_PER_BLOCK
#define INODE_SIZE ((int) V2_INODE_SIZE)
#define WORDS_PER_BLOCK (BLOCK_SIZE / (int) sizeof(bitchunk_t))
#define MAX_ZONES (V2_NR_DZONES+V2_INDIRECTS+(long)V2_INDIRECTS*V2_INDIRECTS)
#define NR_DZONE_NUM V2_NR_DZONES
#define NR_INDIRECTS V2_INDIRECTS
#define NR_ZONE_NUMS V2_NR_TZONES
#define ZONE_NUM_SIZE V2_ZONE_NUM_SIZE
#define bit_nr bit_t
#define block_nr block_t
#define d_inode d2_inode
#define d_inum d_ino
#define dir_struct struct direct
#define i_mode d2_mode
#define i_nlinks d2_nlinks
#define i_size d2_size
#define i_zone d2_zone
#define zone_nr zone_t

/* fsck - file system checker		Author: Robbert van Renesse */

/* Modified by Norbert Schlenker
*   Removed vestiges of standalone/DOS versions:
*     - various unused variables and buffers removed
*     - now uses library functions rather than private internal routines
*     - bytewise structure copies replaced by structure assignment
*     - fixed one bug with 14 character file names
*     - other small tweaks for speed
*
* Modified by Lars Fredriksen at the request of Andy Tanenbaum, 90-03-10.
*   Removed -m option, by which fsck could be told to make a file
*   system on a 360K floppy.  The code had limited utility, was buggy,
*   and failed due to a bug in the ACK C compiler.  Use mkfs instead!
*
* Galtzagorriak (spec N.3, N.4, T17a):
*   -p  preen: nothing to do (no output) when the super block says CLEAN
*       and not CHECK; else check and repair as -a.
*   -v  for the Flipper's 32x10 console: a header, the phase with a
*       spinner and a percentage redrawn in place, one short line per
*       repair, a summary.  The long messages go to /dev/null.
*   -m KB  memory for the maps (fsck_mem, default 64).  The computed zone
*       map is a window of the zones; more windows mean more passes.
*   -L file  append the -v lines to file.  -k  Back (on /dev/buttons)
*       stops the check (exit 8, CHECK stays set).
*   Exit status as BSD: 0 clean, 1 repaired, 2 the mounted root repaired
*   (reboot now), 4 errors left, 8 not checked.  After a check with
*   repairs allowed: CHECK = 0, and CLEAN = 1 if not mounted.
*   The disk maps are compared one block at a time; spec_imap/spec_zmap
*   only with -c/-i/-z; link counts up to the highest inode in use.
*/

#include <sys/types.h>
#include <sys/dir.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <ctype.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <minix/config.h>
#include <minix/const.h>
#include <minix/type.h>
#include "../../fs/const.h"
#include "../../fs/inode.h"
#include "../../fs/type.h"
#include <minix/fslib.h>

#undef printf			/* defined as printk in "../fs/const.h" */

#include <stdio.h>

#define BITSHIFT	  4	/* = log2(#bits(int)) */

#define MAXPRINT	  8	/* max. number of error lines in chkmap */
#define MAXDIRSIZE     5000	/* max. size of a reasonable directory */
#define CINDIR		128	/* number of indirect zno's read at a time */
#define CDIRECT		 16	/* number of dir entries read at a time */

/* Macros for handling bitmaps.  Now bit_t is long, these are bulky and the
 * type demotions produce a lot of lint.  The explicit demotion in POWEROFBIT
 * is for efficiency and assumes 2's complement ints.  Lint should be clever
 * enough not to warn about it since BITMASK is small, but isn't.  (It would
 * be easier to get right if bit_t was was unsigned (long) since then there
 * would be no danger from wierd sign representations.  Lint doesn't know
 * we only use non-negative bit numbers.) There will usually be an implicit
 * demotion when WORDOFBIT is used as an array index.  This should be safe
 * since memory for bitmaps will run out first.
 */
#define BITMASK		((1 << BITSHIFT) - 1)
#define WORDOFBIT(b)	((b) >> BITSHIFT)
#define POWEROFBIT(b)	(1 << ((int) (b) & BITMASK))
#define setbit(w, b)	(w[WORDOFBIT(b)] |= POWEROFBIT(b))
#define clrbit(w, b)	(w[WORDOFBIT(b)] &= ~POWEROFBIT(b))
#define bitset(w, b)	(w[WORDOFBIT(b)] & POWEROFBIT(b))
/* The spec_ maps exist only with -c, -i or -z (spec N.4). */
#define specset(w, b)	do { if (w) setbit(w, b); } while (0)
#define specbit(w, b)	((w) != 0 && bitset(w, b))

#define ZONE_CT 	360	/* default zones  (when making file system) */
#define INODE_CT	 95	/* default inodes (when making file system) */

#include "../../fs/super.h"
struct super_block sb;

#define STICKY_BIT	01000	/* not defined anywhere else */

/* Ztob gives the block address of a zone
 * btoa gives the byte address of a block
 */
#define ztob(z)		((block_nr) (z) << sb.s_log_zone_size)
#define btoa(b)		((long) (b) * BLOCK_SIZE)
#define SCALE		((int) ztob(1))	/* # blocks in a zone */
#define FIRST		((zone_nr) sb.s_firstdatazone)	/* as the name says */

/* # blocks of each type */
#define N_SUPER		1
#define N_IMAP		(sb.s_imap_blocks)
#define N_ZMAP		(sb.s_zmap_blocks)
#define N_ILIST		((sb.s_ninodes+INODES_PER_BLOCK-1) / INODES_PER_BLOCK)
#define N_DATA		(sb.s_zones - FIRST)

/* Block address of each type */
#define BLK_SUPER	(SUPER_BLOCK)
#define BLK_IMAP	(BLK_SUPER + N_SUPER)
#define BLK_ZMAP	(BLK_IMAP  + N_IMAP)
#define BLK_ILIST	(BLK_ZMAP  + N_ZMAP)
#define BLK_FIRST	ztob(FIRST)
#define ZONE_SIZE	((int) ztob(BLOCK_SIZE))
#define NLEVEL		(NR_ZONE_NUMS - NR_DZONE_NUM + 1)

/* Byte address of a zone/of an inode */
#define zaddr(z)	btoa(ztob(z))
#define inoaddr(i)	((long) (i - 1) * INODE_SIZE + (long) btoa(BLK_ILIST))
#define INDCHUNK	((int) (CINDIR * ZONE_NUM_SIZE))
#define DIRCHUNK	((int) (CDIRECT * DIR_ENTRY_SIZE))

char *prog, *device;		/* program name (fsck), device name */
int firstcnterr;		/* is this the first inode ref cnt error? */
bitchunk_t *imap, *spec_imap;	/* inode bit maps */
bitchunk_t *zmap, *spec_zmap;	/* zone bit maps */
bitchunk_t *dirmap;		/* directory (inode) bit map */
char rwbuf[BLOCK_SIZE];		/* one block buffer cache */
block_nr thisblk;		/* block in buffer cache */
char nullbuf[BLOCK_SIZE];	/* null buffer */
nlink_t *count;			/* inode count */
int changed;			/* has the diskette been written to? */
struct stack {
  dir_struct *st_dir;
  struct stack *st_next;
  char st_presence;
} *ftop;

int dev;			/* file descriptor of the device */

#define DOT	1
#define DOTDOT	2

/* Counters for each type of inode/zone. */
int nfreeinode, nregular, ndirectory, nblkspec, ncharspec, nbadinode;
int npipe, nsyml, ztype[NLEVEL];
long nfreezone;

int repair, automatic, listing, listsuper;	/* flags */
int preen, verbose, keyskip;	/* -p, -v, -k (spec N.3) */
long fsck_mem = 64L * 1024;	/* -m: bytes for the maps (spec N.4) */
char *logname;			/* -L: the -v lines also go here */
FILE *vout, *vlog;		/* the console and the log for -v */
int vfd = -1;			/* /dev/buttons for -k */
int ismounted, isroot;		/* the device is mounted; it is / */
int nfixed, nleft;		/* repairs made, errors not repaired */
int stopped;			/* Back pressed */
ino_t maxino;			/* count[] and winmask[] cover 0..maxino */
long nused;			/* inodes in use in the disk map */
ino_t cur_ino;			/* inode whose zones are being marked */
unsigned char *winmask;		/* per inode: the windows its zones touch */
bit_nr wlo, whi, wbits;		/* zmap holds zone bits wlo..whi-1 */
int nwin, curwin;		/* number of windows; the current one */
char *vphase = "";		/* -v: what is going on */
long vdone, vtotal;		/* -v: progress in the phase */
clock_t vstart, vlast;		/* -v: times() at the start, last redraw */
int vspin, vcalls;
int firstlist;			/* has the listing header been printed? */
unsigned part_offset;		/* sector offset for this partition */
char answer[] = "Answer questions with y or n.  Then hit RETURN";

_PROTOTYPE(int main, (int argc, char **argv));
_PROTOTYPE(void initvars, (void));
_PROTOTYPE(void fatal, (char *s));
_PROTOTYPE(int eoln, (int c));
_PROTOTYPE(int yes, (char *question));
_PROTOTYPE(int atoo, (char *s));
_PROTOTYPE(int input, (char *buf, int size));
_PROTOTYPE(char *alloc, (unsigned nelem, unsigned elsize));
_PROTOTYPE(void printname, (char *s));
_PROTOTYPE(void printrec, (struct stack *sp));
_PROTOTYPE(void printpath, (int mode, int nlcr));
_PROTOTYPE(void devopen, (void));
_PROTOTYPE(void devclose, (void));
_PROTOTYPE(void devio, (block_nr bno, int dir));
_PROTOTYPE(void devread, (long offset, char *buf, int size));
_PROTOTYPE(void devwrite, (long offset, char *buf, int size));
_PROTOTYPE(void pr, (char *fmt, int cnt, char *s, char *p));
_PROTOTYPE(void lpr, (char *fmt, long cnt, char *s, char *p));
_PROTOTYPE(bit_nr getnumber, (char *s));
_PROTOTYPE(char **getlist, (char ***argv, char *type));
_PROTOTYPE(void lsuper, (void));
_PROTOTYPE(void getsuper, (void));
_PROTOTYPE(void chksuper, (void));
_PROTOTYPE(void lsi, (char **clist));
_PROTOTYPE(bitchunk_t *allocbitmap, (int nblk));
_PROTOTYPE(void loadbitmap, (bitchunk_t *bitmap, block_nr bno, int nblk));
_PROTOTYPE(void dumpbitmap, (bitchunk_t *bitmap, block_nr bno, int nblk));
_PROTOTYPE(void fillbitmap, (bitchunk_t *bitmap, bit_nr lwb, bit_nr upb, char **list));
_PROTOTYPE(void freebitmap, (bitchunk_t *p));
_PROTOTYPE(void getbitmaps, (void));
_PROTOTYPE(void putbitmaps, (void));
_PROTOTYPE(void chkword, (unsigned w1, unsigned w2, bit_nr bit, char *type, int *n, int *report));
_PROTOTYPE(void chkmap, (bitchunk_t *cmap, bit_nr bit, block_nr blkno, int nblk, bit_nr nbits, char *type));
_PROTOTYPE(void chkilist, (void));
_PROTOTYPE(void getcount, (void));
_PROTOTYPE(void counterror, (Ino_t ino));
_PROTOTYPE(void chkcount, (void));
_PROTOTYPE(void freecount, (void));
_PROTOTYPE(void printperm, (Mode_t mode, int shift, int special, int overlay));
_PROTOTYPE(void list, (Ino_t ino, d_inode *ip));
_PROTOTYPE(int Remove, (dir_struct *dp));
_PROTOTYPE(void make_printable_name, (char *dst, char *src, int n));
_PROTOTYPE(int chkdots, (Ino_t ino, off_t pos, dir_struct *dp, Ino_t exp));
_PROTOTYPE(int chkname, (Ino_t ino, dir_struct *dp));
_PROTOTYPE(int chkentry, (Ino_t ino, off_t pos, dir_struct *dp));
_PROTOTYPE(int chkdirzone, (Ino_t ino, d_inode *ip, off_t pos, zone_nr zno));
_PROTOTYPE(void errzone, (char *mess, zone_nr zno, int level, off_t pos));
_PROTOTYPE(int markzone, (zone_nr zno, int level, off_t pos));
_PROTOTYPE(int chkindzone, (Ino_t ino, d_inode *ip, off_t *pos, zone_nr zno, int level));
_PROTOTYPE(off_t jump, (int level));
_PROTOTYPE(int zonechk, (Ino_t ino, d_inode *ip, off_t *pos, zone_nr zno, int level));
_PROTOTYPE(int chkzones, (Ino_t ino, d_inode *ip, off_t *pos, zone_nr *zlist, int len, int level));
_PROTOTYPE(int chkfile, (Ino_t ino, d_inode *ip));
_PROTOTYPE(int chkdirectory, (Ino_t ino, d_inode *ip));
_PROTOTYPE(int chklink, (Ino_t ino, d_inode *ip));
_PROTOTYPE(int chkspecial, (Ino_t ino, d_inode *ip));
_PROTOTYPE(int chkmode, (Ino_t ino, d_inode *ip));
_PROTOTYPE(int chkinode, (Ino_t ino, d_inode *ip));
_PROTOTYPE(int descendtree, (dir_struct *dp));
_PROTOTYPE(void chktree, (void));
_PROTOTYPE(void printtotal, (void));
_PROTOTYPE(int chkdev, (char *f, char **clist, char **ilist, char **zlist));
_PROTOTYPE(void vline, (char *s));
_PROTOTYPE(void vtick, (int force));
_PROTOTYPE(void vset, (char *phase, long total));
_PROTOTYPE(void needino, (Ino_t ino));
_PROTOTYPE(void findmaxino, (void));
_PROTOTYPE(void getwindow, (void));
_PROTOTYPE(void setwindow, (int w));
_PROTOTYPE(void wzones, (zone_nr *zlist, int len, int level, off_t *pos, off_t size, int *dirty));
_PROTOTYPE(void winpass, (void));
_PROTOTYPE(int sflags, (int set, int clr));
_PROTOTYPE(void whereis, (void));

/* Initialize the variables used by this program. */
void initvars()
{
  register level;

  nregular = ndirectory = nblkspec = ncharspec = nbadinode = npipe = nsyml = 0;
  for (level = 0; level < NLEVEL; level++) ztype[level] = 0;
  changed = 0;
  thisblk = NO_BLOCK;
  firstlist = 1;
  firstcnterr = 1;
}

/* Print the string `s' and exit. */
void fatal(s)
char *s;
{
  char line[40];

  printf("%s\nfatal\n", s);
  if (verbose) {
	sprintf(line, "not checked: %.18s", *s ? s : "error");
	vline(line);
  }
  if (repair) sync();
  exit(8);		/* BSD: could not check (spec N.3) */
}

/* Test for end of line. */
int eoln(c)
int c;
{
  return(c == EOF || c == '\n' || c == '\r');
}

/* Ask a question and get the answer unless automatic is set. */
int yes(question)
char *question;
{
  register int c, answerchar;

  char line[40];

  if (!repair) {
	printf("\n");
	nleft++;
	return(0);
  }
  printf("%s? ", question);
  if (automatic) {
	printf("yes\n");
	nfixed++;
	if (verbose) {	/* one short line per repair (spec N.3) */
		while (*question == '.' || *question == ' ') question++;
		sprintf(line, "fix %u: %.20s", (unsigned) cur_ino, question);
		vline(line);
	}
	return(1);
  }
  fflush(stdout);
  if ((c = answerchar = getchar()) == 'q' || c == 'Q') exit(4);
  while (!eoln(c)) c = getchar();
  if (answerchar == 'n' || answerchar == 'N') {
	nleft++;
	return(0);
  }
  nfixed++;
  return(1);
}

/* Convert string to integer.  Representation is octal. */
int atoo(s)
register char *s;
{
  register int n = 0;

  while ('0' <= *s && *s < '8') {
	n <<= 3;
	n += *s++ - '0';
  }
  return n;
}

/* If repairing the file system, print a prompt and get a string from user. */
int input(buf, size)
char *buf;
int size;
{
  register char *p = buf;

  printf("\n");
  if (repair) {
	printf("--> ");
	fflush(stdout);
	while (--size) {
		*p = getchar();
		if (eoln(*p)) {
			*p = 0;
			return(p > buf);
		}
		p++;
	}
	*p = 0;
	while (!eoln(getchar()));
	return(1);
  }
  return(0);
}

/* Allocate some memory and zero it. */
char *alloc(nelem, elsize)
unsigned nelem, elsize;
{
  char *p;

  if ((p = (char *)malloc((size_t)nelem * elsize)) == 0)fatal("out of memory");
  memset((void *) p, 0, (size_t)nelem * elsize);
  return(p);
}

/* Print the name in a directory entry. */
void printname(s)
char *s;
{
  register n = NAME_MAX;
  int c;

  do {
	if ((c = *s) == 0) break;
	if (!isprint(c)) c = '?';
	putchar(c);
	s++;
  } while (--n);
}

/* Print the pathname given by a linked list pointed to by `sp'.  The
 * names are in reverse order.
 */
void printrec(sp)
struct stack *sp;
{
  if (sp->st_next != 0) {
	printrec(sp->st_next);
	putchar('/');
	printname(sp->st_dir->d_name);
  }
}

/* Print the current pathname.  */
void printpath(mode, nlcr)
int mode;
int nlcr;
{
  if (ftop->st_next == 0)
	putchar('/');
  else
	printrec(ftop);
  switch (mode) {
      case 1:
	printf(" (ino = %u, ", ftop->st_dir->d_inum);
	break;
      case 2:
	printf(" (ino = %u)", ftop->st_dir->d_inum);
	break;
  }
  if (nlcr) printf("\n");
}

/* Open the device.  */
void devopen()
{
  if ((dev = open(device, repair ? O_RDWR : O_RDONLY)) < 0) {
	perror(device);
	fatal("");
  }
}

/* Close the device. */
void devclose()
{
  if (close(dev) != 0) {
	perror("close");
	fatal("");
  }
}

/* Read or write a block. */
void devio(bno, dir)
block_nr bno;
int dir;
{
  if (dir == READING && bno == thisblk) return;
  thisblk = bno;
  vtick(0);

#if 0
printf("%s at block %5d\n", dir == READING ? "reading " : "writing", bno);
#endif
  lseek(dev, (off_t) btoa(bno), SEEK_SET);
  if (dir == READING) {
	if (read(dev, rwbuf, BLOCK_SIZE) == BLOCK_SIZE)
		return;
  } else {
	if (write(dev, rwbuf, BLOCK_SIZE) == BLOCK_SIZE)
		return;
  }

  printf("%s: can't %s block %ld (error = 0x%x)\n", prog,
         dir == READING ? "read" : "write", (long) bno, errno);
  if (dir == READING) {
	printf("Continuing with a zero-filled block.\n");
	memset(rwbuf, 0, BLOCK_SIZE);
	return;
  }
  fatal("");
}

/* Read `size' bytes from the disk starting at byte `offset'. */
void devread(offset, buf, size)
long offset;
char *buf;
int size;
{
  devio((block_nr) (offset / BLOCK_SIZE), READING);
  memmove(buf, &rwbuf[(int) (offset % BLOCK_SIZE)], (size_t)size);  /* lint but OK */
}

/* Write `size' bytes to the disk starting at byte `offset'. */
void devwrite(offset, buf, size)
long offset;
char *buf;
int size;
{
  if (!repair) fatal("internal error (devwrite)");
  if (size != BLOCK_SIZE) devio((block_nr) (offset / BLOCK_SIZE), READING);
  memmove(&rwbuf[(int) (offset % BLOCK_SIZE)], buf, (size_t)size);  /* lint but OK */
  devio((block_nr) (offset / BLOCK_SIZE), WRITING);
  changed = 1;
}

/* Print a string with either a singular or a plural pronoun. */
void pr(fmt, cnt, s, p)
char *fmt, *s, *p;
int cnt;
{
  printf(fmt, cnt, cnt == 1 ? s : p);
}

/* Same as above, but with a long argument */
void lpr(fmt, cnt, s, p)
char *fmt, *s, *p;
long cnt;
{
  printf(fmt, cnt, cnt == 1 ? s : p);
}

/* Convert string to number. */
bit_nr getnumber(s)
register char *s;
{
  register bit_nr n = 0;

  if (s == NULL)
	return NO_BIT;
  while (isdigit(*s))
	n = (n << 1) + (n << 3) + *s++ - '0';
  return (*s == '\0') ? n : NO_BIT;
}

/* See if the list pointed to by `argv' contains numbers. */
char **getlist(argv, type)
char ***argv, *type;
{
  register char **list = *argv;
  register empty = 1;

  while (getnumber(**argv) != NO_BIT) {
	(*argv)++;
	empty = 0;
  }
  if (empty) {
	printf("warning: no %s numbers given\n", type);
	return(NULL);
  }
  return(list);
}

/* Make a listing of the super block.  If `repair' is set, ask the user
 * for changes.
 */
void lsuper()
{
  char buf[80];

  do {
	/* Most of the following atol's enrage lint, for good reason. */  
	printf("ninodes       = %u", sb.s_ninodes);
	if (input(buf, 80)) sb.s_ninodes = atol(buf);
	printf("nzones        = %ld", sb.s_zones);
	if (input(buf, 80)) sb.s_zones = atol(buf);
	printf("imap_blocks   = %u", sb.s_imap_blocks);
	if (input(buf, 80)) sb.s_imap_blocks = atol(buf);
	printf("zmap_blocks   = %u", sb.s_zmap_blocks);
	if (input(buf, 80)) sb.s_zmap_blocks = atol(buf);
	printf("firstdatazone = %u", sb.s_firstdatazone);
	if (input(buf, 80)) sb.s_firstdatazone = atol(buf);
	printf("log_zone_size = %u", sb.s_log_zone_size);
	if (input(buf, 80)) sb.s_log_zone_size = atol(buf);
	printf("maxsize       = %ld", sb.s_max_size);
	if (input(buf, 80)) sb.s_max_size = atol(buf);
	if (yes("ok now")) {
		devwrite(btoa(BLK_SUPER), (char *) &sb, sizeof(sb));
		return;
	}
  } while (yes("Do you want to try again"));
  if (repair) exit(0);
}

/* Get the super block from either disk or user.  Do some initial checks. */
void getsuper()
{
  devread(btoa(BLK_SUPER), (char *) &sb, sizeof(sb));
  if (listsuper) lsuper();
  if (sb.s_magic == SUPER_MAGIC) fatal("Cannot handle V1 file systems");
  if (sb.s_magic != SUPER_V2) fatal("bad magic number in super block");
  if (sb.s_ninodes <= 0) fatal("no inodes");
  if (sb.s_zones <= 0) fatal("no zones");
  if (sb.s_imap_blocks <= 0) fatal("no imap");
  if (sb.s_zmap_blocks <= 0) fatal("no zmap");
  if (sb.s_firstdatazone <= 4) fatal("first data zone too small");
  if (sb.s_log_zone_size < 0) fatal("zone size < block size");
  if (sb.s_max_size <= 0) fatal("max. file size <= 0");
}

/* Check the super block for reasonable contents. */
void chksuper()
{
  register n;
  register off_t maxsize;

  n = bitmapsize((bit_t) sb.s_ninodes + 1);
  if (sb.s_magic != SUPER_V2) fatal("bad magic number in super block");
  if (sb.s_imap_blocks < n) fatal("too few imap blocks");
  if (sb.s_imap_blocks != n) {
	pr("warning: expected %d imap_block%s", n, "", "s");
	printf(" instead of %d\n", sb.s_imap_blocks);
  }
  n = bitmapsize((bit_t) sb.s_zones);
  if (sb.s_zmap_blocks < n) fatal("too few zmap blocks");
  if (sb.s_zmap_blocks != n) {
	pr("warning: expected %d zmap_block%s", n, "", "s");
	printf(" instead of %d\n", sb.s_zmap_blocks);
  }
  if (sb.s_firstdatazone >= sb.s_zones)
	fatal("first data zone too large");
  if (sb.s_log_zone_size >= 8 * sizeof(block_nr))
	fatal("log_zone_size too large");
  if (sb.s_log_zone_size > 8) printf("warning: large log_zone_size (%d)\n",
	       sb.s_log_zone_size);
  n = (BLK_ILIST + N_ILIST + SCALE - 1) >> sb.s_log_zone_size;
  if (sb.s_firstdatazone < n) fatal("first data zone too small");
  if (sb.s_firstdatazone != n) {
	printf("warning: expected first data zone to be %d ", n);
	printf("instead of %u\n", sb.s_firstdatazone);
  }
  maxsize = MAX_FILE_POS;
  if (((maxsize - 1) >> sb.s_log_zone_size) / BLOCK_SIZE >= MAX_ZONES)
	maxsize = ((long) MAX_ZONES * BLOCK_SIZE) << sb.s_log_zone_size;
  if (sb.s_max_size != maxsize) {
	printf("warning: expected max size to be %ld ", maxsize);
	printf("instead of %ld\n", sb.s_max_size);
  }
}

/* Make a listing of the inodes given by `clist'.  If `repair' is set, ask
 * the user for changes.
 */
void lsi(clist)
char **clist;
{
  register bit_nr bit;
  register ino_t ino;
  d_inode inode, *ip = &inode;
  char buf[80];

  if (clist == 0) return;
  while ((bit = getnumber(*clist++)) != NO_BIT) {
	specset(spec_imap, bit);
	ino = bit;
	do {
		devread(inoaddr(ino), (char *) ip, INODE_SIZE);
		printf("inode %u:\n", ino);
		printf("    mode   = %6o", ip->i_mode);
		if (input(buf, 80)) ip->i_mode = atoo(buf);
		printf("    nlinks = %6u", ip->i_nlinks);
		if (input(buf, 80)) ip->i_nlinks = atol(buf);
		printf("    size   = %6ld", ip->i_size);
		if (input(buf, 80)) ip->i_size = atol(buf);
		if (yes("Write this back")) {
			devwrite(inoaddr(ino), (char *) ip, INODE_SIZE);
			break;
		}
	} while (yes("Do you want to change it again"));
  }
}

/* Allocate `nblk' blocks worth of bitmap. */
bitchunk_t *allocbitmap(nblk)
int nblk;
{
  register bitchunk_t *bitmap;

  bitmap = (bitchunk_t *) alloc((unsigned) nblk, BLOCK_SIZE);
  *bitmap |= 1;
  return(bitmap);
}

/* Load the bitmap starting at block `bno' from disk. */
void loadbitmap(bitmap, bno, nblk)
bitchunk_t *bitmap;
block_nr bno;
int nblk;
{
  register i;
  register bitchunk_t *p;

  p = bitmap;
  for (i = 0; i < nblk; i++, bno++, p += WORDS_PER_BLOCK)
	devread(btoa(bno), (char *) p, BLOCK_SIZE);
  *bitmap |= 1;
}

/* Write the bitmap starting at block `bno' to disk. */
void dumpbitmap(bitmap, bno, nblk)
bitchunk_t *bitmap;
block_nr bno;
int nblk;
{
  register i;
  register bitchunk_t *p = bitmap;

  for (i = 0; i < nblk; i++, bno++, p += WORDS_PER_BLOCK)
	devwrite(btoa(bno), (char *) p, BLOCK_SIZE);
}

/* Set the bits given by `list' in the bitmap. */
void fillbitmap(bitmap, lwb, upb, list)
bitchunk_t *bitmap;
bit_nr lwb, upb;
char **list;
{
  register bit_nr bit;

  if (list == 0) return;
  while ((bit = getnumber(*list++)) != NO_BIT)
	if (bit < lwb || bit >= upb) {
		if (bitmap == spec_imap)
			printf("inode number %ld ", bit);
		else
			printf("zone number %ld ", bit);
		printf("out of range (ignored)\n");
	} else
		setbit(bitmap, bit - lwb + 1);
}

/* Deallocate the bitmap `p'. */
void freebitmap(p)
bitchunk_t *p;
{
  free((char *) p);
}

/* Get all the bitmaps used by this program. */
void getbitmaps()
{
  imap = allocbitmap(N_IMAP);
  dirmap = allocbitmap(N_IMAP);
  /* zmap: getwindow(), after the count array */
}

/* Release all the space taken by the bitmaps. */
void putbitmaps()
{
  freebitmap(imap);
  freebitmap(zmap);
  if (spec_imap) freebitmap(spec_imap);
  if (spec_zmap) freebitmap(spec_zmap);
  freebitmap(dirmap);
  if (winmask) free((char *) winmask);
  spec_imap = spec_zmap = 0;
  winmask = 0;
}

/* `w1' and `w2' are differing words from two bitmaps that should be
 * identical.  Print what's the matter with them.
 */
void chkword(w1, w2, bit, type, n, report)
unsigned w1, w2;
char *type;
bit_nr bit;
int *n, *report;
{
  for (; (w1 | w2); w1 >>= 1, w2 >>= 1, bit++)
	if ((w1 ^ w2) & 1 && ++(*n) % MAXPRINT == 0 && *report &&
	    (!repair || automatic || yes("stop this listing")))
		*report = 0;
	else if (*report)
		if ((w1 & 1) && !(w2 & 1))
			printf("%s %ld is missing\n", type, bit);
		else if (!(w1 & 1) && (w2 & 1))
			printf("%s %ld is not free\n", type, bit);
}

/* Check if the given (correct) bitmap is identical with the one that is
 * on the disk.  If not, ask if the disk should be repaired.
 */
void chkmap(cmap, bit, blkno, nblk, nbits, type)
bitchunk_t *cmap;		/* computed map of these nblk blocks */
bit_nr bit;			/* number (for messages) of its first bit */
block_nr blkno;			/* the first of its blocks on the disk */
int nblk;
bit_nr nbits;			/* bits in use from here (the rest ignored) */
char *type;
{
/* Spec N.4: one disk block at a time (no second map in memory).  The bits
 * past the end of the map are what the disk has (mkfs and mkfsimg.py
 * differ there); a wrong block is written from the computed map.
 */
  static bitchunk_t dmap[WORDS_PER_BLOCK];
  register bitchunk_t *p, *q;
  bitchunk_t mask;
  bit_nr b0;
  int report = 1, nerr = 0, i, w, bad, asked = 0, ok = 0;

  printf("Checking %s map\n", type);
  for (i = 0; i < nblk; i++) {
	devread(btoa(blkno + i), (char *) dmap, BLOCK_SIZE);
	if (i == 0 && bit == (bit_nr) 0) dmap[0] |= 1;
	if (i == 0 && type[0] == 'z' && wlo == 0) dmap[0] |= 1;
	bad = 0;
	for (w = 0, p = dmap, q = cmap + i * WORDS_PER_BLOCK;
				w < WORDS_PER_BLOCK; w++, p++, q++) {
		b0 = (bit_nr) i * BLOCK_SIZE * 8 + (bit_nr) w * 16;
		if (b0 >= nbits) {
			*q = *p;
		} else if (b0 + 16 > nbits) {
			mask = (bitchunk_t) ((1 << (int) (nbits - b0)) - 1);
			*q = (*q & mask) | (*p & ~mask);
		}
		if (*p != *q) {
			chkword(*p, *q, bit + b0, type, &nerr, &report);
			bad = 1;
		}
	}
	if (bad) {
		if (!asked) {
			asked = 1;
			ok = yes("install a new map");
		}
		if (ok) devwrite(btoa(blkno + i), (char *)
				(cmap + i * WORDS_PER_BLOCK), BLOCK_SIZE);
	}
	vdone++;
  }

  if ((!repair || automatic) && !report) printf("etc. ");
  if (nerr > MAXPRINT || nerr > 10) printf("%d errors found. ", nerr);
  if (nerr > 0) printf("\n");
}

/* See if the inodes that aren't allocated are cleared. */
void chkilist()
{
  register ino_t ino = 1;
  mode_t mode;

  ino_t last;

  /* Spec N.4: inodes are given out from the bottom; past the highest in
   * use (in either map) and its inode block nothing is to be cleared. */
  last = maxino + INODES_PER_BLOCK;
  if (last > sb.s_ninodes) last = sb.s_ninodes;
  vset("inode list", (long) last);
  printf("Checking inode list\n");
  do
	if (vdone = ino, !bitset(imap, (bit_nr) ino)) {
		devread(inoaddr(ino), (char *) &mode, sizeof(mode));
		if (mode != I_NOT_ALLOC) {
			printf("mode inode %u not cleared", ino);
			if (yes(". clear")) devwrite(inoaddr(ino), nullbuf,
					 INODE_SIZE);
		}
	}
  while (++ino <= last);
  printf("\n");
}

/* Allocate an array to maintain the inode reference counts in. */
void getcount()
{
  count = (nlink_t *) alloc((unsigned) (maxino + 1), sizeof(nlink_t));
}

/* Spec N.4: count[] (and winmask[]) cover the inodes up to the highest
 * in use in the disk map; an entry naming a higher one grows them. */
void needino(ino)
ino_t ino;
{
  ino_t newmax;
  char *p;

  if (ino <= maxino) return;
  newmax = ino | 0xFF;
  if (newmax > sb.s_ninodes) newmax = sb.s_ninodes;
  if ((p = realloc((char *) count, (size_t) (newmax + 1) * sizeof(nlink_t)))
							== NULL)
	fatal("out of memory");
  count = (nlink_t *) p;
  memset(p + (maxino + 1) * sizeof(nlink_t), 0,
			(size_t) (newmax - maxino) * sizeof(nlink_t));
  if (winmask != 0) {
	if ((p = realloc((char *) winmask, (size_t) newmax + 1)) == NULL)
		fatal("out of memory");
	winmask = (unsigned char *) p;
	memset(p + maxino + 1, 0, (size_t) (newmax - maxino));
  }
  maxino = newmax;
}

/* The highest inode in use in the disk's inode map, and how many. */
void findmaxino()
{
  static bitchunk_t m[WORDS_PER_BLOCK];
  bit_nr b, last = ROOT_INODE;
  int i, w, k;

  nused = 0;
  for (i = 0; i < N_IMAP; i++) {
	devread(btoa(BLK_IMAP + i), (char *) m, BLOCK_SIZE);
	for (w = 0; w < WORDS_PER_BLOCK; w++) {
		if (m[w] == 0) continue;
		for (k = 0; k < 16; k++) {
			if (!(m[w] & (1 << k))) continue;
			b = (bit_nr) i * BLOCK_SIZE * 8 + (bit_nr) w * 16 + k;
			if (b == 0 || b > sb.s_ninodes) continue;
			last = b;
			nused++;
		}
	}
  }
  maxino = (ino_t) last;
}

/* Spec N.4: the computed zone map is a window of the zones, as large as
 * fsck_mem allows after the other maps, whole map blocks. */
void getwindow()
{
  long left;
  int wblk;

  left = fsck_mem - 2L * N_IMAP * BLOCK_SIZE - 2L * (maxino + 1)
					- 4L * BLOCK_SIZE;	/* growth */
  if (spec_imap) left -= (long) N_IMAP * BLOCK_SIZE;
  if (spec_zmap) left -= (long) N_ZMAP * BLOCK_SIZE;
  wblk = left < BLOCK_SIZE ? 1 : (int) (left / BLOCK_SIZE);
  if (wblk > N_ZMAP) wblk = N_ZMAP;
  while ((zmap = (bitchunk_t *) malloc((size_t) wblk * BLOCK_SIZE)) == 0
							&& wblk > 1)
	wblk--;		/* what fits of fsck_mem (fsck's chmem) */
  if (zmap == 0) fatal("out of memory");
  wbits = (bit_nr) wblk * BLOCK_SIZE * 8;
  nwin = (N_ZMAP + wblk - 1) / wblk;
  if (nwin > 1) winmask = (unsigned char *) alloc((unsigned) maxino + 1, 1);
  setwindow(0);
}

void setwindow(w)
int w;
{
  curwin = w;
  wlo = (bit_nr) w * wbits;
  whi = wlo + wbits;
  memset((void *) zmap, 0, (size_t) (wbits / 8));
  if (w == 0) *zmap |= 1;
}

/* The reference count for inode `ino' is wrong.  Ask if it should be adjusted. */
void counterror(ino)
ino_t ino;
{
  d_inode inode;

  if (firstcnterr) {
	printf("INODE NLINK COUNT\n");
	firstcnterr = 0;
  }
  devread(inoaddr(ino), (char *) &inode, INODE_SIZE);
  count[ino] += inode.i_nlinks;	/* it was already subtracted; add it back */
  printf("%5u %5u %5u", ino, (unsigned) inode.i_nlinks, count[ino]);
  if (yes(" adjust")) {
	if ((inode.i_nlinks = count[ino]) == 0) {
		fatal("internal error (counterror)");
		inode.i_mode = I_NOT_ALLOC;
		clrbit(imap, (bit_nr) ino);
	}
	devwrite(inoaddr(ino), (char *) &inode, INODE_SIZE);
  }
}

/* Check if the reference count of the inodes are correct.  The array `count'
 * is maintained as follows:  an entry indexed by the inode number is
 * incremented each time a link is found; when the inode is read the link
 * count in there is substracted from the corresponding entry in `count'.
 * Thus, when the whole file system has been traversed, all the entries
 * should be zero.
 */
void chkcount()
{
  register ino_t ino;

  for (ino = 1; ino <= maxino; ino++)
	if (count[ino] != 0) counterror(ino);
  if (!firstcnterr) printf("\n");
}

/* Deallocate the `count' array. */
void freecount()
{
  free((char *) count);
}

/* Print the inode permission bits given by mode and shift. */
void printperm(mode, shift, special, overlay)
mode_t mode;
int shift;
int special;
int overlay;
{
  if (mode >> shift & R_BIT)
	putchar('r');
  else
	putchar('-');
  if (mode >> shift & W_BIT)
	putchar('w');
  else
	putchar('-');
  if (mode & special)
	putchar(overlay);
  else
	if (mode >> shift & X_BIT)
		putchar('x');
	else
		putchar('-');
}

/* List the given inode. */
void list(ino, ip)
ino_t ino;
d_inode *ip;
{
  if (firstlist) {
	firstlist = 0;
	printf(" inode permission link   size name\n");
  }
  printf("%6u ", ino);
  switch (ip->i_mode & I_TYPE) {
      case I_REGULAR:		putchar('-');	break;
      case I_DIRECTORY:		putchar('d');	break;
      case I_CHAR_SPECIAL:	putchar('c');	break;
      case I_BLOCK_SPECIAL:	putchar('b');	break;
      case I_NAMED_PIPE:	putchar('p');	break;
#ifdef I_SYMBOLIC_LINK
      case I_SYMBOLIC_LINK:	putchar('l');	break;
#endif
      default:			putchar('?');
}
  printperm(ip->i_mode, 6, I_SET_UID_BIT, 's');
  printperm(ip->i_mode, 3, I_SET_GID_BIT, 's');
  printperm(ip->i_mode, 0, STICKY_BIT, 't');
  printf(" %3u ", ip->i_nlinks);
  switch (ip->i_mode & I_TYPE) {
      case I_CHAR_SPECIAL:
      case I_BLOCK_SPECIAL:
	printf("  %2x,%2x ", (dev_t) ip->i_zone[0] >> MAJOR & 0xFF,
	       (dev_t) ip->i_zone[0] >> MINOR & 0xFF);
	break;
      default:	printf("%7ld ", ip->i_size);
  }
  printpath(0, 1);
}

/* Remove an entry from a directory if ok with the user.
 * Don't name the function remove() - that is owned by ANSI, and chaos results
 * when it is a macro.
 */
int Remove(dp)
dir_struct *dp;
{
  specset(spec_imap, (bit_nr) dp->d_inum);
  if (yes(". remove entry")) {
	count[dp->d_inum]--;
	memset((void *) dp, 0, sizeof(dir_struct));
	return(1);
  }
  return(0);
}

/* Convert string so that embedded control characters are printable. */
void make_printable_name(dst, src, n)
register char *dst;
register char *src;
register int n;
{
  register int c;

  while (--n >= 0 && (c = *src++) != '\0') {
	if (isprint(c) && c != '\\')
		*dst++ = c;
	else {
		*dst++ = '\\';
		switch (c) {
		      case '\\':
			*dst++ = '\\'; break;
		      case '\b':
			*dst++ = 'b'; break;
		      case '\f':
			*dst++ = 'f'; break;
		      case '\n':
			*dst++ = 'n'; break;
		      case '\r':
			*dst++ = 'r'; break;
		      case '\t':
			*dst++ = 't'; break;
		      default:
			*dst++ = '0' + ((c >> 6) & 03);
			*dst++ = '0' + ((c >> 3) & 07);
			*dst++ = '0' + (c & 07);
		}
	}
  }
  *dst = '\0';
}

/* See if the `.' or `..' entry is as expected. */
int chkdots(ino, pos, dp, exp)
ino_t ino, exp;
off_t pos;
dir_struct *dp;
{
  char printable_name[4 * NAME_MAX + 1];

  if (dp->d_inum != exp) {
	make_printable_name(printable_name, dp->d_name, sizeof(dp->d_name));
	printf("bad %s in ", printable_name);
	printpath(1, 0);
	printf("%s is linked to %u ", printable_name, dp->d_inum);
	printf("instead of %u)", exp);
	specset(spec_imap, (bit_nr) ino);
	specset(spec_imap, (bit_nr) dp->d_inum);
	specset(spec_imap, (bit_nr) exp);
	if (yes(". repair")) {
		count[dp->d_inum]--;
		dp->d_inum = exp;
		count[exp]++;
		return(0);
	}
  } else if (pos != (dp->d_name[1] ? DIR_ENTRY_SIZE : 0)) {
	make_printable_name(printable_name, dp->d_name, sizeof(dp->d_name));
	printf("warning: %s has offset %ld in ", printable_name, pos);
	printpath(1, 0);
	printf("%s is linked to %u)\n", printable_name, dp->d_inum);
	specset(spec_imap, (bit_nr) ino);
	specset(spec_imap, (bit_nr) dp->d_inum);
	specset(spec_imap, (bit_nr) exp);
  }
  return(1);
}

/* Check the name in a directory entry. */
int chkname(ino, dp)
ino_t ino;
dir_struct *dp;
{
  register n = NAME_MAX + 1;
  register char *p = dp->d_name;

  if (*p == '\0') {
	printf("null name found in ");
	printpath(0, 0);
	specset(spec_imap, (bit_nr) ino);
	if (Remove(dp)) return(0);
  }
  while (*p != '\0' && --n != 0)
	if (*p++ == '/') {
		printf("found a '/' in entry of directory ");
		printpath(1, 0);
		specset(spec_imap, (bit_nr) ino);
		printf("entry = '");
		printname(dp->d_name);
		printf("')");
		if (Remove(dp)) return(0);
		break;
	}
  return(1);
}

/* Check a directory entry.  Here the routine `descendtree' is called
 * recursively to check the file or directory pointed to by the entry.
 */
int chkentry(ino, pos, dp)
ino_t ino;
off_t pos;
dir_struct *dp;
{
  if (dp->d_inum < ROOT_INODE || dp->d_inum > sb.s_ninodes) {
	printf("bad inode found in directory ");
	printpath(1, 0);
	printf("ino found = %u, ", dp->d_inum);
	printf("name = '");
	printname(dp->d_name);
	printf("')");
	if (yes(". remove entry")) {
		memset((void *) dp, 0, sizeof(dir_struct));
		return(0);
	}
	return(1);
  }
  needino(dp->d_inum);
  if ((unsigned) count[dp->d_inum] == LINK_MAX) {
	printf("too many links to ino %u\n", dp->d_inum);
	printf("discovered at entry '");
	printname(dp->d_name);
	printf("' in directory ");
	printpath(0, 1);
	if (Remove(dp)) return(0);
  }
  count[dp->d_inum]++;
  if (strcmp(dp->d_name, ".") == 0) {
	ftop->st_presence |= DOT;
	return(chkdots(ino, pos, dp, ino));
  }
  if (strcmp(dp->d_name, "..") == 0) {
	ftop->st_presence |= DOTDOT;
	return(chkdots(ino, pos, dp, ino == ROOT_INODE ? ino :
			ftop->st_next->st_dir->d_inum));
  }
  if (!chkname(ino, dp)) return(0);
  if (bitset(dirmap, (bit_nr) dp->d_inum)) {
	printf("link to directory discovered in ");
	printpath(1, 0);
	printf("name = '");
	printname(dp->d_name);
	printf("', dir ino = %u)", dp->d_inum);
	return !Remove(dp);
  }
  return(descendtree(dp));
}

/* Check a zone of a directory by checking all the entries in the zone.
 * The zone is split up into chunks to not allocate too much stack.
 */
int chkdirzone(ino, ip, pos, zno)
ino_t ino;
d_inode *ip;
off_t pos;
zone_nr zno;
{
  dir_struct dirblk[CDIRECT];
  register dir_struct *dp;
  register n = SCALE * (NR_DIR_ENTRIES / CDIRECT), dirty;
  register long offset = zaddr(zno);
  register off_t size = 0;

  do {
	devread(offset, (char *) dirblk, DIRCHUNK);
	dirty = 0;
	for (dp = dirblk; dp < &dirblk[CDIRECT]; dp++) {
		if (dp->d_inum != NO_ENTRY && !chkentry(ino, pos, dp))
			dirty = 1;
		pos += DIR_ENTRY_SIZE;
		if (dp->d_inum != NO_ENTRY) size = pos;
	}
	if (dirty) devwrite(offset, (char *) dirblk, DIRCHUNK);
	offset += DIRCHUNK;
  } while (--n);

  if (size > ip->i_size) {
	printf("size not updated of directory ");
	printpath(2, 0);
	if (yes(". extend")) {
		specset(spec_imap, (bit_nr) ino);
		ip->i_size = size;
		devwrite(inoaddr(ino), (char *) ip, INODE_SIZE);
	}
  }
  return(1);
}

/* There is something wrong with the given zone.  Print some details. */
void errzone(mess, zno, level, pos)
char *mess;
zone_nr zno;
int level;
off_t pos;
{
  printf("%s zone in ", mess);
  printpath(1, 0);
  printf("zno = %ld, type = ", zno);
  switch (level) {
      case 0:	printf("DATA");	break;
      case 1:	printf("SINGLE INDIRECT");	break;
      case 2:	printf("DOUBLE INDIRECT");	break;
      default:	printf("VERY INDIRECT");
  }
  printf(", pos = %ld)\n", pos);
}

/* Found the given zone in the given inode.  Check it, and if ok, mark it
 * in the zone bitmap.
 */
int markzone(zno, level, pos)
zone_nr zno;
int level;
off_t pos;
{
  register bit_nr bit = (bit_nr) zno - FIRST + 1;

  ztype[level]++;
  if (zno < FIRST || zno >= sb.s_zones) {
	errzone("out-of-range", zno, level, pos);
	return(0);
  }
  if (bit >= whi) {	/* a later window (spec N.4): note it, go on */
	winmask[cur_ino] |= 1 << (int) ((bit / wbits) & 7);
	return(1);
  }
  if (bitset(zmap, bit)) {
	specset(spec_zmap, bit);
	errzone("duplicate", zno, level, pos);
	return(0);
  }
  nfreezone--;
  if (specbit(spec_zmap, bit)) errzone("found", zno, level, pos);
  setbit(zmap, bit);
  return(1);
}

/* Check an indirect zone by checking all of its entries.
 * The zone is split up into chunks to not allocate too much stack.
 */
int chkindzone(ino, ip, pos, zno, level)
ino_t ino;
d_inode *ip;
off_t *pos;
zone_nr zno;
int level;
{
  zone_nr indirect[CINDIR];
  register n = NR_INDIRECTS / CINDIR;
  register long offset = zaddr(zno);

  do {
	devread(offset, (char *) indirect, INDCHUNK);
	if (!chkzones(ino, ip, pos, indirect, CINDIR, level - 1)) return(0);
	offset += INDCHUNK;
  } while (--n && *pos < ip->i_size);
  return(1);
}

/* Return the size of a gap in the file, represented by a null zone number
 * at some level of indirection.
 */
off_t jump(level)
int level;
{
  off_t power = ZONE_SIZE;

  if (level != 0) do
		power *= NR_INDIRECTS;
	while (--level);
  return(power);
}

/* Check a zone, which may be either a normal data zone, a directory zone,
 * or an indirect zone.
 */
int zonechk(ino, ip, pos, zno, level)
ino_t ino;
d_inode *ip;
off_t *pos;
zone_nr zno;
int level;
{
  if (level == 0) {
	if ((ip->i_mode & I_TYPE) == I_DIRECTORY &&
	    !chkdirzone(ino, ip, *pos, zno))
		return(0);
	*pos += ZONE_SIZE;
	return(1);
  } else
	return chkindzone(ino, ip, pos, zno, level);
}

/* Check a list of zones given by `zlist'. */
int chkzones(ino, ip, pos, zlist, len, level)
ino_t ino;
d_inode *ip;
off_t *pos;
zone_nr *zlist;
int len;
int level;
{
  register ok = 1, i;

  /* The check on the position in the next loop is commented out, since FS
   * now requires valid zone numbers in each level that is necessary and FS
   * always deleted all the zones in the double indirect block.
   */
  for (i = 0; i < len /* && *pos < ip->i_size */ ; i++)
	if (zlist[i] == NO_ZONE)
		*pos += jump(level);
	else if (!markzone(zlist[i], level, *pos)) {
		*pos += jump(level);
		ok = 0;
	} else if (!zonechk(ino, ip, pos, zlist[i], level))
		ok = 0;
  return(ok);
}

/* Check a file or a directory. */
int chkfile(ino, ip)
ino_t ino;
d_inode *ip;
{
  register ok, i, level;
  off_t pos = 0;
  ino_t saved = cur_ino;

  cur_ino = ino;
  ok = chkzones(ino, ip, &pos, &ip->i_zone[0], NR_DZONE_NUM, 0);
  for (i = NR_DZONE_NUM, level = 1; i < NR_ZONE_NUMS; i++, level++) {
	cur_ino = ino;
	ok &= chkzones(ino, ip, &pos, &ip->i_zone[i], 1, level);
  }
  cur_ino = saved;
  return(ok);
}

/* Check a directory by checking the contents.  Check if . and .. are present. */
int chkdirectory(ino, ip)
ino_t ino;
d_inode *ip;
{
  register ok;

  setbit(dirmap, (bit_nr) ino);
  if (ip->i_size > MAXDIRSIZE) {
	printf("warning: huge directory: ");
	printpath(2, 1);
  }
  ok = chkfile(ino, ip);
  if (!(ftop->st_presence & DOT)) {
	printf(". missing in ");
	printpath(2, 1);
	ok = 0;
  }
  if (!(ftop->st_presence & DOTDOT)) {
	printf(".. missing in ");
	printpath(2, 1);
	ok = 0;
  }
  return(ok);
}

#ifdef I_SYMBOLIC_LINK

/* Check the validity of a symbolic link. */
int chklink(ino, ip)
ino_t ino;
d_inode *ip;
{
  int ok;

  ok = chkfile(ino, ip);
  if (ip->i_size <= 0 || ip->i_size > BLOCK_SIZE) {
	if (ip->i_size == 0)
		printf("empty symbolic link ");
	else
		printf("symbolic link too large (size %ld) ", ip->i_size);
	printpath(2, 1);
	ok = 0;
  }
  return(ok);
}

#endif

/* Check the validity of a special file. */
int chkspecial(ino, ip)
ino_t ino;
d_inode *ip;
{
  int i, ok;

  ok = 1;
  if ((dev_t) ip->i_zone[0] == NO_DEV) {
	printf("illegal device number %ld for special file ", ip->i_zone[0]);
	printpath(2, 1);
	ok = 0;
  }

  /* FS will not use the remaining "zone numbers" but 1.6.11++ will panic if
   * they are nonzero, since this should not happen.
   */
  for (i = 1; i < NR_ZONE_NUMS; i++)
	if (ip->i_zone[i] != NO_ZONE) {
		printf("nonzero zone number %ld for special file ",
		       ip->i_zone[i]);
		printpath(2, 1);
		ok = 0;
	}
  return(ok);
}

/* Check the mode and contents of an inode. */
int chkmode(ino, ip)
ino_t ino;
d_inode *ip;
{
  switch (ip->i_mode & I_TYPE) {
      case I_REGULAR:
	nregular++;
	return chkfile(ino, ip);
      case I_DIRECTORY:
	ndirectory++;
	return chkdirectory(ino, ip);
      case I_BLOCK_SPECIAL:
	nblkspec++;
	return chkspecial(ino, ip);
      case I_CHAR_SPECIAL:
	ncharspec++;
	return chkspecial(ino, ip);
      case I_NAMED_PIPE:
	npipe++;
	return chkfile(ino, ip);
#ifdef I_SYMBOLIC_LINK
      case I_SYMBOLIC_LINK:
	nsyml++;
	return chklink(ino, ip);
#endif
      default:
	nbadinode++;
	printf("bad mode of ");
	printpath(1, 0);
	printf("mode = %o)", ip->i_mode);
	return(0);
  }
}

/* Check an inode. */
int chkinode(ino, ip)
ino_t ino;
d_inode *ip;
{
  if (ino == ROOT_INODE && (ip->i_mode & I_TYPE) != I_DIRECTORY) {
	printf("root inode is not a directory ");
	printf("(ino = %u, mode = %o)\n", ino, ip->i_mode);
	fatal("");
  }
  if (ip->i_nlinks == 0) {
	printf("link count zero of ");
	printpath(2, 0);
	return(0);
  }
  nfreeinode--;
  setbit(imap, (bit_nr) ino);
  if ((unsigned) ip->i_nlinks > LINK_MAX) {
	printf("link count too big in ");
	printpath(1, 0);
	printf("cnt = %u)\n", (unsigned) ip->i_nlinks);
	count[ino] -= LINK_MAX;
	specset(spec_imap, (bit_nr) ino);
  } else {
	count[ino] -= (unsigned) ip->i_nlinks;
  }
  return chkmode(ino, ip);
}

/* Check the directory entry pointed to by dp, by checking the inode. */
int descendtree(dp)
dir_struct *dp;
{
  d_inode inode;
  register ino_t ino = dp->d_inum;
  register visited;
  struct stack stk;

  if (stopped) return(1);	/* -k: Back pressed, leave it as it is */
  stk.st_dir = dp;
  stk.st_next = ftop;
  ftop = &stk;
  if (specbit(spec_imap, (bit_nr) ino)) {
	printf("found inode %u: ", ino);
	printpath(0, 1);
  }
  visited = bitset(imap, (bit_nr) ino);
  if (!visited) vdone++;
  if (!visited || listing) {
	devread(inoaddr(ino), (char *) &inode, INODE_SIZE);
	if (listing) list(ino, &inode);
	if (!visited && !chkinode(ino, &inode)) {
		specset(spec_imap, (bit_nr) ino);
		if (yes("remove")) {
			count[ino] += inode.i_nlinks - 1;
			clrbit(imap, (bit_nr) ino);
			devwrite(inoaddr(ino), nullbuf, INODE_SIZE);
			memset((void *) dp, 0, sizeof(dir_struct));
			ftop = ftop->st_next;
			return(0);
		}
	}
  }
  ftop = ftop->st_next;
  return(1);
}

/* Check the file system tree. */
void chktree()
{
  dir_struct dir;

  nfreeinode = sb.s_ninodes;
  nfreezone = N_DATA;
  dir.d_inum = ROOT_INODE;
  dir.d_name[0] = 0;
  if (!descendtree(&dir)) fatal("bad root inode");
  putchar('\n');
}

/* Print the totals of all the objects found. */
void printtotal()
{
  printf("blocksize = %5d        ", BLOCK_SIZE);
  printf("zonesize  = %5d\n", ZONE_SIZE);
  printf("\n");
  pr("%6u    Regular file%s\n", nregular, "", "s");
  pr("%6u    Director%s\n", ndirectory, "y", "ies");
  pr("%6u    Block special file%s\n", nblkspec, "", "s");
  pr("%6u    Character special file%s\n", ncharspec, "", "s");
  if (nbadinode != 0) pr("%6u    Bad inode%s\n", nbadinode, "", "s");
  pr("%6u    Free inode%s\n", nfreeinode, "", "s");
  pr("%6u    Named pipe%s\n", npipe, "", "s");
  pr("%6u    Symbolic link%s\n", nsyml, "", "s");
/* Don't print some fields.
  printf("\n");
  pr("%6u    Data zone%s\n",		  ztype[0],	 "",   "s");
  pr("%6u    Single indirect zone%s\n",	  ztype[1],	 "",   "s");
  pr("%6u    Double indirect zone%s\n",	  ztype[2],	 "",   "s");
*/
  lpr("%6ld    Free zone%s\n", nfreezone, "", "s");
}

/* Spec N.4: a later window.  Walk the zones of every inode in use whose
 * zones touch it, as chkfile() did in pass 1 (same positions, so the same
 * zones), mark those inside the window; a duplicate there loses its zone
 * pointer (the file keeps the rest; pass 1 owns the inodes and the tree).
 */
void wzones(zlist, len, level, pos, size, dirty)
zone_nr *zlist;
int len, level;
off_t *pos, size;
int *dirty;
{
  zone_nr indirect[CINDIR];
  register int i, n;
  long offset;
  bit_nr bit;
  int d;

  for (i = 0; i < len; i++) {
	if (zlist[i] == NO_ZONE || zlist[i] < FIRST || zlist[i] >= sb.s_zones) {
		*pos += jump(level);
		continue;
	}
	bit = (bit_nr) zlist[i] - FIRST + 1;
	if (bit >= wlo && bit < whi) {
		if (bitset(zmap, bit - wlo)) {
			errzone("duplicate", zlist[i], level, *pos);
			if (yes("clear the zone pointer")) {
				zlist[i] = NO_ZONE;
				*dirty = 1;
			}
			*pos += jump(level);
			continue;
		}
		setbit(zmap, bit - wlo);
		nfreezone--;
	}
	if (level == 0) {
		*pos += ZONE_SIZE;
		continue;
	}
	offset = zaddr(zlist[i]);
	n = NR_INDIRECTS / CINDIR;
	do {
		devread(offset, (char *) indirect, INDCHUNK);
		d = 0;
		wzones(indirect, CINDIR, level - 1, pos, size, &d);
		if (d) devwrite(offset, (char *) indirect, INDCHUNK);
		offset += INDCHUNK;
	} while (--n && *pos < size);
  }
}

void winpass()
{
  d_inode inode;
  register ino_t ino;
  off_t pos;
  int i, level, dirty;

  vset("zones", (long) maxino);
  for (ino = ROOT_INODE; ino <= maxino && !stopped; ino++) {
	vdone = ino;
	if (!bitset(imap, (bit_nr) ino)
			|| !(winmask[ino] & (1 << (curwin & 7)))) continue;
	devread(inoaddr(ino), (char *) &inode, INODE_SIZE);
	switch (inode.i_mode & I_TYPE) {
	    case I_REGULAR: case I_DIRECTORY: case I_NAMED_PIPE:
#ifdef I_SYMBOLIC_LINK
	    case I_SYMBOLIC_LINK:
#endif
		break;
	    default:
		continue;
	}
	cur_ino = ino;
	pos = 0;
	dirty = 0;
	wzones(&inode.i_zone[0], NR_DZONE_NUM, 0, &pos, inode.i_size, &dirty);
	for (i = NR_DZONE_NUM, level = 1; i < NR_ZONE_NUMS; i++, level++)
		wzones(&inode.i_zone[i], 1, level, &pos, inode.i_size, &dirty);
	if (dirty) devwrite(inoaddr(ino), (char *) &inode, INODE_SIZE);
  }
}

/* -v (spec N.3): a line on the console (and in the -L log), no longer
 * than the LCD's 32 columns; it overwrites the progress line. */
void vline(s)
char *s;
{
  if (vout != NULL) {
	fprintf(vout, "\r%-31.31s\n", s);
	fflush(vout);
  }
  if (vlog != NULL) {
	fprintf(vlog, "%.31s\n", s);
	fflush(vlog);
  }
  vlast = 0;		/* redraw the progress soon */
}

void vset(phase, total)
char *phase;
long total;
{
  vphase = phase;
  vdone = 0;
  vtotal = total;
  vtick(1);
}

/* The progress line: a turning spinner (the owner: "so it's clear the
 * Flipper is working and not hung"), the phase and a percentage, redrawn
 * in place at most 4 times a second (at least once a second while the
 * card is read).  -k: Back stops the check. */
void vtick(force)
int force;
{
  static char spin[] = "|/-\\";
  struct tms tms;
  clock_t now;
  unsigned char ev[2];
  long pct;
  char pass[8];

  if (!verbose && vfd < 0) return;
  if (!force && (++vcalls & 7) != 0) return;
  now = times(&tms);
  if (!force && now - vlast < CLK_TCK / 4) return;
  vlast = now;
  if (vfd >= 0) {
	while (read(vfd, ev, 2) == 2)
		if ((ev[0] & 0x20) && ev[1]) stopped = 1;	/* Back */
  }
  if (vout == NULL) return;
  pct = vtotal > 0 ? vdone * 100 / vtotal : 0;
  if (pct > 100) pct = 100;
  pass[0] = 0;
  if (nwin > 1) sprintf(pass, " %d/%d", curwin + 1, nwin);
  fprintf(vout, "\r%c %-12.12s%-6s %3ld%%", spin[vspin++ & 3], vphase,
							pass, pct);
  fflush(vout);
}

/* Change the CLEAN/CHECK bits of the super block (spec N.1). */
int sflags(set, clr)
int set, clr;
{
  unsigned short f;
  long off = btoa(BLK_SUPER) + (long) offsetof(struct super_block, s_flags);

  devread(off, (char *) &f, sizeof(f));
  f = (f | set) & ~clr & SF_DISK;
  devwrite(off, (char *) &f, sizeof(f));
  return(f);
}

/* Is the device mounted, is it the root?  /etc/mtab may be old at boot,
 * so the root is found by its device number. */
void whereis()
{
  struct stat sd, sm;
  FILE *mt;
  char line[128], *m;

  ismounted = isroot = 0;
  if (stat(device, &sd) != 0 || !S_ISBLK(sd.st_mode)) return;
  if (stat("/", &sm) == 0 && sm.st_dev == sd.st_rdev) {
	ismounted = isroot = 1;
	return;
  }
  if ((mt = fopen("/etc/mtab", "r")) == NULL) return;
  while (fgets(line, sizeof(line), mt) != NULL) {
	if ((m = strchr(line, ' ')) == NULL) continue;
	while (*m == ' ') m++;
	m[strcspn(m, " \n")] = 0;
	if (*m && stat(m, &sm) == 0 && sm.st_dev == sd.st_rdev) ismounted = 1;
  }
  fclose(mt);
}

/* Check the device which name is given by `f'.  The inodes listed by `clist'
 * should be listed separately, and the inodes listed by `ilist' and the zones
 * listed by `zlist' should be watched for while checking the file system.
 * Returns the BSD exit status (spec N.3).
 */
int chkdev(f, clist, ilist, zlist)
char *f, **clist, **ilist, **zlist;
{
  char line[40];
  struct tms tms;
  long t;
  int w, rw;

  if (automatic || preen) repair = automatic = 1;
  device = f;
  initvars();
  nfixed = nleft = 0;
  stopped = 0;
  nwin = 1;
  curwin = 0;
  wlo = 0;
  whi = wbits = 0;

  whereis();
  devopen();
  getsuper();

  /* -p: a clean file system needs nothing (one block read). */
  if (preen && (sb.s_flags & (SF_CLEAN | SF_CHECK)) == SF_CLEAN) {
	devclose();
	return(0);
  }
  vstart = times(&tms);
  if (verbose) {
	sprintf(line, "fsck %.16s: %s", device,
		!(sb.s_flags & SF_CLEAN) ? "unclean" :
		(sb.s_flags & SF_CHECK) ? "not checked" : "check");
	vline(line);
  }

  chksuper();

  lsi(clist);

  getbitmaps();
  if (clist || ilist) spec_imap = allocbitmap(N_IMAP);
  if (zlist) spec_zmap = allocbitmap(N_ZMAP);

  fillbitmap(spec_imap, (bit_nr) 1, (bit_nr) sb.s_ninodes + 1, ilist);
  fillbitmap(spec_zmap, (bit_nr) FIRST, (bit_nr) sb.s_zones, zlist);

  findmaxino();
  getcount();
  getwindow();

  /* Pass 1: the tree, link counts, the inode map, zones in window 0. */
  vset("tree", nused);
  chktree();
  for (w = 0; w < nwin && !stopped; w++) {
	if (w > 0) {
		setwindow(w);
		winpass();
	}
	if (stopped) break;
	vset("zone map", (long) (w == nwin - 1 ? N_ZMAP - w * (wbits / 8
		/ BLOCK_SIZE) : wbits / 8 / BLOCK_SIZE));
	chkmap(zmap, (bit_nr) FIRST - 1 + wlo,
		BLK_ZMAP + (block_nr) (wlo / (BLOCK_SIZE * 8)),
		w == nwin - 1 ? (int) (N_ZMAP - wlo / (BLOCK_SIZE * 8))
			      : (int) (wbits / (BLOCK_SIZE * 8)),
		(bit_nr) N_DATA + 1 - wlo, "zone");
  }
  if (!stopped) {
	vset("links", 0L);
	chkcount();
	vset("inode map", (long) N_IMAP);
	chkmap(imap, (bit_nr) 0, BLK_IMAP, N_IMAP, (bit_nr) sb.s_ninodes + 1,
								"inode");
	chkilist();
	printtotal();
  }

  /* Spec N.3: checked with repairs allowed: CHECK = 0, and CLEAN = 1 when
   * no one else writes it (not mounted).  Stopped (Back): CHECK = 1. */
  if (repair) {
	if (stopped)
		(void) sflags(SF_CHECK, 0);
	else if (nleft == 0)
		(void) sflags(ismounted ? 0 : SF_CLEAN, SF_CHECK);
	changed = changed && nfixed > 0;
  }

  putbitmaps();
  freecount();
  devclose();
  if (repair) sync();

  if (changed) printf("----- FILE SYSTEM HAS BEEN MODIFIED -----\n\n");
  if (verbose) {
	t = (long) (times(&tms) - vstart) * 10 / CLK_TCK;
	if (stopped)
		sprintf(line, "skipped (Back), %ld.%ld s", t / 10, t % 10);
	else
		sprintf(line, "fixed %d, left %d, %ld.%ld s", nfixed, nleft,
							t / 10, t % 10);
	vline(line);
  }
  if (stopped) return(8);
  if (nleft > 0) return(4);
  if (nfixed > 0) return(isroot ? 2 : 1);
  return(0);
}

int main(argc, argv)
int argc;
char **argv;
{
  register char **clist = 0, **ilist = 0, **zlist = 0;

  register devgiven = 0;
  register char *arg;
  int r, worst = 0, fd;

  if ((1 << BITSHIFT) != 8 * sizeof(bitchunk_t)) {
	printf("Fsck was compiled with the wrong BITSHIFT!\n");
	exit(8);
  }

  sync();
  prog = *argv++;
  while ((arg = *argv++) != 0)
	if (arg[0] == '-' && arg[1] != 0 && arg[2] == 0) switch (arg[1]) {
		    case 'a':	automatic ^= 1;	break;
		    case 'c':
			clist = getlist(&argv, "inode");
			break;
		    case 'i':
			ilist = getlist(&argv, "inode");
			break;
		    case 'z':
			zlist = getlist(&argv, "zone");
			break;
		    case 'r':	repair ^= 1;	break;
		    case 'l':	listing ^= 1;	break;
		    case 's':	listsuper ^= 1;	break;
		    case 'p':	preen = 1;	break;
		    case 'k':	keyskip = 1;	break;
		    case 'v':
			if (!verbose) {
				/* the long messages go nowhere; the short
				 * lines to the console (fd 1 as it was) */
				verbose = 1;
				fflush(stdout);
				if ((fd = dup(1)) >= 0)
					vout = fdopen(fd, "w");
				(void) freopen("/dev/null", "w", stdout);
			}
			break;
		    case 'm':
			if (*argv == 0 || (fsck_mem = atol(*argv++) * 1024L)
								< 8192L)
				fsck_mem = 8192L;	/* the minimum */
			break;
		    case 'L':
			if (*argv == 0) break;
			logname = *argv++;
			vlog = fopen(logname, "a");
			break;
		    default:
			printf("%s: unknown flag '%s'\n", prog, arg);
		}
	else {
		if (keyskip && vfd < 0)
			vfd = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
		r = chkdev(arg, clist, ilist, zlist);
		if (r > worst) worst = r;
		clist = 0;
		ilist = 0;
		zlist = 0;
		devgiven = 1;
	}
  if (!devgiven) {
	printf("Usage: fsck [-acilprsvz] [-m KB] [-L log] file\n");
	exit(8);
  }
  return(worst);
}
