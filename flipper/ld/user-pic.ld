/* MINIX user program for ARM Cortex-M, position-independent (A_SEP):
 * compiled with -fpic -msingle-pic-base -mpic-register=r9
 * -mno-pic-data-is-text-relative, so the text holds no absolute
 * addresses and can run from anywhere - from RAM or from the flash text
 * cache - shared by every process running it.  Text and read-only data
 * are linked at 0; data at A_DATA_LINK (0x10000000, <a.out.h>), starting
 * with the GOT: the kernel sets r9 to the start of the data segment.
 * tools/elf2mnx.py tells text from data addresses by their value.
 */
ENTRY(_start)

SECTIONS
{
	. = 0;
	.text : {
		KEEP(*crtso*.o(.text))
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
		_etext = .;
	}

	. = 0x10000000;
	.got : {
		*(.got.plt) *(.igot.plt) *(.got) *(.igot)
	}
	.data : {
		_sdata = .;
		*(.data .data.*)
		*(.data.rel.ro .data.rel.ro.*)
		. = ALIGN(4);
		_edata = .;
	}

	.bss : {
		_sbss = .;
		*(.bss .bss.* COMMON)
		. = ALIGN(4);
		_ebss = .;
		_end = .;		/* initial break */
	}

	/DISCARD/ : {
		*(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes)
		*(.init_array*) *(.fini_array*) *(.dynamic) *(.dynsym)
		*(.dynstr) *(.hash) *(.gnu.hash) *(.interp)
	}
}
