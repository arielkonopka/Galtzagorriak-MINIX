/* The shared Berry interpreter in flash (spec R7c.1, Phase 7c part 3,
 * issue #431; tools/mkshlibc.py, <minix/shlibc.h>).
 *
 * Like ld/shfuri.ld, a library on top of the shared libc: its text runs in
 * place at SHBERRY_BASE (a fixed flash address after the furi library),
 * starting with its header and frozen entry table (.shlibc.hdr,
 * berry.exports).  The difference: .data.rel.ro is in the text.  Berry's
 * constant tables (generate/: the string table, the built-in classes and
 * modules) are pointer tables; -fpic puts them in .data.rel.ro, which a
 * position-independent program must copy to RAM to relocate (7.2 KB per
 * script before this library).  Here the library has a fixed address, so
 * the pointers are final at link time and the tables stay in flash;
 * tools/mkshlibc.py refuses the image if one of them points into RAM.
 * It calls the shared libc through the libc's stubs, so its per-process
 * data (its GOT, data and bss: the "berry area") follows the libc area at r9:
 * the GOT section starts SHLIBC_AREA bytes into the data, as in a
 * program's link (ld/user-shlibc.ld), and ld counts GOT offsets from the
 * start of the section.  A program that uses it (ld/user-shberry.ld) has
 * both areas before its own GOT.
 */
/* SHLIBC_AREA and the libc's variables (build/<board>/minix/shlibc) */
INCLUDE shlibc-syms.ld

SECTIONS
{
	. = SHBERRY_BASE;
	.text : {
		_shlibc_start = .;
		KEEP(*(.shlibc.hdr))
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.data.rel.ro .data.rel.ro.*)
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
