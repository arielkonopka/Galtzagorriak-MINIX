/* QEMU mps2-an386.  Code runs from SSRAM1 at 0; RAM is limited to the same
 * 192 KB the Flipper has so memory pressure is realistic. */
MEMORY
{
	FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 512K
	RAM   (xrw) : ORIGIN = 0x20000008, LENGTH = 0x2FFF8
}
INCLUDE sections.ld
