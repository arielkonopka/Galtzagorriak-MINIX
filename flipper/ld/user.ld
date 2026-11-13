/* MINIX user program for ARM Cortex-M.  Linked at address 0 as one
 * contiguous image (common I&D): text, read-only data, data, bss.  Link with
 * -Wl,-q so the relocations are kept; tools/elf2mnx.py turns the result
 * into a MINIX a.out with a relocation table, and exec (mm/exec.c) moves
 * it to wherever MM allocated memory.
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

	.data : {
		_sdata = .;
		*(.data .data.*)
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
		*(.init_array*) *(.fini_array*)
	}
}
