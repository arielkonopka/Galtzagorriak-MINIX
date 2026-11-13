/*	pkg - a very small package manager for Galtzagorriak	(Flipper port)
 *
 *	pkg install file.tar	install (or upgrade) a package
 *	pkg remove name		remove a package
 *	pkg list		list installed packages
 *	pkg info name		show a package's control file
 *	pkg files name		list the files a package installed
 *
 * A package is a tar archive (name_version.tar, made on the PC with
 * flipper/tools/mkpkg.py) holding its files relative to / and a CONTROL
 * directory:
 *	CONTROL/control		Package:, Version:, Depends:, Description:
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
#include <unistd.h>

#define DB	"/usr/var/pkg/info"
#define TMPDIR	"/usr/tmp"
#define LINE	256

static char tmp[64];			/* unpack directory */
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

/* Write the regular files under dir (minus CONTROL) to list, as paths
 * relative to /.  path holds the current directory relative to dir. */
static void walk(FILE *list, const char *dir, char *path, size_t len)
{
	char full[PATH_MAX];
	DIR *d;
	struct dirent *e;
	struct stat st;
	size_t n;

	sprintf(full, "%s%s", dir, path);
	if ((d = opendir(full)) == NULL) die("cannot read directory", full);
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (len == 0 && strcmp(e->d_name, "CONTROL") == 0) continue;
		n = strlen(e->d_name);
		if (len + 1 + n >= PATH_MAX - strlen(dir) - 1) continue;
		path[len] = '/';
		strcpy(path + len + 1, e->d_name);
		sprintf(full, "%s%s", dir, path);
		if (stat(full, &st) == 0) {
			if (S_ISDIR(st.st_mode))
				walk(list, dir, path, len + 1 + n);
			else
				fprintf(list, "%s\n", path);
		}
		path[len] = 0;
	}
	closedir(d);
}

static int install(const char *file)
{
	char abs[PATH_MAX], path[PATH_MAX], rel[PATH_MAX], buf[PATH_MAX];
	char *argv[4];
	FILE *list;
	char *dep, *p;
	int upgrade;

	if (!exists(file)) die("no such file:", file);
	if (file[0] == '/') strcpy(abs, file);
	else if (getcwd(abs, sizeof abs - strlen(file) - 2) != NULL)
		strcat(strcat(abs, "/"), file);
	else die("cannot get the current directory", NULL);

	/* Unpack into a scratch directory to read CONTROL and the file list. */
	sprintf(tmp, "%s/pkg.%d", TMPDIR, (int) getpid());
	if (mkdir(tmp, 0700) < 0) { tmp[0] = 0; die("cannot make", TMPDIR "/pkg.*"); }
	argv[0] = "/bin/tar"; argv[1] = "xf"; argv[2] = abs; argv[3] = NULL;
	if (run(tmp, argv) != 0) die("cannot unpack", file);
	sprintf(path, "%s/CONTROL/control", tmp);
	if (read_control(path) < 0) die("no valid CONTROL/control in", file);

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
	if ((list = fopen(buf, "w")) == NULL) die("cannot write", buf);
	rel[0] = 0;
	walk(list, tmp, rel, 0);
	fclose(list);

	argv[0] = "/bin/tar"; argv[1] = "xf"; argv[2] = abs; argv[3] = NULL;
	if (run("/", argv) != 0) die("unpacking into / failed", NULL);
	rm_r("/CONTROL");

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
			if (line[0] == '/') unlink(line);
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

int main(int argc, char **argv)
{
	struct stat st;

	if (stat(DB, &st) < 0 || !S_ISDIR(st.st_mode))
		die(DB " is missing (is /usr mounted?)", NULL);
	if (argc == 3 && strcmp(argv[1], "install") == 0) return install(argv[2]);
	if (argc == 3 && strcmp(argv[1], "remove") == 0) return remove_pkg(argv[2]);
	if (argc == 2 && strcmp(argv[1], "list") == 0) return list_pkgs();
	if (argc == 3 && strcmp(argv[1], "info") == 0) return cat(argv[2], "control");
	if (argc == 3 && strcmp(argv[1], "files") == 0) return cat(argv[2], "list");
	fprintf(stderr, "usage: pkg install file.tar | remove name | list | "
			"info name | files name\n");
	return 1;
}
