/*	pkg - a very small package manager for Galtzagorriak	(Flipper port)
 *
 *	pkg install [--flash] file.tar
 *				install (or upgrade) a package; --flash: its
 *				programs also run from internal flash (root)
 *	pkg flash file...	run these programs from internal flash
 *	pkg unflash file...	stop that (their flash pages become free)
 *	pkg bind file...	give programs built into the firmware (make
 *				base-flash) to these files (/etc/rc, at boot)
 *	pkg remove name		remove a package
 *	pkg list		list installed packages
 *	pkg info name		show a package's control file
 *	pkg files name		list the files a package installed
 *	pkg get [--flash]	receive packages over the console and install
 *				them; on the PC: flipper/tools/flsend file.tar
 *
 * A package is a tar archive (name_version.tar, made on the PC with
 * flipper/tools/mkpkg.py) holding its files relative to / and a CONTROL
 * directory:
 *	CONTROL/control		Package:, Version:, Depends:, Description:,
 *				Flash: yes (as with install --flash)
 *	CONTROL/postinst	optional, run with sh after installing
 *	CONTROL/prerm		optional, run with sh before removing
 * The database is /usr/var/pkg/info: NAME.control, NAME.list (the files),
 * NAME.postinst, NAME.prerm.
 *
 * Written in C rather than as a script: on the Flipper every shell costs
 * ~35 KB of the 136 KB there is.  The archives are unpacked by tar(1).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <lib.h>
#include <minix/callnr.h>
#include <minix/gzversion.h>

#define DB	"/usr/var/pkg/info"
#define TMPDIR	"/usr/tmp"
#define LINE	256

static char tmp[64];			/* unpack directory */
static int to_flash;			/* install --flash */
static int ctl_flash;			/* this package says Flash: yes */
static char ctl_abi[8];			/* Abi: N or any; none = 1 (J.3) */
static char ctl_name[64], ctl_version[32], ctl_depends[LINE];

static void cleanup(void);

static void die(const char *msg, const char *arg)
{
	fprintf(stderr, "pkg: %s%s%s\n", msg, arg ? " " : "", arg ? arg : "");
	cleanup();
	exit(1);
}

/* Run a program in a directory and wait for it; returns its exit status. */
static int run(const char *dir, char *const argv[])
{
	pid_t pid;
	int st;

	fflush(stdout);
	if ((pid = fork()) < 0) die("cannot fork:", strerror(errno));
	if (pid == 0) {
		if (dir != NULL && chdir(dir) < 0) _exit(127);
		if (strcmp(argv[0], "/bin/tar") == 0) {	/* "Made directory" */
			close(1);
			open("/dev/null", O_WRONLY);
			dup2(1, 2);	/* (tar's status still reports errors) */
		}
		execv(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int rm_r(const char *path)
{
	char *argv[4];

	argv[0] = "/bin/rm"; argv[1] = "-r"; argv[2] = (char *) path;
	argv[3] = NULL;
	return run(NULL, argv);
}

static void cleanup(void)
{
	if (tmp[0] != 0) { rm_r(tmp); tmp[0] = 0; }
}

static void dbpath(char *buf, const char *name, const char *ext)
{
	sprintf(buf, "%s/%s.%s", DB, name, ext);
}

static int exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static int copy(const char *from, const char *to)
{
	char buf[512];
	int in, out, n;

	if ((in = open(from, O_RDONLY)) < 0) return -1;
	if ((out = creat(to, 0644)) < 0) { close(in); return -1; }
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, n) != n) { n = -1; break; }
	close(in);
	if (close(out) < 0) n = -1;
	return n < 0 ? -1 : 0;
}

/* Read the fields we need from a control file. */
static int read_control(const char *path)
{
	FILE *f;
	char line[LINE], *v;
	size_t n;

	ctl_name[0] = ctl_version[0] = ctl_depends[0] = 0;
	ctl_flash = 0;
	ctl_abi[0] = 0;
	if ((f = fopen(path, "r")) == NULL) return -1;
	while (fgets(line, sizeof line, f) != NULL) {
		if ((n = strlen(line)) > 0 && line[n - 1] == '\n') line[n - 1] = 0;
		if ((v = strchr(line, ':')) == NULL) continue;
		*v++ = 0;
		while (*v == ' ') v++;
		if (strcmp(line, "Package") == 0)
			strncpy(ctl_name, v, sizeof ctl_name - 1);
		else if (strcmp(line, "Version") == 0)
			strncpy(ctl_version, v, sizeof ctl_version - 1);
		else if (strcmp(line, "Depends") == 0)
			strncpy(ctl_depends, v, sizeof ctl_depends - 1);
		else if (strcmp(line, "Abi") == 0)
			strncpy(ctl_abi, v, sizeof ctl_abi - 1);
		else if (strcmp(line, "Flash") == 0 && strcmp(v, "yes") == 0)
			ctl_flash = 1;	/* spec R2b.5 */
	}
	fclose(f);
	return ctl_name[0] != 0 ? 0 : -1;
}

/* Is word a member of the comma/space separated list? */
static int in_list(const char *list, const char *word)
{
	size_t n = strlen(word);
	const char *p = list;

	while (*p != 0) {
		while (*p == ' ' || *p == ',') p++;
		if (strncmp(p, word, n) == 0
		    && (p[n] == 0 || p[n] == ' ' || p[n] == ',')) return 1;
		while (*p != 0 && *p != ' ' && *p != ',') p++;
	}
	return 0;
}

/* Write the files in the archive (minus CONTROL and directories) to list,
 * as absolute paths, using "tar tf": no directory walking, which would need
 * a buffer per directory level (memory is tight). */
static int file_list(const char *archive, const char *listpath)
{
	char raw[PATH_MAX], line[PATH_MAX];
	char *argv[4];
	FILE *in, *out;
	size_t n;
	pid_t pid;
	int st, fd;

	sprintf(raw, "%s.raw", listpath);
	fflush(stdout);
	if ((pid = fork()) < 0) return -1;
	if (pid == 0) {
		if ((fd = creat(raw, 0600)) < 0) _exit(127);
		dup2(fd, 1);
		dup2(fd, 2);		/* MINIX tar lists on stderr */
		close(fd);
		argv[0] = "/bin/tar"; argv[1] = "tf"; argv[2] = (char *) archive;
		argv[3] = NULL;
		execv(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { unlink(raw); return -1; }

	if ((in = fopen(raw, "r")) == NULL) return -1;
	if ((out = fopen(listpath, "w")) == NULL) { fclose(in); return -1; }
	while (fgets(line, sizeof line, in) != NULL) {
		if ((n = strlen(line)) > 0 && line[n - 1] == '\n') line[--n] = 0;
		if (n == 0 || line[n - 1] == '/') continue;	/* directory */
		if (strncmp(line, "CONTROL/", 8) == 0) continue;
		fprintf(out, "/%s\n", line[0] == '/' ? line + 1 : line);
	}
	fclose(in);
	fclose(out);
	unlink(raw);
	return 0;
}

/* Make the parent directories of path (mkdir -p). */
static void make_parents(char *path)
{
	char *p;

	for (p = path + 1; (p = strchr(p, '/')) != NULL; p++) {
		*p = 0;
		if (!exists(path)) mkdir(path, 0755);
		*p = '/';
	}
}

/* Install a program's text in internal flash (op 0) or remove it (op 1):
 * MM checks it (root, a file only root/bin could write), writes it once
 * and reads it back (spec Phase 2b, R2b.5).  Returns 0 or -errno. */
static int flash_op(const char *path, int op)
{
	message m;

	m.m1_p1 = (char *) path;
	m.m1_i1 = strlen(path) + 1;
	m.m1_i2 = MI_INSTALL;
	m.m1_i3 = op;
	return _syscall(MM, MEMINFO, &m) < 0 ? -errno : 0;
}

static int is_program(const char *path)	/* separate I&D a.out? */
{
	unsigned char h[4];
	int fd, n;

	if ((fd = open(path, O_RDONLY)) < 0) return 0;
	n = read(fd, (char *) h, 4);
	close(fd);
	return n == 4 && h[0] == 0x01 && h[1] == 0x03 && (h[2] & 0x20);
}

static int flash_files(char **v, int n, int op)
{
	int i, e, r = 0;

	for (i = 0; i < n; i++) {
		if ((e = flash_op(v[i], op)) == 0) continue;
		fprintf(stderr, "pkg: %s: %s\n", v[i], strerror(-e));
		r = 1;
	}
	return r;
}

/* Programs built into the firmware's flash (spec R2b.6) are found by the
 * hash of their text, once: then by the file, as installed ones. */
static int bind(char **v, int n)
{
	message m;
	int i, done = 0;

	m.m1_i2 = MI_INSTALL;
	m.m1_i3 = 3;				/* how many to bind? */
	if (_syscall(MM, MEMINFO, &m) <= 0) return 0;
	for (i = 0; i < n; i++)
		if (flash_op(v[i], 2) == 0) done++;
	if (done > 0) printf("pkg: %d built-in programs bound\n", done);
	return 0;
}

/* Copy each file named in the list from the scratch directory to its
 * place, keeping the mode; link first (same file system: no copy). */
static int place_files(const char *listpath)
{
	char line[PATH_MAX], src[PATH_MAX];
	struct stat st;
	FILE *list;
	size_t n;
	int r = 0, e, had;

	if ((list = fopen(listpath, "r")) == NULL) return -1;
	while (fgets(line, sizeof line, list) != NULL) {
		if ((n = strlen(line)) > 0 && line[n - 1] == '\n') line[--n] = 0;
		if (line[0] != '/') continue;
		sprintf(src, "%s%s", tmp, line);
		if (stat(src, &st) < 0) { r = -1; continue; }
		make_parents(line);
		/* the old version's flash copy goes; the new one follows */
		had = flash_op(line, 1) == 0;
		unlink(line);
		if (link(src, line) < 0) {
			if (copy(src, line) < 0) {
				fprintf(stderr, "pkg: cannot install %s\n", line);
				r = -1;
				continue;
			}
		}
		/* owner as in the package (chown clears set-uid: first) */
		chown(line, st.st_uid, st.st_gid);
		chmod(line, st.st_mode & 07777);
		/* last: the flash copy is bound to the ctime set just now */
		if ((to_flash || ctl_flash || had) && S_ISREG(st.st_mode)
		    && (st.st_mode & 0111)
		    && is_program(line) && (e = flash_op(line, 0)) != 0)
			fprintf(stderr, "pkg: %s stays in RAM: %s\n", line,
								strerror(-e));
	}
	fclose(list);
	return r;
}

static int install(const char *file)
{
	char abs[PATH_MAX], path[PATH_MAX], rel[PATH_MAX], buf[PATH_MAX];
	char *argv[4];
	char *dep, *p;
	int upgrade;

	if (!exists(file)) die("no such file:", file);
	if (file[0] == '/') strcpy(abs, file);
	else if (getcwd(abs, sizeof abs - strlen(file) - 2) != NULL)
		strcat(strcat(abs, "/"), file);
	else die("cannot get the current directory", NULL);

	/* Unpack into a scratch directory to read CONTROL and the file list. */
	sprintf(tmp, "%s/pkg.%d", TMPDIR, (int) getpid());
	if (mkdir(tmp, 0700) < 0) {
		/* Left over by a run that died (pids start again at boot):
		 * remove it and try once more.
		 */
		int e = errno;

		if (e == EEXIST) rm_r(tmp);
		if (e != EEXIST || mkdir(tmp, 0700) < 0) {
			tmp[0] = 0;
			die("cannot make", TMPDIR "/pkg.*");
		}
	}
	argv[0] = "/bin/tar"; argv[1] = "xf"; argv[2] = abs; argv[3] = NULL;
	if (run(tmp, argv) != 0) die("cannot unpack", file);
	sprintf(path, "%s/CONTROL/control", tmp);
	if (read_control(path) < 0) die("no valid CONTROL/control in", file);
	if (strcmp(ctl_abi, "any") != 0
	    && (ctl_abi[0] ? atoi(ctl_abi) : 1) != GZ_ABI) {
		fprintf(stderr, "pkg: %s needs ABI %s, this system is ABI %d\n",
			ctl_name, ctl_abi[0] ? ctl_abi : "1", GZ_ABI);
		cleanup();
		return 1;
	}

	for (p = ctl_depends; *p != 0; ) {	/* dependencies installed? */
		while (*p == ' ' || *p == ',') p++;
		for (dep = p; *p != 0 && *p != ' ' && *p != ','; p++) ;
		if (p == dep) break;
		strncpy(rel, dep, p - dep);
		rel[p - dep] = 0;
		dbpath(buf, rel, "control");
		if (!exists(buf)) {
			fprintf(stderr, "pkg: %s needs %s, which is not installed\n",
							ctl_name, rel);
			cleanup();
			return 1;
		}
	}

	dbpath(buf, ctl_name, "control");
	upgrade = exists(buf);
	printf("%s %s %s\n", upgrade ? "Upgrading" : "Installing", ctl_name,
								ctl_version);

	dbpath(buf, ctl_name, "list");
	if (file_list(abs, buf) < 0) die("cannot list the files of", file);

	/* Move the files from the scratch directory into place ourselves: a
	 * second tar would need another fork + exec, which may not fit. */
	dbpath(buf, ctl_name, "list");
	if (place_files(buf) < 0) die("installing the files failed", NULL);

	dbpath(buf, ctl_name, "control");
	copy(path, buf);
	dbpath(buf, ctl_name, "postinst"); unlink(buf);
	dbpath(buf, ctl_name, "prerm"); unlink(buf);
	sprintf(path, "%s/CONTROL/postinst", tmp);
	dbpath(buf, ctl_name, "postinst");
	if (exists(path)) copy(path, buf);
	sprintf(path, "%s/CONTROL/prerm", tmp);
	dbpath(buf, ctl_name, "prerm");
	if (exists(path)) copy(path, buf);
	cleanup();

	dbpath(buf, ctl_name, "postinst");
	if (exists(buf)) {
		argv[0] = "/bin/sh"; argv[1] = buf; argv[2] = NULL;
		run(NULL, argv);
	}
	sync();
	printf("%s %s installed\n", ctl_name, ctl_version);
	return 0;
}

static int remove_pkg(const char *name)
{
	char buf[PATH_MAX], line[PATH_MAX], other[64];
	char *argv[3];
	DIR *d;
	struct dirent *e;
	FILE *list;
	size_t n;

	dbpath(buf, name, "control");
	if (!exists(buf)) die("not installed:", name);

	/* Refuse if another package depends on it. */
	if ((d = opendir(DB)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			n = strlen(e->d_name);
			if (n < 9 || strcmp(e->d_name + n - 8, ".control") != 0)
				continue;
			sprintf(line, "%s/%s", DB, e->d_name);
			if (read_control(line) < 0) continue;
			if (strcmp(ctl_name, name) != 0 && in_list(ctl_depends, name)) {
				strcpy(other, ctl_name);
				closedir(d);
				fprintf(stderr, "pkg: %s depends on %s\n", other, name);
				return 1;
			}
		}
		closedir(d);
	}

	dbpath(buf, name, "prerm");
	if (exists(buf)) {
		argv[0] = "/bin/sh"; argv[1] = buf; argv[2] = NULL;
		run(NULL, argv);
	}
	dbpath(buf, name, "list");
	if ((list = fopen(buf, "r")) != NULL) {
		while (fgets(line, sizeof line, list) != NULL) {
			if ((n = strlen(line)) > 0 && line[n - 1] == '\n') line[n - 1] = 0;
			if (line[0] != '/') continue;
			(void) flash_op(line, 1);	/* if it was there */
			unlink(line);
		}
		fclose(list);
	}
	dbpath(buf, name, "list"); unlink(buf);
	dbpath(buf, name, "control"); unlink(buf);
	dbpath(buf, name, "postinst"); unlink(buf);
	dbpath(buf, name, "prerm"); unlink(buf);
	sync();
	printf("%s removed\n", name);
	return 0;
}

static int list_pkgs(void)
{
	char path[PATH_MAX];
	DIR *d;
	struct dirent *e;
	size_t n;

	if ((d = opendir(DB)) == NULL) die("cannot read", DB);
	while ((e = readdir(d)) != NULL) {
		n = strlen(e->d_name);
		if (n < 9 || strcmp(e->d_name + n - 8, ".control") != 0) continue;
		sprintf(path, "%s/%s", DB, e->d_name);
		if (read_control(path) == 0)
			printf("%-14s %s\n", ctl_name, ctl_version);
	}
	closedir(d);
	return 0;
}

static int cat(const char *name, const char *ext)
{
	char path[PATH_MAX], buf[512];
	int fd, n;

	dbpath(path, name, ext);
	if ((fd = open(path, O_RDONLY)) < 0) die("not installed:", name);
	while ((n = read(fd, buf, sizeof buf)) > 0) write(1, buf, n);
	close(fd);
	return 0;
}

#define INCOMING TMPDIR "/incoming"
#define MAXPKG	(4L * 1024 * 1024)	/* largest package accepted */

/* CRC-32 (IEEE 802.3, as Python's zlib.crc32), bitwise: no table. */
static unsigned long crc32_update(unsigned long crc, unsigned char *p, int n)
{
	int k;

	crc = ~crc;
	while (n-- > 0) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320UL & (0 - (crc & 1)));
	}
	return ~crc;
}

/* Read one line (without the newline) from fd 0; -1 on EOF. */
static int get_line(char *buf, int len)
{
	int n = 0;
	char c;

	while (read(0, &c, 1) == 1) {
		if (c == '\n') { buf[n] = 0; return n; }
		if (c == '\r') continue;
		if (n < len - 1) buf[n++] = c;
	}
	return -1;
}

static void say(const char *s)
{
	write(1, s, strlen(s));
}

static int good_name(const char *s)
{
	int n = strlen(s);

	if (n < 5 || n > 13 || strcmp(s + n - 4, ".tar") != 0) return 0;
	for (; *s != 0; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')
		      || (*s >= '0' && *s <= '9') || *s == '.' || *s == '_'
		      || *s == '-' || *s == '+')) return 0;
	return 1;
}

/* Receive files sent by flipper/tools/flsend over the console:
 *	host: "FILE name size crc32\n" + size raw bytes
 *	pkg:  "OK\n" (stored) or "BAD ...\n" (host may resend)
 *	host: "DONE\n"
 * The console is in raw mode meanwhile; the USB and TTY flow control make
 * sure no byte is lost, the CRC checks that none was changed.
 */
static int receive(void)
{
	struct termios old, raw;
	char line[96], name[16], path[PATH_MAX];
	unsigned char buf[512];
	unsigned long size, crc, want, left;
	int fd, n, files = 0;

	if (tcgetattr(0, &old) < 0) die("stdin is not a terminal", NULL);
	raw = old;
	raw.c_iflag = 0;
	raw.c_oflag = 0;
	raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);
	mkdir(INCOMING, 0700);
	say("GPKG READY\n");

	while (get_line(line, sizeof line) >= 0) {
		if (strcmp(line, "DONE") == 0) break;
		if (sscanf(line, "FILE %15s %lu %lx", name, &size, &want) != 3
		    || !good_name(name) || size > MAXPKG) {
			say("BAD header\n");
			continue;
		}
		sprintf(path, "%s/%s", INCOMING, name);
		if ((fd = creat(path, 0600)) < 0) { say("BAD create\n"); continue; }
		crc = 0;
		for (left = size; left > 0; left -= n) {
			n = left < sizeof buf ? (int) left : (int) sizeof buf;
			if ((n = read(0, buf, n)) <= 0) break;
			crc = crc32_update(crc, buf, n);
			if (write(fd, buf, n) != n) { n = -1; break; }
		}
		close(fd);
		if (left != 0 || crc != want) {
			unlink(path);
			say(left != 0 ? "BAD short\n" : "BAD crc\n");
			continue;
		}
		say("OK\n");
		files++;
	}
	tcsetattr(0, TCSANOW, &old);
	return files;
}

static int get(void)
{
	char path[PATH_MAX];
	DIR *d;
	struct dirent *e;
	size_t n;
	int r = 0, count = 0;

	if (receive() == 0) {
		fprintf(stderr, "pkg: no packages received\n");
		return 1;
	}
	if ((d = opendir(INCOMING)) == NULL) die("cannot read", INCOMING);
	while ((e = readdir(d)) != NULL) {
		n = strlen(e->d_name);
		if (n < 5 || strcmp(e->d_name + n - 4, ".tar") != 0) continue;
		sprintf(path, "%s/%s", INCOMING, e->d_name);
		if (install(path) != 0) r = 1;
		unlink(path);
		count++;
	}
	closedir(d);
	return r;
}

int main(int argc, char **argv)
{
	struct stat st;

	if (argc >= 2 && strcmp(argv[1], "bind") == 0)	/* no /usr needed */
		return bind(argv + 2, argc - 2);
	if (stat(DB, &st) < 0 || !S_ISDIR(st.st_mode))
		die(DB " is missing (is /usr mounted?)", NULL);
	if (argc == 3 && strcmp(argv[1], "install") == 0) return install(argv[2]);
	if (argc == 4 && strcmp(argv[1], "install") == 0
			&& strcmp(argv[2], "--flash") == 0) {
		to_flash = 1;
		return install(argv[3]);
	}
	if (argc >= 3 && strcmp(argv[1], "flash") == 0)
		return flash_files(argv + 2, argc - 2, 0);
	if (argc >= 3 && strcmp(argv[1], "unflash") == 0)
		return flash_files(argv + 2, argc - 2, 1);
	if (argc == 3 && strcmp(argv[1], "remove") == 0) return remove_pkg(argv[2]);
	if (argc == 2 && strcmp(argv[1], "list") == 0) return list_pkgs();
	if (argc == 3 && strcmp(argv[1], "info") == 0) return cat(argv[2], "control");
	if (argc == 3 && strcmp(argv[1], "files") == 0) return cat(argv[2], "list");
	if (argc == 2 && strcmp(argv[1], "get") == 0) return get();
	if (argc == 3 && strcmp(argv[1], "get") == 0
			&& strcmp(argv[2], "--flash") == 0) {
		to_flash = 1;
		return get();
	}
	fprintf(stderr, "usage: pkg install [--flash] file.tar | remove name | "
		"list | info name | files name | get [--flash] | flash file... | "
		"unflash file...\n");
	return 1;
}
