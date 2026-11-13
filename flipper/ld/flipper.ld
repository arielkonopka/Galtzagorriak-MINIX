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
