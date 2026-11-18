/* desktop - the module records of spec K.3 / K.6 (tiles), see desktop.c */
#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#define DK_PATH		"/var/run/gui"	/* the desktop's FIFO */
#define DK_LINE		128		/* one record, its '\n' included */
#define DK_NTILES	16		/* the table: 16 entries of 48 B */
#define DK_NAME		10		/* longest module name */
#define DK_TEXT		31		/* longest tile text (32 columns) */
#define DK_STATUS	6		/* status text drawn on the status line */

/* Verbs (dk_parse) */
#define DV_TILE		1		/* tile NAME UID PID TEXT */
#define DV_STATUS	2		/* status NAME UID PID TEXT */
#define DV_NOTIFY	3		/* notify NAME UID PID TEXT */
#define DV_CLOSE	4		/* close NAME UID PID */

/* What a record changed (dk_apply, dk_purge): bits */
#define DC_TILES	1		/* the Home page */
#define DC_STATUS	2		/* the status line's module text */
#define DC_NOTE		4		/* a notification came */
#define DC_FULL		8		/* dropped: the table is full */

/* One entry: 48 B.  kind 0: free. */
struct dk_tile {
	unsigned short uid;		/* the module: uid + name */
	short pid;			/* its process, for "exited" */
	char kind;			/* DV_TILE or DV_STATUS */
	char name[DK_NAME + 1];
	char text[DK_TEXT + 1];
};

/* The desktop's state: the table and the last notification. */
struct desk {
	struct dk_tile t[DK_NTILES];
	char note[DK_TEXT + 1];		/* the last notification's text */
	unsigned char unread;		/* notifications not seen (max 99) */
	signed char st;			/* the status shown: entry, -1 none */
};

/* One parsed record: pointers into the line given to dk_parse. */
struct dk_rec {
	int verb;
	unsigned short uid;
	short pid;
	const char *name, *text;
};

int dk_parse(char *line, struct dk_rec *r);
int dk_apply(struct desk *d, const struct dk_rec *r);
int dk_purge(struct desk *d, int (*alive)(int pid));
int dk_tiles(const struct desk *d);
const struct dk_tile *dk_tile(const struct desk *d, int i);
int dk_record(char *buf, int size, const char *verb, const char *name,
	      unsigned uid, int pid, const char *text);

#endif /* GUI_DESKTOP_H */
