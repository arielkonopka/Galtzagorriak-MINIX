/* ver - the version of the system (spec J.2)
 *
 *	Galtzagorriak 0.1.0+7.g1a2b3c4
 *	built from a commit of 2026-11-15
 *	based on MINIX 2.0.0
 *	ABI 1
 *
 * The hardware revision and the CPU2 stack version follow once the system
 * can read them (Phases 6, 15).  All from the build: no file is read.
 */
#include <stdio.h>
#include <minix/gzversion.h>
#include <gzbuild.h>

int main(void)
{
	printf("%s %s%s\n", GZ_NAME, GZ_RELEASE, GZ_BUILD);
	if (GZ_BUILD_DATE[0] != 0)
		printf("built from a commit of %s\n", GZ_BUILD_DATE);
	printf("based on %s\nABI %d\n", GZ_BASE, GZ_ABI);
	return 0;
}
