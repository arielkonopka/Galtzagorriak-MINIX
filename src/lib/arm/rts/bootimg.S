/* Header of a boot server image (MM, FS, INIT): struct boot_image in
 * src/kernel/arm/armcpu.h.  Assembled per server with -DIMAGE_NAME="\"MM\""
 * and -DSTACK_SIZE=...; the symbols come from flipper/ld/server.ld. */
	.equ	BOOT_IMAGE_MAGIC, 0x56534D58

	.section .boot_header, "a"
	.p2align 2
	.global	_boot_header
_boot_header:
	.word	BOOT_IMAGE_MAGIC
	.word	_start			/* Thumb function: bit 0 is set */
	.word	_stext
	.word	_image_text_size	/* text, including the .data copy */
	.word	_sidata
	.word	_sdata
	.word	_image_data_size
	.word	_image_bss_size
	.word	STACK_SIZE
1:	.ascii	IMAGE_NAME
	.fill	8 - (. - 1b), 1, 0
