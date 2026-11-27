/* winsrv - the window server of the desktop (spec K.8), see winsrv.c */
#ifndef GUI_WINSRV_H
#define GUI_WINSRV_H

#include <pwd.h>

/* extra: one more argument (a file to open, issue #413), or NULL. */
/* log: 1 write the records to stderr (GuiLog); WIN_LOCKED: the app is the
 * lock screen: it gets no keys, the hint line says how to unlock, and
 * Back, OK, Back in a row close it; win_run() then returns win_unlocked. */
#define WIN_LOCKED	2
extern const char win_unlocked[];
char *win_run(const char *label, const char *cmd, struct passwd *pw,
	      int log, char *extra);
int win_next(char **label, char **file);
int win_pending(void);
void win_reap(int now);

#endif /* GUI_WINSRV_H */
