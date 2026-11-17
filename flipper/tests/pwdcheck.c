/* pwdcheck - R3.3: pwdauth answers a shadow ("##") password check only for
 * root or the user itself, and a failed one takes 2 s.  Run as root with
 * root's password as the argument (a test system only: it shows in ps).
 *  1. root, right password: "##root" comes back
 *  2. root, wrong password: refused, after >= 2 s
 *  3. nobody, right password: refused (not its account)
 *  4. empty password (login and su ask that first): refused at once
 */
#include "t.h"
#include <time.h>

static char *pw;

static void nobody_asks(void)
{
	setgid(99);
	setuid(9999);
	_exit(strcmp(crypt(pw, "##root"), "##root") == 0 ? 1 : 0);
}

int main(int argc, char **argv)
{
	time_t t;
	int ok, st;

	if (getuid() != 0 || argc != 2) {
		printf("usage (as root): pwdcheck root's-password\n");
		return 1;
	}
	pw = argv[1];
	t_result("root, right password", strcmp(crypt(pw, "##root"),
						"##root") == 0, NULL);
	t = time(NULL);
	ok = strcmp(crypt("not-the-password", "##root"), "##root") != 0;
	t_result("root, wrong password: refused after >= 2 s", ok
		&& time(NULL) - t >= 2, NULL);
	st = t_child(nobody_asks);
	t_result("nobody, right password: refused", st != -1
		&& WIFEXITED(st) && WEXITSTATUS(st) == 0, t_status(st));
	t = time(NULL);
	ok = strcmp(crypt("", "##root"), "##root") != 0;
	t_result("empty password: refused at once", ok
		&& time(NULL) - t < 2, NULL);
	return t_fail;
}
