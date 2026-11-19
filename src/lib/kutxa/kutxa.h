/* kutxa.h - a small database: a copy-on-write B+tree appended to one file
 * (spec section R, task T34).  "Kutxa" is Basque for a chest.
 *
 * THIS IS THE CONTRACT ONLY.  The library (kutxa.c, krec.c) is written in
 * T34a; flipper/tests/kutxa/kutxatest.c tests it against this header and is
 * written first, from the use cases the user asked for (RR.12).
 *
 * One file holds up to KT_MAXOPEN open trees out of any number of named
 * trees (a catalog tree maps names to roots).  Pages are KT_PAGE bytes, the
 * FS block size, each with a CRC-32.  A change copies the path from the leaf
 * to the root into the caller's cache (spilled to the end of the file when
 * the cache is full, RR.2); kt_commit() appends the changed pages, syncs,
 * appends a commit page, syncs (RR.3).  Old pages are never overwritten, so a
 * power cut leaves the last commit (or, if the card damaged its pages, the
 * one before it: kt_stat() rollback = 1).  Readers opened with KT_RDONLY see
 * the commit that was newest when they opened or last called kt_refresh().
 *
 * Errors: -1 with errno set (ENOENT not found or end of a cursor, E2BIG the
 * value buffer is too small and *vlen holds the size needed, EINVAL bad
 * argument or keys not ascending in kt_load, EROFS write on a KT_RDONLY
 * handle, EBUSY another writer holds the file, EIO read error or a page whose
 * CRC fails, ENOSPC the file would pass KT_MAXFILE or the card is full,
 * EINVAL or ENOMEM also when `mem` is below KT_MEM(3)).
 *
 * No malloc: every byte the library uses is in the caller's `mem`.
 */
#ifndef KUTXA_H
#define KUTXA_H

#include <stddef.h>

#define KT_PAGE		1024		/* page = FS block */
#define KT_MAXKEY	255		/* bytes in a key */
#define KT_MAXVAL	65535		/* bytes in a value (> KT_INLINE: overflow pages) */
#define KT_INLINE	400		/* key + value up to this stay in the leaf */
#define KT_MAXDEPTH	6		/* tree height limit (also a loop guard) */
#define KT_MAXOPEN	8		/* trees open at once per handle */
#define KT_MAXNAME	31		/* tree name length */
#define KT_MAXFILE	0xFFFFFC00UL	/* file size limit (off_t is 32-bit) */

/* Bytes of `mem` for a handle with a cache of `pages` pages (minimum 3). */
#define KT_STATE	256
#define KT_MEM(pages)	(KT_STATE + (pages) * (KT_PAGE + 8))

/* kt_open flags */
#define KT_RDONLY	0x01	/* reader: no lock, sees a snapshot */
#define KT_CREAT	0x02	/* create the file (open) or the tree (kt_tree) */
#define KT_NOLOCK	0x04	/* writer without fcntl lock (tests, single user) */

typedef struct kt KT;

/* The file under the database.  kt_open() fills one in for a path (pread
 * and pwrite through lseek, sync = fsync(), O_NOCACHE for scans, RR.7);
 * tests pass their own to cut the power at any write (kutxatest.c). */
struct kt_io {
	void *h;
	long (*pread)(void *h, void *buf, unsigned long n, unsigned long off);
	long (*pwrite)(void *h, const void *buf, unsigned long n, unsigned long off);
	int (*sync)(void *h);
	long (*size)(void *h);			/* -1 on error */
	int (*truncate)(void *h, unsigned long size);
};

KT *kt_open(const char *path, void *mem, size_t memsize, int flags);
KT *kt_open_io(const struct kt_io *io, void *mem, size_t memsize, int flags);
int kt_close(KT *db);			/* drops what is not committed */

/* Trees.  Tree 0 is "main" and always exists.  kt_tree() returns a small
 * handle (1 .. KT_MAXOPEN-1) for a named tree, creating it with KT_CREAT
 * (the creation is part of the next commit). */
int kt_tree(KT *db, const char *name, int flags);
int kt_trees(KT *db, int (*fn)(void *arg, const char *name), void *arg);
int kt_droptree(KT *db, const char *name);

int kt_get(KT *db, int t, const void *key, size_t klen, void *val, size_t *vlen);
int kt_put(KT *db, int t, const void *key, size_t klen, const void *val, size_t vlen);
int kt_del(KT *db, int t, const void *key, size_t klen);
/* Delete every key in [lo, hi) (lo NULL: from the start, hi NULL: to the
 * end).  Whole subtrees are dropped without reading their leaves.  Returns the number deleted. */
long kt_delrange(KT *db, int t, const void *lo, size_t lolen,
		 const void *hi, size_t hilen);

int kt_commit(KT *db);			/* durable when it returns 0 */
void kt_abort(KT *db);			/* drops what is not committed */
int kt_refresh(KT *db);			/* reader: 1 if a newer commit was taken */

/* Cursors live in the caller's memory and stay valid until the next change
 * of the same handle (a reader's cursors until kt_refresh). */
struct kt_cur {
	KT *db;
	unsigned char tree, depth, flags, pad;
	unsigned short slot[KT_MAXDEPTH];
	unsigned long page[KT_MAXDEPTH];
	unsigned long txn;
};
/* First key >= key (klen 0: the first key). */
int kt_seek(KT *db, int t, struct kt_cur *c, const void *key, size_t klen);
int kt_seeklast(KT *db, int t, struct kt_cur *c);	/* the last key */
/* Copy out the current entry and step forward (kt_prev: backward).  key
 * holds KT_MAXKEY bytes; *vlen in: buffer size, out: value size. */
int kt_next(struct kt_cur *c, void *key, size_t *klen, void *val, size_t *vlen);
int kt_prev(struct kt_cur *c, void *key, size_t *klen, void *val, size_t *vlen);

/* Bulk load into an empty tree, keys strictly ascending: leaves are filled
 * to 100 % and written in order (the packed layout of read-only databases:
 * holidays, IR codes).  kt_load_end() commits. */
int kt_load_begin(KT *db, int t);
int kt_load(KT *db, const void *key, size_t klen, const void *val, size_t vlen);
int kt_load_end(KT *db);

/* Compaction (RR.6): copy the live trees of the last commit to `dst`, at
 * most `steps` pages per call; returns 1 while there is more, 0 when dst is
 * complete and synced.  kt_compact() does it to PATH.new and renames. */
int kt_compact_io(KT *db, const struct kt_io *dst, int steps);
int kt_compact(KT *db, int steps);

/* Secondary indexes (RR.5).  kt_index() creates tree `name` holding, for
 * every record of tree t whose value (a krec) has field `field`, the key
 * kt_ixkey(field value) + primary key with an empty value, and builds it
 * from the existing records, at most `steps` pages per call (1 while there
 * is more).  From then on kt_put/kt_del on t keep it up to date (one extra
 * read of the old record per change). */
int kt_index(KT *db, int t, const char *name, const char *field, int steps);
/* The encoded field value that starts an index key: use it with kt_seek and
 * compare prefixes.  kt_ixsplit() gives the primary key inside an index key. */
int kt_ixkey(void *buf, size_t cap, int type, const void *val, size_t vlen);
int kt_ixsplit(const void *ikey, size_t iklen, const void **pk, size_t *pklen);

struct kt_stat {
	unsigned long txn;		/* commit number */
	unsigned long filesize;
	unsigned long live;		/* bytes of pages the last commit uses */
	unsigned long reads;		/* pages read from the file (counter) */
	unsigned long hits;		/* pages found in the cache */
	unsigned long written;		/* bytes appended (counter) */
	unsigned char height[KT_MAXOPEN];
	unsigned char rollback;		/* 1: opened at the commit before the last */
};
int kt_stat(KT *db, struct kt_stat *st);
int kt_check(KT *db, int (*report)(void *arg, const char *msg), void *arg);

/* Keys that sort as numbers and times (big-endian, sign bit flipped). */
size_t kt_key_u64(void *buf, unsigned long long v);
size_t kt_key_i64(void *buf, long long v);
long long kt_key_geti64(const void *buf);

/* ---- Records: typed fields "name -> value" in a value (RR.4) ----
 * A record is a sequence of fields: name length (1 B), name, type (1 B),
 * value length (LEB128), value.  Fields may be added to any record at any
 * time; a record without the field simply does not have it. */
#define KR_STR	's'	/* UTF-8 text */
#define KR_INT	'i'	/* signed 64-bit, 8 bytes big-endian */
#define KR_TIME	't'	/* UTC time_t, 8 bytes big-endian */
#define KR_BYTES 'b'

struct krec { unsigned char *buf; size_t len, cap; };
struct kfield { const char *name; size_t nlen; int type; const void *val; size_t vlen; };

void kr_init(struct krec *r, void *buf, size_t cap);
int kr_put(struct krec *r, const char *name, int type, const void *val, size_t vlen);
int kr_putstr(struct krec *r, const char *name, const char *s);
int kr_putint(struct krec *r, const char *name, long long v);
int kr_puttime(struct krec *r, const char *name, long long t);
/* Find a field in a record (val, len from kt_get). */
int kr_get(const void *rec, size_t len, const char *name, struct kfield *f);
int kr_getint(const void *rec, size_t len, const char *name, long long *v);
/* Walk the fields: *pos starts at 0; 0 = field, ENOENT at the end, EIO if
 * the record is malformed (never reads past len). */
int kr_next(const void *rec, size_t len, size_t *pos, struct kfield *f);

#endif /* KUTXA_H */
