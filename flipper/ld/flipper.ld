/* Flipper Zero, STM32WB55RG.
 * FLASH: the upper part of the 1 MB holds the CPU2 radio stack and FUS and
 *        must never be overwritten; 512 KB is far below it.
 * RAM:   SRAM1 only.  The first 8 bytes are skipped like the stock firmware
 *        does.  SRAM2a/2b (10 KB each usable; the rest is secure, owned by
 *        CPU2) are added later for the FS buffer cache. */
MEMORY
{
	FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
	RAM   (xrw) : ORIGIN = 0x20000008, LENGTH = 0x2FFF8
	RAM2A (xrw) : ORIGIN = 0x20030000, LENGTH = 10K
	RAM2B (xrw) : ORIGIN = 0x20038000, LENGTH = 10K
}
INCLUDE sections.ld

/* R2b.10: CPU2's shared tables, where it looks for them (0x20030000, the
 * reference table first); the FS buffer cache starts after (server.ld). */
SECTIONS
{
	.cpu2map 0x20030000 (NOLOAD) : { KEEP(*(.cpu2map)) } > RAM2A
	.cpu2mem2 0x2003A4C0 (NOLOAD) : { KEEP(*(.cpu2mem2)) } > RAM2B
}
ASSERT(SIZEOF(.cpu2mem2) <= 0x340, "CPU2 buffers over their 832 bytes of SRAM2b")
/* SRAM2b is the FS buffer cache up to 0x2003A4C0 (server.ld): the CPU2
 * buffers are its last 832 bytes, 0x2003A4C0..0x2003A800. */
ASSERT(ADDR(.cpu2mem2) == 0x20038000 + 10K - 0x340, "CPU2 buffers not at the end of SRAM2b")
ASSERT(SIZEOF(.cpu2map) <= 0x340, "CPU2 tables over their 832 bytes of SRAM2a")
