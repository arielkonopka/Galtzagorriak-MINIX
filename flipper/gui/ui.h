/* ui - list, message and keyboard screens shared by the ported stock apps
 * (infrared, archive; spec Phase 7, R5.6a).  See ui.c. */
#ifndef GUI_UI_H
#define GUI_UI_H

#define UI_ROWS		5		/* list rows under the title */
#define UI_ITEM		32		/* longest item text kept */

/* Item i of a list into buf (size UI_ITEM); 0, or -1 when there is none.
 * Called only for the rows on screen, so a list need not be in memory. */
typedef int (*ui_item_fn)(void *ctx, int i, char *buf);

/* A list of n items, sel first selected.  Returns the item OK was pressed
 * on, -1 on Back; *key (if not NULL) gets the key that ended it: KEY_OK,
 * or KEY_RIGHT when the caller set `right` (an item's actions). */
int ui_list(const char *title, ui_item_fn item, void *ctx, int n, int sel,
	int right, int *key);
/* The same for a fixed array of strings. */
int ui_menu(const char *title, const char *const *items, int n);
void ui_msg(const char *l1, const char *l2);	/* drawn, no wait */
void ui_note(const char *l1, const char *l2);	/* drawn, waits for a key */
/* An on-screen keyboard: edits buf (at most max - 1 characters, all safe
 * in a file name).  1 when finished with Ret, 0 on Back. */
int ui_text(const char *title, char *buf, int max);

#endif /* GUI_UI_H */
