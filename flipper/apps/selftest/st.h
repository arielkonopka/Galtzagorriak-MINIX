/* st.h - shared by the self test app and its modules (spec 7b.3).
 *
 * A module is a program in /usr/lib/selftest/ with a one-line description
 * file NAME.d next to it:  "a description" (runs in All tests) or
 * "i description" (interactive: only when picked).  It draws its own
 * screen with the gui library and writes one result line to standard
 * output: "PASS", "FAIL reason" or "SKIP reason".  With -a it must not
 * wait for the user.  Long Back (R4b.8) ends it: the app logs "FAIL
 * stopped".
 */
#ifndef ST_H
#define ST_H

#define ST_DIR		"/usr/lib/selftest"
#define ST_LOGDIR	"/home/flipper/apps_data/selftest"
#define ST_LOG		ST_LOGDIR "/log"

/* The module's result: on standard output for the app, on standard
 * error too when run from a shell. */
void st_result(const char *what, const char *reason);

#endif
