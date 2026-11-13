/* Section layout shared by all boards.  The board script defines the FLASH
 * and RAM memory regions and then INCLUDEs this file. */
ENTRY(Reset_Handler)

_stack_size = 0x800;		/* boot/kernel stack (MSP) */

SECTIONS
{
	.isr_vector : {
		. = ALIGN(4);
		KEEP(*(.isr_vector))
	} > FLASH

	.text : {
		. = ALIGN(4);
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.glue_7 .glue_7t .eh_frame)
		. = ALIGN(4);
		_etext = .;
	} > FLASH

	.ARM.exidx : {
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
	} > FLASH

	_sidata = LOADADDR(.data);
	.data : {
		. = ALIGN(4);
		_sdata = .;
		*(.data .data.*)
		. = ALIGN(4);
		_edata = .;
	} > RAM AT > FLASH

	.bss (NOLOAD) : {
		. = ALIGN(4);
		_sbss = .;
		*(.bss .bss.* COMMON)
		. = ALIGN(4);
		_ebss = .;
	} > RAM

	/* Everything from _end to the bottom of the stack is free memory. */
	_end = .;
	_estack = ORIGIN(RAM) + LENGTH(RAM);
	ASSERT(_end + _stack_size <= _estack, "RAM overflow: no room for stack")

	/* Used by the MINIX kernel (src/kernel/arm). */
	_stext = ORIGIN(FLASH);
	_ram_start = ORIGIN(RAM);
	_ram_end = ORIGIN(RAM) + LENGTH(RAM);
	/* The boot server images (MM, FS, INIT) follow in flash and RAM. */
	_next_flash = ALIGN(LOADADDR(.data) + SIZEOF(.data), 256);
	_next_ram = ALIGN(_ebss, 256);
	_boot_images = _next_flash;

	/DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
