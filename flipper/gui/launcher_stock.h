/* launcher_stock - the launcher's "Stock" style (spec R5.6b); see
 * launcher_stock.c.  It only draws: the launcher keeps the pages, keys
 * and settings (R5.6b.1). */
#ifndef GUI_LAUNCHER_STOCK_H
#define GUI_LAUNCHER_STOCK_H

/* One row as the launcher gives it: the label, and a 10x10 icon (10 rows
 * of 2 bytes, MSB left) or else an 8x6 one (6 rows of 1 byte), or none. */
struct stock_row {
	const char *label;
	const unsigned char *icon10;
	const unsigned char *icon6;
	unsigned char rows[6];		/* room for an icon the row makes */
	char text[28];			/* and for a label it makes */
};
typedef void (*stock_row_fn)(int i, struct stock_row *r);

#define STOCK_SUB_ROWS	3		/* rows of a submenu with a header */

/* The Apps page as the stock main menu (menu.c): the entry before, the
 * selected one in its frame, the one after; the scroll bar. */
void stock_menu(int n, int pos, stock_row_fn row);
/* Another page as a stock submenu with header: rows top.. of n. */
void stock_submenu(const char *header, int n, int pos, int top,
		   stock_row_fn row);
/* A message over the frame: a stock-style popup box. */
void stock_popup(const char *msg);
/* The first-boot wizard's sample (spec R18b.2 step 6): a short list of
 * apps in the Pine (stock = 0) or the Stock style, drawn into the frame
 * buffer (not flushed). */
void stock_sample(int stock);

#endif /* GUI_LAUNCHER_STOCK_H */
