/* About (spec K.7): what this Flipper runs.  Nothing to change; every row
 * is read when the screen opens.
 *	Base		the base package's version (pkg's database)
 *	ABI		of the programs (J.3)
 *	Kernel		the system image's build id (MM, MI_BUILD)
 *	Free RAM	total free and the largest hole (free)
 *	libc, libfuri	the shared libraries of free -v: size in flash and
 *			their data in every process
 *	Serial		the chip's unique id (UID64, through /dev/mem)
 *	Licence */
#include <lib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <minix/callnr.h>
#include <minix/gzversion.h>
#include <minix/shlibc.h>
#include "sgroup.h"
#include "afmt.h"

#define BASE_CTL	"/usr/var/pkg/info/base.control"
#define UID64		0x1FFF7580L	/* STM32WB: 64-bit unique device id */

static char base[12], abi[4], kern[24], ram[24], libc[24], furi[24],
	serial[17];

static struct srow rows[] = {
	{ "Base", S_TEXT, S_NOSAVE, 0, 0, 0, base, 0, 0 },
	{ "ABI", S_TEXT, S_NOSAVE, 0, 0, 0, abi, 0, 0 },
	{ "Kernel", S_TEXT, S_NOSAVE, 0, 0, 0, kern, 0, 0 },
	{ "Free RAM", S_TEXT, S_NOSAVE, 0, 0, 0, ram, 0, 0 },
	{ "libc", S_TEXT, S_NOSAVE, 0, 0, 0, libc, 0, 0 },
	{ "libfuri", S_TEXT, S_NOSAVE, 0, 0, 0, furi, 0, 0 },
	{ "Serial", S_TEXT, S_NOSAVE, 0, 0, 0, serial, 0, 0 },
	{ "Licence", S_TEXT, S_NOSAVE, 0, 0, 0, "GPLv3, BSD-3", 0, 0 },
};

static void base_version(void)
{
	char line[48];
	FILE *f;

	strcpy(base, "?");
	if ((f = fopen(BASE_CTL, "r")) == NULL) return;
	while (fgets(line, sizeof line, f) != NULL)
		if (af_field(line, "Version", base, sizeof base)) break;
	fclose(f);
}

static void kernel_build(void)
{
	message m;

	m.m1_p1 = kern;
	m.m1_i1 = (int) sizeof kern;
	m.m1_i2 = MI_BUILD;
	if (_syscall(MM, MEMINFO, &m) < 0) strcpy(kern, "?");
	kern[sizeof kern - 1] = 0;
}

static void free_ram(void)
{
	message m;
	int total;

	m.m1_p1 = NULL;
	m.m1_i1 = 0;
	m.m1_i2 = 0;
	if ((total = _syscall(MM, MEMINFO, &m)) < 0) strcpy(ram, "?");
	else af_ram(ram, (long) total, (long) m.m2_i1);
}

static void uid(void)
{
	unsigned char b[8];
	int fd, ok = 0;

	if ((fd = open("/dev/mem", O_RDONLY)) >= 0) {
		ok = lseek(fd, UID64, SEEK_SET) == UID64
			&& read(fd, (char *) b, sizeof b) == (int) sizeof b;
		close(fd);
	}
	if (ok) af_hex(serial, b, (int) sizeof b);
	else strcpy(serial, "?");
}

static void load(struct sgroup *g)
{
	(void) g;
	base_version();
	sprintf(abi, "%d", GZ_ABI);
	kernel_build();
	free_ram();
	if (SHLIBC_HDR->sh_magic == SHLIBC_MAGIC)
		af_lib(libc, SHLIBC_HDR->sh_size, SHLIBC_HDR->sh_area);
	else strcpy(libc, "none");
	if (SHFURI_HDR->sh_magic == SHFURI_MAGIC)
		af_lib(furi, SHFURI_HDR->sh_size, SHFURI_HDR->sh_area);
	else strcpy(furi, "none");
	uid();
}

struct sgroup about_group = {
	"about", "ABOUT", rows, 8, load, NULL, NULL
};
