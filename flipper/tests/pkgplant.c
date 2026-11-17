/* pkgplant - AC3.3 (spec R3.5): another user cannot slip a package into a
 * later "pkg get" by root.
 *   pkgplant plant   (as root, before pkg get) as nobody: makes the old
 *                    incoming directory /usr/tmp/incoming and plants a
 *                    copy of the hello package in it; tries to write into
 *                    pkg's directories under /usr/var/pkg (must fail)
 *   pkgplant check   (after pkg get sent another package) hello was not
 *                    installed; /usr/var/pkg/in and tmp are root's, 700
 * Run "pkg remove hello" first if it is installed.
 */
#include "t.h"

#define HELLO	"/usr/var/pkg/info/hello.control"

static int refused(int r)
{
	return r < 0 && (errno == EACCES || errno == EEXIST || errno == ENOENT);
}

static void as_nobody(void)
{
	int ok = 1, fd;

	setgid(99);
	setuid(9999);
	/* The old place: /usr/tmp is everybody's, so this works. */
	(void) mkdir("/usr/tmp/incoming", 0777);
	if (t_copy("/lib/pkg/hello_1.0.tar", "/usr/tmp/incoming/hello_1.0.tar",
								0666) < 0)
		printf("  (could not plant in /usr/tmp/incoming)\n");
	/* The new places: root's only. */
	ok &= refused(mkdir("/usr/var/pkg/in", 0777));
	fd = creat("/usr/var/pkg/in/hello_1.0.tar", 0666);
	ok &= refused(fd);
	fd = creat("/usr/var/pkg/tmp/x", 0666);
	ok &= refused(fd);
	fd = creat("/usr/var/pkg/info/hello.list", 0666);
	ok &= refused(fd);
	_exit(ok ? 0 : 1);
}

static int root_dir(char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0
		&& (st.st_mode & 07777) == 0700;
}

int main(int argc, char **argv)
{
	int st;

	if (getuid() != 0 || argc != 2) {
		printf("usage (as root): pkgplant plant | check\n");
		return 1;
	}
	if (strcmp(argv[1], "plant") == 0) {
		if (access(HELLO, F_OK) == 0) {
			printf("pkgplant: run pkg remove hello first\n");
			return 1;
		}
		st = t_child(as_nobody);
		t_result("nobody cannot write pkg's directories", st != -1
			&& WIFEXITED(st) && WEXITSTATUS(st) == 0, t_status(st));
		printf("Now: pkg get, send a package other than hello "
			"(flsend), then pkgplant check\n");
		return t_fail;
	}
	t_result("the planted package was not installed",
		access(HELLO, F_OK) < 0 && access("/usr/bin/hello", F_OK) < 0,
		NULL);
	t_result("/usr/var/pkg/in and /usr/var/pkg/tmp are root's, 700",
		root_dir("/usr/var/pkg/in") && root_dir("/usr/var/pkg/tmp"),
		NULL);
	return t_fail;
}
