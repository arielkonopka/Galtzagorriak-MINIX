/* gztree - switch the base files to the staged ones (spec RS.14, T33a
 * part 3).
 *
 *	gztree		started by INIT before /etc/rc when /etc/gz-tree
 *			is not the epoch built into the running system
 *
 * gzupdate apply (RS.10) unpacked the new base files into
 * /.gzupdate/<new>/ and /usr/.gzupdate/<new>/ and wrote
 * /.gzupdate/<new>/MANIFEST ("path mode uid gid sha512-first-32-bytes"
 * a line).  For each entry: a file already in place with that hash is
 * skipped; otherwise the file in place moves to /.gzupdate/<old>/ (or
 * /usr/.gzupdate/<old>/: each rename stays inside one file system) and
 * the staged one takes its place.  Last /etc/gz-tree gets the new epoch
 * (temporary file and rename) and everything is synced.  A power cut at
 * any point: the next boot's INIT runs gztree again, which goes on where it
 * stopped.  <old> is the epoch in /etc/gz-tree, "0" if there is none (a
 * DFU-flashed card).  The old files stay for one rollback (RS.16).
 *
 * INIT runs the staged copy (/.gzupdate/<new>/bin/gztree, which matches
 * the new shared libc), or /bin/gztree once that one has been moved.
 * Exit 0 done, 1 a file could not be switched (INIT boots anyway; the next
 * boot tries again).
 *
 * INIT runs it before /etc/rc, so /usr is not mounted yet: gztree mounts
 * /dev/hd2 (/etc/fstab's usr) on /usr itself for the /usr entries and
 * unmounts it again; /etc/rc checks and mounts it as always.
 *
 * RAM only while it runs: the 1 KB hash buffer and SHA-512 state (stack).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <minix/gzversion.h>
#include "gzsig.h"

#define TOPDIR	"/.gzupdate/"
#define USRDIR	"/usr/.gzupdate/"
#define GZTREE	"/etc/gz-tree"

static char old[12];
static char line[200], cur[128], from[128], to[128];

static void mkparents(char *p)
{
	char *s;

	for (s = p + 1; *s; s++) {
		if (*s != '/') continue;
		*s = 0;
		mkdir(p, 0755);
		*s = '/';
	}
}

/* The staged or saved place of `path` for epoch e. */
static void place(char *out, const char *e, const char *path)
{
	if (strncmp(path, "/usr/", 5) == 0)
		sprintf(out, USRDIR "%s/%s", e, path + 5);
	else
		sprintf(out, TOPDIR "%s%s", e, path);
}

/* 1 if `path` is there with the SHA-512 prefix hx (64 hex digits). */
static int in_place(const char *path, const char *hx)
{
	static const char d[] = "0123456789abcdef";
	struct gz_sha512 h;
	gz_u8 b[1024], dg[64];
	int fd, n, i;

	if ((fd = open(path, O_RDONLY)) < 0) return 0;
	gz_sha512_init(&h);
	while ((n = read(fd, b, sizeof b)) > 0) gz_sha512_update(&h, b, n);
	close(fd);
	if (n < 0) return 0;
	gz_sha512_final(&h, dg);
	for (i = 0; i < 32; i++)
		if (hx[2 * i] != d[dg[i] >> 4] || hx[2 * i + 1] != d[dg[i] & 15])
			return 0;
	return 1;
}

int main(void)
{
	char new[12], *p, *mode, *hx;
	FILE *m;
	int fd, n, bad = 0, usr;

	sprintf(new, "%ld", (long) GZ_EPOCH);
	strcpy(old, "0");
	if ((fd = open(GZTREE, O_RDONLY)) >= 0) {
		n = read(fd, old, sizeof old - 1);
		close(fd);
		old[n > 0 ? n : 0] = 0;
		if ((p = strchr(old, '\n'))) *p = 0;
		if (!old[0]) strcpy(old, "0");
	}
	if (strcmp(old, new) == 0) return 0;
	sprintf(cur, TOPDIR "%s/MANIFEST", new);
	if (!(m = fopen(cur, "r"))) {
		printf("gztree: no %s\n", cur);
		return 1;
	}
	printf("gztree: switching the base files to epoch %s\n", new);
	usr = access(USRDIR, F_OK) != 0 && mount("/dev/hd2", "/usr", 0) == 0;
	while (fgets(line, sizeof line, m)) {
		if (line[0] != '/' || !(mode = strchr(line, ' '))) continue;
		*mode++ = 0;
		if (!(hx = strrchr(mode, ' ')) || strlen(hx + 1) < 64) continue;
		hx++;
		if (in_place(line, hx)) continue;	/* done before a cut */
		place(from, new, line);
		place(to, old, line);
		if (access(from, F_OK) != 0) {
			printf("gztree: %s: staged copy missing\n", line);
			bad = 1;
			continue;
		}
		if (access(line, F_OK) == 0) {
			mkparents(to);
			unlink(to);
			if (rename(line, to) < 0) {
				printf("gztree: cannot move %s\n", line);
				bad = 1;
				continue;
			}
		}
		printf("gztree: %s\n", line);
		strcpy(cur, line);
		mkparents(cur);
		if (rename(from, line) < 0) {
			printf("gztree: cannot place %s\n", line);
			bad = 1;
		}
	}
	fclose(m);
	sync();
	if (usr) umount("/dev/hd2");	/* MINIX: the device */
	if (bad) return 1;
	if ((fd = open(GZTREE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0
	    || write(fd, new, strlen(new)) != (int) strlen(new)
	    || write(fd, "\n", 1) != 1 || close(fd) < 0
	    || rename(GZTREE ".new", GZTREE) < 0) {
		printf("gztree: cannot write %s\n", GZTREE);
		return 1;
	}
	sync();
	printf("gztree: done\n");
	return 0;
}
