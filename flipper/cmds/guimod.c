/* guimod - a desktop module's record from the shell (spec K.3, K.6; T23a).
 *
 * Usage:	guimod tile NAME TEXT ...	the module's tile on the Home
 *						page (set or change it)
 *		guimod status NAME TEXT ...	a short text (6 characters) on
 *						the status line
 *		guimod notify NAME TEXT ...	a notification on the NOTES
 *						page (T23c), counted unread
 *		guimod close NAME		the module's tile and status go
 *
 * One record per call, one write() to the desktop's FIFO /var/run/gui.
 * The module is the caller's real uid plus NAME (1-10 characters): it can
 * change only its own tiles.  Its process is guimod's parent, the shell
 * running the module: when that exits (or is killed) its tiles go.
 *
 * A FIFO does not say who wrote into it, so guimod is set-uid root (the
 * FIFO is root's, 0620, group gui): it writes the real uid into the
 * record, and drops root right after opening the FIFO.  It never waits:
 * no desktop, or a desktop busy with a full-screen app and a full FIFO,
 * is exit status 1.  A small C program, no stdio.
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "../gui/desktop.h"

static void say(const char *a, const char *b)
{
	(void) write(2, "guimod: ", 8);
	(void) write(2, a, strlen(a));
	if (b != (const char *) 0) (void) write(2, b, strlen(b));
	(void) write(2, "\n", 1);
}

int main(int argc, char **argv)
{
	char rec[DK_LINE], text[DK_LINE];
	int i, n, fd, close_rec, t = 0;
	uid_t uid = getuid();

	close_rec = argc == 3 && strcmp(argv[1], "close") == 0;
	if (!close_rec && (argc < 4 || (strcmp(argv[1], "tile") != 0
	    && strcmp(argv[1], "status") != 0
	    && strcmp(argv[1], "notify") != 0))) {
		(void) setuid(uid);
		say("usage: guimod tile|status|notify NAME TEXT ... | "
		    "guimod close NAME", (const char *) 0);
		return 2;
	}
	n = (int) strlen(argv[2]);
	if (n == 0 || n > DK_NAME || strchr(argv[2], '\t') != NULL
	    || strchr(argv[2], '\n') != NULL || strchr(argv[2], '\\') != NULL) {
		(void) setuid(uid);
		say("NAME: 1 to 10 characters, no tab, newline or \\", (const char *) 0);
		return 2;
	}
	for (i = 3; i < argc; i++) {		/* the words, as echo */
		n = (int) strlen(argv[i]);
		if (t + n + 1 >= DK_LINE) n = DK_LINE - 1 - t - (i > 3);
		if (n <= 0) break;
		if (i > 3) text[t++] = ' ';
		memcpy(text + t, argv[i], (size_t) n);
		t += n;
	}
	text[t] = 0;
	n = dk_record(rec, DK_LINE, argv[1], argv[2], (unsigned) uid,
		      (int) getppid(), close_rec ? (const char *) 0 : text);
	if (n < 0) {
		(void) setuid(uid);
		say("TEXT too long (a record is at most 128 bytes)", (const char *) 0);
		return 2;
	}
	fd = open(DK_PATH, O_WRONLY | O_NONBLOCK);
	if (setuid(uid) < 0) return 1;		/* root's rights end here */
	if (fd < 0) {
		say(errno == ENXIO ? "no desktop is running: " : "cannot open ",
		    DK_PATH);
		return 1;
	}
	if (write(fd, rec, (size_t) n) != n) {
		say(errno == EAGAIN ? "the desktop is busy: record dropped"
		    : "cannot write to ", errno == EAGAIN ? (const char *) 0
		    : DK_PATH);
		return 1;
	}
	return 0;
}
