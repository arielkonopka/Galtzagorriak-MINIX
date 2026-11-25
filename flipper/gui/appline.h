/* appline - /etc/apps lines and launcher icons (spec R5.6a); see appline.c */
#ifndef GUI_APPLINE_H
#define GUI_APPLINE_H

#define ICON_H		6		/* rows of an 8x6 icon */
#define ICON10_B	20		/* bytes of a 10x10 icon (R5.6b.3) */
#define APP_CMDMAX	64		/* longest command line kept */

struct app_fields {
	char *label;
	char *user;
	char *icon;			/* NULL: the old two-field form */
	char *cmd;
};

int app_line(char *line, struct app_fields *a);
int app_dir(char *line, struct app_fields *a);	/* folder lines, #461 */
int icon_parse(const char *f, unsigned char *rows,
		int (*glyph)(int c, int row));
int icon10_parse(const char *f, unsigned char *rows10);
const unsigned char *icon10_of(const unsigned char *rows);
int icon_count(void);
const char *icon_name(int i, unsigned char *rows);

#endif /* GUI_APPLINE_H */
