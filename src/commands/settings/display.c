/* Display (spec K.7): brightness, the idle timeout and the forced
 * backlight mode, through /dev/backlight (R6.9, R6.10).  The forced mode
 * is never saved: a reboot always gives the normal light back (R6.10). */
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "sgroup.h"

#define BL_LEVEL(s)	((s) >> 16 & 0xFF)
#define BL_TIMEOUT(s)	((s) & 0x3FFF)
#define BL_MODE(s)	((s) >> 14 & 3)

static const short tmo_s[] = { 5, 10, 15, 30, 60, 120, 300, 0 };

const struct srow display_rows[] = {
	{ "Brightness", S_INT, 0, 10, 100, 10, "%", "" },
	{ "Timeout", S_ENUM, 0, 0, 7, 1, "",
	  "5 s|10 s|15 s|30 s|1 min|2 min|5 min|never" },
	{ "Light", S_ENUM, S_NOSAVE, 0, 2, 1, "", "auto|off|on" },
};

static unsigned long bl_read(void)
{
	unsigned long st = 0;
	int fd;

	if ((fd = open("/dev/backlight", O_RDONLY)) >= 0) {
		if (read(fd, (char *) &st, 4) != 4) st = 0;
		close(fd);
	}
	return st;
}

static void load(void)
{
	unsigned long st = bl_read();
	int i, t = (int) BL_TIMEOUT(st), v;

	v = ((int) BL_LEVEL(st) * 100 + 127) / 255;
	v = (v + 5) / 10 * 10;
	sg_val[0] = (short) (v < 10 ? 10 : v);
	sg_val[1] = 2;
	for (i = 0; i < 8; i++) if (tmo_s[i] == t) sg_val[1] = (short) i;
	sg_val[2] = (short) ((int) BL_MODE(st) <= 2 ? BL_MODE(st) : 0);
}

/* Brightness, timeout and mode in one 4-byte write. */
static void apply(void)
{
	unsigned char b[4];
	int fd, lv = (sg_val[0] * 255 + 50) / 100, t = tmo_s[sg_val[1] & 7];

	b[0] = (unsigned char) lv;
	b[1] = (unsigned char) (t & 0xFF);
	b[2] = (unsigned char) (t >> 8);
	b[3] = (unsigned char) sg_val[2];
	if ((fd = open("/dev/backlight", O_WRONLY)) < 0) return;
	(void) write(fd, (char *) b, 4);
	close(fd);
}

int display_fn(int op, int row, void *buf)
{
	(void) row; (void) buf;
	if (op == SG_LOAD) load();
	else if (op == SG_APPLY) apply();
	return 0;
}
