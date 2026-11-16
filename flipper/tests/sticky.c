/* sticky - AC3.5 (spec R3.7): in a directory with the sticky bit (/tmp,
 * /usr/tmp) only the owner of a file or of the directory, or root, may
 * remove or rename it.  Run as root; the attempts run as nobody.
 *  1. /tmp has the sticky bit
 *  2. nobody: rm /tmp/rootfile (root's) -> EPERM; mv it -> EPERM
 *  3. nobody: its own file can be renamed and removed
 */
#include "t.h"

static void as_nobody(void)
{
	int fd, a, b, c;

	setgid(99);
	setuid(9999);
	a = unlink("/tmp/rootfile") == -1 && errno == EPERM;
	b = rename("/tmp/rootfile", "/tmp/nobodys") == -1 && errno == EPERM;
	c = (fd = creat("/tmp/nobodys", 0644)) >= 0 && close(fd) == 0
		&& rename("/tmp/nobodys", "/tmp/nobodys2") == 0
		&& unlink("/tmp/nobodys2") == 0;
	_exit((a ? 0 : 1) | (b ? 0 : 2) | (c ? 0 : 4));
}

int main(void)
{
	struct stat sb;
	int fd, st;

	if (getuid() != 0) {
		printf("sticky: run it as root\n");
		return 1;
	}
	t_result("/tmp has the sticky bit", stat("/tmp", &sb) == 0
		&& (sb.st_mode & 01000) != 0, NULL);
	if ((fd = creat("/tmp/rootfile", 0666)) >= 0) close(fd);
	st = t_child(as_nobody);
	t_result("nobody: rm /tmp/rootfile -> EPERM", st != -1
		&& WIFEXITED(st) && !(WEXITSTATUS(st) & 1), t_status(st));
	t_result("nobody: mv /tmp/rootfile -> EPERM", st != -1
		&& WIFEXITED(st) && !(WEXITSTATUS(st) & 2), t_status(st));
	t_result("nobody: mv and rm of its own file", st != -1
		&& WIFEXITED(st) && !(WEXITSTATUS(st) & 4), t_status(st));
	t_result("root may remove it", unlink("/tmp/rootfile") == 0, NULL);
	return t_fail;
}
