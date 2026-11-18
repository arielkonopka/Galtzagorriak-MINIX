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

static const char * const tmo_name[] = { "5 s", "10 s", "15 s", "30 s",
	"1 min", "2 min", "5 min", "never" };
static const short tmo_s[] = { 5, 10, 15, 30, 60, 120, 300, 0 };
static const char * const mode_name[] = { "auto", "off", "on" };

static struct srow rows[] = {
	{ "Brightness", S_INT, 0, 10, 100, 10, "%", 0, 100 },
	{ "Timeout", S_ENUM, 0, 0, 7, 1, 0, tmo_name, 2 },
	{ "Light", S_ENUM, S_NOSAVE, 0, 2, 1, 0, mode_name, 0 },
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

static void load(struct sgroup *g)
{
	unsigned long st = bl_read();
	int i, t = (int) BL_TIMEOUT(st);

	(void) g;
	rows[0].val = ((int) BL_LEVEL(st) * 100 + 127) / 255;
	rows[0].val = (rows[0].val + 5) / 10 * 10;
	if (rows[0].val < 10) rows[0].val = 10;
	rows[1].val = 2;
	for (i = 0; i < 8; i++) if (tmo_s[i] == t) rows[1].val = i;
	rows[2].val = (int) BL_MODE(st) <= 2 ? (int) BL_MODE(st) : 0;
}

/* Brightness, timeout and mode in one 4-byte write. */
static void apply(struct sgroup *g, int row)
{
	unsigned char b[4];
	int fd, lv = (rows[0].val * 255 + 50) / 100, t = tmo_s[rows[1].val];

	(void) g; (void) row;
	b[0] = (unsigned char) lv;
	b[1] = (unsigned char) (t & 0xFF);
	b[2] = (unsigned char) (t >> 8);
	b[3] = (unsigned char) rows[2].val;
	if ((fd = open("/dev/backlight", O_WRONLY)) < 0) return;
	(void) write(fd, (char *) b, 4);
	close(fd);
}

struct sgroup display_group = {
	"display", "DISPLAY", rows, 3, load, apply
};
