/* The shared C library in flash (spec Phase 2b, R2b.9), see
 * tools/mkshlibc.py and <minix/shlibc.h>.
 *
 * The text runs in place at SHLIBC_BASE (a fixed flash address, after the
 * 192 KB system image), starting with the header and the frozen entry
 * table (.shlibc.hdr).  The library is compiled like every user program
 * (-fpic -msingle-pic-base, r9 = GOT base), and its GOT, data and bss are
 * linked at A_DATA_LINK: they become the first bytes of every process's
 * data segment (the "libc area"), so the program and the library share r9.
 * The program's own GOT comes after the area (ld/user-shlibc.ld).
 */
SECTIONS
{
	. = SHLIBC_BASE;
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
	_shlibc_area = _shlibc_end - 0x10000000;

	/DISCARD/ : {
		*(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes)
		*(.init_array*) *(.fini_array*) *(.dynamic) *(.dynsym)
		*(.dynstr) *(.hash) *(.gnu.hash) *(.interp)
	}
}
