/* gmodem - move data between two Flippers (Galtzagorriak MINIX, spec
 * section P; libmodem in src/lib/modem).  Not MINIX's own modem(1), which
 * switches a serial line between dial-in and dial-out.
 *
 * Usage:	gmodem send -m MEDIUM [-p PRESET] [-n ROUNDS] [-z] [FILE]
 *		gmodem recv -m MEDIUM [-p PRESET] [-o FILE] [-t SEC]
 *		gmodem chat -m MEDIUM [-p PRESET] [-f MHZ]
 *		gmodem info [-m MEDIUM] [-p PRESET] [-f MHZ]
 *
 * MEDIUM is rfid, nfc, subghz or ble.  Today rfid works (one way, the
 * sender is the tag, the receiver the reader: hold the two Flippers back to
 * back); subghz has its presets and legal limits in `gmodem info` and sends
 * once /dev/subghz can transmit (spec RP.8, T26b); nfc and ble follow their
 * drivers (Phases 13 and 15).
 *
 * RFID (spec P.5): the sender plays the file as a carousel of frames of 24
 * bytes, two frames at a time through RFIDIOCEMU in the line code P4, over
 * and over (-n ROUNDS, default until Ctrl-C); the receiver reads the
 * reader's captures, keeps every frame it has not got yet, writes it at its
 * place in the output and ends when all are in (or after -t SEC without a
 * new frame, default 30).  -p fast (Q = 16 carrier periods, about 4.4 kbit/s
 * on the antenna) or robust (Q = 32).  A file or a pipe of up to 1 KB to
 * send; the output must be a file (-o, or stdout redirected to one) unless
 * it is at most 1 KB.
 *
 * -z compresses what is sent (heatshrink LZSS, spec P.3a; often half the
 * frames for text); the receiver sees it in the frames and unpacks by
 * itself.  A pipe of any size can then be sent (through a scratch file).
 *
 * Exit status: 0 everything arrived with a good CRC, 1 the transfer failed
 * or timed out, 2 not possible here (medium not there yet, usage).
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <minix/rfid.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mdmz.h"

#define SMALL	1024		/* a pipe in or out is buffered up to this */

static unsigned long pat[512];	/* the tag's segments, or the reader's edges */
static unsigned char map[MDM_CAR_MAX / 8];

static char *medium, *preset, *outname;
static unsigned long freq;	/* Hz, 0: the region's default */
static int rounds, idle = 30, zflag;

/* A pipe's data (in or out) and the -z state (spec P.3a: the encoder or
 * the decoder) are never needed at once: one union keeps .bss under 4 KB. */
static union {
	unsigned char small[SMALL];
	struct mdm_zsrc src;
	struct mdm_zsink sink;
} z;

/* A scratch file in /tmp, gone from the directory at once: it lives only
 * while it is open (also after Ctrl-C). */
static int scratch(void)
{
	char name[24];
	int fd;

	strcpy(name, "/tmp/gmzXXXXXX");
	if (mktemp(name) == NULL
	    || (fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600)) < 0) {
		perror("gmodem: /tmp");
		return -1;
	}
	unlink(name);
	return fd;
}

static int fd_get(void *u, unsigned char *b, int max)
{
	return read(*(int *) u, b, (size_t) max);
}

static int fd_put(void *u, const unsigned char *b, int n)
{
	return write(*(int *) u, b, (size_t) n) == n ? 0 : -1;
}

static void usage(void)
{
	fprintf(stderr,
"usage: gmodem send -m MEDIUM [-p PRESET] [-n ROUNDS] [-z] [FILE]\n"
"       gmodem recv -m MEDIUM [-p PRESET] [-o FILE] [-t SEC]\n"
"       gmodem chat -m MEDIUM [-p PRESET] [-f MHZ]\n"
"       gmodem info [-m MEDIUM] [-p PRESET] [-f MHZ]\n"
"MEDIUM: rfid (works), subghz, nfc, ble\n");
	exit(2);
}

static int notyet(const char *what, const char *needs)
{
	fprintf(stderr, "gmodem: %s: not implemented yet (needs %s)\n", what,
		needs);
	return 2;
}

static unsigned long mhz(const char *s)
{
	/* "433.92" -> 433920000, without the FPU */
	unsigned long v = 0, scale = 1000000UL;

	while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long) (*s++ - '0');
	v *= 1000000UL;
	if (*s == '.')
		for (s++; *s >= '0' && *s <= '9' && scale > 1; s++) {
			scale /= 10;
			v += (unsigned long) (*s - '0') * scale;
		}
	return *s ? 0 : v;
}

static void pmhz(unsigned long hz)
{
	printf("%lu.%03lu MHz", hz / 1000000UL, hz / 1000UL % 1000UL);
}

/* ---- RFID (spec P.5) ---- */

static int rfid_q(void)
{
	if (preset == NULL || strcmp(preset, "fast") == 0) return MDM_P4_FAST;
	if (strcmp(preset, "robust") == 0) return MDM_P4_ROBUST;
	fprintf(stderr, "gmodem: rfid presets: fast, robust\n");
	exit(2);
}

static int rfid_send(char *name)
{
	unsigned char fr[MDM_OVH + MDM_RFID_MTU], chunk[MDM_RFID_MTU];
	struct rfid_emu e;
	long size, nf, k, j;
	int in = 0, seek, fd, q = rfid_q(), ns, n, len, r, dwell;
	unsigned char sid = (unsigned char) (time(NULL) ^ getpid() * 37);

	if (name != NULL && (in = open(name, O_RDONLY)) < 0) {
		perror(name);
		return 1;
	}
	if (name == NULL && isatty(0)) {
		fprintf(stderr, "gmodem: give a FILE, or send through a pipe\n");
		return 2;
	}
	if (zflag) {		/* compress it into a scratch file first */
		int t, src = in;

		if ((t = scratch()) < 0) return 1;
		mdm_zsrc_init(&z.src, fd_get, &src);
		while ((n = mdm_zget(&z.src, chunk, (int) sizeof(chunk))) > 0)
			if (write(t, chunk, (size_t) n) != n) n = -1;
		if (n < 0) {
			perror("gmodem: compress");
			return 1;
		}
		if (in != 0) close(in);
		in = t;
	}
	size = (long) lseek(in, 0L, SEEK_END);	/* off_t is unsigned here */
	seek = lseek(in, 0L, SEEK_SET) != (off_t) -1;
	if (!seek) {			/* a pipe: up to 1 KB in memory */
		for (size = 0; size < SMALL && (n = read(in, z.small + size,
				(size_t) (SMALL - size))) > 0; size += n) ;
		if (size == SMALL && read(in, chunk, 1) > 0) {
			fprintf(stderr, "gmodem: more than %d bytes from a pipe: "
				"give a file\n", SMALL);
			return 2;
		}
	}
	nf = size == 0 ? 1 : (size + MDM_RFID_MTU - 1) / MDM_RFID_MTU;
	if (nf > MDM_CAR_MAX) {
		fprintf(stderr, "gmodem: at most %d bytes over RFID\n",
			MDM_CAR_MAX * MDM_RFID_MTU);
		return 2;
	}
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("gmodem: /dev/rfid");
		return 1;
	}
	dwell = q == MDM_P4_FAST ? 600 : 1200;
	fprintf(stderr, "gmodem: %ld bytes, %ld frames; hold the reader to the "
		"back, Ctrl-C to stop\n", size, nf);
	for (r = 0; rounds == 0 || r < rounds; r++) {
		for (k = 0; k < nf; k += 2) {
			ns = 0;
			for (j = k; j < k + 2 && j < nf; j++) {
				len = j == nf - 1 ? (int) (size - j * MDM_RFID_MTU)
					: MDM_RFID_MTU;
				if (seek) {
					if (lseek(in, j * MDM_RFID_MTU, SEEK_SET) == (off_t) -1
					    || read(in, chunk, (size_t) len) != len) {
						perror("gmodem: read");
						return 1;
					}
				} else memcpy(chunk, z.small + j * MDM_RFID_MTU, (size_t) len);
				n = mdm_car_pack(sid, (int) j, (j == nf - 1 ? MDM_F_LAST : 0)
					| (zflag ? MDM_F_Z : 0), chunk, len,
					fr, (int) sizeof(fr));
				ns += mdm_p4_encode(fr, n, q, pat + ns, pat + 256 + ns,
					256 - ns);
			}
			e.dur = pat;
			e.pulse = pat + 256;
			e.n = (unsigned) ns;
			if (ioctl(fd, RFIDIOCEMU, &e) < 0) {
				perror("gmodem: rfid emulate");
				return 1;
			}
			(void) poll((struct pollfd *) NULL, 0, dwell);
			e.n = 0;
			(void) ioctl(fd, RFIDIOCEMU, &e);
		}
		fprintf(stderr, "\rgmodem: round %d", r + 1);
	}
	fprintf(stderr, "\n");
	close(fd);
	return 0;
}

static struct mdm_car car;
static int out = 1, out_seek, failed, zfd = -1;	/* zfd: -z frames */

static void rfid_frame(void *u, const unsigned char *b, int n)
{
	struct mdm_frame f;
	long at;

	(void) u;
	if (mdm_unpack(b, n, MDM_RFID_MTU, &f) < 0
	    || mdm_car_take(&car, &f, MDM_RFID_MTU) != 1) return;
	at = (long) (f.a | f.b << 8) * MDM_RFID_MTU;
	if (car.z) {		/* compressed: into a scratch file, unpacked at the end */
		if (zfd < 0 && (zfd = scratch()) < 0) { failed = 1; return; }
		if (lseek(zfd, at, SEEK_SET) == (off_t) -1
		    || write(zfd, f.pay, f.len) != (int) f.len) {
			perror("gmodem: /tmp");
			failed = 1;
		}
		return;
	}
	if (!out_seek) {
		if (at + f.len > SMALL) {
			fprintf(stderr, "\ngmodem: more than %d bytes to a pipe: "
				"use -o FILE\n", SMALL);
			failed = 1;
		} else memcpy(z.small + at, f.pay, f.len);
		return;
	}
	if (lseek(out, at, SEEK_SET) == (off_t) -1
	    || write(out, f.pay, f.len) != (int) f.len) {
		perror("gmodem: write");
		failed = 1;
	}
}

static int rfid_recv(void)
{
	int fd, q = rfid_q(), n, had = 0;
	time_t last, start, shown = 0;

	if (outname != NULL
	    && (out = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
		perror(outname);
		return 1;
	}
	out_seek = lseek(out, 0L, SEEK_CUR) != (off_t) -1;
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("gmodem: /dev/rfid");
		return 1;
	}
	mdm_car_init(&car, map);
	start = last = time(NULL);
	fprintf(stderr, "gmodem: reading; hold the sender to the back\n");
	while (!mdm_car_done(&car) && !failed) {
		if ((n = read(fd, pat, sizeof(pat))) < 0) {
			perror("gmodem: /dev/rfid");
			return 1;
		}
		mdm_p4_decode(pat, n / 4, q, MDM_RFID_MTU, rfid_frame, NULL);
		if (car.have > had) {
			had = car.have;
			last = time(NULL);
		}
		if (time(NULL) != shown) {	/* at most once a second */
			shown = time(NULL);
			if (car.total >= 0)
				fprintf(stderr, "\rgmodem: %d/%d frames", car.have,
					car.total);
			else fprintf(stderr, "\rgmodem: %d frames", car.have);
		}
		if (time(NULL) - last > idle) {
			if (car.total >= 0)
				fprintf(stderr, "\ngmodem: nothing new for %d s: "
					"%d of %d frames\n", idle, car.have, car.total);
			else fprintf(stderr, "\ngmodem: nothing new for %d s: "
				"%d frames, the last one not seen\n", idle, car.have);
			return 1;
		}
	}
	close(fd);
	if (failed) return 1;
	if (car.z) {
		long k;
		int r;

		mdm_zsink_init(&z.sink, fd_put, &out);
		if (lseek(zfd, 0L, SEEK_SET) == (off_t) -1) failed = 1;
		/* pat holds no edges any more: the compressed bytes go
		 * through it (z.small is the decoder's) */
		for (k = 0; !failed && k < car.size; k += r) {
			r = read(zfd, (char *) pat, (size_t) (car.size - k
				< (long) sizeof(pat) ? car.size - k : (long) sizeof(pat)));
			if (r <= 0 || mdm_zput(&z.sink, (unsigned char *) pat, r) < 0)
				failed = 1;
		}
		if (failed || mdm_zend(&z.sink) < 0) {
			fprintf(stderr, "\ngmodem: unpacking failed\n");
			return 1;
		}
	} else if (!out_seek && write(out, z.small, (size_t) car.size) != car.size) {
		perror("gmodem: write");
		return 1;
	}
	fprintf(stderr, "\rgmodem: %ld bytes, %d frames, %ld s\n", car.size,
		car.total, (long) (time(NULL) - start));	/* timecheck: ok (a duration) */
	return 0;
}

/* ---- Sub-GHz (spec P.4) ---- */

static void region(char *r, int max)
{
	/* /etc/flipper/region: the region code (I.4); unknown without it */
	int fd, n, i;

	strcpy(r, "unknown");
	if ((fd = open("/etc/flipper/region", O_RDONLY)) < 0) return;
	n = read(fd, r, (size_t) (max - 1));
	close(fd);
	if (n <= 0) { strcpy(r, "unknown"); return; }
	for (i = 0; i < n && r[i] > ' '; i++) ;
	r[i] = 0;
}

static void info_subghz(void)
{
	const struct mdm_preset *p;
	const struct mdm_band *b;
	struct mdm_cc1101 c;
	char reg[8];
	unsigned long f;
	long air, bps;
	int rc;

	region(reg, (int) sizeof(reg));
	f = freq ? freq : strcmp(reg, "US") == 0 ? 915000000UL : 433920000UL;
	printf("subghz: region %s, ", reg);
	pmhz(f);
	printf("; sending needs /dev/subghz transmit (spec RP.8, T26b)\n");
	printf("  preset   rate      deviation  filter     occupies   frame  "
		"peak       here\n");
	for (p = mdm_presets; p->name[0]; p++) {
		if (preset != NULL && strcmp(preset, p->name) != 0) continue;
		if (mdm_cc1101_regs(f, &p->phy, &c) < 0) {
			printf("  %-7s  (the CC1101 cannot do this here)\n", p->name);
			continue;
		}
		air = mdm_sg_airtime(c.baud, MDM_OVH + 54);
		bps = 54L * 1000L / (air + 25);	/* + turnaround, W = 4 */
		printf("  %-7s  %6lu Bd  %6lu Hz  %6lu Hz  %6lu Hz  %3ld ms  "
			"%5ld B/s  ", p->name, c.baud, c.dev, c.bw, mdm_occupied(&c),
			air, bps);
		rc = mdm_sg_check(reg, &c, &b);
		if (rc == MDM_SG_OK)
			printf("ok, %d dBm, %s\n", mdm_sg_dbm(b), b->note);
		else if (rc == MDM_SG_BAND)
			printf("not allowed: leaves the region's bands\n");
		else printf("not allowed: no data bands for this region\n");
	}
	printf("  bands for %s:", reg);
	for (b = mdm_bands; b->region[0]; b++)
		if (strcmp(b->region, reg) == 0) {
			printf("\n    ");
			pmhz(b->lo);
			printf(" - ");
			pmhz(b->hi);
			printf("  %s", b->note);
		}
	if (strcmp(reg, "EU") != 0 && strcmp(reg, "US") != 0)
		printf(" none; root writes EU or US to /etc/flipper/region");
	printf("\n  nothing sent over Sub-GHz is encrypted\n");
}

static void info_rfid(void)
{
	printf("rfid: one way, tag (send) -> reader (recv), backs together, "
		"1-3 cm\n");
	printf("  fast    Q=16  %ld ms a frame of %d bytes, %ld bit/s on the "
		"antenna\n", mdm_p4_us(MDM_OVH + MDM_RFID_MTU, MDM_P4_FAST) / 1000,
		MDM_RFID_MTU, (MDM_OVH + MDM_RFID_MTU) * 8000000L
		/ mdm_p4_us(MDM_OVH + MDM_RFID_MTU, MDM_P4_FAST));
	printf("  robust  Q=32  %ld ms a frame of %d bytes, %ld bit/s on the "
		"antenna\n", mdm_p4_us(MDM_OVH + MDM_RFID_MTU, MDM_P4_ROBUST) / 1000,
		MDM_RFID_MTU, (MDM_OVH + MDM_RFID_MTU) * 8000000L
		/ mdm_p4_us(MDM_OVH + MDM_RFID_MTU, MDM_P4_ROBUST));
	printf("  a carousel: the sender repeats, the reader keeps what is new;"
		" not encrypted\n");
}

static int info(void)
{
	int all = medium == NULL;

	if (all || strcmp(medium, "rfid") == 0) info_rfid();
	if (all || strcmp(medium, "subghz") == 0) info_subghz();
	if (all || strcmp(medium, "nfc") == 0)
		printf("nfc: not implemented yet (needs /dev/nfc, Phase 13)\n");
	if (all || strcmp(medium, "ble") == 0)
		printf("ble: not implemented yet (needs the BLE stack, Phase 15)\n");
	return 0;
}

int main(int argc, char **argv)
{
	char *verb, *file = NULL;
	int i;

	if (argc < 2) usage();
	verb = argv[1];
	for (i = 2; i < argc; i++) {
		if (argv[i][0] != '-' || argv[i][1] == 0) {
			if (file != NULL) usage();
			file = argv[i];
			continue;
		}
		if (argv[i][1] == 'z' && argv[i][2] == 0) {
			zflag = 1;
			continue;
		}
		if (argv[i][2] != 0 || i + 1 >= argc) usage();
		switch (argv[i][1]) {
		case 'm': medium = argv[++i]; break;
		case 'p': preset = argv[++i]; break;
		case 'o': outname = argv[++i]; break;
		case 'n': rounds = atoi(argv[++i]); break;
		case 't': idle = atoi(argv[++i]); break;
		case 'f':
			if ((freq = mhz(argv[++i])) == 0) usage();
			break;
		default: usage();
		}
	}
	if (strcmp(verb, "info") == 0) return info();
	if (medium == NULL) usage();
	if (strcmp(medium, "nfc") == 0)
		return notyet("nfc", "/dev/nfc, Phase 13");
	if (strcmp(medium, "ble") == 0)
		return notyet("ble", "the BLE stack on CPU2, Phase 15");
	if (strcmp(medium, "subghz") == 0)
		return notyet("subghz", "/dev/subghz transmit, spec RP.8, T26b");
	if (strcmp(medium, "rfid") != 0) usage();
	if (strcmp(verb, "send") == 0) return rfid_send(file);
	if (strcmp(verb, "recv") == 0 && file == NULL) return rfid_recv();
	if (strcmp(verb, "chat") == 0)
		return notyet("rfid chat", "the reverse channel, spec RP.5c, T26b");
	usage();
	return 2;
}
