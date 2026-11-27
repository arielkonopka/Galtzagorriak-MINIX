/* Power-off (spec K.7, R6): the delays after which an idle Flipper locks
 * its keys or turns itself off, as stock's Auto Lock and Power settings.
 * The backlight's own timeout stays in Display (R6.9).  Kept in
 * /etc/flipper/power.conf on the card, written only when the user changes
 * a row (on leaving the group); read by the launcher, which owns the idle
 * time of the keys.  Nothing is applied at boot. */
#include "sgroup.h"

const struct srow power_rows[] = {
	{ "Auto off", S_ENUM, 0, 0, 5, 1, "",
	  "never|10 min|30 min|1 h|2 h|4 h" },
	{ "Auto lock", S_ENUM, 0, 0, 4, 1, "",
	  "never|1 min|5 min|10 min|30 min" },
	/* the locked screen: the time in the small font, or Ordua's desk
	 * clock (the user's wish, 2026-11-27) */
	{ "Lock clock", S_ENUM, 0, 0, 1, 1, "", "small|desk clock" },
};

int power_fn(int op, int row, void *buf)
{
	int i;

	(void) row; (void) buf;
	if (op == SG_LOAD)
		for (i = 0; i < 3; i++) if (sg_val[i] < 0) sg_val[i] = 0;
	return 0;
}
