/* groups.c - the groups of the system settings (spec K.7), in the order of
 * the launcher's SETTINGS page.  A switch, not a table of pointers: a
 * pointer table would be relocated into RAM in a PIC program. */
#include <string.h>
#include "sgroup.h"

short sg_val[SG_ROWS];

static const char names[SG_N][8] = { "display", "sound", "time", "about" };
static const char titles[SG_N][8] = { "Display", "Sound", "Time", "About" };

const char *sg_name(int g)
{
	return names[g];
}

const char *sg_title(int g)
{
	return titles[g];
}

int sg_find(const char *name)
{
	int g;

	for (g = 0; g < SG_N; g++) if (strcmp(names[g], name) == 0) return g;
	return -1;
}

const struct srow *sg_rows(int g, int *n)
{
	switch (g) {
	case 0: *n = 3; return display_rows;
	case 1: *n = 2; return sound_rows;
	case 2: *n = 4; return time_rows;
	default: *n = 8; return about_rows;
	}
}

int sg_call(int g, int op, int row, void *buf)
{
	switch (g) {
	case 0: return display_fn(op, row, buf);
	case 1: return sound_fn(op, row, buf);
	case 2: return time_fn(op, row, buf);
	default: return about_fn(op, row, buf);
	}
}
