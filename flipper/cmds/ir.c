/* ir - infrared receive and send on the Flipper (Galtzagorriak MINIX)
 *
 * Usage:	ir rx [-r] [-n COUNT]	print received frames (default: one),
 *					decoded; -r: also the raw timings
 *		ir tx PROTO ADDR CMD	send one message
 *		ir tx raw [-f HZ] US ...  send marks and spaces, microseconds,
 *					mark first; carrier HZ (38000)
 *		ir list FILE		the signals in a remote file
 *		ir send FILE NAME	send the signal NAME from FILE
 *		ir learn FILE NAME	receive one frame, append it to FILE
 *
 * Protocols, with the numbering of the stock firmware so that its remote
 * files (Flipper "IR signals file", version 1) work as they are:
 *	NEC NECext NEC42 NEC42ext Samsung32 RC5 RC5X RC6 SIRC SIRC15
 *	SIRC20 Kaseikyo RCA Pioneer
 * Uses /dev/ir (kernel/arm/flipdev.c): 32-bit durations in microseconds;
 * a first word with bit 31 set gives the carrier frequency and duty.
 */
#include <sys/types.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXD	1024		/* durations; the kernel takes up to 2048 */
#define FREQ	0x80000000UL	/* first word: carrier, not a duration */

static unsigned long d[MAXD + 1];
static int nd;

enum { NEC, NECEXT, NEC42, NEC42EXT, SAMSUNG, RC5, RC5X, RC6, SIRC, SIRC15,
	SIRC20, KASEIKYO, RCA, PIONEER, NPROTO };
static char *pname[NPROTO] = { "NEC", "NECext", "NEC42", "NEC42ext",
	"Samsung32", "RC5", "RC5X", "RC6", "SIRC", "SIRC15", "SIRC20",
	"Kaseikyo", "RCA", "Pioneer" };
static unsigned long pfreq[NPROTO] = { 38000, 38000, 38000, 38000, 38000,
	36000, 36000, 36000, 40000, 40000, 40000, 38000, 38000, 40000 };
static unsigned long amax[NPROTO] = { 0xFF, 0xFFFF, 0x1FFF, 0x3FFFFFF, 0xFF,
	0x1F, 0x1F, 0xFF, 0x1F, 0xFF, 0x1FFF, 0x3FFFFFF, 0xF, 0xFF };
static unsigned long cmax[NPROTO] = { 0xFF, 0xFFFF, 0xFF, 0xFFFF, 0xFF,
	0x3F, 0x3F, 0xFF, 0x7F, 0x7F, 0x7F, 0x3FF, 0xFF, 0xFF };

struct msg {
	int proto;		/* NEC.., or -1: a repeat code of NEC/Samsung */
	unsigned long addr, cmd;
};

/* Pulse distance codes: header, then each bit a mark and a short (0) or
 * long (1) space, LSB first, then a stop mark. */
struct pd { unsigned long hm, hs, bm, s0, s1; };
static struct pd pd_nec = { 9000, 4500, 562, 562, 1687 };
static struct pd pd_sam = { 4500, 4500, 550, 550, 1650 };
static struct pd pd_kas = { 3456, 1728, 432, 432, 1296 };
static struct pd pd_rca = { 4000, 4000, 500, 1000, 2000 };
static struct pd pd_pio = { 8500, 4225, 500, 500, 1500 };

#define RC5_U	889		/* half a bit */
#define RC6_U	444
#define SIRC_PERIOD 45000	/* a message every 45 ms, at least 3 */
#define PIO_GAP	26000		/* Pioneer: twice, this far apart */

static void usage(void)
{
	fprintf(stderr, "Usage: ir rx [-r] [-n COUNT]\n"
			"       ir tx PROTO ADDR CMD\n"
			"       ir tx raw [-f HZ] US US ...\n"
			"       ir list FILE\n"
			"       ir send FILE NAME\n"
			"       ir learn FILE NAME\n"
			"PROTO: NEC NECext NEC42 NEC42ext Samsung32 RC5 RC5X "
			"RC6\n       SIRC SIRC15 SIRC20 Kaseikyo RCA Pioneer\n");
	exit(1);
}

static int same(char *a, char *b)	/* equal, ignoring case */
{
	while (*a && tolower(*a) == tolower(*b)) a++, b++;
	return *a == 0 && *b == 0;
}

/* Within 25 % (and 150 us) of the nominal duration? */
static int near(unsigned long v, unsigned long nom)
{
	unsigned long tol = nom / 4 > 150 ? nom / 4 : 150;

	return v + tol >= nom && v <= nom + tol;
}

static void put(unsigned long us)
{
	if (nd < MAXD) d[nd++] = us;
}

/*===========================================================================*
 *				Decoding				     *
 *===========================================================================*/
/* Each decoder looks at a message starting with the mark d[i] and returns
 * the index after its last mark (the gap before the next message), or 0. */

static int ended(int k)		/* nothing, or a long gap, at d[k]? */
{
	return k >= nd || d[k] > 10000;
}

static int pd_get(int i, struct pd *p, int nbits, unsigned char *b)
{
	int k;

	if (i + 2 + 2 * nbits >= nd) return 0;
	if (!near(d[i], p->hm) || !near(d[i + 1], p->hs)) return 0;
	memset(b, 0, (nbits + 7) / 8);
	for (k = 0; k < nbits; k++) {
		if (!near(d[i + 2 + 2 * k], p->bm)) return 0;
		if (near(d[i + 3 + 2 * k], p->s1)) b[k >> 3] |= 1 << (k & 7);
		else if (!near(d[i + 3 + 2 * k], p->s0)) return 0;
	}
	if (!near(d[i + 2 + 2 * nbits], p->bm)) return 0;
	if (!ended(i + 3 + 2 * nbits)) return 0;	/* more bits follow */
	return i + 3 + 2 * nbits;
}

static int dec_nec(int i, struct msg *m)
{
	unsigned char b[6];
	int r;

	if (i + 2 < nd && near(d[i], 9000) && near(d[i + 1], 2250)
			&& near(d[i + 2], 562) && ended(i + 3)) {
		m->proto = -1;
		return i + 3;
	}
	if ((r = pd_get(i, &pd_nec, 42, b)) != 0) {	/* NEC42 */
		unsigned long lo = b[0] | (unsigned long) b[1] << 8
			| (unsigned long) b[2] << 16 | (unsigned long) b[3] << 24;
		unsigned hi = b[4] | (unsigned) b[5] << 8;
		unsigned long ad = lo & 0x1FFF, adi = (lo >> 13) & 0x1FFF;
		unsigned long c = lo >> 26 | (hi & 3) << 6, ci = (hi >> 2) & 0xFF;

		if (adi == (~ad & 0x1FFF) && ci == (~c & 0xFF)) {
			m->proto = NEC42;
			m->addr = ad;
			m->cmd = c;
		} else {
			m->proto = NEC42EXT;
			m->addr = lo & 0x3FFFFFF;
			m->cmd = c | ci << 8;
		}
		return r;
	}
	if ((r = pd_get(i, &pd_nec, 32, b)) == 0) return 0;
	if (b[0] == (unsigned char) ~b[1] && b[2] == (unsigned char) ~b[3]) {
		m->proto = NEC;
		m->addr = b[0];
		m->cmd = b[2];
	} else {
		m->proto = NECEXT;
		m->addr = b[0] | (unsigned) b[1] << 8;
		m->cmd = b[2] | (unsigned) b[3] << 8;
	}
	return r;
}

static int dec_samsung(int i, struct msg *m)
{
	unsigned char b[4];
	int r;

	if (i + 4 < nd && near(d[i], 4500) && near(d[i + 1], 4500)
			&& near(d[i + 2], 550) && near(d[i + 3], 1650)
			&& near(d[i + 4], 550) && ended(i + 5)) {
		m->proto = -1;
		return i + 5;
	}
	if ((r = pd_get(i, &pd_sam, 32, b)) == 0) return 0;
	if (b[0] != b[1] || b[2] != (unsigned char) ~b[3]) return 0;
	m->proto = SAMSUNG;
	m->addr = b[0];
	m->cmd = b[2];
	return r;
}

static int dec_rca(int i, struct msg *m)
{
	unsigned char b[3];
	unsigned long v;
	int r;

	if ((r = pd_get(i, &pd_rca, 24, b)) == 0) return 0;
	v = b[0] | (unsigned long) b[1] << 8 | (unsigned long) b[2] << 16;
	if (((v >> 12) & 0xF) != (~v & 0xF)
		|| ((v >> 16) & 0xFF) != (~(v >> 4) & 0xFF)) return 0;
	m->proto = RCA;
	m->addr = v & 0xF;
	m->cmd = (v >> 4) & 0xFF;
	return r;
}

/* Pioneer: NEC at 40 kHz with a slightly shorter header; told apart by
 * the header alone, so it has to fit closely (as on the stock firmware,
 * within 200 us).  Sent with a 33rd bit, a 0, before the stop mark. */
static int dec_pioneer(int i, struct msg *m)
{
	unsigned char b[5];
	int r;

	if (i + 1 >= nd || d[i] + 200 < 8500 || d[i] > 8500 + 200
		|| d[i + 1] + 200 < 4225 || d[i + 1] > 4225 + 200) return 0;
	if ((r = pd_get(i, &pd_pio, 33, b)) == 0
		&& (r = pd_get(i, &pd_pio, 32, b)) == 0) return 0;
	if (b[1] != (unsigned char) ~b[0] || b[3] != (unsigned char) ~b[2])
		return 0;
	m->proto = PIONEER;
	m->addr = b[0];
	m->cmd = b[2];
	return r;
}

static int dec_kaseikyo(int i, struct msg *m)
{
	unsigned char b[6], vp;
	int r;

	if ((r = pd_get(i, &pd_kas, 48, b)) == 0) return 0;
	vp = b[0] ^ b[1];
	vp = (vp & 0xF) ^ (vp >> 4);
	if ((b[2] & 0xF) != vp || b[5] != (b[2] ^ b[3] ^ b[4])) return 0;
	m->proto = KASEIKYO;
	m->addr = (unsigned long) (b[4] >> 6) << 24
		| (unsigned long) (b[0] | (unsigned) b[1] << 8) << 8
		| (b[2] >> 4) << 4 | (b[3] & 0xF);
	m->cmd = (b[3] >> 4) | (unsigned) (b[4] & 0x3F) << 4;
	return r;
}

static int dec_sirc(int i, struct msg *m)
{
	unsigned long v = 0;
	int k, j;

	if (i + 2 >= nd || !near(d[i], 2400) || !near(d[i + 1], 600))
		return 0;
	for (k = 0; k < 20; k++) {
		j = i + 2 + 2 * k;
		if (j >= nd) return 0;
		if (near(d[j], 1200)) v |= 1UL << k;
		else if (!near(d[j], 600)) return 0;
		if (j + 1 >= nd || d[j + 1] > 1000) break;	/* the gap */
		if (!near(d[j + 1], 600)) return 0;
	}
	switch (k + 1) {
	case 12: m->proto = SIRC; break;
	case 15: m->proto = SIRC15; break;
	case 20: m->proto = SIRC20; break;
	default: return 0;
	}
	m->cmd = v & 0x7F;
	m->addr = v >> 7;
	return j + 1;
}

/* Manchester codes: the durations as a row of half bits (1: mark). */
static unsigned char lv[64];
static int nlv;

static int halves(int i, unsigned long u, int maxm)
{
	int k, n, mark;

	for (k = i; k < nd; k++) {
		mark = (k - i) % 2 == 0;
		if (!mark && d[k] > 4 * u) break;		/* the gap */
		n = (d[k] + u / 2) / u;
		if (n < 1 || n > maxm) return 0;
		while (n-- > 0) {
			if (nlv == sizeof(lv)) return 0;
			lv[nlv++] = mark;
		}
	}
	return k;
}

/* RC5: 14 bits, MSB first, a 1 is space-mark: S1 (1), S2 (0: RC5X), the
 * toggle, 5 address bits, 6 command bits.  S1 starts with a space, so the
 * frame seems to begin half a bit late. */
static int dec_rc5(int i, struct msg *m)
{
	unsigned v = 0;
	int k, r;

	nlv = 0;
	lv[nlv++] = 0;
	if ((r = halves(i, RC5_U, 2)) == 0) return 0;
	if (nlv == 27) lv[nlv++] = 0;		/* ended in a space */
	if (nlv != 28) return 0;
	for (k = 0; k < 14; k++) {
		if (lv[2 * k] == lv[2 * k + 1]) return 0;
		v = v << 1 | lv[2 * k + 1];
	}
	if (!(v & 0x2000)) return 0;
	m->proto = v & 0x1000 ? RC5 : RC5X;
	m->addr = (v >> 6) & 0x1F;
	m->cmd = v & 0x3F;
	return r;
}

/* RC6 mode 0: a leader (6 + 2 units), then MSB first, a 1 is mark-space:
 * start bit (1), mode (000), the toggle (twice as long), 8 address bits,
 * 8 command bits. */
static int dec_rc6(int i, struct msg *m)
{
	unsigned v = 0;
	int k, r;

	if (i + 2 >= nd || !near(d[i], 2666) || !near(d[i + 1], 889))
		return 0;
	nlv = 0;
	if ((r = halves(i + 2, RC6_U, 3)) == 0) return 0;
	if (nlv == 43) lv[nlv++] = 0;
	if (nlv != 44) return 0;
	if (lv[0] != 1 || lv[1] != 0) return 0;			/* start */
	for (k = 1; k < 4; k++)					/* mode 0 */
		if (lv[2 * k] != 0 || lv[2 * k + 1] != 1) return 0;
	if (lv[8] != lv[9] || lv[10] != lv[11] || lv[8] == lv[10]) return 0;
	for (k = 0; k < 16; k++) {
		if (lv[12 + 2 * k] == lv[13 + 2 * k]) return 0;
		v = v << 1 | lv[12 + 2 * k];
	}
	m->proto = RC6;
	m->addr = v >> 8;
	m->cmd = v & 0xFF;
	return r;
}

static int (*decoders[])(int, struct msg *) = {
	dec_pioneer, dec_nec, dec_samsung, dec_rca, dec_kaseikyo, dec_sirc,
	dec_rc5, dec_rc6
};
#define NDEC	(sizeof(decoders) / sizeof(decoders[0]))

/* The message at d[i], or 0. */
static int decode1(int i, struct msg *m)
{
	unsigned k;
	int r;

	for (k = 0; k < NDEC; k++)
		if ((r = decoders[k](i, m)) != 0) return r;
	return 0;
}

static void show(struct msg *m, int times)
{
	if (m->proto < 0) printf("repeat");
	else printf("%s address 0x%02lx command 0x%02lx", pname[m->proto],
			m->addr, m->cmd);
	if (times > 1) printf(" (x%d)", times);
	printf("\n");
}

/* Print the messages in the frame; returns how many, and the first. */
static int decode(struct msg *first)
{
	struct msg m, last;
	int i = 0, r, n = 0, times = 0;

	while (i < nd && (r = decode1(i, &m)) != 0) {
		if (n++ == 0) *first = m;
		if (times > 0 && m.proto == last.proto && m.addr == last.addr
						&& m.cmd == last.cmd) {
			times++;
		} else {
			if (times > 0) show(&last, times);
			last = m;
			times = 1;
		}
		i = r + 1;
	}
	if (times > 0) show(&last, times);
	return n;
}

/*===========================================================================*
 *				Encoding				     *
 *===========================================================================*/
static void pd_put(struct pd *p, unsigned char *b, int nbits)
{
	int k;

	put(p->hm);
	put(p->hs);
	for (k = 0; k < nbits; k++) {
		put(p->bm);
		put(b[k >> 3] & (1 << (k & 7)) ? p->s1 : p->s0);
	}
	put(p->bm);
}

/* Half bits lv[] to durations: from the first mark to the last. */
static void lv_put(unsigned long u)
{
	int k = 0, n;

	while (k < nlv && lv[k] == 0) k++;
	while (nlv > k && lv[nlv - 1] == 0) nlv--;
	while (k < nlv) {
		for (n = 1; k + n < nlv && lv[k + n] == lv[k]; n++) ;
		put(n * u);
		k += n;
	}
}

static void lv_bit(int bit, int first)	/* first: the level of the 1st half */
{
	lv[nlv++] = first ? bit : !bit;
	lv[nlv++] = first ? !bit : bit;
}

static void encode(int proto, unsigned long a, unsigned long c)
{
	unsigned char b[6], vp;
	unsigned long v, len;
	int k, t, nbits, start;

	t = getpid() & 1;		/* the toggle: differs run to run */
	switch (proto) {
	case NEC:
	case NECEXT:
	case SAMSUNG:
		b[0] = a;
		b[1] = proto == NEC ? ~a : proto == NECEXT ? a >> 8 : a;
		b[2] = c;
		b[3] = proto == NECEXT ? c >> 8 : ~c;
		pd_put(proto == SAMSUNG ? &pd_sam : &pd_nec, b, 32);
		break;
	case NEC42:
	case NEC42EXT:
		v = proto == NEC42 ? (a & 0x1FFF) | (~a & 0x1FFF) << 13
			| (c & 0x3F) << 26 : (a & 0x3FFFFFF) | (c & 0x3F) << 26;
		len = proto == NEC42 ? (c & 0xC0) >> 6 | (~c & 0xFF) << 2
			: (c & 0xFFC0) >> 6;
		for (k = 0; k < 4; k++) b[k] = v >> (8 * k);
		b[4] = len;
		b[5] = len >> 8;
		pd_put(&pd_nec, b, 42);
		break;
	case RCA:
		v = (a & 0xF) | c << 4 | (~a & 0xF) << 12 | (~c & 0xFF) << 16;
		for (k = 0; k < 3; k++) b[k] = v >> (8 * k);
		pd_put(&pd_rca, b, 24);
		break;
	case PIONEER:
		b[0] = a;
		b[1] = ~a;
		b[2] = c;
		b[3] = ~c;
		b[4] = 0;
		pd_put(&pd_pio, b, 33);
		put(PIO_GAP);
		pd_put(&pd_pio, b, 33);
		break;
	case KASEIKYO:
		b[0] = a >> 8;
		b[1] = a >> 16;
		vp = b[0] ^ b[1];
		vp = (vp & 0xF) ^ (vp >> 4);
		b[2] = vp | ((a >> 4) & 0xF) << 4;
		b[3] = (a & 0xF) | (c & 0xF) << 4;
		b[4] = ((a >> 24) & 3) << 6 | c >> 4;
		b[5] = b[2] ^ b[3] ^ b[4];
		pd_put(&pd_kas, b, 48);
		break;
	case RC5:
	case RC5X:
		v = 0x2000 | (proto == RC5 ? 0x1000 : 0) | t << 11
			| a << 6 | c;
		nlv = 0;
		for (k = 13; k >= 0; k--) lv_bit((v >> k) & 1, 0);
		lv_put(RC5_U);
		break;
	case RC6:
		put(2666);
		put(889);
		nlv = 0;
		lv_bit(1, 1);
		for (k = 0; k < 3; k++) lv_bit(0, 1);
		lv[nlv++] = t; lv[nlv++] = t; lv[nlv++] = !t; lv[nlv++] = !t;
		v = a << 8 | c;
		for (k = 15; k >= 0; k--) lv_bit((v >> k) & 1, 1);
		lv_put(RC6_U);
		break;
	case SIRC:
	case SIRC15:
	case SIRC20:
		nbits = proto == SIRC ? 12 : proto == SIRC15 ? 15 : 20;
		v = c | a << 7;
		for (t = 0; t < 3; t++) {
			start = nd;
			put(2400);
			for (k = 0; k < nbits; k++) {
				put(600);
				put(v & (1UL << k) ? 1200 : 600);
			}
			if (t == 2) break;
			for (len = 0, k = start; k < nd; k++) len += d[k];
			put(SIRC_PERIOD - len);
		}
		break;
	}
}

/*===========================================================================*
 *				/dev/ir					     *
 *===========================================================================*/
static int devir(int mode)
{
	int fd;

	if ((fd = open("/dev/ir", mode)) < 0) {
		perror("ir: /dev/ir");
		exit(1);
	}
	return fd;
}

static int receive(int fd)
{
	int n;

	if ((n = read(fd, (char *) d, MAXD * 4)) < 0) {
		perror("ir");
		exit(1);
	}
	return nd = n / 4;
}

static void raw(void)
{
	int i;

	printf("%d:", nd);
	for (i = 0; i < nd; i++) printf(" %lu", d[i]);
	printf("\n");
}

static int rx(int count, int rawtoo)
{
	struct msg m;
	int fd = devir(O_RDONLY);

	while (count-- > 0) {
		receive(fd);
		if (decode(&m) == 0 || rawtoo) raw();
		fflush(stdout);
	}
	return 0;
}

static int tx(unsigned long hz, unsigned duty)
{
	int fd;

	if (nd % 2 == 0) nd--;			/* ends in a space: drop it */
	if (nd <= 0) {
		fprintf(stderr, "ir: nothing to send\n");
		return 1;
	}
	memmove(d + 1, d, nd * sizeof(d[0]));
	d[0] = FREQ | hz | (unsigned long) duty << 20;
	fd = devir(O_WRONLY);
	if (write(fd, (char *) d, (nd + 1) * 4) != (nd + 1) * 4) {
		perror("ir");
		return 1;
	}
	close(fd);
	return 0;
}

static int protocol(char *name)
{
	int p;

	for (p = 0; p < NPROTO; p++)
		if (same(name, pname[p])) return p;
	return -1;
}

static int send_msg(int p, unsigned long a, unsigned long c)
{
	if (a > amax[p] || c > cmax[p]) {
		fprintf(stderr, "ir: %s takes addresses up to 0x%lx and "
			"commands up to 0x%lx\n", pname[p], amax[p], cmax[p]);
		return 1;
	}
	nd = 0;
	encode(p, a, c);
	return tx(pfreq[p], 33);
}

/*===========================================================================*
 *				Remote files				     *
 *===========================================================================*/
/* The stock firmware's format: "key: value" lines, signals separated by
 * "#" lines, each starting with "name:".  Parsed signals give protocol,
 * address and command (4 hex bytes, little endian); raw ones frequency,
 * duty_cycle (a fraction) and data (durations, mark first). */
struct sig {
	char name[64];
	int raw, proto;
	unsigned long addr, cmd, freq;
	unsigned duty;
};

static int field(FILE *f, char *key, char *val)
{
	unsigned long v;
	int c, n = 0;

	key[0] = 0;
	if ((c = getc(f)) == EOF) return 0;
	while (c != EOF && c != '\n' && c != ':') {
		if (n < 31) key[n++] = c;
		c = getc(f);
	}
	key[n] = 0;
	val[0] = 0;
	if (c != ':') return 1;			/* "#", or an empty line */
	while ((c = getc(f)) == ' ') ;
	if (strcmp(key, "data") == 0) {
		nd = 0;
		while (c != EOF && c != '\n') {
			if (!isdigit(c)) {
				c = getc(f);
				continue;
			}
			for (v = 0; isdigit(c); c = getc(f)) v = v * 10 + c - '0';
			put(v);
		}
		return 1;
	}
	n = 0;
	while (c != EOF && c != '\n') {
		if (n < 63) val[n++] = c;
		c = getc(f);
	}
	while (n > 0 && (val[n - 1] == '\r' || val[n - 1] == ' ')) n--;
	val[n] = 0;
	return 1;
}

static unsigned long hexbytes(char *s)	/* "07 00 00 00": little endian */
{
	unsigned long v = 0;
	int k;

	for (k = 0; k < 4 && *s; k++) {
		v |= strtoul(s, &s, 16) << (8 * k);
		while (*s == ' ') s++;
	}
	return v;
}

static unsigned percent(char *s)	/* "0.330000" -> 33 */
{
	unsigned p = 0;
	int k;

	if (*s == '1') return 100;
	while (*s && *s != '.') s++;
	if (*s == '.') s++;
	for (k = 0; k < 2; k++) p = p * 10 + (isdigit(*s) ? *s++ - '0' : 0);
	return p;
}

/* Calls fn for each signal of the file, until it returns nonzero. */
static int each(char *file, int (*fn)(struct sig *, char *), char *arg)
{
	FILE *f;
	struct sig s;
	char key[32], val[64];
	int have = 0, r = 0, more;

	if ((f = fopen(file, "r")) == NULL) {
		perror(file);
		exit(1);
	}
	do {
		more = field(f, key, val);
		if (have && (!more || strcmp(key, "name") == 0)) {
			if ((r = fn(&s, arg)) != 0) break;
			have = 0;
		}
		if (strcmp(key, "name") == 0) {
			memset(&s, 0, sizeof(s));
			strncpy(s.name, val, sizeof(s.name) - 1);
			s.proto = -1;
			s.freq = 38000;
			s.duty = 33;
			nd = 0;
			have = 1;
		} else if (strcmp(key, "type") == 0) s.raw = same(val, "raw");
		else if (strcmp(key, "protocol") == 0) s.proto = protocol(val);
		else if (strcmp(key, "address") == 0) s.addr = hexbytes(val);
		else if (strcmp(key, "command") == 0) s.cmd = hexbytes(val);
		else if (strcmp(key, "frequency") == 0)
			s.freq = strtoul(val, NULL, 10);
		else if (strcmp(key, "duty_cycle") == 0) s.duty = percent(val);
	} while (more);
	fclose(f);
	return r;
}

static int list1(struct sig *s, char *arg)
{
	(void) arg;
	if (s->raw) printf("%-20s raw, %d durations at %lu Hz\n", s->name,
			nd, s->freq);
	else if (s->proto < 0) printf("%-20s unknown protocol\n", s->name);
	else printf("%-20s %s address 0x%02lx command 0x%02lx\n", s->name,
			pname[s->proto], s->addr, s->cmd);
	return 0;
}

static int send1(struct sig *s, char *name)
{
	if (strcmp(s->name, name) != 0) return 0;
	if (s->raw) {
		if (s->freq < 10000 || s->freq > 60000 || s->duty > 90) {
			fprintf(stderr, "ir: %s: carrier %lu Hz, %u %%: "
				"out of range\n", name, s->freq, s->duty);
			exit(1);
		}
		exit(tx(s->freq, s->duty));
	}
	if (s->proto < 0) {
		fprintf(stderr, "ir: %s: unknown protocol\n", name);
		exit(1);
	}
	exit(send_msg(s->proto, s->addr, s->cmd));
	return 1;
}

static void hexout(FILE *f, char *key, unsigned long v)
{
	fprintf(f, "%s: %02lX %02lX %02lX %02lX\n", key, v & 0xFF,
		(v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
}

static int learn(char *file, char *name)
{
	struct msg m;
	FILE *f;
	int fd, i, n, isnew;

	isnew = access(file, 0) != 0;
	fd = devir(O_RDONLY);
	printf("Point the remote at the Flipper and press the button.\n");
	fflush(stdout);
	do {
		receive(fd);
		n = decode(&m);
	} while (n > 0 && m.proto < 0);		/* only a repeat: again */
	if ((f = fopen(file, "a")) == NULL) {
		perror(file);
		return 1;
	}
	if (isnew) fprintf(f, "Filetype: IR signals file\nVersion: 1\n");
	fprintf(f, "#\nname: %s\n", name);
	if (n > 0) {
		fprintf(f, "type: parsed\nprotocol: %s\n", pname[m.proto]);
		hexout(f, "address", m.addr);
		hexout(f, "command", m.cmd);
	} else {
		fprintf(f, "type: raw\nfrequency: 38000\n"
			   "duty_cycle: 0.330000\ndata:");
		for (i = 0; i < nd; i++) fprintf(f, " %lu", d[i]);
		fprintf(f, "\n");
		printf("Not a known protocol: saved %d raw durations.\n", nd);
	}
	if (fclose(f) != 0) {
		perror(file);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned long hz = 38000;
	int i, p, count = 1, rawtoo = 0;

	if (argc < 2) usage();
	if (strcmp(argv[1], "rx") == 0) {
		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-r") == 0) rawtoo = 1;
			else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
				count = atoi(argv[++i]);
			else usage();
		}
		return rx(count, rawtoo);
	}
	if (strcmp(argv[1], "tx") == 0 && argc >= 4
					&& strcmp(argv[2], "raw") == 0) {
		i = 3;
		if (strcmp(argv[i], "-f") == 0 && argc >= 6) {
			hz = strtoul(argv[i + 1], NULL, 0);
			if (hz < 10000 || hz > 60000) usage();
			i += 2;
		}
		for (nd = 0; i < argc; i++) put(strtoul(argv[i], NULL, 0));
		if (nd % 2 == 0) {
			fprintf(stderr, "ir: need an odd number of durations\n");
			return 1;
		}
		return tx(hz, 33);
	}
	if (strcmp(argv[1], "tx") == 0 && argc == 5) {
		if ((p = protocol(argv[2])) < 0) usage();
		return send_msg(p, strtoul(argv[3], NULL, 0),
				   strtoul(argv[4], NULL, 0));
	}
	if (strcmp(argv[1], "list") == 0 && argc == 3)
		return each(argv[2], list1, NULL);
	if (strcmp(argv[1], "send") == 0 && argc == 4) {
		each(argv[2], send1, argv[3]);
		fprintf(stderr, "ir: %s: no signal %s\n", argv[2], argv[3]);
		return 1;
	}
	if (strcmp(argv[1], "learn") == 0 && argc == 4)
		return learn(argv[2], argv[3]);
	usage();
	return 1;
}
