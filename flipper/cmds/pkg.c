/*	pkg - a very small package manager for Galtzagorriak	(Flipper port)
 *
 *	pkg install [--flash] file.tar
 *				install (or upgrade) a package; --flash: its
 *				programs also run from internal flash (root)
 *	pkg flash file...	run these programs from internal flash
 *	pkg unflash file...	stop that (their flash pages become free)
 *	pkg bind file...	give programs built into the firmware (make
 *				base-flash) to these files (/etc/rc, at boot)
 *	pkg budget [pages]	show (set: root) how many run-time cache pages
 *				may be erased a day (R2b.11; 0: no caching;
 *				/etc/rc sets fcache_budget= from rc.conf)
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
 *				Flash: yes (as with install --flash); the
 *				app manifest (spec F.2): Timers: yes|no,
 *				Notify: alarm|reminder|info, DeskMode:
 *				yes|no, checked here, granted with the
 *				app's own uid (A.3); Type: app|window
 *				(window: a window app, spec K.8)
 *	CONTROL/postinst	optional, run with sh after installing
 *	CONTROL/prerm		optional, run with sh before removing
 * The database is /usr/var/pkg/info: NAME.control, NAME.list (the files),
 * NAME.postinst, NAME.prerm.
 *
 * Hardening (spec R3.5, R3.6): packages are unpacked in a scratch directory
 * with a random name under /usr/var/pkg/tmp and received into
 * /usr/var/pkg/in, both made here and used only if stat() shows root's
 * with mode 700; pkg get installs only what it received in that run.  The
 * archive's headers are read before anything is unpacked: absolute names,
 * ".." components, names tar would cut short, and members other than files
 * and directories (links, devices) refuse the package.  Package names and
 * Depends: tokens are short words of [A-Za-z0-9._+-]; every path is built
 * with snprintf; pkg remove unlinks only the paths of the package's list.
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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tar.h>
#include <termios.h>
#include <unistd.h>
#include <lib.h>
#include <minix/callnr.h>
#include <minix/gzversion.h>
#include <minix/shlibc.h>

#define PKGDIR	"/usr/var/pkg"
#define DB	PKGDIR "/info"
#define SCRATCH	PKGDIR "/tmp"		/* unpack directories: root, 700 */
#define LINE	256
#define NAMEMAX	32			/* package names, Depends: tokens */

static char tmp[64];			/* unpack directory */
static char tmplist[72];		/* its file list, next to it */
static int to_flash;			/* install --flash */
static int ctl_flash;			/* this package says Flash: yes */
static char ctl_abi[8];			/* Abi: N or any; none = 1 (J.3) */
static char ctl_libc[12];		/* Libc: shared libc id (R2b.9) */
static char ctl_furi[12];		/* Furi: shared libfuri id (R2b.9) */
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
	if ((pid = vfork()) < 0) die("cannot fork:", strerror(errno));
	if (pid == 0) {			/* vfork: only chdir, open, dup, exec */
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
	if (tmplist[0] != 0) { unlink(tmplist); tmplist[0] = 0; }
}

/* snprintf into buf, which has size bytes; a path that does not fit
 * stops pkg (R3.6: nothing unbounded on package data). */
static void fmt(char *buf, size_t size, const char *f, ...)
{
	va_list ap;
	int r;

	va_start(ap, f);
	r = vsnprintf(buf, size, f, ap);
	va_end(ap);
	if (r < 0) die("name too long:", buf);
}

/* bufs are PATH_MAX bytes */
static void dbpath(char *buf, const char *name, const char *ext)
{
	fmt(buf, PATH_MAX, "%s/%s.%s", DB, name, ext);
}

static int name_char(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		|| (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'
		|| c == '+';
}

/* A package name or a Depends: token (R3.6): 1-32 characters of
 * [A-Za-z0-9._+-], not starting with '.', '-' or '+' ("..", options). */
static int good_token(const char *s, size_t len)
{
	size_t i;

	if (len == 0 || len > NAMEMAX || !name_char(s[0]) || s[0] == '.'
	    || s[0] == '-' || s[0] == '+') return 0;
	for (i = 0; i < len; i++) if (!name_char(s[i])) return 0;
	return 1;
}

static int good_pkg(const char *s)
{
	return good_token(s, strlen(s));
}

/* Does path contain a ".." component? */
static int dotdot(const char *p)
{
	while (*p != 0) {
		if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0))
			return 1;
		while (*p != 0 && *p != '/') p++;
		while (*p == '/') p++;
	}
	return 0;
}

/* The directories pkg writes into (R3.5): made if missing, then used only
 * if they are root's directories with mode 700 in a directory only root
 * can write.  Not so: somebody else made them, stop. */
static void safe_dir(const char *path)
{
	struct stat st;

	if (stat(PKGDIR, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0
	    || (st.st_mode & 022) != 0)
		die(PKGDIR " is not root's, or others may write it", NULL);
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		die("cannot make", path);
	if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0
	    || (st.st_mode & 07777) != 0700)
		die(path, "is not root's directory with mode 700: not used");
}

/* A scratch directory with a name from /dev/random (R3.5). */
static void make_scratch(void)
{
	unsigned char r[4];
	int fd, n, i;

	safe_dir(SCRATCH);
	for (i = 0; i < 8; i++) {
		if ((fd = open("/dev/random", O_RDONLY)) < 0)
			die("cannot read /dev/random", NULL);
		n = read(fd, (char *) r, sizeof r);
		close(fd);
		if (n != (int) sizeof r) continue;
		fmt(tmp, sizeof tmp, "%s/%02x%02x%02x%02x", SCRATCH, r[0], r[1],
								r[2], r[3]);
		if (mkdir(tmp, 0700) == 0) {
			fmt(tmplist, sizeof tmplist, "%s.list", tmp);
			return;
		}
	}
	tmp[0] = 0;
	die("cannot make a scratch directory in", SCRATCH);
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

/* The build id of the shared libc in flash (R2b.9), read in place; 0:
 * none there. */
static unsigned long libc_id(void)
{
	return SHLIBC_HDR->sh_magic == SHLIBC_MAGIC ? SHLIBC_HDR->sh_id : 0;
}

/* The same for the shared libfuri + libapp after it. */
static unsigned long furi_id(void)
{
	return SHFURI_HDR->sh_magic == SHFURI_MAGIC ? SHFURI_HDR->sh_id : 0;
}

/* Copy a control field into a buffer of size bytes; 0: it does not fit. */
static int field(char *to, size_t size, const char *v)
{
	if (strlen(v) >= size) return 0;
	strcpy(to, v);
	return 1;
}

/* Read the fields we need from a control file and check them (R3.6);
 * -1 with a message if the file is bad. */
static int read_control(const char *path)
{
	FILE *f;
	char line[LINE], *v, *p, *dep;
	char *why = NULL;
	size_t n;

	ctl_name[0] = ctl_version[0] = ctl_depends[0] = 0;
	ctl_flash = 0;
	ctl_abi[0] = ctl_libc[0] = ctl_furi[0] = 0;
	if ((f = fopen(path, "r")) == NULL) return -1;
	while (why == NULL && fgets(line, sizeof line, f) != NULL) {
		if ((n = strlen(line)) > 0 && line[n - 1] == '\n')
			line[n - 1] = 0;
		else if (!feof(f)) why = "a line is too long";
		if ((v = strchr(line, ':')) == NULL) continue;
		*v++ = 0;
		while (*v == ' ') v++;
		if (strcmp(line, "Package") == 0) {
			if (!field(ctl_name, sizeof ctl_name, v)) why = "Package: too long";
		} else if (strcmp(line, "Version") == 0) {
			if (!field(ctl_version, sizeof ctl_version, v))
				why = "Version: too long";
		} else if (strcmp(line, "Depends") == 0) {
			if (!field(ctl_depends, sizeof ctl_depends, v))
				why = "Depends: too long";
		} else if (strcmp(line, "Abi") == 0) {
			if (!field(ctl_abi, sizeof ctl_abi, v)) why = "Abi: too long";
		} else if (strcmp(line, "Libc") == 0) {
			if (!field(ctl_libc, sizeof ctl_libc, v)) why = "Libc: too long";
		} else if (strcmp(line, "Furi") == 0) {
			if (!field(ctl_furi, sizeof ctl_furi, v)) why = "Furi: too long";
		} else if (strcmp(line, "Flash") == 0 && strcmp(v, "yes") == 0) {
			ctl_flash = 1;	/* spec R2b.5 */
		} else if (strcmp(line, "Timers") == 0
			   || strcmp(line, "DeskMode") == 0) {
			if (strcmp(v, "yes") != 0 && strcmp(v, "no") != 0)
				why = "Timers:/DeskMode: is not yes or no";
		} else if (strcmp(line, "Type") == 0) {
			if (strcmp(v, "app") != 0 && strcmp(v, "window") != 0)
				why = "Type: is not app or window";
		} else if (strcmp(line, "Notify") == 0) {
			if (strcmp(v, "alarm") != 0 && strcmp(v, "reminder") != 0
			    && strcmp(v, "info") != 0)
				why = "Notify: is not alarm, reminder or info";
		}
	}
	fclose(f);
	if (why == NULL && !good_pkg(ctl_name)) why = "Package: is not a name";
	for (p = ctl_version; why == NULL && *p != 0; p++)
		if (!name_char(*p)) why = "Version: has a bad character";
	for (p = ctl_depends; why == NULL && *p != 0; ) {
		while (*p == ' ' || *p == ',') p++;
		for (dep = p; *p != 0 && *p != ' ' && *p != ','; p++) ;
		if (p != dep && !good_token(dep, p - dep))
			why = "Depends: has a bad name";
	}
	if (why != NULL) {
		fprintf(stderr, "pkg: %s: %s\n", path, why);
		ctl_name[0] = 0;
		return -1;
	}
	return 0;
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

/* An octal header field; -1 if it is not one. */
static long octal(const char *p, int n)
{
	long v = 0;
	int digits = 0;

	while (n > 0 && *p == ' ') { p++; n--; }
	for (; n > 0 && *p >= '0' && *p <= '7'; p++, n--, digits++) {
		if (v > 0x7FFFFFFL) return -1;
		v = v * 8 + (*p - '0');
	}
	if (n > 0 && *p != ' ' && *p != 0) return -1;
	return digits > 0 ? v : -1;
}

/* Read the archive's headers as tar(1) does and write its files (minus
 * CONTROL/ and directories) to listpath as absolute paths: no unpacking,
 * no directory walking (memory is tight).  Refused before anything is
 * unpacked (R3.6): absolute names, ".." components, names that fill tar's
 * 100-byte field or use the ustar prefix (MINIX tar would unpack them
 * somewhere else), names too long for the scratch directory, and members
 * other than files and directories (links could point anywhere). */
static int scan_archive(const char *archive, const char *listpath)
{
	union hblock h;
	FILE *out;
	char *why = NULL, *name = h.dbuf.name;
	long size, sum;
	size_t n;
	int fd, i, t;

	if ((fd = open(archive, O_RDONLY)) < 0) return -1;
	if ((out = fopen(listpath, "w")) == NULL) { close(fd); return -1; }
	for (;;) {
		if (read(fd, h.dummy, TBLOCK) != TBLOCK) {
			why = "the archive ends early";
			break;
		}
		if (name[0] == 0) break;		/* the end, as tar sees it */
		for (sum = 0, i = 0; i < TBLOCK; i++)
			sum += (i >= 148 && i < 156) ? ' '
						: (unsigned char) h.dummy[i];
		t = h.dbuf.typeflag;
		size = octal(h.dbuf.size, TSIZLEN);
		if (octal(h.dbuf.chksum, TCKSLEN) != sum)
			why = "a header checksum is wrong";
		else if (memchr(name, 0, NAMSIZ) == NULL || h.dbuf.prefix[0] != 0)
			why = "a member's name is too long for tar";
		else if (name[0] == '/')
			why = "a member has an absolute path";
		else if (dotdot(name))
			why = "a member's path has a .. component";
		else if (t != REGTYPE && t != AREGTYPE && t != ' ' && t != DIRTYPE)
			why = "a member is a link, device or long name";
		else if (strlen(tmp) + 1 + strlen(name) >= PATH_MAX)
			why = "a member's path is too long";
		else if (size < 0)
			why = "a member's size is wrong";
		if (why != NULL) break;
		n = strlen(name);
		if (t != DIRTYPE && name[n - 1] != '/'
		    && strncmp(name, "CONTROL/", 8) != 0)
			fprintf(out, "/%s\n", name);
		if (size > 0 && lseek(fd, (size + TBLOCK - 1) / TBLOCK * TBLOCK,
							SEEK_CUR) < 0) {
			why = "the archive ends early";
			break;
		}
	}
	close(fd);
	if (fclose(out) != 0 && why == NULL) why = "cannot write the file list";
	if (why != NULL) {
		h.dbuf.name[NAMSIZ - 1] = 0;
		fprintf(stderr, "pkg: %s: %s%s%s\n", archive, why,
			name[0] != 0 ? ": " : "", name);
		return -2;
	}
	return 0;
}

/* One line of a file list into buf (PATH_MAX bytes), without the newline;
 * 0 at the end.  A line too long or not an absolute path without ".." is
 * skipped with a message (R3.6: pkg acts only on whole, plain paths). */
static int list_line(FILE *f, char *buf)
{
	size_t n;
	int c;

	while (fgets(buf, PATH_MAX, f) != NULL) {
		if ((n = strlen(buf)) == 0 || buf[n - 1] != '\n') {
			while ((c = getc(f)) != EOF && c != '\n') ;
			fprintf(stderr, "pkg: skipped a path that is too long\n");
			continue;
		}
		buf[n - 1] = 0;
		if (buf[0] != '/' || dotdot(buf)) {
			fprintf(stderr, "pkg: skipped %s\n", buf);
			continue;
		}
		return 1;
	}
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
 * and reads it back (spec Phase 2b, R2b.5).  An install that writes
 * answers 1: then op 4 does one flash step per call (a page erase or 256
 * bytes), so that MM serves exec and the rest in between (AC2b.R2).
 * Returns 0 or -errno. */
static int flash_op(const char *path, int op)
{
	message m;
	int r;

	m.m1_p1 = (char *) path;
	m.m1_i1 = strlen(path) + 1;
	m.m1_i2 = MI_INSTALL;
	m.m1_i3 = op;
	while ((r = _syscall(MM, MEMINFO, &m)) == 1) {
		m.m1_i2 = MI_INSTALL;
		m.m1_i3 = 4;
	}
	return r < 0 ? -errno : 0;
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
		if (e == -EAGAIN) {
			fprintf(stderr, "pkg: %s: the clock is not set (RTC): "
				"it runs from RAM\n", v[i]);
			r = 1;
			continue;
		}
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
	int i, e, done = 0;

	/* Every file, also when no built-in record is unbound: a file that
	 * was copied back (new inode) is rebound to its text by SHA-256
	 * (R2b.12).  A file whose binding matches costs one call, no hash. */
	(void) m;
	for (i = 0; i < n; i++)
		if ((e = flash_op(v[i], 2)) == 0) done++;
		else if (e == -EAGAIN) {	/* say it once, not per file */
			printf("pkg: the clock is not set (RTC): built-in "
				"programs run from RAM until it is\n");
			return 0;
		}
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
	int r = 0, e, had;

	if ((list = fopen(listpath, "r")) == NULL) return -1;
	while (list_line(list, line)) {
		fmt(src, sizeof src, "%s%s", tmp, line);
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
	if (file[0] == '/') fmt(abs, sizeof abs, "%s", file);
	else if (getcwd(rel, sizeof rel) != NULL)
		fmt(abs, sizeof abs, "%s/%s", rel, file);
	else die("cannot get the current directory", NULL);

	/* Check the members, then unpack into a scratch directory to read
	 * CONTROL; the file list is made from the headers (R3.6). */
	make_scratch();
	switch (scan_archive(abs, tmplist)) {
	case 0:		break;
	case -1:	die("cannot read", file);
	default:	cleanup();
			return 1;
	}
	argv[0] = "/bin/tar"; argv[1] = "xf"; argv[2] = abs; argv[3] = NULL;
	if (run(tmp, argv) != 0) die("cannot unpack", file);
	fmt(path, sizeof path, "%s/CONTROL/control", tmp);
	if (read_control(path) < 0) {
		fprintf(stderr, "pkg: no valid CONTROL/control in %s\n", file);
		cleanup();
		return 1;
	}
	if (strcmp(ctl_abi, "any") != 0
	    && (ctl_abi[0] ? atoi(ctl_abi) : 1) != GZ_ABI) {
		fprintf(stderr, "pkg: %s needs ABI %s, this system is ABI %d\n",
			ctl_name, ctl_abi[0] ? ctl_abi : "1", GZ_ABI);
		cleanup();
		return 1;
	}
	/* Programs linked with another build of the shared libc in flash
	 * would not run (exec: ENOEXEC), not even sh: refuse them all. */
	if (ctl_libc[0] != 0 && strtoul(ctl_libc, NULL, 16) != libc_id()) {
		fprintf(stderr, "pkg: %s needs shared libc %s, this system has "
			"%08lx: flash minix-base.bin of the same build\n",
			ctl_name, ctl_libc, libc_id());
		cleanup();
		return 1;
	}
	if (ctl_furi[0] != 0 && strtoul(ctl_furi, NULL, 16) != furi_id()) {
		fprintf(stderr, "pkg: %s needs shared libfuri %s, this system has "
			"%08lx: flash minix-base.bin of the same build\n",
			ctl_name, ctl_furi, furi_id());
		cleanup();
		return 1;
	}

	for (p = ctl_depends; *p != 0; ) {	/* dependencies installed? */
		while (*p == ' ' || *p == ',') p++;
		for (dep = p; *p != 0 && *p != ' ' && *p != ','; p++) ;
		if (p == dep) break;
		memcpy(rel, dep, p - dep);	/* <= NAMEMAX: read_control */
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
	if (copy(tmplist, buf) < 0) die("cannot write", buf);

	/* Move the files from the scratch directory into place ourselves: a
	 * second tar would need another fork + exec, which may not fit. */
	dbpath(buf, ctl_name, "list");
	if (place_files(buf) < 0) die("installing the files failed", NULL);

	dbpath(buf, ctl_name, "control");
	copy(path, buf);
	dbpath(buf, ctl_name, "postinst"); unlink(buf);
	dbpath(buf, ctl_name, "prerm"); unlink(buf);
	fmt(path, sizeof path, "%s/CONTROL/postinst", tmp);
	dbpath(buf, ctl_name, "postinst");
	if (exists(path)) copy(path, buf);
	fmt(path, sizeof path, "%s/CONTROL/prerm", tmp);
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
			fmt(line, sizeof line, "%s/%s", DB, e->d_name);
			if (read_control(line) < 0) continue;
			if (strcmp(ctl_name, name) != 0 && in_list(ctl_depends, name)) {
				fmt(other, sizeof other, "%s", ctl_name);
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
	/* Only the paths the list recorded at install time, whole (R3.6). */
	if ((list = fopen(buf, "r")) != NULL) {
		while (list_line(list, line)) {
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
		fmt(path, sizeof path, "%s/%s", DB, e->d_name);
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

#define INCOMING PKGDIR "/in"		/* pkg get: root, 700 (R3.5) */
#define MAXGET	16			/* packages per pkg get */
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

static int good_name(const char *s)	/* a file name from flsend */
{
	int n = strlen(s);

	if (n < 5 || n > 13 || strcmp(s + n - 4, ".tar") != 0) return 0;
	return good_token(s, n - 4);
}

/* Receive files sent by flipper/tools/flsend over the console:
 *	host: "FILE name size crc32\n" + size raw bytes
 *	pkg:  "OK\n" (stored) or "BAD ...\n" (host may resend)
 *	host: "DONE\n"
 * The console is in raw mode meanwhile; the USB and TTY flow control make
 * sure no byte is lost, the CRC checks that none was changed.
 */
static int receive(char got[MAXGET][16])
{
	struct termios old, raw;
	char line[96], name[16], path[PATH_MAX];
	unsigned char buf[512];
	unsigned long size, crc, want, left;
	int fd, n, i, files = 0;

	if (tcgetattr(0, &old) < 0) die("stdin is not a terminal", NULL);
	raw = old;
	raw.c_iflag = 0;
	raw.c_oflag = 0;
	raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);
	say("GPKG READY\n");

	while (get_line(line, sizeof line) >= 0) {
		if (strcmp(line, "DONE") == 0) break;
		if (sscanf(line, "FILE %15s %lu %lx", name, &size, &want) != 3
		    || !good_name(name) || size > MAXPKG) {
			say("BAD header\n");
			continue;
		}
		for (i = 0; i < files && strcmp(got[i], name) != 0; i++) ;
		if (i == MAXGET) {
			say("BAD too many packages at once\n");
			continue;
		}
		fmt(path, sizeof path, "%s/%s", INCOMING, name);
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
		if (i == files) strcpy(got[files++], name);	/* a resend */
	}
	tcsetattr(0, TCSANOW, &old);
	return files;
}

/* Receive, then install only what this run received (R3.5): the incoming
 * directory is root's (safe_dir) and emptied first. */
static int get(void)
{
	char path[PATH_MAX], got[MAXGET][16];
	int i, n, r = 0;

	safe_dir(INCOMING);
	rm_r(INCOMING);
	safe_dir(INCOMING);
	if ((n = receive(got)) == 0) {
		fprintf(stderr, "pkg: no packages received\n");
		return 1;
	}
	for (i = 0; i < n; i++) {
		fmt(path, sizeof path, "%s/%s", INCOMING, got[i]);
		if (install(path) != 0) r = 1;
		unlink(path);
	}
	return r;
}

static int budget(int argc, char **argv)	/* R2b.11 */
{
	message m;
	struct fccount fc;

	m.m1_p1 = (char *) &fc;
	m.m1_i1 = argc == 3 ? atoi(argv[2]) : -1;
	m.m1_i2 = MI_FCBUDGET;
	if (argc == 3 && (argv[2][0] < '0' || argv[2][0] > '9'))
		die("budget: a number of pages, 0-255", NULL);
	if (_syscall(MM, MEMINFO, &m) < 0) {
		fprintf(stderr, "pkg: budget: %s\n", strerror(errno));
		return 1;
	}
	printf("run-time cache: %lu of %lu pages erased today, %lu in all; "
		"installed programs: %lu\n", fc.today, fc.budget, fc.erased,
		fc.inst_erased);
	return 0;
}

int main(int argc, char **argv)
{
	struct stat st;

	if (argc >= 2 && strcmp(argv[1], "bind") == 0)	/* no /usr needed */
		return bind(argv + 2, argc - 2);
	if ((argc == 2 || argc == 3) && strcmp(argv[1], "budget") == 0)
		return budget(argc, argv);
	if (stat(DB, &st) < 0 || !S_ISDIR(st.st_mode))
		die(DB " is missing (is /usr mounted?)", NULL);
	if (argc == 3 && (strcmp(argv[1], "remove") == 0
		|| strcmp(argv[1], "info") == 0 || strcmp(argv[1], "files") == 0)
	    && !good_pkg(argv[2]))
		die("not a package name:", argv[2]);
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
		"unflash file... | budget [pages]\n");
	return 1;
}
