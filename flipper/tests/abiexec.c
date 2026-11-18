/* abiexec - J.3: MM's exec checks the ABI stamp (a.out a_version).
 *  1. a program stamped with another ABI (99) -> ENOEXEC, nothing runs
 *  2. a program from before the stamp (a_version 0 = ABI 1, 32-bit time_t)
 *     -> ENOEXEC since ABI 2 (R2d.1); one with this ABI's stamp runs
 *  3. the system keeps running (this test goes on)
 * R2b.9 (shared libc in flash):
 *  4. a program linked with another build of the shared libc (a_tbase, the
 *     library's id, changed) -> ENOEXEC
 *  5. a string in the library's flash (strerror()) is a valid source for a
 *     system call, like a constant in the program's own text
 *  6. a program linked with another build of the shared libfuri + libapp
 *     (a_dbase, that library's id) -> ENOEXEC; with this build's id it runs
 *     (issue #201)
 * The copies are made of this test itself in /tmp.
 */
#include "t.h"
#include <a.out.h>
#include <minix/gzversion.h>
#include <minix/shlibc.h>

static int libc_other;			/* copy: flip the libc id */
static unsigned long furi_as;		/* copy: a_dbase (0: as it is) */

static int copy_stamped(char *from, char *to, unsigned short abi)
{
	char buf[512];
	int in, out, n, first = 1;

	if ((in = open(from, O_RDONLY)) < 0) return -1;
	if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755)) < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof buf)) > 0) {
		if (first) {		/* a_version: bytes 6-7, little-endian */
			buf[6] = abi & 0xFF;
			buf[7] = abi >> 8;
			if (libc_other && n >= 48) buf[40] ^= 1; /* a_tbase */
			if (furi_as != 0 && n >= 48) {	/* a_dbase */
				buf[44] = furi_as & 0xFF;
				buf[45] = (furi_as >> 8) & 0xFF;
				buf[46] = (furi_as >> 16) & 0xFF;
				buf[47] = (furi_as >> 24) & 0xFF;
			}
			first = 0;
		}
		if (write(out, buf, n) != n) n = -1;
	}
	close(in);
	if (close(out) < 0 || n < 0) return -1;
	return chmod(to, 0755);
}

static char *self;

static void run99(void)
{
	execl("/tmp/abi99", "abi99", "child", (char *) 0);
	_exit(errno == ENOEXEC ? 0 : 1);	/* exec must fail */
}

static void run0(void)
{
	execl("/tmp/abi0", "abi0", "child", (char *) 0);
	_exit(errno == ENOEXEC ? 0 : 1);	/* ABI 1: exec must fail */
}

static void runlib(void)
{
	execl("/tmp/abilib", "abilib", "child", (char *) 0);
	_exit(errno == ENOEXEC ? 0 : 1);	/* another libc: must fail */
}

static void runfuri(void)
{
	execl("/tmp/abifuri", "abifuri", "child", (char *) 0);
	_exit(errno == ENOEXEC ? 0 : 1);	/* another libfuri: must fail */
}

static void runfurok(void)
{
	execl("/tmp/abifurok", "abifurok", "child", (char *) 0);
	_exit(1);				/* exec must work */
}

static void runcur(void)
{
	execl("/tmp/abicur", "abicur", "child", (char *) 0);
	_exit(1);				/* exec must work */
}

int main(int argc, char **argv)
{
	int st, fd;
	struct t_hdr h;
	char *msg;

	if (argc > 1 && strcmp(argv[1], "child") == 0) _exit(7);
	self = argc > 0 && argv[0][0] == '/' ? argv[0] : "/usr/tests/abiexec";

	if (copy_stamped(self, "/tmp/abi99", 99) < 0
	    || copy_stamped(self, "/tmp/abi0", 0) < 0
	    || copy_stamped(self, "/tmp/abicur", GZ_ABI) < 0) {
		printf("FAIL abiexec: cannot copy %s to /tmp\n", self);
		return 1;
	}
	st = t_child(run99);
	t_result("exec of a program for ABI 99", st == 0,
		st == 0 ? "ENOEXEC" : "it ran or failed otherwise");
	st = t_child(run0);
	t_result("exec of a program from before the stamp (ABI 1)", st == 0,
		st == 0 ? "ENOEXEC" : "it ran or failed otherwise");
	st = t_child(runcur);
	t_result("exec of a program for this ABI",
		WIFEXITED(st) && WEXITSTATUS(st) == 7, NULL);

	if (t_header(self, &h) == 0 && (h.flags & A_LIBC)) {
		libc_other = 1;
		if (copy_stamped(self, "/tmp/abilib", GZ_ABI) < 0) {
			printf("FAIL abiexec: cannot copy %s to /tmp\n", self);
			return 1;
		}
		libc_other = 0;
		st = t_child(runlib);
		t_result("exec of a program for another shared libc build",
			st == 0, st == 0 ? "ENOEXEC" : "it ran or failed otherwise");
		unlink("/tmp/abilib");
		if (SHFURI_HDR->sh_magic == SHFURI_MAGIC) {
			furi_as = SHFURI_HDR->sh_id ^ 1;
			if (copy_stamped(self, "/tmp/abifuri", GZ_ABI) < 0)
				return 1;
			furi_as = SHFURI_HDR->sh_id;
			if (copy_stamped(self, "/tmp/abifurok", GZ_ABI) < 0)
				return 1;
			furi_as = 0;
			st = t_child(runfuri);
			t_result("exec of a program for another shared libfuri "
				"build", st == 0, st == 0 ? "ENOEXEC"
				: "it ran or failed otherwise");
			st = t_child(runfurok);
			t_result("exec of a program for this shared libfuri "
				"build", WIFEXITED(st) && WEXITSTATUS(st) == 7,
				NULL);
			unlink("/tmp/abifuri");
			unlink("/tmp/abifurok");
		} else {
			printf("SKIP abiexec: no shared libfuri in flash\n");
		}
	} else {
		printf("SKIP abiexec: not linked with the shared libc\n");
	}
	msg = strerror(ENOENT);		/* in the shared libc's flash */
	fd = open("/dev/null", O_WRONLY);
	st = fd < 0 ? -1 : write(fd, msg, strlen(msg));
	t_result("a string in the shared libc is a write() source",
		st == (int) strlen(msg), st < 0 ? strerror(errno) : NULL);
	if (fd >= 0) close(fd);
	unlink("/tmp/abi99");
	unlink("/tmp/abi0");
	unlink("/tmp/abicur");
	return t_fail;
}
