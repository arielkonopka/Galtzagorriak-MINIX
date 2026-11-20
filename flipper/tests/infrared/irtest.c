/* SPDX-License-Identifier: GPL-3.0-or-later
 * make irtest (spec Phase 7, AC7.1; issue #389): libir (src/lib/infrared,
 * the Unleashed infrared encoders and decoders, and irfile.c) on the PC,
 * under AddressSanitizer and UBSan.
 *
 *  1. decoder: every decoder_inputN of the stock unit tests' .irtest files
 *     (vectors/) through the decoders gives decoder_expectedN, checked as
 *     the stock unit test checks (infrared_test.c, run_decoder).
 *  2. encoder: every encoder_inputN gives the timings of encoder_expectedN
 *     (each within 120 us, stock's tolerance).
 *  3. encoder -> decoder: every encoder_decoder_inputN.
 *  4. .ir files: a remote as stock saves it (stock_saved.ir) is read and
 *     written back byte for byte; the TV universal database (tv.ir) is
 *     read signal by signal with a 1024-timing buffer, written, read
 *     again (same signals), written again (same bytes); every parsed
 *     signal in it goes encoder -> decoder unchanged; irf_find streams
 *     every "Power".
 *  5. bad input: broken signals are skipped and the next one is read.
 *
 *	irtest VECDIR
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "irfile.h"

#define MATCH(a, b, t) ((a) + (t) >= (b) && (b) + (t) >= (a))
#define RAWCAP 8192

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* One loaded .irtest signal. */
struct item {
	char name[IRF_NAME_MAX + 1];
	int type;
	uint32_t *t;
	size_t n;
	InfraredMessage *m;
	unsigned long count;
};

static struct item items[256];
static int nitems;
static InfraredDecoderHandler *dec;
static InfraredEncoderHandler *enc;

static void load(const char *path)
{
	FILE *fp = fopen(path, "r");
	struct irf f;
	struct irf_sig s;
	static uint32_t buf[RAWCAP];
	unsigned long i;
	int r;

	nitems = 0;
	CHECK(fp != NULL, "open %s", path);
	if (!fp) return;
	CHECK(irf_open(&f, fp) == 0 && f.kind == IRF_TESTS, "header %s", path);
	s.data = buf;
	s.cap = RAWCAP;
	while ((r = irf_next(&f, &s)) != 0) {
		struct item *it = &items[nitems];

		CHECK(r > 0, "%s: bad signal near line %lu", path, f.line);
		if (r < 0 || nitems == 256) continue;
		memset(it, 0, sizeof(*it));
		strcpy(it->name, s.name);
		it->type = s.type;
		if (s.type == IRF_RAW) {
			it->t = calloc(s.n + 1, sizeof(uint32_t));
			memcpy(it->t, buf, s.n * sizeof(uint32_t));
			it->n = s.n;
		} else if (s.type == IRF_ARRAY) {
			it->m = calloc(s.count + 1, sizeof(InfraredMessage));
			it->count = s.count;
			for (i = 0; i < s.count; i++)
				CHECK(irf_msg(&f, &it->m[i]) == 0,
					"%s %s: message %lu", path, s.name, i);
		}
		nitems++;
	}
	fclose(fp);
}

static void unload(void)
{
	int i;

	for (i = 0; i < nitems; i++) { free(items[i].t); free(items[i].m); }
	nitems = 0;
}

static struct item *find(const char *pre, int k)
{
	char nm[64];
	int i;

	snprintf(nm, sizeof(nm), "%s%d", pre, k);
	for (i = 0; i < nitems; i++)
		if (!strcmp(items[i].name, nm)) return &items[i];
	return NULL;
}

static void same_msg(const char *what, const InfraredMessage *d,
	const InfraredMessage *e)
{
	CHECK(d->protocol == e->protocol && d->address == e->address
		&& d->command == e->command, "%s: got %s %lX %lX, want %s %lX %lX",
		what, infrared_get_protocol_name(d->protocol),
		(unsigned long) d->address, (unsigned long) d->command,
		infrared_get_protocol_name(e->protocol),
		(unsigned long) e->address, (unsigned long) e->command);
	if (e->protocol == InfraredProtocolSIRC
		|| e->protocol == InfraredProtocolSIRC15
		|| e->protocol == InfraredProtocolSIRC20)
		CHECK(!d->repeat, "%s: SIRC repeat", what);
	else
		CHECK(d->repeat == e->repeat, "%s: repeat %d, want %d", what,
			d->repeat, e->repeat);
}

static void run_decoder(const char *file, struct item *in, struct item *ex)
{
	char what[96];
	const InfraredMessage *d, *chk;
	InfraredMessage local;
	unsigned long got = 0;
	bool level = false;
	size_t i;

	snprintf(what, sizeof(what), "%s %s", file, in->name);
	infrared_reset_decoder(dec);
	for (i = 0; i < in->n; i++) {
		chk = NULL;
		if (in->t[i] > INFRARED_RAW_RX_TIMING_DELAY_US) {
			chk = infrared_check_decoder_ready(dec);
			if (chk) { local = *chk; chk = &local; }
		}
		d = infrared_decode(dec, level, in->t[i]);
		if (chk || d) {
			CHECK(!(chk && d), "%s: decoded twice", what);
			if (chk) d = chk;
			CHECK(got < ex->count, "%s: more messages than %lu", what,
				ex->count);
			if (got < ex->count) same_msg(what, d, &ex->m[got]);
			got++;
		}
		level = !level;
	}
	d = infrared_check_decoder_ready(dec);
	if (d) {
		CHECK(got < ex->count, "%s: one more at the end", what);
		if (got < ex->count) same_msg(what, d, &ex->m[got]);
		got++;
	}
	CHECK(got == ex->count, "%s: %lu messages, want %lu", what, got,
		ex->count);
}

/* As stock's infrared_test_run_encoder_fill_array: levels merged. */
static size_t encode(uint32_t *t, size_t cap, bool *start)
{
	uint32_t dur = 0;
	bool lv = false, lr;
	InfraredStatus st;
	size_t i = 0;
	int first = 1;

	for (;;) {
		st = infrared_encode(enc, &dur, &lr);
		if (first) {
			if (start) *start = lr;
			first = 0;
			t[0] = 0;
		} else if (lr != lv) {
			if (++i >= cap) return 0;
			t[i] = 0;
		}
		lv = lr;
		t[i] += dur;
		if (st != InfraredStatusOk && st != InfraredStatusDone) return 0;
		if (st == InfraredStatusDone) break;
	}
	return i + 1;
}

static void run_encoder(const char *file, struct item *in, struct item *ex)
{
	char what[96];
	uint32_t t[200];
	unsigned long k;
	size_t n, i, j = 0;

	snprintf(what, sizeof(what), "%s %s", file, in->name);
	for (k = 0; k < in->count; k++) {
		if (!in->m[k].repeat) infrared_reset_encoder(enc, &in->m[k]);
		n = encode(t, 200, NULL);
		CHECK(n > 0, "%s: encoder failed", what);
		for (i = 0; i < n; i++, j++) {
			CHECK(j < ex->n, "%s: more timings than %zu", what, ex->n);
			if (j >= ex->n) return;
			CHECK(MATCH(t[i], ex->t[j], 120), "%s: timing %zu is %lu, "
				"want %lu", what, j, (unsigned long) t[i],
				(unsigned long) ex->t[j]);
		}
	}
	CHECK(j == ex->n, "%s: %zu timings, want %zu", what, j, ex->n);
}

/* Encode m, decode it again; 1 when the same message comes back. */
static int roundtrip(const char *what, const InfraredMessage *m, int reset)
{
	uint32_t t[200];
	const InfraredMessage *d = NULL;
	bool level = false;
	size_t n, i;

	if (reset) infrared_reset_encoder(enc, m);
	n = encode(t, 200, &level);
	CHECK(n > 0, "%s: encoder failed", what);
	for (i = 0; i < n; i++) {
		d = infrared_decode(dec, level, t[i]);
		if (i == n - 2 && level && d) break;
		if (i < n - 1) CHECK(!d, "%s: decoded early at %zu", what, i);
		else if (!d) d = infrared_check_decoder_ready(dec);
		level = !level;
	}
	CHECK(d != NULL, "%s: not decoded", what);
	if (!d) return 0;
	same_msg(what, d, m);
	return 1;
}

static void run_encdec(const char *file, struct item *in)
{
	char what[96];
	unsigned long k;

	snprintf(what, sizeof(what), "%s %s", file, in->name);
	infrared_reset_decoder(dec);
	for (k = 0; k < in->count; k++)
		roundtrip(what, &in->m[k], !in->m[k].repeat);
}

static void irtests(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[512];
	int k, nd = 0, ne = 0, ned = 0;
	struct item *a, *b;

	CHECK(d != NULL, "opendir %s", dir);
	if (!d) return;
	while ((e = readdir(d)) != NULL) {
		size_t l = strlen(e->d_name);

		if (l < 8 || strcmp(e->d_name + l - 7, ".irtest")) continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		load(path);
		for (k = 1; k < 100; k++) {
			if ((a = find("decoder_input", k)) != NULL) {
				b = find("decoder_expected", k);
				CHECK(b != NULL, "%s: no decoder_expected%d", path, k);
				if (b) { run_decoder(e->d_name, a, b); nd++; }
			}
			if ((a = find("encoder_input", k)) != NULL) {
				b = find("encoder_expected", k);
				CHECK(b != NULL, "%s: no encoder_expected%d", path, k);
				if (b) { run_encoder(e->d_name, a, b); ne++; }
			}
			if ((a = find("encoder_decoder_input", k)) != NULL) {
				run_encdec(e->d_name, a);
				ned++;
			}
		}
		unload();
	}
	closedir(d);
	printf("irtest: %d decoder, %d encoder, %d encoder-decoder vectors\n",
		nd, ne, ned);
	CHECK(nd == 45 && ne == 5 && ned == 11, "vector count changed");
}

/* Copy a whole .ir file through irf_next/irf_write.  Signals read. */
static int copy(const char *from, FILE *to, int *bad)
{
	FILE *fp = fopen(from, "r");
	struct irf f;
	struct irf_sig s;
	static uint32_t buf[IRF_RAW_MAX];
	int r, n = 0;

	*bad = 0;
	if (!fp) return -1;
	if (irf_open(&f, fp) < 0) { fclose(fp); return -1; }
	irf_write_header(to, f.kind);
	s.data = buf;
	s.cap = IRF_RAW_MAX;
	while ((r = irf_next(&f, &s)) != 0) {
		if (r < 0) { (*bad)++; continue; }
		CHECK(irf_write(to, &s) == 0, "%s: write %s", from, s.name);
		n++;
	}
	fclose(fp);
	return n;
}

static long fsize(const char *p, char **data)
{
	FILE *fp = fopen(p, "rb");
	long n;

	if (!fp) return -1;
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	rewind(fp);
	*data = malloc(n + 1);
	if (fread(*data, 1, n, fp) != (size_t) n) n = -1;
	fclose(fp);
	return n;
}

static int same_file(const char *a, const char *b)
{
	char *x, *y;
	long n = fsize(a, &x), m = fsize(b, &y);
	int r = n >= 0 && n == m && !memcmp(x, y, n);

	if (n >= 0) free(x);
	if (m >= 0) free(y);
	return r;
}

static void irfiles(const char *dir)
{
	char in[512], t1[] = "/tmp/irtestAXXXXXX", t2[] = "/tmp/irtestBXXXXXX";
	FILE *fp;
	struct irf f;
	struct irf_sig s;
	static uint32_t buf[IRF_RAW_MAX];
	int n1, n2, bad, r, parsed = 0, power = 0;

	/* stock_saved.ir: as stock writes it; ours must be the same bytes. */
	snprintf(in, sizeof(in), "%s/stock_saved.ir", dir);
	fp = fdopen(mkstemp(t1), "w");
	n1 = copy(in, fp, &bad);
	fclose(fp);
	CHECK(n1 == 3 && bad == 0, "stock_saved.ir: %d signals, %d bad", n1, bad);
	CHECK(same_file(in, t1), "stock_saved.ir: written back differently");

	/* tv.ir: read, write, read, write. */
	snprintf(in, sizeof(in), "%s/tv.ir", dir);
	fp = fopen(t1, "w");
	n1 = copy(in, fp, &bad);
	fclose(fp);
	CHECK(n1 == 613 && bad == 0, "tv.ir: %d signals, %d bad", n1, bad);
	fp = fdopen(mkstemp(t2), "w");
	n2 = copy(t1, fp, &bad);
	fclose(fp);
	CHECK(n2 == n1 && bad == 0, "tv.ir again: %d signals", n2);
	CHECK(same_file(t1, t2), "tv.ir: second copy differs");

	/* Every parsed signal of the database round trips the codecs. */
	fp = fopen(in, "r");
	CHECK(fp && irf_open(&f, fp) == 0 && f.kind == IRF_LIBRARY, "tv.ir");
	s.data = buf;
	s.cap = IRF_RAW_MAX;
	while (fp && (r = irf_next(&f, &s)) != 0) {
		if (r < 0 || s.type != IRF_PARSED) continue;
		infrared_reset_decoder(dec);
		parsed += roundtrip(s.name, &s.msg, 1);
	}
	if (fp) rewind(fp);
	if (fp && irf_open(&f, fp) == 0)
		while (irf_find(&f, "Power", &s)) power++;
	if (fp) fclose(fp);
	printf("irtest: tv.ir %d signals, %d parsed round trips, %d Power\n",
		n1, parsed, power);
	CHECK(power > 100, "tv.ir: only %d Power signals", power);
	remove(t1);
	remove(t2);
}

static void badinput(void)
{
	static const char text[] =
		"Filetype: IR signals file\nVersion: 1\n"
		"#\nname: no_type\nprotocol: NEC\n"	/* ended by the next name */
		"#\nname: bad_proto\ntype: parsed\nprotocol: XYZ\n"
		"address: 00 00 00 00\ncommand: 00 00 00 00\n"
		"#\nname: too_big\ntype: parsed\nprotocol: NEC\n"
		"address: 00 01 00 00\ncommand: 00 00 00 00\n"
		"#\nname: bad_hex\ntype: parsed\nprotocol: NEC\n"
		"address: 0G 00 00 00\ncommand: 00 00 00 00\n"
		"#\nname: long_raw\ntype: raw\nfrequency: 38000\n"
		"duty_cycle: 0.33\ndata: 1 2 3 4 5 6 7 8 9\n"
		"#\nname: good\ntype: parsed\nprotocol: RC5\n"
		"address: 1F 00 00 00\ncommand: 3F 00 00 00\n"
		"#\nname: cut\ntype: raw\nfrequency: 38000\n";
	FILE *fp = tmpfile();
	struct irf f;
	struct irf_sig s;
	uint32_t buf[8];
	int r, good = 0, bad = 0;

	fputs(text, fp);
	rewind(fp);
	CHECK(irf_open(&f, fp) == 0, "badinput header");
	s.data = buf;
	s.cap = 8;
	while ((r = irf_next(&f, &s)) != 0) {
		if (r > 0) {
			good++;
			CHECK(!strcmp(s.name, "good") && s.msg.address == 0x1F
				&& s.msg.command == 0x3F, "badinput: got %s", s.name);
		} else bad++;
	}
	CHECK(good == 1 && bad == 6, "badinput: %d good, %d bad", good, bad);
	fclose(fp);
	CHECK(!irf_name_ok("") && !irf_name_ok("a\tb") && irf_name_ok("Piztu"),
		"irf_name_ok");
}

int main(int argc, char **argv)
{
	if (argc != 2) { fprintf(stderr, "usage: irtest VECDIR\n"); return 2; }
	extern size_t ir_heap_bytes;
	size_t h0;

	dec = infrared_alloc_decoder();
	h0 = ir_heap_bytes;
	enc = infrared_alloc_encoder();
	printf("irtest: heap (64-bit host): decoders %zu B, encoders %zu B\n",
		h0, ir_heap_bytes - h0);
	irtests(argv[1]);
	irfiles(argv[1]);
	badinput();
	infrared_free_decoder(dec);
	infrared_free_encoder(enc);
	printf("irtest: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
