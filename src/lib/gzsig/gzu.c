/* gzu.c - check of the update file Galtzagorriak.update, format 1 (spec
 * RS.1, RS.2, RS.8 step 1-2, RS.13; layout in flipper/PORTING.md).  The
 * same checks in the same order as tools/gzupdate-check.py, with the same
 * reasons, so both reject the ACS.1 bad files alike (flipper/tests/gzsig).
 * No libc: atea (T33b) links it too. */
#include "gzsig.h"

static const char *const tnames[] = { "?", "flash", "root", "pkg", "notes" };

const char *gzu_type_name(int type)
{
	return type >= 1 && type <= 4 ? tnames[type] : tnames[0];
}

static gz_u32 le32(const gz_u8 *p)
{
	return p[0] | (gz_u32) p[1] << 8 | (gz_u32) p[2] << 16
		| (gz_u32) p[3] << 24;
}

static int same(const gz_u8 *a, const gz_u8 *b, int n)
{
	int r = 0;

	while (n--) r |= *a++ ^ *b++;
	return r == 0;
}

static int zero(const gz_u8 *a, unsigned long n)
{
	while (n--) if (*a++) return 0;
	return 1;
}

/* A NUL-padded printable string field: copies it to out (n + 1 bytes). */
static int cstr(const gz_u8 *f, int n, char *out)
{
	int i, end = 0;

	for (i = 0; i < n; i++) {
		if (!end && f[i] == 0) end = 1;
		if (end) { if (f[i]) return -1; out[i] = 0; continue; }
		if (f[i] < 0x21 || f[i] > 0x7e) return -1;
		out[i] = (char) f[i];
	}
	out[n] = 0;
	return 0;
}

/* Tiny string builder for info->why (no sprintf in atea). */
struct sb { char *p; int n, max; };

static void sbs(struct sb *b, const char *s)
{
	while (*s && b->n < b->max - 1) b->p[b->n++] = *s++;
	b->p[b->n] = 0;
}

static void sbu(struct sb *b, unsigned long v)
{
	char t[12];
	int i = 0;

	do t[i++] = (char) ('0' + v % 10); while ((v /= 10) && i < 11);
	while (i && b->n < b->max - 1) b->p[b->n++] = t[--i];
	b->p[b->n] = 0;
}

static void sbv(struct sb *b, const gz_u8 *r)
{
	sbu(b, r[0]); sbs(b, "."); sbu(b, r[1]); sbs(b, "."); sbu(b, r[2]);
}

int gzu_relcmp(const gz_u8 *a, const gz_u8 *b)
{
	int i;

	for (i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
	return 0;
}

const char *gzu_check_header(const gz_u8 sec0[GZU_SECTOR],
			     const gz_u8 (*keys)[32], int nkeys,
			     struct gzu_info *in)
{
	gz_u8 h[64];
	int k;

	in->why[0] = 0;
	in->key = -1;
	if (!same(sec0, (const gz_u8 *) "GZUPDATE", 8))
		return "not an update file";
	if (sec0[8] != 1 || sec0[9])
		return "format not supported (this checker knows 1)";
	if (sec0[10] != (GZU_SIGNED & 0xff) || sec0[11] != GZU_SIGNED >> 8
	    || le32(sec0 + 12))
		return "bad header";
	if (!zero(sec0 + 176, GZU_SIGNED - 176) || sec0[35] || sec0[39])
		return "bad header: reserved bytes not zero";
	if (cstr(sec0 + 16, 16, in->board))
		return "bad header: board not NUL-padded";
	if (cstr(sec0 + 144, 32, in->release))
		return "bad header: release text not NUL-padded";
	for (k = 0; k < 4; k++) {
		in->rel[k] = sec0[32 + k];
		in->from_min[k] = sec0[36 + k];
		in->kid[k] = sec0[64 + k];
		in->kid[k + 4] = sec0[68 + k];
	}
	in->epoch = le32(sec0 + 40);
	in->abi = le32(sec0 + 44);
	in->libc_id = le32(sec0 + 48);
	in->furi_id = le32(sec0 + 52);
	in->btime = le32(sec0 + 56) | (gz_u64) le32(sec0 + 60) << 32;
	in->nsect = le32(sec0 + 72);
	in->size = le32(sec0 + 76);
	/* The key id names a trusted key; then the signature. */
	for (k = 0; k < nkeys; k++) {
		gz_sha512(keys[k], 32, h);
		if (same(h, sec0 + 64, 8)) break;
	}
	if (k == nkeys) return "not signed by a trusted key";
	if (gz_ed25519_verify(sec0 + GZU_SIGNED, keys[k], sec0, GZU_SIGNED))
		return "bad signature";
	in->key = k;
	return 0;
}

static int rd_all(gzu_read_fn rd, void *ctx, gz_u32 off, gz_u8 *buf,
		  unsigned long n)
{
	long r = rd(ctx, off, buf, n);

	return r >= 0 && (unsigned long) r == n ? 0 : -1;
}

/* Reads [from, to) and checks it is zero. */
static const char *padding(gzu_read_fn rd, void *ctx, gz_u32 from, gz_u32 to,
			   gz_u8 *buf, unsigned long bufsz)
{
	unsigned long n;

	while (from < to) {
		n = to - from < bufsz ? to - from : bufsz;
		if (rd_all(rd, ctx, from, buf, n)) return "truncated file";
		if (!zero(buf, n)) return "padding not zero";
		from += (gz_u32) n;
	}
	return 0;
}

const char *gzu_check(gzu_read_fn rd, void *ctx, gz_u32 size,
		      const gz_u8 (*keys)[32], int nkeys,
		      const struct gzu_running *run, struct gzu_info *in,
		      gz_u8 *buf, unsigned long bufsz)
{
	struct gz_sha512 s;
	struct sb b;
	gz_u8 h[64], tabhash[64], *e;
	gz_u32 end, pos, left, i, j;
	int seen[5] = { 0, 0, 0, 0, 0 };
	const char *why;
	unsigned long n;

	in->why[0] = 0;
	in->nsect = 0;
	b.p = in->why; b.n = 0; b.max = (int) sizeof in->why;
	if (size < GZU_DATA) return "truncated file";
	if (bufsz < GZU_SECTOR || bufsz % GZU_SECTOR) return "buffer too small";
	if (rd_all(rd, ctx, 0, buf, GZU_SECTOR)) return "truncated file";
	if ((why = gzu_check_header(buf, keys, nkeys, in)) != 0) return why;
	for (i = 0; i < 64; i++) tabhash[i] = buf[80 + i];
	/* From here on the header is the signer's. */
	if (run && run->board) {
		for (i = 0; run->board[i] && run->board[i] == in->board[i]; i++)
			;
		if (run->board[i] || in->board[i]) {
			sbs(&b, "for another board ("); sbs(&b, in->board);
			sbs(&b, ", this is "); sbs(&b, run->board); sbs(&b, ")");
			return in->why;
		}
	}
	if (in->size != size)
		return size < in->size ? "truncated file"
			: "file longer than its header says";
	if (in->size % GZU_SECTOR) return "bad header: size not sector aligned";
	/* Table and full hashes (sectors 1-3): hash, then entries one sector
	 * at a time (the caller's buffer may be only 512 B). */
	gz_sha512_init(&s);
	for (i = 1; i < 4; i++) {
		if (rd_all(rd, ctx, i * GZU_SECTOR, buf, GZU_SECTOR))
			return "truncated file";
		gz_sha512_update(&s, buf, GZU_SECTOR);
		if (i == 1) {
			for (j = 0; j < GZU_NSLOTS; j++) {
				e = buf + 32 * j;
				in->sect[j].type = e[0];
				in->sect[j].flags = e[1];
				in->sect[j].index = (unsigned short) (e[2] | e[3] << 8);
				in->sect[j].off = le32(e + 4);
				in->sect[j].len = le32(e + 8);
				in->sect[j].load = le32(e + 12);
				/* prefix kept in hash[0..16) until the full one */
				for (n = 0; n < 16; n++) in->sect[j].hash[n] = e[16 + n];
			}
		} else {
			for (j = 0; j < 8; j++) {
				struct gzu_sect *t = &in->sect[(i - 2) * 8 + j];

				e = buf + 64 * j;
				/* the prefix check comes below; remember a
				 * mismatch in flags' spare bit 7 */
				for (n = 0; n < 16; n++)
					if (t->hash[n] != e[n]) t->flags |= 0x80;
				for (n = 0; n < 64; n++) t->hash[n] = e[n];
			}
		}
	}
	gz_sha512_final(&s, h);
	if (!same(h, tabhash, 64)) return "section table damaged (hash mismatch)";
	if (in->nsect < 1 || in->nsect > GZU_NSLOTS)
		return "bad section table: section count";
	end = GZU_DATA;
	for (i = 0; i < GZU_NSLOTS; i++) {
		struct gzu_sect *t = &in->sect[i];

		if (i >= in->nsect) {
			if (t->type || t->flags || t->index || t->off || t->len
			    || t->load || !zero(t->hash, 64))
				return "bad section table: unused entry not zero";
			continue;
		}
		if (t->type < 1 || t->type > 4 || (t->flags & 0x7f)
		    || t->index != i) {
			sbs(&b, "bad section table: entry "); sbu(&b, i);
			return in->why;
		}
		if (t->off % GZU_SECTOR || t->off < end || t->len == 0
		    || t->off > in->size || t->len > in->size - t->off) {
			sbs(&b, "bad section table: entry "); sbu(&b, i);
			sbs(&b, " offset/length");
			return in->why;
		}
		if (t->flags & 0x80) {
			sbs(&b, "bad section table: entry "); sbu(&b, i);
			sbs(&b, " hash prefix");
			return in->why;
		}
		if (t->type != GZU_FLASH && t->load) {
			sbs(&b, "bad section table: entry "); sbu(&b, i);
			sbs(&b, " load address");
			return in->why;
		}
		if (t->type == GZU_FLASH && t->load % 2048)
			return "bad section table: flash section not page aligned";
		if (t->type == GZU_NOTES && t->len > GZU_NOTES_MAX)
			return "notes over 4 KB";
		seen[t->type]++;
		end = t->off + t->len + (-t->len & (GZU_SECTOR - 1));
	}
	if (end != in->size)
		return "bad section table: does not end at the file's end";
	if (seen[GZU_FLASH] != 1 || seen[GZU_ROOT] != 1 || seen[GZU_NOTES] > 1)
		return "bad section table: needs one flash and one root section";
	/* Every section's SHA-512, streamed; the padding must be zero. */
	pos = GZU_DATA;
	for (i = 0; i < in->nsect; i++) {
		struct gzu_sect *t = &in->sect[i];

		if ((why = padding(rd, ctx, pos, t->off, buf, bufsz)) != 0)
			return why;
		gz_sha512_init(&s);
		for (pos = t->off, left = t->len; left; left -= (gz_u32) n) {
			n = left < bufsz ? left : bufsz;
			if (rd_all(rd, ctx, pos, buf, n)) return "truncated file";
			gz_sha512_update(&s, buf, n);
			pos += (gz_u32) n;
		}
		gz_sha512_final(&s, h);
		if (!same(h, t->hash, 64)) {
			sbs(&b, "section "); sbu(&b, i); sbs(&b, " (");
			sbs(&b, gzu_type_name(t->type));
			sbs(&b, ") damaged (hash mismatch)");
			return in->why;
		}
	}
	if ((why = padding(rd, ctx, pos, in->size, buf, bufsz)) != 0)
		return why;
	/* Release rules (RS.8 step 2, RS.13). */
	if (run && run->epoch >= 0 && in->epoch <= (gz_u32) run->epoch
	    && !run->downgrade) {
		if (in->epoch == (gz_u32) run->epoch) return "already installed";
		sbs(&b, "older than the running system (epoch ");
		sbu(&b, in->epoch); sbs(&b, ", running ");
		sbu(&b, (unsigned long) run->epoch); sbs(&b, ")");
		return in->why;
	}
	if (run && run->have_rel && gzu_relcmp(in->from_min, run->rel) > 0) {
		sbs(&b, "needs at least "); sbv(&b, in->from_min);
		sbs(&b, " (running "); sbv(&b, run->rel); sbs(&b, ")");
		return in->why;
	}
	return 0;
}
