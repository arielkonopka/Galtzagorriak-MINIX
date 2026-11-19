/* subghz - Sub-GHz remotes: receive, decode, send (Galtzagorriak MINIX,
 * spec Phase 9 "Sub-GHz CLI", R8.2, devguide 6.10; issue #302)
 *
 * Usage:	subghz rx FREQ [-v]	listen at FREQ Hz; one line per decoded
 *					packet: "Protocol 24bit Key:6AB234"
 *					(-v: also the protocol's own text, as
 *					stock's CLI prints it); Ctrl-C ends
 *		subghz decode FILE [-v]	the same for a .sub RAW capture
 *		subghz tx KEY FREQ TE REPEAT
 *					send KEY (hex, 24 bits) as Princeton
 *					with TE us, REPEAT times (stock's
 *					`subghz tx`)
 *		subghz tx_from_file FILE [REPEAT]
 *					send a .sub file (a key file through
 *					its protocol's encoder, or RAW) at
 *					its Frequency; a rolling code's new
 *					counter is written back to the file,
 *					as stock does
 *		subghz chat FREQ	packets as text lines, both ways
 *					(stock's `subghz chat`)
 *		subghz region [XX]	the radio region (R8.2): show it, or
 *					set it (once per boot, /etc/rc)
 *		subghz list		the protocols, what each can do
 *		subghz heap		the heap receiving takes (bytes)
 *
 * The protocols are the Unleashed firmware's (libsubghz, src/lib/subghz):
 * every decoder and encoder.  KeeLoq and the other rolling codes need the
 * manufacturer keys to show the button and counter: the user's own plain
 * keystore in /home/flipper/subghz/assets (spec D.3: never shipped).
 * Receiving reads /dev/subghzraw (the CC1101's demodulated levels,
 * timed); sending writes frames there, and the kernel checks every one
 * against the region's bands and duty cycle (R8.2).  On the Flipper the
 * transmitter is not written yet: send says so (ENOTSUP).
 *
 * RAM: the decoders' state (a few KB, malloc'd once), one frame buffer of
 * FRAME words; a file is read a line at a time.
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <minix/subghz.h>
#include <unistd.h>
#include <sgfile.h>
#include <subghz/registry.h>
#include <lib/flipper_format/flipper_format.h>

#define FRAME 512		/* durations per read or write */
#define GAP_US 20000		/* a silence that ends a frame */

static uint32_t frame[FRAME];
static int verbose;
static volatile int stop;
static long packets;

static void onsig(int sig)
{
	(void) sig;
	stop = 1;
}

static int usage(void)
{
	fprintf(stderr,
		"Usage: subghz rx FREQ [-v]\n"
		"       subghz decode FILE [-v]\n"
		"       subghz tx KEY FREQ TE REPEAT\n"
		"       subghz tx_from_file FILE [REPEAT]\n"
		"       subghz chat FREQ\n"
		"       subghz region [CC]\n"
		"       subghz list\n"
		"       subghz heap\n");
	return 2;
}

static int bad(const char *what, const char *why)
{
	fprintf(stderr, "subghz: %s: %s\n", what, why);
	return 1;
}

static const char *errtext(int e)
{
	switch (e) {
	case EPERM: return "refused: outside the region's bands, or no "
		"region set (subghz region)";
	case EAGAIN: return "refused: the band's duty cycle for this hour is "
		"used up";
	case EIO: return "the radio is not tuned";
	case ENOTSUP: return "no transmitter on this device yet";
	case EBUSY: return "the radio is busy";
	default: return strerror(e);
	}
}

static int parse_freq(const char *s, unsigned long *hz)
{
	char *e;

	*hz = strtoul(s, &e, 10);
	return *e == 0 && *hz >= 300000000UL && *hz <= 928000000UL;
}

/* Listen at hz: write it to /dev/subghz.  0, or -1 with a message. */
static int tune(unsigned long hz)
{
	char t[16];
	int fd = open("/dev/subghz", O_WRONLY), n;

	if (fd < 0) return bad("/dev/subghz", strerror(errno)), -1;
	n = snprintf(t, sizeof(t), "%lu", hz);
	if (write(fd, t, n) != n) {
		bad("cannot tune to that frequency", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static void untune(void)
{
	int fd = open("/dev/subghz", O_WRONLY);

	if (fd >= 0) {
		(void) write(fd, "0", 1);
		close(fd);
	}
}

/* ---- receive ---- */

static void on_packet(SubGhzReceiver *r, SubGhzProtocolDecoderBase *d,
	void *ctx)
{
	FuriString *s = furi_string_alloc();

	(void) ctx;
	sg_describe(d, s);
	printf("%s\n", furi_string_get_cstr(s));
	if (verbose) {
		furi_string_reset(s);
		subghz_protocol_decoder_base_get_string(d, s);
		printf("%s\n", furi_string_get_cstr(s));
	}
	fflush(stdout);
	furi_string_free(s);
	packets++;
	subghz_receiver_reset(r);
}

static SubGhzReceiver *receiver(SubGhzEnvironment **env)
{
	SubGhzReceiver *rx;

	subghz_heap_room = 0;		/* no Bin RAW (10 KB of buffers) */
	*env = sg_env_alloc(NULL, NULL);
	rx = subghz_receiver_alloc_init(*env);
	subghz_receiver_set_filter(rx, SubGhzProtocolFlag_Decodable);
	subghz_receiver_set_rx_callback(rx, on_packet, NULL);
	return rx;
}

static int cmd_rx(int argc, char **argv)
{
	SubGhzEnvironment *env;
	SubGhzReceiver *rx;
	unsigned long hz;
	int fd, n, i;

	if (argc < 3 || !parse_freq(argv[2], &hz)) return usage();
	verbose = argc > 3 && strcmp(argv[3], "-v") == 0;
	if (tune(hz) < 0) return 1;
	if ((fd = open("/dev/subghzraw", O_RDONLY)) < 0) {
		untune();
		return bad("/dev/subghzraw", strerror(errno));
	}
	rx = receiver(&env);
	signal(SIGINT, onsig);
	signal(SIGTERM, onsig);
	printf("Listening at %lu. Press Ctrl+C to stop.\n", hz);
	fflush(stdout);
	while (!stop) {
		n = read(fd, (char *) frame, sizeof(frame));
		if (n < 0) {
			if (errno == EINTR) break;
			bad("/dev/subghzraw", strerror(errno));
			break;
		}
		for (i = 0; i < n / 4; i++)
			subghz_receiver_decode(rx, !(i & 1), frame[i]);
		/* the frame ended in a silence; a full one did not (the
		 * kernel ends it there): its packet goes on in the next */
		if (n < (int) sizeof(frame))
			subghz_receiver_decode(rx, false, GAP_US);
	}
	close(fd);
	untune();
	printf("Packets received %ld\n", packets);
	subghz_receiver_free(rx);
	sg_env_free(env);
	return 0;
}

static int cmd_decode(int argc, char **argv)
{
	SubGhzEnvironment *env;
	SubGhzReceiver *rx;
	SgFile s;
	const char *why;
	bool level;
	uint32_t us;
	int r;

	if (argc < 3) return usage();
	verbose = argc > 3 && strcmp(argv[3], "-v") == 0;
	if (sg_open(&s, argv[2], &why) < 0) return bad(argv[2], why);
	if (!s.raw) {
		sg_close(&s);
		return bad(argv[2], "not a RAW capture (a key file: "
			"subghz tx_from_file sends it)");
	}
	rx = receiver(&env);
	while ((r = sg_raw_next(&s, &level, &us)) == 1)
		subghz_receiver_decode(rx, level, us);
	subghz_receiver_decode(rx, false, GAP_US);
	if (r < 0) fprintf(stderr, "subghz: %s: bad RAW_Data near line %lu\n",
		argv[2], s.line);
	sg_close(&s);
	subghz_receiver_free(rx);
	sg_env_free(env);
	return r < 0 ? 1 : 0;
}

/* ---- send ---- */

static int txfd = -1;
static int nframe;
static bool frame_level;		/* the level of frame[nframe - 1] */
static int tx_errno;

static int flush_frame(void)
{
	int n = nframe * 4;

	if (nframe == 0) return 0;
	nframe = 0;
	if (write(txfd, (char *) frame, n) != n) {
		tx_errno = errno;
		return -1;
	}
	return 0;
}

/* One level: merged with the one before when the same (as stock's async
 * TX middleware), a frame starts with the carrier on, a full one goes out
 * after a silence (so a write ends between bursts when it can). */
static int put_level(bool level, uint32_t us)
{
	if (us == 0) return 0;
	if (us > SG_DURATION_MAX) us = SG_DURATION_MAX;
	if (nframe == 0 && !level) return 0;	/* off before: nothing */
	if (nframe > 0 && level == frame_level) {
		uint32_t t = frame[nframe - 1] + us;
		frame[nframe - 1] = t > SG_DURATION_MAX || t < us
			? SG_DURATION_MAX : t;
		return 0;
	}
	if (nframe == FRAME) {
		if (flush_frame() < 0) return -1;
		if (!level) return 0;
	}
	frame[nframe++] = us;
	frame_level = level;
	if (!level && us >= GAP_US && nframe > FRAME / 2) return flush_frame();
	return 0;
}

static int tx_open(unsigned long hz)
{
	if (tune(hz) < 0) return -1;
	if ((txfd = open("/dev/subghzraw", O_WRONLY)) < 0) {
		bad("/dev/subghzraw", strerror(errno));
		untune();
		return -1;
	}
	nframe = 0;
	tx_errno = 0;
	return 0;
}

static int tx_close(const char *what)
{
	int r = flush_frame();

	close(txfd);
	txfd = -1;
	untune();
	if (r < 0 || tx_errno != 0) return bad(what, errtext(tx_errno));
	return 0;
}

/* A transmitter's whole output, REPEAT is the encoder's own. */
static int tx_encoder(SubGhzTransmitter *t)
{
	long guard = 0;

	for (;;) {
		LevelDuration ld = subghz_transmitter_yield(t);

		if (level_duration_is_reset(ld) || stop) break;
		if (++guard > 4000000L) break;	/* an endless encoder */
		if (put_level(level_duration_get_level(ld),
			level_duration_get_duration(ld)) < 0) return -1;
	}
	return 0;
}

static int send_text(SubGhzEnvironment *env, FlipperFormat *ff, const char *what)
{
	const char *why;
	SubGhzTransmitter *t = sg_transmitter(env, ff, &why);
	int r;

	if (t == NULL) return bad(what, why);
	r = tx_encoder(t);
	subghz_transmitter_free(t);
	return r;
}

static int cmd_tx(int argc, char **argv)
{
	SubGhzEnvironment *env;
	FlipperFormat *ff;
	unsigned long key, hz, te, repeat;
	char text[320], *e;
	int n, r;

	if (argc < 6) return usage();
	key = strtoul(argv[2], &e, 16);
	if (*e != 0 || key > 0xFFFFFFUL) return bad(argv[2], "KEY: 24-bit hex");
	if (!parse_freq(argv[3], &hz)) return bad(argv[3], "FREQ: Hz, 300-928 MHz");
	te = strtoul(argv[4], &e, 10);
	if (*e != 0 || te < 50 || te > 5000) return bad(argv[4], "TE: 50-5000 us");
	repeat = strtoul(argv[5], &e, 10);
	if (*e != 0 || repeat < 1 || repeat > 1000) return bad(argv[5], "REPEAT: 1-1000");
	n = snprintf(text, sizeof(text),
		"Filetype: Flipper SubGhz Key File\nVersion: 1\n"
		"Frequency: %lu\nPreset: FuriHalSubGhzPresetOok650Async\n"
		"Protocol: Princeton\nBit: 24\n"
		"Key: 00 00 00 00 00 %02lX %02lX %02lX\nTE: %lu\nRepeat: %lu\n",
		hz, key >> 16 & 0xFF, key >> 8 & 0xFF, key & 0xFF, te, repeat);
	env = sg_env_alloc(NULL, NULL);
	ff = flipper_format_string_alloc();
	flipper_format_set_text(ff, text, (size_t) n);
	signal(SIGINT, onsig);
	if (tx_open(hz) < 0) r = 1;
	else {
		printf("Transmitting at %lu, key %06lX, te %lu, repeat %lu. "
			"Press Ctrl+C to stop\n", hz, key, te, repeat);
		r = send_text(env, ff, "tx");
		r = tx_close("tx") || r;
	}
	flipper_format_free(ff);
	sg_env_free(env);
	return r ? 1 : 0;
}

static int cmd_tx_file(int argc, char **argv)
{
	SubGhzEnvironment *env;
	FlipperFormat *ff;
	SgFile s;
	const char *why;
	unsigned long repeat = 1, i;
	char *e;
	int r = 0;

	if (argc < 3) return usage();
	if (argc > 3) {
		repeat = strtoul(argv[3], &e, 10);
		if (*e != 0 || repeat < 1 || repeat > 1000)
			return bad(argv[3], "REPEAT: 1-1000");
	}
	if (sg_open(&s, argv[2], &why) < 0) return bad(argv[2], why);
	if (s.frequency == 0) {
		sg_close(&s);
		return bad(argv[2], "no Frequency line");
	}
	signal(SIGINT, onsig);
	if (tx_open(s.frequency) < 0) {
		sg_close(&s);
		return 1;
	}
	printf("Transmitting %s at %lu (%s)\n", argv[2], s.frequency, s.protocol);
	fflush(stdout);
	if (s.raw) {
		/* RAW: the values a line at a time, REPEAT times */
		for (i = 0; i < repeat && !stop && r == 0; i++) {
			bool level;
			uint32_t us;
			int k;

			if (i > 0) {
				sg_close(&s);
				if (sg_open(&s, argv[2], &why) < 0) {
					r = bad(argv[2], why);
					break;
				}
			}
			while (!stop && (k = sg_raw_next(&s, &level, &us)) == 1)
				if (put_level(level, us) < 0) {
					r = 1;
					break;
				}
			if (k < 0) r = bad(argv[2], "bad RAW_Data");
		}
		sg_close(&s);
		r = tx_close(argv[2]) || r;
		return r ? 1 : 0;
	}
	sg_close(&s);
	env = sg_env_alloc(NULL, NULL);
	ff = flipper_format_file_alloc(NULL);
	if (!flipper_format_file_open_existing(ff, argv[2])) {
		r = bad(argv[2], "cannot read the key file (over 4 KB?)");
	} else {
		for (i = 0; i < repeat && !stop && r == 0; i++)
			r = send_text(env, ff, argv[2]);
		/* a rolling code stepped its counter: keep it, as stock */
		if (r == 0 && flipper_format_changed(ff)
				&& !flipper_format_save(ff, argv[2]))
			bad(argv[2], "could not write the new counter back");
	}
	r = tx_close(argv[2]) || r;
	flipper_format_free(ff);
	sg_env_free(env);
	return r ? 1 : 0;
}

/* ---- chat ---- */

static int cmd_chat(int argc, char **argv)
{
	unsigned long hz;
	char line[SG_PKT_MAX + 1], pkt[SG_PKT_MAX + 1];
	int fd, n, fl;

	if (argc < 3 || !parse_freq(argv[2], &hz)) return usage();
	if (tune(hz) < 0) return 1;
	if ((fd = open("/dev/subghzpkt", O_RDWR)) < 0) {
		untune();
		return bad("/dev/subghzpkt", strerror(errno));
	}
	signal(SIGINT, onsig);
	signal(SIGTERM, onsig);
	printf("Receiving at frequency %lu Hz\n"
		"Press Ctrl+C to stop\n", hz);
	fflush(stdout);
	fl = fcntl(0, F_GETFL);
	(void) fcntl(0, F_SETFL, fl | O_NONBLOCK);
	while (!stop) {
		/* a line typed: one packet */
		n = read(0, line, SG_PKT_MAX);
		if (n > 0) {
			if (write(fd, line, n) != n) {
				bad("send", errtext(errno));
				if (errno == ENOTSUP) break;
			}
		} else if (n == 0) break;	/* end of input */
		/* a packet heard: printed as it came */
		n = read(fd, pkt, SG_PKT_MAX);
		if (n > 0) {
			pkt[n] = 0;
			printf("%s", pkt);
			if (pkt[n - 1] != '\n') putchar('\n');
			fflush(stdout);
		} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
			bad("receive", errtext(errno));
			break;
		}
		if (n <= 0)	/* poll: 20 times a second */
			(void) poll((struct pollfd *) NULL, 0, 50);
	}
	(void) fcntl(0, F_SETFL, fl);
	close(fd);
	untune();
	return 0;
}

/* ---- region, list ---- */

static int cmd_region(int argc, char **argv)
{
	int fd = open("/dev/subghz", O_RDONLY), v = 0;

	if (fd < 0) return bad("/dev/subghz", strerror(errno));
	if (argc < 3) {
		if (ioctl(fd, SGIOCGREGION, &v) < 0)
			return bad("region", strerror(errno));
		if (v == 0) printf("none (no transmit)\n");
		else printf("%c%c\n", v >> 8 & 0xFF, v & 0xFF);
		close(fd);
		return 0;
	}
	if (strlen(argv[2]) != 2)
		return bad(argv[2], "a country code: PL, US, ... (region list)");
	v = SG_REGION(argv[2][0] & ~0x20, argv[2][1] & ~0x20);
	if (ioctl(fd, SGIOCREGION, &v) < 0) {
		int e = errno;
		close(fd);
		return bad(argv[2], e == EPERM ? "the region is already set "
			"(once per boot)" : e == EINVAL ? "no table for this "
			"country; transmit stays off" : strerror(e));
	}
	close(fd);
	return 0;
}

static int cmd_list(void)
{
	const SubGhzProtocolRegistry *reg = &subghz_protocol_registry;
	size_t i;

	for (i = 0; i < subghz_protocol_registry_count(reg); i++) {
		const SubGhzProtocol *p = subghz_protocol_registry_get_by_index(reg, i);

		printf("%-20s %s%s%s\n", p->name,
			p->decoder ? "decode" : "",
			p->encoder ? (p->decoder ? ", send" : "send") : "",
			p->type == SubGhzProtocolTypeDynamic ? " (rolling code)"
			: p->type == SubGhzProtocolTypeRAW ? "RAW: tx_from_file" : "");
	}
	return 0;
}

/* What receiving takes from the heap: every decoder's state (rx). */
static int cmd_heap(void)
{
	SubGhzEnvironment *env;
	SubGhzReceiver *rx = receiver(&env);

	printf("heap: %lu bytes for rx (the environment and every decoder's "
		"state), frame buffer %lu bytes static\n",
		(unsigned long) subghz_heap_bytes, (unsigned long) sizeof(frame));
	subghz_receiver_free(rx);
	sg_env_free(env);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) return usage();
	if (strcmp(argv[1], "rx") == 0) return cmd_rx(argc, argv);
	if (strcmp(argv[1], "decode") == 0) return cmd_decode(argc, argv);
	if (strcmp(argv[1], "tx") == 0) return cmd_tx(argc, argv);
	if (strcmp(argv[1], "tx_from_file") == 0) return cmd_tx_file(argc, argv);
	if (strcmp(argv[1], "chat") == 0) return cmd_chat(argc, argv);
	if (strcmp(argv[1], "region") == 0) return cmd_region(argc, argv);
	if (strcmp(argv[1], "list") == 0) return cmd_list();
	if (strcmp(argv[1], "heap") == 0) return cmd_heap();
	return usage();
}
