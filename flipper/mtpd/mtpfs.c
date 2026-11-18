/* mtpfs.c - mtpd's files: handles = inode numbers (spec R10.4, T29a).
 *
 * An MTP object handle is the file's inode number, so it stays valid
 * while the file is renamed on the Flipper.  No FS change: a parent ring
 * (inode -> parent inode, 256 entries x 8 B) is filled while folders are
 * listed, and a path is rebuilt by walking the ring up to the storage
 * root and finding each name with one scan of its parent folder.  The
 * last folder used is remembered with its path, so the common case (the
 * host asks about each object of the folder it just listed) is one scan.
 * A ring miss looks in that folder once more, then answers "no such
 * object" (Invalid_ObjectHandle; the host lists the folder again).
 *
 * Paths are only ever made from names found in a folder scan or checked
 * by mtpcore.c (no '/', no "..").  Symbolic links, devices and FIFOs are
 * not shown, nor anything on another device (a mount point below the
 * storage root): handles are unique per device only.
 *
 * RAM: the ring 2 KB, two path buffers of 256 B, one DIR (1 KB; on
 * MINIX in .bss, not malloc'd: one folder is scanned at a time).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>		/* rename() */
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef _MINIX
#include <sys/statvfs.h>
#include <sys/utsname.h>
#else
#include <sys/utsname.h>
#endif
#include "mtp.h"

#define RING		256		/* spec: <= 256 entries x 8 B */
#define DEPTH		32		/* folders below the root, at most */

#ifdef _MINIX
#define LSTAT		stat		/* no symbolic links in MINIX 2 */
#define O_NOFOLLOW	0
/* opendir() mallocs its 1 KB DIR, and a heap costs MM's 1.5 KB gap on
 * top: mtpd keeps one DIR in .bss instead (one folder is read at a
 * time), set up as libc's opendir() does, read by libc's readdir().
 * closedir() would free() it: the descriptor is closed here. */
static DIR dirbuf;
static int dirbusy;

static DIR *d_open(const char *name)
{
	struct stat st;
	int fd;

	if (dirbusy) { errno = EBUSY; return NULL; }
	if ((fd = open(name, O_RDONLY | O_NONBLOCK)) < 0) return NULL;
	if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode)
	    || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		close(fd);
		errno = ENOTDIR;
		return NULL;
	}
	dirbuf._fd = fd;
	dirbuf._v7 = -1;
	dirbuf._count = 0;
	dirbuf._pos = 0;
	dirbusy = 1;
	return &dirbuf;
}

static void d_close(DIR *d)
{
	close(d->_fd);
	dirbusy = 0;
}
#define opendir		d_open
#define closedir	d_close
#else
#define LSTAT		lstat
#endif

static struct { mu32 ino, par; } ring[RING];
static unsigned rhead;

static const char *root;	/* the storage root's path (argv) */
static mu32 root_ino;
static dev_t root_dev;

static mu32 cdir;		/* the folder last used, 0 none */
static char cpath[MTP_PATH_MAX + 1];	/*   its path */
static char path[MTP_PATH_MAX + 1];	/* the path rebuilt last */
static mu32 nnew;		/* the file being received, 0 none */
static int fd = -1;		/* the file being read or written */
static DIR *ld;			/* the folder being listed */
static mu32 ldir;		/*   its inode */
static char label[32];

/*===========================================================================*
 *				the ring				     *
 *===========================================================================*/
static int ring_find(mu32 ino)
{
	int i;

	for (i = 0; i < RING; i++)
		if (ring[i].ino == ino) return i;
	return -1;
}

static mu32 ring_get(mu32 ino)
{
	int i = ring_find(ino);

	return i < 0 ? 0 : ring[i].par;
}

/* Remember ino's parent.  An entry found in the older half moves to the
 * head, so what the host uses stays (LRU, roughly). */
static void ring_put(mu32 ino, mu32 par)
{
	int i = ring_find(ino);

	if (ino == 0) return;
	if (i >= 0) {
		ring[i].par = par;
		if (((rhead - i - 1) & (RING - 1)) < RING / 2) return;
		ring[i].ino = 0;
	}
	ring[rhead].ino = ino;
	ring[rhead].par = par;
	rhead = (rhead + 1) & (RING - 1);
}

static void ring_del(mu32 ino)
{
	int i = ring_find(ino);

	if (i >= 0) ring[i].ino = 0;
}

/*===========================================================================*
 *				paths					     *
 *===========================================================================*/
/* dir + "/" + name into out (size MTP_PATH_MAX + 1): 0, or -1 too long. */
static int join(char *out, const char *dir, const char *name)
{
	size_t a = strlen(dir), b = strlen(name);

	if (a + 1 + b > MTP_PATH_MAX) return -1;
	if (out != dir) memmove(out, dir, a);
	out[a] = '/';
	memcpy(out + a + 1, name, b + 1);
	return 0;
}

/* The name of ino in folder dir (one scan) into name; 0 or -1. */
static int find_name(const char *dir, mu32 ino, char *name)
{
	DIR *d;
	struct dirent *e;
	int r = -1;

	if ((d = opendir(dir)) == NULL) return -1;
	while ((e = readdir(d)) != NULL) {
		if ((mu32) e->d_ino != ino) continue;
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (strlen(e->d_name) > MTP_NAME_MAX) continue;
		strcpy(name, e->d_name);
		r = 0;
		break;
	}
	closedir(d);
	return r;
}

/* The path of object h into path[]: FS_OK or FS_NOOBJ.  If st is not
 * NULL, the object's lstat (checked: same inode, same device, a file or
 * a folder). */
static int resolve(mu32 h, struct stat *st)
{
	mu32 chain[DEPTH], x, p;
	char name[MTP_NAME_MAX + 1];
	struct stat sb;
	int n = 0, i;
	size_t plen = 0;

	if (h == 0) return FS_NOOBJ;
	for (x = h; x != root_ino && !(x != h && x == cdir); x = p) {
		if (n == DEPTH) return FS_NOOBJ;
		chain[n++] = x;
		if ((p = ring_get(x)) == 0) {
			/* A ring miss: is it in the folder last used? */
			if (cdir == 0 || find_name(cpath, x, name) < 0)
				return FS_NOOBJ;
			ring_put(x, cdir);
			p = cdir;
		}
		if (p == h) return FS_NOOBJ;		/* a loop */
	}
	strcpy(path, x == root_ino ? root : cpath);
	for (i = n - 1; i >= 0; i--) {
		if (i == 0) plen = strlen(path);
		if (find_name(path, chain[i], name) < 0
		    || join(path, path, name) < 0)
			return FS_NOOBJ;
	}
	if (n >= 2) {			/* remember h's folder */
		memcpy(cpath, path, plen);
		cpath[plen] = 0;
		cdir = chain[1];
	}
	if (st == NULL) st = &sb;
	if (LSTAT(path, st) < 0 || (mu32) st->st_ino != h
	    || st->st_dev != root_dev
	    || !(S_ISREG(st->st_mode) || S_ISDIR(st->st_mode))) {
		cdir = 0;	/* changed on the Flipper: forget the cache */
		return FS_NOOBJ;
	}
	return FS_OK;
}

/* A folder's path into cpath (cdir = dir): FS_OK or FS_NOOBJ. */
static int resolve_dir(mu32 dir)
{
	struct stat st;
	int e;

	if (dir == root_ino) {
		if (strlen(root) > MTP_PATH_MAX) return FS_NOOBJ;
		strcpy(cpath, root);
		cdir = root_ino;
		return FS_OK;
	}
	if (cdir != 0 && dir == cdir) return FS_OK;
	if ((e = resolve(dir, &st)) != FS_OK) return e;
	if (!S_ISDIR(st.st_mode)) return FS_NOOBJ;
	strcpy(cpath, path);
	cdir = dir;
	return FS_OK;
}

static int errno_fs(void)
{
	switch (errno) {
	case EACCES: case EPERM: case EROFS:	return FS_DENIED;
	case ENOSPC: case EFBIG:		return FS_FULL;
	case EEXIST:				return FS_EXIST;
	case ENOENT: case ENOTDIR:		return FS_NOOBJ;
	default:				return FS_IO;
	}
}

static void fill(struct mtp_obj *o, mu32 h, mu32 par, const char *name,
		 const struct stat *st)
{
	o->handle = h;
	o->parent = par == root_ino ? 0 : par;
	o->dir = S_ISDIR(st->st_mode);
	o->size = (unsigned long) st->st_size > 0xFFFFFFFFUL ?
		0xFFFFFFFFUL : (mu32) st->st_size;
	o->mtime = st->st_mtime;
	strcpy(o->name, name);
}

/*===========================================================================*
 *				the interface				     *
 *===========================================================================*/
int fs_init(const char *r)
{
	struct stat st;
	struct utsname u;

	memset(ring, 0, sizeof(ring));
	rhead = 0;
	cdir = 0;
	if (fd >= 0) { close(fd); fd = -1; }
	nnew = 0;
	if (strlen(r) > MTP_PATH_MAX - 2 || stat(r, &st) < 0 || !S_ISDIR(st.st_mode))
		return -1;
	root = r;
	root_ino = st.st_ino;
	root_dev = st.st_dev;
	strcpy(label, "Flipper");
	if (uname(&u) == 0 && u.nodename[0] != 0) {
		strncpy(label, u.nodename, sizeof(label) - 1);
		label[sizeof(label) - 1] = 0;
	}
	return 0;
}

mu32 fs_root(void)
{
	return root_ino;
}

int fs_stat(mu32 h, struct mtp_obj *o)
{
	struct stat st;
	const char *s;
	int e;

	if ((e = resolve(h, &st)) != FS_OK) return e;
	s = strrchr(path, '/');
	fill(o, h, ring_get(h), s + 1, &st);
	return FS_OK;
}

int fs_list_open(mu32 dir)
{
	int e;

	fs_list_close();
	if ((e = resolve_dir(dir)) != FS_OK) return e;
	if ((ld = opendir(cpath)) == NULL) return errno_fs();
	ldir = dir;
	return FS_OK;
}

int fs_list_next(struct mtp_obj *o)
{
	struct dirent *e;
	struct stat st;

	if (ld == NULL) return 0;
	while ((e = readdir(ld)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (strlen(e->d_name) > MTP_NAME_MAX) continue;
		if (join(path, cpath, e->d_name) < 0) continue;
		if (LSTAT(path, &st) < 0 || st.st_dev != root_dev) continue;
		if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) continue;
		if ((mu32) st.st_ino == 0) continue;
		ring_put(st.st_ino, ldir);
		fill(o, st.st_ino, ldir, e->d_name, &st);
		return 1;
	}
	return 0;
}

void fs_list_close(void)
{
	if (ld != NULL) closedir(ld);
	ld = NULL;
}

int fs_open_read(mu32 h, mu32 off)
{
	struct stat st;
	int e;

	if ((e = resolve(h, &st)) != FS_OK) return e;
	if (!S_ISREG(st.st_mode)) return FS_NOOBJ;
	fs_close();
	if ((fd = open(path, O_RDONLY | O_NOFOLLOW)) < 0) return errno_fs();
	if (fstat(fd, &st) < 0 || (mu32) st.st_ino != h || !S_ISREG(st.st_mode)) {
		fs_close();
		return FS_NOOBJ;
	}
	if (off != 0 && lseek(fd, (off_t) off, SEEK_SET) < 0) {
		fs_close();
		return FS_IO;
	}
	return FS_OK;
}

int fs_read(void *buf, unsigned n)
{
	unsigned k = 0;
	int r;

	while (k < n) {
		r = read(fd, (char *) buf + k, n - k);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		k += r;
	}
	return k;
}

int fs_create(mu32 dir, const char *name, int folder, mu32 *h)
{
	struct stat st;
	int e;

	fs_close();
	if ((e = resolve_dir(dir)) != FS_OK) return e;
	if (join(path, cpath, name) < 0) return FS_DENIED;
	if (folder) {
		if (mkdir(path, 0777) < 0) return errno_fs();
		if (LSTAT(path, &st) < 0) return FS_IO;
	} else {
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
		if (fd < 0) return errno_fs();
		if (fstat(fd, &st) < 0) {
			close(fd);
			fd = -1;
			unlink(path);
			return FS_IO;
		}
		nnew = st.st_ino;
	}
	*h = st.st_ino;
	ring_put(*h, dir);
	return FS_OK;
}

int fs_write(const void *buf, unsigned n)
{
	unsigned k = 0;
	int r;

	if (fd < 0) return -1;
	while (k < n) {
		r = write(fd, (const char *) buf + k, n - k);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		k += r;
	}
	return 0;
}

void fs_close(void)
{
	if (fd >= 0) close(fd);
	fd = -1;
	nnew = 0;
}

/* The new file is found again by its handle (the ring has it), checked
 * to be the same inode, and removed. */
void fs_abandon(void)
{
	struct stat st;
	mu32 h = nnew;

	fs_close();
	if (h != 0 && resolve(h, &st) == FS_OK && S_ISREG(st.st_mode))
		unlink(path);
}

/* Remove the folder at path[] with everything in it.  One DIR open at a
 * time: files are removed during the scan; on a subfolder the scan stops
 * and goes down into it, and starts again when it comes back up. */
static int rm_tree(void)
{
	char sub[MTP_PATH_MAX + 1];
	size_t base = strlen(path), n;
	struct dirent *e;
	struct stat st;
	DIR *d;
	int down, err;

	for (;;) {
		if ((d = opendir(path)) == NULL) return FS_PARTIAL;
		down = err = 0;
		while ((e = readdir(d)) != NULL) {
			if (strcmp(e->d_name, ".") == 0
			    || strcmp(e->d_name, "..") == 0)
				continue;
			if (join(sub, path, e->d_name) < 0
			    || LSTAT(sub, &st) < 0 || st.st_dev != root_dev) {
				err = 1;
				continue;
			}
			if (S_ISDIR(st.st_mode)) { down = 1; break; }
			if (unlink(sub) < 0) err = 1;
			else ring_del(st.st_ino);
		}
		closedir(d);
		if (down) {
			strcpy(path, sub);
			continue;
		}
		if (err) return FS_PARTIAL;
		if (LSTAT(path, &st) == 0) ring_del(st.st_ino);
		if (rmdir(path) < 0) return FS_PARTIAL;
		if ((n = strlen(path)) <= base) return FS_OK;
		while (n > base && path[n - 1] != '/') n--;
		path[n - 1] = 0;		/* up one */
	}
}

int fs_delete(mu32 h)
{
	struct stat st;
	int e;

	if ((e = resolve(h, &st)) != FS_OK) return e;
	if (S_ISDIR(st.st_mode)) {
		cdir = 0;		/* its path may be cached */
		e = rm_tree();
	} else {
		e = unlink(path) < 0 ? errno_fs() : FS_OK;
	}
	if (e == FS_OK) ring_del(h);
	return e;
}

int fs_rename(mu32 h, const char *name)
{
	char to[MTP_PATH_MAX + 1];
	struct stat st;
	char *s;
	int e;

	if ((e = resolve(h, &st)) != FS_OK) return e;
	s = strrchr(path, '/');
	*s = 0;
	if (join(to, path, name) < 0) return FS_DENIED;
	*s = '/';
	if (strcmp(to, path) == 0) return FS_OK;
	if (LSTAT(to, &st) == 0) return FS_EXIST;	/* never replace */
	if (rename(path, to) < 0) return errno_fs();
	cdir = 0;			/* a cached path may name it */
	return FS_OK;
}

int fs_move(mu32 h, mu32 dir)
{
	char from[MTP_PATH_MAX + 1];
	struct stat st;
	const char *s;
	mu32 x;
	int e, n = 0;

	/* Never into itself or below itself. */
	for (x = dir; x != 0 && x != root_ino && n < DEPTH; x = ring_get(x), n++)
		if (x == h) return FS_BADPARENT;
	if ((e = resolve(h, &st)) != FS_OK) return e;
	strcpy(from, path);
	if ((e = resolve_dir(dir)) != FS_OK) return FS_BADPARENT;
	s = strrchr(from, '/');
	if (join(path, cpath, s + 1) < 0) return FS_DENIED;
	if (strcmp(from, path) == 0) return FS_OK;
	if (LSTAT(path, &st) == 0) return FS_EXIST;
	if (rename(from, path) < 0) return errno == EINVAL ? FS_BADPARENT : errno_fs();
	cdir = 0;
	ring_put(h, dir);
	return FS_OK;
}

int fs_space(mu32 *ch, mu32 *cl, mu32 *fh, mu32 *fl)
{
#ifdef _MINIX
	/* O.5's free counters (FSTATFS) do not exist yet (T27): unknown. */
	(void) ch; (void) cl; (void) fh; (void) fl;
	return -1;
#else
	struct statvfs v;
	unsigned long long c, f;

	if (statvfs(root, &v) < 0) return -1;
	c = (unsigned long long) v.f_blocks * v.f_frsize;
	f = (unsigned long long) v.f_bavail * v.f_frsize;
	*ch = c >> 32; *cl = (mu32) c;
	*fh = f >> 32; *fl = (mu32) f;
	return 0;
#endif
}

const char *fs_label(void)
{
	return label;
}

int fs_locked(void)
{
	/* The desktop PIN lock (spec R10.4 "Lock") is not specified yet:
	 * never locked.  When it exists, this asks it. */
	return 0;
}

long fs_now(void)
{
	return (long) time(NULL);
}

static char *put2(char *p, int v)
{
	*p++ = '0' + v / 10 % 10;
	*p++ = '0' + v % 10;
	return p;
}

/* UTC with MTP's "Z" (the Flipper keeps UTC, spec 2d; localtime() would
 * need TZ, which devd does not pass, and 400 B of RAM). */
void fs_date(long t, char *buf)
{
	time_t tt = t;
	struct tm *tm = gmtime(&tt);
	char *p = buf;
	int y;

	if (tm == NULL) { strcpy(buf, "19700101T000000Z"); return; }
	y = tm->tm_year + 1900;
	p = put2(p, y / 100);
	p = put2(p, y % 100);
	p = put2(p, tm->tm_mon + 1);
	p = put2(p, tm->tm_mday);
	*p++ = 'T';
	p = put2(p, tm->tm_hour);
	p = put2(p, tm->tm_min);
	p = put2(p, tm->tm_sec);
	*p++ = 'Z';
	*p = 0;
}
