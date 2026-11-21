/* libradio: the CPU2 stack images on the SD card (spec R16.0.1, R16.0.2)
 * and the radio slot of the flash layout (R2b.1a); task T30a.
 *
 * Pure logic, no system calls: the `radio` command (flipper/cmds/radio.c)
 * and the host tests (flipper/tests/radio) use the same code.  All
 * addresses are ST's, on a 1 MB part (0x0800_0000 .. 0x0810_0000); a board
 * whose flash is elsewhere (QEMU: at 0) converts with RD_FLASH.
 */
#ifndef RADIO_H
#define RADIO_H

#define RD_FLASH	0x08000000UL	/* ST's flash base */
#define RD_FLASH_END	0x08100000UL	/* 1 MB */
#define RD_PAGE		4096UL
#define RD_GUARD	(16 * RD_PAGE)	/* R2b.1: room for FUS below SFSA */
#define RD_INST		0x48000UL	/* installed region, from the base */
#define RD_INST_RSV	(2 * RD_PAGE + 32 * 1024UL)	/* its directory
					 * pages and /dev/rom (HAL_ROM_OFF,
					 * HAL_ROM_SIZE): never given away */
#define RD_CACHE	0x20000UL	/* the text cache, 128 KB */
#define RD_CACHE_MIN	0x8000UL	/* R2b.1a: at least 32 KB of it */

#define RD_NAME		24		/* longest name + 1 */
#define RD_NOTE		48

struct rd_man {			/* an R16.0.1 manifest */
  char name[RD_NAME];
  char type[RD_NAME];
  char version[16];
  char fus[16];
  unsigned long addr;		/* ST install address, 1 MB part */
  unsigned long size;		/* bytes of the image */
  unsigned char sha[32];
  char note[RD_NOTE];
};

/* Errors of rd_parse() and rd_check(); rd_err() names them. */
enum { RD_OK, RD_EKEY, RD_EMISSING, RD_EVALUE, RD_EHASH, RD_EFUS,
       RD_ESLOTLOW, RD_ESLOTHIGH, RD_ESIZE, RD_ENAME, RD_NERR };

/* Parse the manifest text (key TAB value, one per line; unknown keys and
 * '#' lines are refused / skipped as R16.0.1 says).  0 or RD_E*; *line
 * gets the line of the error. */
int rd_parse(const char *text, unsigned long n, struct rd_man *m, int *line);

/* Write m as manifest text into buf (size bs); its length or -1. */
int rd_format(const struct rd_man *m, char *buf, int bs);

/* Compare two versions "X.Y.Z": <0, 0, >0; a bad one compares lowest. */
int rd_vercmp(const char *a, const char *b);

/* Check an image before `radio add` / list (R16.0.2): its SHA-256 sha
 * and size as read from the card, the FUS version on the device (NULL:
 * unknown, not checked).  0 or RD_E*. */
int rd_check(const struct rd_man *m, const unsigned char *sha,
	     unsigned long size, const char *fus);

/* The flash layout of R2b.1/R2b.1a, all as ST addresses. */
struct rd_layout {
  unsigned long sfsa;		/* CPU2's flash */
  unsigned long guard;		/* sfsa - 16 pages: nothing of ours above */
  unsigned long slot;		/* lowest stack address on the card, or 0 */
  unsigned long inst, inst_end;	/* installed programs */
  unsigned long cache, cache_end;	/* run-time text cache, = guard */
};

/* Lay out flash for SFSA sfsa and radio slot slot (0: none).  -1 when
 * sfsa leaves no room (neither region used), else 0.  The installed
 * region ends at min(guard - 128 KB, slot); the cache fills the rest up
 * to the guard, so space in the slot is used while a smaller stack runs
 * and a swap destroys only cache entries. */
int rd_layout(unsigned long sfsa, unsigned long slot, struct rd_layout *l);

/* Whether a flash record [addr, addr+len) is dead after a swap (at or
 * above the guard of the current layout). */
int rd_dead(const struct rd_layout *l, unsigned long addr, unsigned long len);

/* The slot an image at addr would need, checked against R2b.1a: the
 * stack installed there keeps the base system, libc, launcher and the
 * installed region's reserved pages below it, plus the 16-page guard and
 * the 32 KB minimum cache.  0 or RD_ESLOTLOW / RD_ESLOTHIGH. */
int rd_slot_ok(unsigned long addr);

/* The slot of a set of addresses: the lowest, or 0 for none. */
unsigned long rd_slot(const unsigned long *addr, int n);

const char *rd_err(int e);

#endif
