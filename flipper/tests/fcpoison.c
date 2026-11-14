/* fcpoison - S12/R1.4: the flash text cache must not let one user's file
 * supply the text that runs for another user's program.
 *  1. A nobody-owned copy of /bin/echo with its text replaced (same header:
 *     same CRC and size) is run as nobody.  Then /bin/echo must still
 *     print what it is told.  (Strongest when echo was not cached before;
 *     the fix makes the order irrelevant: the owner is part of the key.)
 *  2. A root-owned, mode 777 (group/other-writable) program is never put
 *     in the cache: the number of cached programs stays the same.
 */
#include "t.h"
#include <lib.h>
#include <pwd.h>
#include <minix/callnr.h>

#define FAKE	"/tmp/fcpoison.x"
#define OPENPROG	"/tmp/fcopen.x"

static int nobody;

static void run_fake(void)
{
	setgid(nobody);
	setuid(nobody);
	execl(FAKE, "echo", "x", (char *) 0);
	_exit(2);
}

static void run_open(void)
{
	int fd = open("/dev/null", O_WRONLY);

	dup2(fd, 1);
	execl(OPENPROG, "echo", "x", (char *) 0);
	_exit(2);
}

static int cached(void)
{
	message m;
	struct fcinfo fi;

	m.m1_p1 = (char *) &fi;
	m.m1_i2 = MI_FCACHE;
	if (_syscall(MM, MEMINFO, &m) != 0) return -1;
	return fi.entries;
}

int main(void)
{
	struct t_hdr h;
	struct passwd *pw;
	char buf[64], *p;
	int st, fd[2], n, i, before;
	static unsigned char udf[512];

	if ((pw = getpwnam("nobody")) == NULL) {
		t_result("user nobody", 0, NULL);
		return 1;
	}
	nobody = pw->pw_uid;
	if (t_header("/bin/echo", &h) < 0) {
		t_result("read /bin/echo", 0, NULL);
		return 1;
	}

	/* 1: same header, text all UDF (0xDE00: permanently undefined) */
	if (t_copy("/bin/echo", FAKE, 0755) < 0) {
		t_result("copy /bin/echo", 0, NULL);
		return 1;
	}
	for (i = 0; i < (int) sizeof(udf); i += 2) {
		udf[i] = 0x00;
		udf[i + 1] = 0xDE;
	}
	for (n = 0; n < h.text; n += sizeof(udf))
		t_patch(FAKE, h.hdrlen + n, udf,
			h.text - n < (long) sizeof(udf) ? (int) (h.text - n)
							 : (int) sizeof(udf));
	chown(FAKE, nobody, nobody);
	chmod(FAKE, 0755);
	st = t_child(run_fake);
	printf("     (the fake ran as nobody: %s)\n", t_status(st));

	pipe(fd);
	if (fork() == 0) {
		dup2(fd[1], 1);
		close(fd[0]);
		execl("/bin/echo", "echo", "real-echo", (char *) 0);
		_exit(2);
	}
	close(fd[1]);
	n = read(fd[0], buf, sizeof(buf) - 1);
	close(fd[0]);
	wait(&st);
	buf[n > 0 ? n : 0] = 0;
	if ((p = strchr(buf, '\n')) != NULL) *p = 0;
	t_result("/bin/echo runs its own text after the fake",
		strcmp(buf, "real-echo") == 0, buf);
	unlink(FAKE);

	/* 2: writable by others: never cached */
	before = cached();
	if (t_copy("/bin/echo", OPENPROG, 0777) < 0) {
		t_result("copy /bin/echo", 0, NULL);
		return 1;
	}
	/* a different CRC so it can't match echo's entry */
	h.syms ^= 0x5A5A5A5AL;
	t_patch(OPENPROG, 24 + 4, &h.syms, 4);
	st = t_child(run_open);
	n = cached();
	sprintf(buf, "%d cached before, %d after (%s)", before, n,
							t_status(st));
	t_result("mode 777 program not cached", before >= 0 && n <= before,
									buf);
	unlink(OPENPROG);

	t_result("system still running", 1, NULL);
	return t_fail;
}
