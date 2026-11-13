/* Milestone 2 test INIT: runs as process 1 with the real MM and FS and the
 * root file system on the SD card (QEMU: disk image).  Uses only the normal
 * C library: stdio on /dev/console, directories, file I/O, and finally
 * exec()s /bin/sh if present.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void check(int ok, const char *what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) fails++;
}

/* Leave QEMU with a status (semihosting); on the Flipper just stop. */
static void finish(void)
{
	printf(fails == 0 ? "M2 PASS\n" : "M2 FAIL\n");
	fflush(stdout);
#ifdef BOARD_QEMU
	{
		unsigned block[2] = { 0x20026, fails != 0 };
		register unsigned r0 __asm("r0") = 0x20;
		register unsigned r1 __asm("r1") = (unsigned) block;
		__asm volatile ("bkpt 0xAB" : : "r" (r0), "r" (r1) : "memory");
	}
#endif
	for (;;) pause();
}

int main(void)
{
	static char buf[2048];
	struct stat st;
	DIR *d;
	struct dirent *e;
	int fd, n, i, nfiles = 0;
	long sum;

	/* stdin, stdout, stderr - like the real init. */
	open("/dev/console", O_RDONLY);
	open("/dev/console", O_WRONLY);
	dup(1);

	printf("\nINIT (milestone 2): root file system is mounted\n");

	check(stat("/", &st) == 0 && S_ISDIR(st.st_mode), "stat /");
	if ((d = opendir("/")) != NULL) {
		printf("  / :");
		while ((e = readdir(d)) != NULL) {
			printf(" %s", e->d_name);
			nfiles++;
		}
		printf("\n");
		closedir(d);
	} else {
		perror("  opendir /");
	}
	check(nfiles >= 4, "readdir /");

	fd = open("/etc/motd", O_RDONLY);
	n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
	if (fd >= 0) close(fd);
	check(n > 0, "read /etc/motd");
	if (n > 0) {
		buf[n] = 0;
		printf("  motd: %s", buf);
	}

	/* Write a file spanning indirect blocks, read it back. */
	fd = creat("/tmp/test", 0644);
	check(fd >= 0, "creat /tmp/test");
	for (i = 0; i < (int) sizeof buf; i++) buf[i] = (char) (i * 7);
	for (i = 0, n = 0; i < 12; i++) n += write(fd, buf, sizeof buf);
	close(fd);
	check(n == 12 * (int) sizeof buf, "write 24 KB");
	sum = 0;
	fd = open("/tmp/test", O_RDONLY);
	while ((n = read(fd, buf, sizeof buf)) > 0)
		for (i = 0; i < n; i++) sum += (unsigned char) buf[i];
	close(fd);
	check(stat("/tmp/test", &st) == 0 && st.st_size == 12 * 2048L,
							"size of /tmp/test");
	{
		long expect = 0;
		for (i = 0; i < 2048; i++) expect += (unsigned char) (i * 7);
		check(sum == 12 * expect, "read back /tmp/test");
	}
	check(unlink("/tmp/test") == 0 && stat("/tmp/test", &st) < 0,
							"unlink /tmp/test");
	sync();

	if (access("/bin/m2test", X_OK) == 0) {
		static char *env[] = { "HOME=/", "PATH=/bin:/usr/bin", NULL };
		printf("  exec /bin/m2test\n");
		fflush(stdout);
		execle("/bin/m2test", "m2test", "hello", "world", (char *) NULL,
									env);
		check(0, "exec /bin/m2test");
	}
	finish();
	return 0;
}
