/* gzsig.h - SHA-512, Ed25519 verification (RFC 8032, pure) and the check of
 * the update file Galtzagorriak.update (spec section S, RS.1, RS.2, RS.4,
 * RS.8 step 1-2, RS.13).
 *
 * One source for gzupdate (T33a), atea (T33b) and the host tests
 * (flipper/tests/gzsig).  Verification only: no secret key, no signing.
 * No static data but const tables (flash), no malloc; the caller owns every
 * buffer.  Stack: <= 1.5 KB for gz_ed25519_verify, gzu_check adds its
 * SHA-512 state and one sector (see flipper/PORTING.md for the numbers).
 */
#ifndef GZSIG_H
#define GZSIG_H

typedef unsigned char gz_u8;
typedef unsigned int gz_u32;		/* 32 bit on ARM and the hosts */
typedef unsigned long long gz_u64;
typedef long long gz_i64;

/* SHA-512 (FIPS 180-4), streaming. */
struct gz_sha512 {
	gz_u64 h[8];
	gz_u64 len;			/* bytes so far (< 2^61 here) */
	gz_u8 buf[128];
	unsigned int n;			/* bytes in buf */
};
void gz_sha512_init(struct gz_sha512 *s);
void gz_sha512_update(struct gz_sha512 *s, const void *p, unsigned long n);
void gz_sha512_final(struct gz_sha512 *s, gz_u8 out[64]);
void gz_sha512(const void *p, unsigned long n, gz_u8 out[64]);

/* Ed25519 (RFC 8032 section 5.1.7, pure, no context): 0 if sig is a valid
 * signature of m[0..n) under pk, -1 otherwise.  Rejects S >= L and
 * non-canonical or invalid points. */
int gz_ed25519_verify(const gz_u8 sig[64], const gz_u8 pk[32],
		      const gz_u8 *m, unsigned long n);

/* The update file (format 1, flipper/PORTING.md). */
#define GZU_SECTOR	512
#define GZU_SIGNED	448		/* signed header bytes */
#define GZU_NSLOTS	16
#define GZU_DATA	(4 * GZU_SECTOR)	/* first section byte */
#define GZU_NOTES_MAX	4096
#define GZU_FLASH	1
#define GZU_ROOT	2
#define GZU_PKG		3
#define GZU_NOTES	4

struct gzu_sect {
	gz_u8 type, flags;
	unsigned short index;
	gz_u32 off, len, load;
	gz_u8 hash[64];			/* full SHA-512 */
};

struct gzu_info {			/* filled by gzu_check */
	char board[17], release[33];
	gz_u8 rel[4], from_min[4];
	gz_u32 epoch, abi, libc_id, furi_id;
	gz_u64 btime;
	gz_u8 kid[8];
	int key;			/* which trusted key: 0 or 1 */
	gz_u32 nsect, size;
	struct gzu_sect sect[GZU_NSLOTS];
	char why[72];			/* a reason with numbers in it */
};

/* Reads n bytes at off into buf; returns the number read (0 at the end,
 * < 0 on error).  A short read is only allowed at the end of the file. */
typedef long (*gzu_read_fn)(void *ctx, gz_u32 off, void *buf, unsigned long n);

/* What the running system is, for RS.8 step 2 and RS.13. */
struct gzu_running {
	const char *board;		/* "flipper-f7" (NULL: any) */
	long epoch;			/* < 0: no epoch rule */
	int downgrade;			/* the press in atea (RS.13) */
	int have_rel;			/* rel valid: check from_min */
	gz_u8 rel[3];			/* running X.Y.Z */
};

/* Checks the whole file of `size` bytes in the order of PORTING.md: header,
 * key id, signature, board, size, table hash, entries, zero padding, every
 * section's SHA-512 (streamed through buf, `bufsz` bytes, a multiple of 512,
 * at least 512; 4096 in gzupdate), then the release rules.  `keys` holds
 * nkeys (1 or 2, atea's slots) public keys of 32 B.  Returns NULL if the file
 * is good, else the reason (the same words as tools/gzupdate-check.py, e.g.
 * "not signed by a trusted key"; reasons with numbers in them, such as
 * "section 1 (root) damaged (hash mismatch)", are built in info->why).
 * `info` is filled as far as the check got. */
const char *gzu_check(gzu_read_fn rd, void *ctx, gz_u32 size,
		      const gz_u8 (*keys)[32], int nkeys,
		      const struct gzu_running *run, struct gzu_info *info,
		      gz_u8 *buf, unsigned long bufsz);

/* Only the header and signature (atea's quick re-check of the staged file
 * starts with it); the same reasons. */
const char *gzu_check_header(const gz_u8 sec0[GZU_SECTOR],
			     const gz_u8 (*keys)[32], int nkeys,
			     struct gzu_info *info);

/* Releases compared as X.Y.Z (a, b: 3 bytes): <0, 0, >0. */
int gzu_relcmp(const gz_u8 *a, const gz_u8 *b);

/* Section type name ("flash", ...; "?" for others). */
const char *gzu_type_name(int type);

#endif /* GZSIG_H */
