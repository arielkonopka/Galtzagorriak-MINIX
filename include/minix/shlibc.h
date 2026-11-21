/* The shared C library in flash (Galtzagorriak MINIX, spec Phase 2b,
 * R2b.9).
 *
 * One copy of the C library's code runs in place from a fixed flash
 * address, right after the 192 KB system image; minix.bin carries it.  It
 * starts with this header and the entry table: one word per exported
 * function, the function's address.  The table's order is frozen for an ABI
 * (flipper/libc.exports, append only): a program calls slot n through a
 * 12-byte stub in its own text (tools/mkshlibc.py), so it needs no GOT
 * entry (RAM) per function.
 *
 * The library's writable data - its GOT, errno, stdio, malloc and atexit
 * state - is per process: the first sh_area bytes of every data segment
 * (the "libc area", at r9, which the program and the library share).  The
 * program carries the area's initial contents in its own data, so a
 * program fits only the library build it was linked with: a.out flag
 * A_LIBC, and a_tbase holds the build's id (sh_id).  MM's exec refuses
 * any other with ENOEXEC (mm/exec.c), before allocating anything.
 *
 * The second shared library, libfuri + libapp (the "furi" library, spec
 * R2b.9, R5.2, 7b; flipper/furi.exports, ld/shfuri.ld), has the same
 * header at SHFURI_BASE, right after the libc's slot.  It calls the libc
 * through the libc's stubs, so its per-process data (the "furi area")
 * follows the libc area at r9 and its build id covers the libc's.  Only
 * the GUI apps carry it: a_dbase of their long header holds its id
 * (0: the program does not use it).
 */
#ifndef _MINIX_SHLIBC_H
#define _MINIX_SHLIBC_H

#ifdef BOARD_FLIPPER
#define SHLIBC_BASE	0x08030000UL	/* FLASH_BASE + 192 KB */
#else
#define SHLIBC_BASE	0x00030000UL	/* QEMU: flash at 0 */
#endif
#define SHLIBC_MAX	0xC000UL	/* 48 KB; the furi library follows */
#define SHLIBC_MAGIC	0x434C5A47UL	/* "GZLC" */
#define SHFURI_BASE	(SHLIBC_BASE + SHLIBC_MAX)	/* + 240 KB */
#define SHFURI_MAX	0xC000UL	/* 48 KB; installed programs follow */
#define SHFURI_MAGIC	0x464C5A47UL	/* "GZLF" */
#define SHLIB_END	(SHFURI_BASE + SHFURI_MAX)	/* + 288 KB */

struct shlibc_hdr {
  unsigned long sh_magic;	/* SHLIBC_MAGIC */
  unsigned short sh_abi;	/* GZ_ABI it was built for */
  unsigned short sh_nslot;	/* entries in sh_slot[] */
  unsigned long sh_id;		/* build id: CRC-32 of text and area */
  unsigned long sh_size;	/* bytes of text, from SHLIBC_BASE */
  unsigned long sh_area;	/* bytes of per-process data at r9 */
  unsigned long sh_resv;
  unsigned long sh_slot[1];	/* function addresses (Thumb bit set) */
};

#define SHLIBC_HDR	((struct shlibc_hdr *) SHLIBC_BASE)
#define SHFURI_HDR	((struct shlibc_hdr *) SHFURI_BASE)

#endif /* _MINIX_SHLIBC_H */
