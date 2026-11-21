/* sgroup.h - one group of the system settings (spec K.7): its rows, how
 * its values are read and applied, its file.  Each group is one source
 * file here with one entry function and a const table of rows; the
 * window app setgroup (win.c) describes the rows to the desktop with
 * libgm (K.8: the desktop draws them and changes the values), and
 * settings apply (flipper/cmds/settings.c) applies the files at boot.
 *
 * RAM: the rows are const and hold no pointers, so they stay in flash
 * (a pointer would be relocated into RAM in a PIC program); the values
 * of the open group are sg_val[], 2 bytes a row. */
#ifndef SGROUP_H
#define SGROUP_H

#define S_ENUM		1	/* one of opts ("a|b|c"), index min..max */
#define S_INT		2	/* min..max by step, shown with unit */
#define S_ACT		3	/* OK does something (apply with the row) */
#define S_TEXT		4	/* shown only: SG_TEXT gives it (About) */
#define S_DATE		5	/* a date: val = SG_DATE(y, m, d) */
#define S_TIME		6	/* a time of day: val = minutes */
#define S_ITEM		7	/* OK opens a window of the group's own
				 * (SG_OPEN); SG_TEXT gives its right side */

#define S_NOSAVE	1	/* flags: applied, never written to the file */

#define SG_ROWS		8	/* rows of a group at most */
#define SG_TEXTMAX	32	/* SG_TEXT's buffer */

/* Packed dates (16 bits: years 2000-2127) */
#define SG_DATE(y, m, d)	((((y) - 2000) << 9) | ((m) << 5) | (d))
#define SG_Y(v)			(((v) >> 9 & 127) + 2000)
#define SG_M(v)			((v) >> 5 & 15)
#define SG_D(v)			((v) & 31)

struct srow {			/* const, in flash */
	char name[11];		/* the label; also the key in the file */
	unsigned char type, flags;
	short min, max, step;
	char unit[4];		/* S_INT: after the number */
	char opts[44];		/* S_ENUM: the options, '|' between */
};

/* What an entry function is asked to do (row: the row, -1 none) */
#define SG_LOAD		1	/* sg_val[] from the drivers now */
#define SG_APPLY	2	/* row's new value: now; row -1: at boot.
				 * Returns 1: show the window again */
#define SG_LEAVE	3	/* the screen is left (Time: /etc/TZ) */
#define SG_TEXT		4	/* the text of an S_TEXT/S_ITEM row to buf */
#define SG_OPEN		5	/* OK on an S_ITEM row: its window */
#define SG_EVENT	6	/* an event for a window of the group's own
				 * (buf: the event's fields, char **) */

extern short sg_val[SG_ROWS];

/* The groups (groups.c): their number, name, title and rows; the call. */
#define SG_N		7
const char *sg_name(int g);
const char *sg_title(int g);
const struct srow *sg_rows(int g, int *n);
int sg_find(const char *name);			/* -1: none */
int sg_call(int g, int op, int row, void *buf);

/* The file (conf.c): /etc/flipper/NAME.conf, "Key: value" lines (the
 * number, or the option of an enum), read and written with read/write,
 * no stdio.  conf_load: the card's copy, else the default in /rom. */
void conf_load(int g);
int conf_save(int g);				/* 0, or -1 */
const char *sg_opt(const char *opts, int k, int *len);	/* option k */
void conf_parse(int g, char *buf);		/* pure: host test */
int conf_format(int g, char *buf, int n);	/* pure: its length */

/* win.c: the group's main window again (from SG_EVENT) */
void sg_show(int g);

int display_fn(int op, int row, void *buf);
int sound_fn(int op, int row, void *buf);
int led_fn(int op, int row, void *buf);
int time_fn(int op, int row, void *buf);
int power_fn(int op, int row, void *buf);
int region_fn(int op, int row, void *buf);
int about_fn(int op, int row, void *buf);
extern const struct srow display_rows[], sound_rows[], led_rows[],
	time_rows[], power_rows[], region_rows[], about_rows[];

#endif
