/* winsrv - the window server of the desktop (spec K.8), see winsrv.c */
#ifndef GUI_WINSRV_H
#define GUI_WINSRV_H

#include <pwd.h>

char *win_run(const char *label, const char *cmd, struct passwd *pw,
	      int log);
int win_pending(void);
void win_reap(int now);

#endif /* GUI_WINSRV_H */
