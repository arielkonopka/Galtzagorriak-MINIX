/* pkgbad - AC3.4 (spec R3.6): pkg refuses crafted packages with a message,
 * without crashing, and installs nothing of them.  Run as root with /usr
 * mounted; the packages are in /usr/tests/bad (tools/mkbadpkg.py):
 *  1. Depends: a 255-character name
 *  2. a member path of 250 characters (ustar prefix)
 *  3. a member ../pkgbad_dotdot
 *  4. Package: ../x
 *  5. a member with an absolute path
 *  6. a hard link member to /etc/shadow
 */
#include "t.h"

static char *tag[] = { "dep", "long", "dotdot", "name", "abs", "link" };
static char *what[] = {
	"a 255-character Depends: name",
	"a 250-character member path",
	"a ../ member",
	"Package: ../x",
	"an absolute member path",
	"a hard link to /etc/shadow",
};
/* Must not exist afterwards: what each package would have made. */
static char *leftover[] = {
	"/usr/var/pkg/info/baddep.control", "/usr/var/pkg/info/badlong.control",
	"/usr/var/pkg/tmp/pkgbad_dotdot", "/usr/var/pkg/pkgbad_dotdot",
	"/usr/var/pkg/x.control", "/usr/var/pkg/info/x.control",
	"/tmp/pkgbad_abs", "/usr/tests/bad/shadow", NULL
};

/* pkg install file; its message (stderr) into msg.  Returns the status. */
static int install(char *file, char *msg, int len)
{
	int p[2], pid, st, n = 0, r;

	fflush(stdout);
	if (pipe(p) < 0 || (pid = fork()) < 0) return -1;
	if (pid == 0) {
		close(p[0]);
		dup2(p[1], 2);
		close(p[1]);
		execl("/bin/pkg", "pkg", "install", file, (char *) 0);
		_exit(127);
	}
	close(p[1]);
	while (n < len - 1 && (r = read(p[0], msg + n, len - 1 - n)) > 0)
		n += r;
	msg[n] = 0;
	close(p[0]);
	if (waitpid(pid, &st, 0) != pid) return -1;
	return st;
}

int main(void)
{
	char file[64], mark[64], msg[256], *nl;
	int i, st;

	if (getuid() != 0) {
		printf("pkgbad: run it as root\n");
		return 1;
	}
	for (i = 0; i < 6; i++) {
		snprintf(file, sizeof(file), "/usr/tests/bad/%s.tar", tag[i]);
		snprintf(mark, sizeof(mark), "/usr/tests/bad/got_%s", tag[i]);
		if (access(file, R_OK) < 0) {
			t_result(what[i], 0, "missing: pkg install "
						"/lib/pkg/tests_1.0.tar");
			continue;
		}
		st = install(file, msg, sizeof(msg));
		if ((nl = strchr(msg, '\n')) != NULL) *nl = 0;
		printf("  %s\n", msg);
		t_result(what[i], st != -1 && WIFEXITED(st)
			&& WEXITSTATUS(st) == 1 && strncmp(msg, "pkg: ", 5) == 0
			&& access(mark, F_OK) < 0, t_status(st));
		unlink(mark);
	}
	for (i = 0; leftover[i] != NULL; i++)
		if (access(leftover[i], F_OK) == 0) {
			t_result("nothing left over", 0, leftover[i]);
			unlink(leftover[i]);
		}
	return t_fail;
}
