/* The shared libfuri + libapp library in flash (spec R2b.9, R5.2, 7b;
 * tools/mkshlibc.py, <minix/shlibc.h>).
 *
 * Like ld/shlibc.ld, a second library: its text runs in place at
 * SHFURI_BASE (a fixed flash address after the shared libc), starting
 * with its header and frozen entry table (.shlibc.hdr, furi.exports).  It
 * calls the shared libc through the libc's stubs, so its per-process data
 * (its GOT, data and bss: the "furi area") follows the libc area at r9:
 * the GOT section starts SHLIBC_AREA bytes into the data, as in a
 * program's link (ld/user-shlibc.ld), and ld counts GOT offsets from the
 * start of the section.  A program that uses it (ld/user-shfuri.ld) has
 * both areas before its own GOT.
 */
/* SHLIBC_AREA and the libc's variables (build/<board>/minix/shlibc) */
INCLUDE shlibc-syms.ld

SECTIONS
{
	. = SHFURI_BASE;
	.text : {
		_shlibc_start = .;
		KEEP(*(.shlibc.hdr))
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(8);
		_shlibc_etext = .;
	}

	. = 0x10000000;
	.got : {
		. = . + SHLIBC_AREA;
		_shlibc_astart = .;
		*(.got.plt) *(.igot.plt) *(.got) *(.igot)
	}
	.data : {
		*(.data .data.*)
		*(.data.rel.ro .data.rel.ro.*)
		. = ALIGN(4);
	}
	.bss : {
		*(.bss .bss.* COMMON)
		. = ALIGN(8);
		_shlibc_end = .;
	}
	_shlibc_size = _shlibc_etext - _shlibc_start;
	_shlibc_area = _shlibc_end - _shlibc_astart;

	/DISCARD/ : {
		*(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes)
		*(.init_array*) *(.fini_array*) *(.dynamic) *(.dynsym)
		*(.dynstr) *(.hash) *(.gnu.hash) *(.interp)
	}
}
