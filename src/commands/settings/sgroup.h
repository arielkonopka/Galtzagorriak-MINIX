/* sgroup.h - one group of the system settings (spec K.7): its rows, how
 * its values are read and applied, its file.  Each group is one source
 * file here with one `struct sgroup`; the settings program
 * (flipper/cmds/settings.c) draws the rows, changes the values and saves
 * /etc/flipper/<name>.conf.  The drawing is the program's, not the
 * group's, so the groups stay the same when the screens move to the
 * desktop's window records (K.8). */
#ifndef SGROUP_H
#define SGROUP_H

#define S_ENUM		1	/* one of labels[0..max] */
#define S_INT		2	/* min..max by step, shown with unit */
#define S_ACT		3	/* OK does something (apply with the row) */
#define S_TEXT		4	/* shown only: the text in unit (About) */

#define S_NOSAVE	1	/* flags: applied, never written to the file */

struct srow {
	const char *name;		/* also the key in the file */
	unsigned char type, flags;
	short min, max, step;
	const char *unit;		/* S_INT: after the number; S_TEXT:
					 * the text */
	const char * const *labels;	/* S_ENUM; NULL: the group's label() */
	int val;			/* now */
};

struct sgroup {
	const char *name;		/* settings NAME, NAME.conf */
	const char *title;		/* the screen's title */
	struct srow *rows;
	int nrows;
	void (*load)(struct sgroup *g);	/* the values now (the drivers) */
	void (*apply)(struct sgroup *g, int row);	/* row's value: now;
					 * row -1: at boot.  NULL: nothing to
					 * apply (settings apply skips it) */
	void (*leave)(struct sgroup *g);	/* or NULL: on Back, before
					 * the file (Time: /etc/TZ) */
	const char *(*label)(int val);	/* an S_ENUM without labels: the
					 * zones, from one string in flash */
};

extern struct sgroup display_group, sound_group, time_group, about_group;

#endif
