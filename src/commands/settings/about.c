/* About (spec K.7): what this Flipper runs.  Nothing to change; each row
 * is read when the window is made (SG_TEXT), so nothing is kept in RAM.
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
#include <string.h>
#include <unistd.h>
#include <minix/callnr.h>
#include <minix/gzversion.h>
#include <minix/shlibc.h>
#include "sgroup.h"
#include "afmt.h"

#define BASE_CTL	"/usr/var/pkg/info/base.control"
#define UID64		0x1FFF7580L	/* STM32WB: 64-bit unique device id */

const struct srow about_rows[] = {
	{ "Base", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "ABI", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Kernel", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Free RAM", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "libc", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "libfuri", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Serial", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Licence", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
};

/* The Version: line of the base package's control file (near its top). */
static void base_version(char *v)
{
	char buf[256], *line, *next;
	int fd, n = -1;

	strcpy(v, "?");
	if ((fd = open(BASE_CTL, O_RDONLY)) >= 0) {
		n = (int) read(fd, buf, sizeof buf - 1);
		close(fd);
	}
	if (n <= 0) return;
	buf[n] = 0;
	for (line = buf; *line != 0; line = next) {
		if ((next = strchr(line, '\n')) != NULL) *next++ = 0;
		else next = line + strlen(line);
		if (af_field(line, "Version", v, 12)) return;
	}
}

static void kernel_build(char *v)
{
	message m;

	m.m1_p1 = v;
	m.m1_i1 = 24;
	m.m1_i2 = MI_BUILD;
	if (_syscall(MM, MEMINFO, &m) < 0) strcpy(v, "?");
	v[23] = 0;
}

static void free_ram(char *v)
{
	message m;
	int total;

	m.m1_p1 = NULL;
	m.m1_i1 = 0;
	m.m1_i2 = 0;
	if ((total = _syscall(MM, MEMINFO, &m)) < 0) strcpy(v, "?");
	else af_ram(v, (long) total, (long) m.m2_i1);
}

static void uid(char *v)
{
	unsigned char b[8];
	int fd, ok = 0;

	if ((fd = open("/dev/mem", O_RDONLY)) >= 0) {
		ok = lseek(fd, UID64, SEEK_SET) == UID64
			&& read(fd, (char *) b, sizeof b) == (int) sizeof b;
		close(fd);
	}
	if (ok) af_hex(v, b, (int) sizeof b);
	else strcpy(v, "?");
}

int about_fn(int op, int row, void *buf)
{
	char *v = buf;

	if (op != SG_TEXT) return 0;
	switch (row) {
	case 0: base_version(v); break;
	case 1: (void) af_dec(v, GZ_ABI); break;
	case 2: kernel_build(v); break;
	case 3: free_ram(v); break;
	case 4:
		if (SHLIBC_HDR->sh_magic == SHLIBC_MAGIC)
			af_lib(v, SHLIBC_HDR->sh_size, SHLIBC_HDR->sh_area);
		else strcpy(v, "none");
		break;
	case 5:
		if (SHFURI_HDR->sh_magic == SHFURI_MAGIC)
			af_lib(v, SHFURI_HDR->sh_size, SHFURI_HDR->sh_area);
		else strcpy(v, "none");
		break;
	case 6: uid(v); break;
	default: strcpy(v, "GPLv3, BSD-3");
	}
	return 0;
}
