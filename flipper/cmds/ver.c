/* ver - the version of the system (spec J.2)
 *
 *	Galtzagorriak 0.1.0+7.g1a2b3c4
 *	built from a commit of 2026-11-15
 *	based on MINIX 2.0.0
 *	ABI 2
 *
 *	reset: watchdog (dev mode: a watchdog reset goes to DFU)
 *
 * The hardware revision and the CPU2 stack version follow once the system
 * can read them (Phases 6, 15).  All from the build: no file is read.
 */
#include <lib.h>
#include <stdio.h>
#include <minix/callnr.h>
#include <minix/gzversion.h>
#include <gzbuild.h>

/* RCC_CSR reset flags >> 24, most telling first (a watchdog reset also
 * sets the pin flag) */
static const struct { unsigned char bit; char name[16]; } cause[] = {
	{ 0x20, "watchdog" }, { 0x40, "window watchdog" },
	{ 0x80, "low power" }, { 0x08, "power on" }, { 0x10, "software" },
	{ 0x02, "option bytes" }, { 0x04, "reset pin" },
};

int main(void)
{
	printf("%s %s%s\n", GZ_NAME, GZ_RELEASE, GZ_BUILD);
	if (GZ_BUILD_DATE[0] != 0)
		printf("built from a commit of %s\n", GZ_BUILD_DATE);
	printf("based on %s\nABI %d\n", GZ_BASE, GZ_ABI);
	{
		message m;
		int r, i;

		m.m1_i2 = MI_RESET;
		if ((r = _syscall(MM, MEMINFO, &m)) >= 0) {
			for (i = 0; i < sizeof cause / sizeof cause[0]; i++)
				if (r & cause[i].bit) break;
			printf("reset: %s%s\n", i < sizeof cause / sizeof cause[0]
				? cause[i].name : "unknown", r & 0x100
				? " (dev mode: a watchdog reset goes to DFU)" : "");
		}
	}
	return 0;
}
