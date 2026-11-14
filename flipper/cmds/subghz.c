/* subghz - the Sub-GHz radio of the Flipper (Galtzagorriak MINIX)
 *
 * Usage:	subghz info			the chip, the region, its bands
 *		subghz rssi FREQ [-n N] [-i MS]	signal strength on FREQ, N
 *						times (0: for ever), MS apart
 *		subghz scan FROM TO STEP [-n N]	signal strength across a
 *						range, STEP in kHz, N sweeps
 *						(default 1)
 *		subghz spi HEX ... [/ HEX ...]	raw transactions (hex bytes,
 *						"/" between them, "w N" waits
 *						N*10 us); prints the answers
 *
 * Frequencies in MHz ("433.92") or Hz.  Receive only so far.  Talks to
 * /dev/subghz (kernel/arm/subghz.c) register by register; the receiver is
 * set up like the stock firmware's "OOK 650 kHz" preset.
 *
 * scan calibrates every channel once and keeps the synthesizer settings
 * (FSCAL1-3), so later sweeps hop without the ~0.7 ms calibration.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XOSC	26000000UL

/* Registers, strobes */
#define IOCFG0	0x02
#define FIFOTHR	0x03
#define PKTCTRL0 0x08
#define FSCTRL1	0x0B
#define FREQ2	0x0D
#define MDMCFG4	0x10
#define MDMCFG3	0x11
#define MDMCFG2	0x12
#define MDMCFG1	0x13
#define MDMCFG0	0x14
#define MCSM0	0x18
#define FOCCFG	0x19
#define AGCCTRL2 0x1B
#define AGCCTRL1 0x1C
#define AGCCTRL0 0x1D
#define WORCTRL	0x20
#define FREND1	0x21
#define FREND0	0x22
#define FSCAL3	0x23
#define SRES	0x30
#define SCAL	0x33
#define SRX	0x34
#define SIDLE	0x36
#define SPWD	0x39
#define PARTNUM	0x30		/* status registers: read with BURST */
#define VERSION	0x31
#define RSSI	0x34
#define MARCSTATE 0x35
#define RD	0x80
#define BURST	0x40

static int fd;
static unsigned char cmd[256], ans[256];
static int nc;

/* The stock firmware's OOK 650 kHz async preset (cc1101_configs.c). */
static unsigned char ook650[] = {
	IOCFG0, 0x0D, FIFOTHR, 0x07, PKTCTRL0, 0x32, FSCTRL1, 0x06,
	MDMCFG0, 0x00, MDMCFG1, 0x00, MDMCFG2, 0x30, MDMCFG3, 0x32,
	MDMCFG4, 0x17, MCSM0, 0x18, FOCCFG, 0x18, AGCCTRL0, 0x91,
	AGCCTRL1, 0x00, AGCCTRL2, 0x07, WORCTRL, 0xFB, FREND0, 0x11,
	FREND1, 0xB6
};

static void usage(void)
{
	fprintf(stderr, "Usage: subghz info\n"
		"       subghz rssi FREQ [-n N] [-i MS]\n"
		"       subghz scan FROM TO STEP [-n N]\n"
		"       subghz spi HEX ... [/ HEX ... ] [/ w N]\n"
		"FREQ in MHz (433.92) or Hz, STEP in kHz\n");
	exit(1);
}

/* Build a write: transactions and pauses, then do it. */
static void t_begin(void) { nc = 0; }

static void t_add(unsigned char *b, int n)
{
	cmd[nc++] = n;
	memcpy(cmd + nc, b, n);
	nc += n;
}

static void t_1(int a) { unsigned char b[1]; b[0] = a; t_add(b, 1); }

static void t_reg(int a, int v)
{
	unsigned char b[2];

	b[0] = a;
	b[1] = v;
	t_add(b, 2);
}

static void t_status(int a)
{
	unsigned char b[2];

	b[0] = a | RD | BURST;
	b[1] = 0;
	t_add(b, 2);
}

static void t_wait(unsigned us)		/* pauses of up to 2.55 ms each */
{
	unsigned k;

	while (us > 0) {
		k = us > 2550 ? 2550 : us;
		cmd[nc++] = 0;
		cmd[nc++] = (k + 9) / 10;
		us -= k;
	}
}

static int t_do(void)
{
	int n;

	if (write(fd, (char *) cmd, nc) != nc) {
		perror("subghz");
		exit(1);
	}
	if ((n = read(fd, (char *) ans, sizeof(ans))) < 0) {
		perror("subghz");
		exit(1);
	}
	return n;
}

/* MHz with decimals, or Hz. */
static unsigned long freq_arg(char *s)
{
	unsigned long v = 0, frac = 0, scale = 1000000;

	while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9' && scale > 1) {
			scale /= 10;
			frac += (*s++ - '0') * scale;
		}
	}
	if (*s != 0) usage();
	if (v < 10000) v = v * 1000000 + frac;	/* it was MHz */
	if (v < 280000000 || v > 960000000) {
		fprintf(stderr, "subghz: %lu Hz: the CC1101 tunes 280-960 MHz\n",
			v);
		exit(1);
	}
	return v;
}

/* FREQ word = f * 2^16 / fxosc, by long division (no 64-bit maths). */
static unsigned long freq_word(unsigned long f)
{
	unsigned long q = f / XOSC, r = f % XOSC;
	int i;

	for (i = 0; i < 16; i++) {
		r <<= 1;
		q <<= 1;
		if (r >= XOSC) {
			r -= XOSC;
			q |= 1;
		}
	}
	if (2 * r >= XOSC) q++;
	return q;
}

static unsigned long word_freq(unsigned long w)	/* back to Hz */
{
	unsigned long lo = w & 0xFFFF;

	/* fxosc / 2^16 = 396 + 47744 / 2^16 exactly */
	return (w >> 16) * XOSC + lo * 396 + (lo * 47744 >> 16);
}

static void t_freq(unsigned long w)
{
	unsigned char b[4];

	b[0] = FREQ2 | BURST;
	b[1] = w >> 16;
	b[2] = w >> 8;
	b[3] = w;
	t_add(b, 4);
}

static void show_mhz(unsigned long f)
{
	printf("%lu.%06lu", f / 1000000, f % 1000000);
}

/* RSSI register to dBm, in halves: (r - 256 if negative) / 2 - 74. */
static void show_dbm(int r)
{
	int h = (r >= 128 ? r - 256 : r) - 148;		/* half dB */

	printf("%s%d.%d", h < 0 ? "-" : "", (h < 0 ? -h : h) / 2,
		(h < 0 ? -h : h) % 2 ? 5 : 0);
}

static void rx_setup(void)
{
	unsigned i;

	t_begin();
	t_1(SRES);
	t_wait(100);
	for (i = 0; i < sizeof(ook650); i += 2) t_reg(ook650[i], ook650[i+1]);
	/* For measuring: a high data rate (~250 kBaud) and the shortest AGC
	 * wait; at the preset's 3.8 kBaud the gain takes ~30 ms to settle
	 * after entering RX, here ~0.3 ms (measured on the device). */
	t_reg(MDMCFG4, 0x1D);
	t_reg(MDMCFG3, 0x3B);
	t_reg(AGCCTRL0, 0x81);
	t_do();
}

static void sleep_radio(void)
{
	t_begin();
	t_1(SIDLE);
	t_1(SPWD);
	t_do();
}

static void delay_ms(unsigned ms)
{
	unsigned k;

	while (ms > 0) {
		k = ms > 250 ? 250 : ms;
		t_begin();
		t_wait(k * 1000);
		t_do();
		ms -= k;
	}
}

static int info(void)
{
	t_begin();
	t_1(SRES);
	t_wait(100);
	t_status(PARTNUM);
	t_status(VERSION);
	t_1(SPWD);
	t_do();
	/* answers: SRES status, PARTNUM (status, value), VERSION (...) */
	printf("CC1101: part number %d, version %d%s\n", ans[2], ans[4],
		ans[2] == 0 && (ans[4] == 0x14 || ans[4] == 0x04)
		? "" : " (not a CC1101?)");
	return 0;
}

static int rssi(unsigned long f, long n, unsigned ms)
{
	long k;

	rx_setup();
	t_begin();
	t_freq(freq_word(f));
	t_1(SRX);
	t_wait(1000);
	t_do();
	for (k = 0; n == 0 || k < n; k++) {
		if (k > 0) delay_ms(ms);
		t_begin();
		t_status(RSSI);
		t_do();
		show_mhz(f);
		printf(" ");
		show_dbm(ans[1]);
		printf("\n");
		fflush(stdout);
	}
	sleep_radio();
	return 0;
}

#define MAXCH	512

static unsigned char cal[MAXCH][3];
static unsigned settle = 400;		/* us in RX before reading RSSI */		/* FSCAL3, 2, 1 per channel */

static int scan(unsigned long from, unsigned long to, unsigned long step,
		int passes)
{
	unsigned long w0, w, ws, f;
	int nch, c, p, i, k, batch;
	unsigned char b[4];

	w0 = freq_word(from);
	ws = freq_word(step);
	if (ws == 0 || to < from) usage();
	nch = (freq_word(to) - w0) / ws + 1;
	if (nch > MAXCH) {
		fprintf(stderr, "subghz: at most %d channels\n", MAXCH);
		return 1;
	}
	rx_setup();

	/* Calibrate each channel once, keep FSCAL3..1. */
	for (c = 0; c < nch; c++) {
		t_begin();
		t_1(SIDLE);
		t_freq(w0 + c * ws);
		t_1(SCAL);
		t_wait(800);
		b[0] = FSCAL3 | RD | BURST;
		b[1] = b[2] = b[3] = 0;
		t_add(b, 4);
		t_do();
		/* answers: SIDLE 1, freq 4, SCAL 1, then status + 3 */
		memcpy(cal[c], ans + 7, 3);
	}
	t_begin();
	t_reg(MCSM0, 0x08);		/* no calibration on IDLE -> RX */
	t_do();

	for (p = 0; p < passes; p++) {
		for (c = 0; c < nch; c += batch) {
			/* several channels per write: 4 + 4 + 1 + wait + 2 */
			batch = nch - c < 12 ? nch - c : 12;
			t_begin();
			for (i = 0; i < batch; i++) {
				w = w0 + (c + i) * ws;
				t_1(SIDLE);
				t_freq(w);
				b[0] = FSCAL3 | BURST;
				memcpy(b + 1, cal[c + i], 3);
				t_add(b, 4);
				t_1(SRX);
				t_wait(settle);
				t_status(RSSI);
			}
			t_do();
			for (i = 0; i < batch; i++) {
				/* per channel: 1 + 4 + 4 + 1 + 2 answers */
				k = i * 12 + 11;
				f = word_freq(w0 + (c + i) * ws);
				show_mhz(f);
				printf(" ");
				show_dbm(ans[k]);
				printf("\n");
			}
		}
		if (passes > 1) printf("\n");
		fflush(stdout);
	}
	sleep_radio();
	return 0;
}

static int spi(int argc, char **argv)
{
	unsigned char b[64];
	int i, n = 0, k, got;

	t_begin();
	for (i = 0; i <= argc; i++) {
		if (i == argc || strcmp(argv[i], "/") == 0) {
			if (n > 0) t_add(b, n);
			n = 0;
		} else if (strcmp(argv[i], "w") == 0 && i + 1 < argc) {
			cmd[nc++] = 0;
			cmd[nc++] = atoi(argv[++i]);
		} else if (n < 64) {
			b[n++] = strtoul(argv[i], NULL, 16);
		}
	}
	got = t_do();
	for (k = 0; k < got; k++) printf("%02x%c", ans[k],
					k + 1 == got ? '\n' : ' ');
	return 0;
}

int main(int argc, char **argv)
{
	long n = 1;
	unsigned ms = 100;
	int i;

	if (argc < 2) usage();
	if ((fd = open("/dev/subghz", O_RDWR)) < 0) {
		perror("subghz: /dev/subghz");
		return 1;
	}
	if (strcmp(argv[1], "info") == 0 && argc == 2) return info();
	if (strcmp(argv[1], "spi") == 0 && argc >= 3)
		return spi(argc - 2, argv + 2);
	if (strcmp(argv[1], "rssi") == 0 && argc >= 3) {
		for (i = 3; i < argc; i++) {
			if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
				n = atol(argv[++i]);
			else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
				ms = atoi(argv[++i]);
			else usage();
		}
		return rssi(freq_arg(argv[2]), n, ms);
	}
	if (strcmp(argv[1], "scan") == 0 && argc >= 5) {
		for (i = 5; i < argc; i++) {
			if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
				n = atol(argv[++i]);
			else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
				settle = atoi(argv[++i]);
			else usage();
		}
		if (settle > 2550) settle = 2550;
		return scan(freq_arg(argv[2]), freq_arg(argv[3]),
			    strtoul(argv[4], NULL, 10) * 1000, (int) n);
	}
	usage();
	return 1;
}
