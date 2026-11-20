/* libradio: manifests of the CPU2 stack images (R16.0.1) and the radio
 * slot (R2b.1a).  See radio.h.  No malloc, no system calls; RAM is the
 * caller's struct rd_man (~170 B on its stack).
 */
#include <string.h>
#include "radio.h"

static const char *const errs[RD_NERR] = {
  "ok", "unknown key", "key missing", "bad value", "SHA-256 differs",
  "FUS too old", "slot too low (reaches the system, libc or launcher)",
  "address outside flash", "size differs", "bad name"
};

const char *rd_err(int e)
{
  return e >= 0 && e < RD_NERR ? errs[e] : "?";
}

static int hexval(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* An unsigned number, decimal or 0x hex, filling all of s[0..n). */
static int number(const char *s, unsigned long n, unsigned long *v)
{
  unsigned long i = 0, x = 0;
  int base = 10, d;

  if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) base = 16, i = 2;
  if (i == n) return -1;
  for (; i < n; i++) {
	d = hexval((unsigned char) s[i]);
	if (d < 0 || d >= base) return -1;
	if (x > (0xFFFFFFFFUL - d) / base) return -1;
	x = x * base + d;
  }
  *v = x;
  return 0;
}

static int version_ok(const char *s, unsigned long n)
{
  unsigned long i;
  int dots = 0, digits = 0;

  for (i = 0; i < n; i++) {
	if (s[i] == '.') {
		if (digits == 0) return 0;
		dots++, digits = 0;
	} else if (s[i] >= '0' && s[i] <= '9') {
		if (++digits > 4) return 0;
	} else return 0;
  }
  return dots == 2 && digits > 0;
}

/* Names: what `radio use <name>` takes and a directory on the card. */
static int name_ok(const char *s, unsigned long n)
{
  unsigned long i;

  if (n == 0 || n >= RD_NAME) return 0;
  for (i = 0; i < n; i++)
	if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9')
	      || (i > 0 && (s[i] == '-' || s[i] == '_' || s[i] == '.'))))
		return 0;
  return 1;
}

static int copy(char *d, int dn, const char *s, unsigned long n)
{
  unsigned long i;

  if (n >= (unsigned long) dn) return -1;
  for (i = 0; i < n; i++)
	if ((unsigned char) s[i] < ' ' || s[i] == 0x7F) return -1;
  memcpy(d, s, n);
  d[n] = 0;
  return 0;
}

#define K_NAME	0x01
#define K_TYPE	0x02
#define K_VER	0x04
#define K_FUS	0x08
#define K_ADDR	0x10
#define K_SIZE	0x20
#define K_SHA	0x40
#define K_NOTE	0x80
#define K_NEED	0x7F

int rd_parse(const char *t, unsigned long n, struct rd_man *m, int *line)
{
  unsigned long i = 0, e, k, v, ve, j;
  int seen = 0, bit, ln = 0;

  memset(m, 0, sizeof(*m));
  *line = 0;
  while (i < n) {
	ln++;
	for (e = i; e < n && t[e] != '\n'; e++) ;
	ve = e;
	if (ve > i && t[ve - 1] == '\r') ve--;
	if (ve == i || t[i] == '#') { i = e + 1; continue; }
	for (k = i; k < ve && t[k] != '\t'; k++) ;
	*line = ln;
	if (k == ve) return RD_EKEY;
	v = k + 1;
#define KEY(s) (k - i == sizeof(s) - 1 && memcmp(t + i, s, k - i) == 0)
	if (KEY("name")) {
		bit = K_NAME;
		if (!name_ok(t + v, ve - v)) return RD_ENAME;
		copy(m->name, RD_NAME, t + v, ve - v);
	} else if (KEY("type")) {
		bit = K_TYPE;
		if (!name_ok(t + v, ve - v)) return RD_EVALUE;
		copy(m->type, RD_NAME, t + v, ve - v);
	} else if (KEY("version") || KEY("fus")) {
		bit = t[i] == 'v' ? K_VER : K_FUS;
		if (!version_ok(t + v, ve - v)) return RD_EVALUE;
		copy(bit == K_VER ? m->version : m->fus, 16, t + v, ve - v);
	} else if (KEY("addr") || KEY("size")) {
		bit = t[i] == 'a' ? K_ADDR : K_SIZE;
		if (number(t + v, ve - v, bit == K_ADDR ? &m->addr : &m->size))
			return RD_EVALUE;
	} else if (KEY("sha256")) {
		bit = K_SHA;
		if (ve - v != 64) return RD_EVALUE;
		for (j = 0; j < 32; j++) {
			int h = hexval((unsigned char) t[v + 2 * j]);
			int l = hexval((unsigned char) t[v + 2 * j + 1]);
			if (h < 0 || l < 0) return RD_EVALUE;
			m->sha[j] = (unsigned char) (h << 4 | l);
		}
	} else if (KEY("note")) {
		bit = K_NOTE;
		if (copy(m->note, RD_NOTE, t + v, ve - v)) return RD_EVALUE;
	} else return RD_EKEY;
#undef KEY
	if (seen & bit) return RD_EKEY;		/* twice */
	seen |= bit;
	i = e + 1;
  }
  *line = 0;
  if ((seen & K_NEED) != K_NEED) return RD_EMISSING;
  if (m->addr % RD_PAGE != 0 || m->addr < RD_FLASH
      || m->addr >= RD_FLASH_END || m->size == 0
      || m->size > RD_FLASH_END - m->addr) return RD_ESLOTHIGH;
  return RD_OK;
}

int rd_format(const struct rd_man *m, char *buf, int bs)
{
  static const char hx[] = "0123456789abcdef";
  char sha[65], num[2][11];
  unsigned long x;
  int i, j, n;

  for (i = 0; i < 32; i++) {
	sha[2 * i] = hx[m->sha[i] >> 4];
	sha[2 * i + 1] = hx[m->sha[i] & 15];
  }
  sha[64] = 0;
  num[0][0] = '0', num[0][1] = 'x';		/* addr: 0x%08lX */
  for (i = 0; i < 8; i++) num[0][2 + i] = "0123456789ABCDEF"
					[(m->addr >> (28 - 4 * i)) & 15];
  num[0][10] = 0;
  x = m->size;				/* size: decimal */
  j = 10;
  num[1][j] = 0;
  do num[1][--j] = (char) ('0' + x % 10); while ((x /= 10) != 0);
  {
	const char *k[8] = { "name", "type", "version", "fus", "addr",
			     "size", "sha256", "note" };
	const char *v[8];

	v[0] = m->name, v[1] = m->type, v[2] = m->version, v[3] = m->fus;
	v[4] = num[0], v[5] = num[1] + j, v[6] = sha, v[7] = m->note;
	for (n = i = 0; i < 8; i++) {
		int kl = (int) strlen(k[i]), vl = (int) strlen(v[i]);

		if (i == 7 && vl == 0) break;
		if (n + kl + vl + 2 >= bs) return -1;
		memcpy(buf + n, k[i], kl), n += kl;
		buf[n++] = '\t';
		memcpy(buf + n, v[i], vl), n += vl;
		buf[n++] = '\n';
	}
  }
  buf[n] = 0;
  return n;
}

static int vparse(const char *s, unsigned long v[3])
{
  int i;

  if (s == NULL || !version_ok(s, strlen(s))) return -1;
  for (i = 0; i < 3; i++) {
	v[i] = 0;
	while (*s >= '0' && *s <= '9') v[i] = v[i] * 10 + (*s++ - '0');
	if (*s == '.') s++;
  }
  return 0;
}

int rd_vercmp(const char *a, const char *b)
{
  unsigned long x[3], y[3];
  int ea = vparse(a, x), eb = vparse(b, y), i;

  if (ea || eb) return ea && eb ? 0 : ea ? -1 : 1;
  for (i = 0; i < 3; i++)
	if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

int rd_slot_ok(unsigned long addr)
{
  if (addr % RD_PAGE != 0 || addr >= RD_FLASH_END) return RD_ESLOTHIGH;
  if (addr < RD_FLASH + RD_INST + RD_INST_RSV + RD_GUARD + RD_CACHE_MIN)
	return RD_ESLOTLOW;
  return RD_OK;
}

int rd_check(const struct rd_man *m, const unsigned char *sha,
	     unsigned long size, const char *fus)
{
  int r;

  if (size != m->size) return RD_ESIZE;
  if (memcmp(sha, m->sha, 32) != 0) return RD_EHASH;
  if (fus != NULL && rd_vercmp(fus, m->fus) < 0) return RD_EFUS;
  if ((r = rd_slot_ok(m->addr)) != RD_OK) return r;
  return RD_OK;
}

int rd_layout(unsigned long sfsa, unsigned long slot, struct rd_layout *l)
{
  unsigned long floor = RD_FLASH + RD_INST + RD_INST_RSV, end;

  memset(l, 0, sizeof(*l));
  l->sfsa = sfsa;
  l->slot = slot;
  l->inst = RD_FLASH + RD_INST;
  if (sfsa < floor + RD_GUARD + RD_CACHE_MIN || sfsa > RD_FLASH_END)
	return -1;			/* no room: neither region used */
  l->guard = sfsa - RD_GUARD;
  end = l->guard - RD_CACHE;		/* today's layout (R2b.1) */
  if (end < floor) end = floor;		/* a big stack: a smaller cache */
  if (slot != 0 && slot < end) end = slot < floor ? floor : slot;
  l->inst_end = end;
  l->cache = end;
  l->cache_end = l->guard;
  return 0;
}

int rd_dead(const struct rd_layout *l, unsigned long addr, unsigned long len)
{
  return addr + len > l->guard;
}

unsigned long rd_slot(const unsigned long *addr, int n)
{
  unsigned long s = 0;
  int i;

  for (i = 0; i < n; i++)
	if (addr[i] != 0 && (s == 0 || addr[i] < s)) s = addr[i];
  return s;
}
