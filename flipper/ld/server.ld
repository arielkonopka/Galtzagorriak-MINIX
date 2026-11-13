/* Boot server image (MM, FS, INIT) for the MINIX kernel.  Linked to follow
 * the previous image: the Makefile passes --defsym FLASH_ORG=... RAM_ORG=...
 * (both click aligned) and STACK_SIZE.  RAM_END is the end of usable RAM.
 * The image starts with a struct boot_image header (src/lib/arm/rts/
 * bootimg.S); the kernel copies .data, clears .bss and the stack. */
ENTRY(_start)

MEMORY
{
	FLASH (rx)  : ORIGIN = FLASH_ORG, LENGTH = 512K
	RAM   (rwx) : ORIGIN = RAM_ORG, LENGTH = RAM_END - RAM_ORG
}

SECTIONS
{
	.text : {
		_stext = .;
		KEEP(*(.boot_header))
		*(.text .text.*)
		*(.rodata .rodata.*)
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

	_flash_end = LOADADDR(.data) + SIZEOF(.data);
	/* Sizes for the struct boot_image header (bootimg.S). */
	_image_text_size = _flash_end - _stext;
	_image_data_size = _edata - _sdata;
	_image_bss_size = _ebss - _sbss;
	_next_flash = ALIGN(_flash_end, 256);
	_next_ram = ALIGN(_ebss + STACK_SIZE, 256);
	ASSERT(_next_ram <= RAM_END, "boot server does not fit in RAM")

	/DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
