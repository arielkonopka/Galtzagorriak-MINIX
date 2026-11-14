/* Host tests for sbproto (make test): golden bytes per model and request,
 * worked out from storzandbickel-ble 0.1.25, and sample status packets. */
#include <stdio.h>
#include <string.h>
#include "sbproto.h"

static int fails, checks;

#define CHECK(c, what) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

/* "10110003" + family tail -> 16 bytes */
static void uuid(const char *s, unsigned char *u)
{
	int i;

	for (i = 0; i < 16; i++) {
		while (*s == '-') s++;
		sscanf(s, "%2hhx", &u[i]);
		s += 2;
	}
}

#define SBT	"-5354-4f52-5a26-4249434b454c"
#define CRT	"-4c45-4b43-4942-265a524f5453"

static int wr(const struct sb_write *w, const char *u, const char *hex,
							int len, int delay)
{
	unsigned char uu[16], b[20];
	int i;

	uuid(u, uu);
	memset(b, 0, sizeof(b));
	for (i = 0; i < len; i++) sscanf(hex + 2 * i, "%2hhx", &b[i]);
	return memcmp(w->uuid, uu, 16) == 0 && w->len == len
		&& memcmp(w->data, b, len) == 0 && w->response == 0
		&& w->delay == delay;
}

static void encode_tests(void)
{
	struct sb_state st;
	struct sb_write w[SB_MAXW];
	char q[41];

	/* Volcano */
	sb_init(&st, SB_VOLCANO);
	CHECK(sb_encode(&st, SB_TARGET, 1850, w) == 1
		&& wr(&w[0], "10110003" SBT, "3a070000", 4, 0), "vol target");
	CHECK(sb_encode(&st, SB_TARGET, 2300, w) == 1, "vol 230 C allowed");
	CHECK(sb_encode(&st, SB_TARGET, 2301, w) == SB_ERANGE, "vol > 230");
	CHECK(sb_encode(&st, SB_TARGET, 399, w) == SB_ERANGE, "vol < 40");
	CHECK(sb_encode(&st, SB_HEATER, 1, w) == 1
		&& wr(&w[0], "1011000f" SBT, "01", 1, 0), "vol heater on");
	CHECK(sb_encode(&st, SB_HEATER, 0, w) == 1
		&& wr(&w[0], "10110010" SBT, "01", 1, 0), "vol heater off");
	CHECK(sb_encode(&st, SB_PUMP, 1, w) == 1
		&& wr(&w[0], "10110013" SBT, "01", 1, 0), "vol pump on");
	CHECK(sb_encode(&st, SB_PUMP, 0, w) == 1
		&& wr(&w[0], "10110014" SBT, "01", 1, 0), "vol pump off");
	CHECK(sb_encode(&st, SB_BRIGHTNESS, 70, w) == 1
		&& wr(&w[0], "10110005" SBT, "4600", 2, 0), "vol led");
	CHECK(sb_encode(&st, SB_AUTOOFF, 1800, w) == 1
		&& wr(&w[0], "1011000d" SBT, "0807", 2, 0), "vol auto-off");
	CHECK(sb_encode(&st, SB_FIND, 0, w) == SB_ENOTSUP, "vol no find");
	CHECK(sb_encode(&st, SB_MODE, 2, w) == SB_ENOTSUP, "vol no boost");

	/* Venty and Veazy: one characteristic, 20-byte packets */
	sb_init(&st, SB_VENTY);
	strcpy(q, "0102000076070000000000000000000000000000");
	CHECK(sb_encode(&st, SB_TARGET, 1910, w) == 1
		&& wr(&w[0], "00000001" SBT, q, 20, 0), "venty target");
	CHECK(sb_encode(&st, SB_TARGET, 2110, w) == SB_ERANGE, "venty > 210");
	strcpy(q, "0120000000000000000000010000000000000000");
	CHECK(sb_encode(&st, SB_HEATER, 1, w) == 1
		&& wr(&w[0], "00000001" SBT, q, 20, 0), "venty heater on");
	st.heater = 2;				/* in boost: off, then on */
	CHECK(sb_encode(&st, SB_HEATER, 1, w) == 2
		&& wr(&w[0], "00000001" SBT,
			"0120000000000000000000000000000000000000", 20, 0)
		&& wr(&w[1], "00000001" SBT, q, 20, 30), "venty boost->normal");
	CHECK(sb_encode(&st, SB_MODE, 3, w) == 1
		&& wr(&w[0], "00000001" SBT,
			"0120000000000000000000030000000000000000", 20, 0),
		"venty superboost");
	CHECK(sb_encode(&st, SB_BRIGHTNESS, 5, w) == 1
		&& wr(&w[0], "00000001" SBT,
			"0601050000000000000000000000000000000000", 20, 0),
		"venty brightness");
	CHECK(sb_encode(&st, SB_BRIGHTNESS, 10, w) == SB_ERANGE, "venty 1-9");
	CHECK(sb_encode(&st, SB_FIND, 0, w) == 1
		&& wr(&w[0], "00000001" SBT,
			"0d01000000000000000000000000000000000000", 20, 0),
		"venty find");
	CHECK(sb_encode(&st, SB_PUMP, 1, w) == SB_ENOTSUP, "venty no pump");
	CHECK(sb_encode(&st, SB_AUTOOFF, 60, w) == SB_ENOTSUP, "venty no auto");
	sb_init(&st, SB_VEAZY);
	CHECK(sb_encode(&st, SB_TARGET, 1800, w) == 1
		&& wr(&w[0], "00000001" SBT,
			"0102000008070000000000000000000000000000", 20, 0),
		"veazy target");

	/* Crafty */
	sb_init(&st, SB_CRAFTY);
	CHECK(sb_encode(&st, SB_TARGET, 1800, w) == 1
		&& wr(&w[0], "00000021" CRT, "0807", 2, 0), "crafty target");
	st.boost = 100;				/* known: rewritten after */
	CHECK(sb_encode(&st, SB_TARGET, 1800, w) == 2
		&& wr(&w[1], "00000031" CRT, "6400", 2, 0), "crafty +boost");
	CHECK(sb_encode(&st, SB_TARGET, 2101, w) == SB_ERANGE, "crafty > 210");
	CHECK(sb_encode(&st, SB_HEATER, 1, w) == 1
		&& wr(&w[0], "00000081" CRT, "0000", 2, 0), "crafty on");
	CHECK(sb_encode(&st, SB_HEATER, 0, w) == 1
		&& wr(&w[0], "00000091" CRT, "0000", 2, 0), "crafty off");
	CHECK(sb_encode(&st, SB_AUTOOFF, 120, w) == 2
		&& wr(&w[0], "000001b3" CRT, "2f03", 2, 0)
		&& wr(&w[1], "00000061" CRT, "7800", 2, 0), "crafty code+auto");
	st.reg[2] = 0x1001;
	CHECK(sb_encode(&st, SB_FIND, 0, w) == 1
		&& wr(&w[0], "000001c3" CRT, "0910", 2, 0), "crafty find");
	CHECK(sb_encode(&st, SB_BRIGHTNESS, 40, w) == 1
		&& wr(&w[0], "00000051" CRT, "2800", 2, 0), "crafty led");
	CHECK(sb_encode(&st, SB_MODE, 2, w) == SB_ENOTSUP, "crafty no boost");
	CHECK(sb_encode(&st, SB_PUMP, 1, w) == SB_ENOTSUP, "crafty no pump");

	sb_init(&st, SB_UNKNOWN);
	CHECK(sb_encode(&st, SB_HEATER, 1, w) == SB_EINVAL, "unknown model");
}

static int dec(struct sb_state *st, const char *u, const char *hex)
{
	unsigned char uu[16], b[32];
	int i, n = strlen(hex) / 2;

	uuid(u, uu);
	for (i = 0; i < n; i++) sscanf(hex + 2 * i, "%2hhx", &b[i]);
	return sb_decode(st, uu, b, n);
}

static void decode_tests(void)
{
	struct sb_state st;
	unsigned char r[16][16];

	/* Venty status: current 0x8000 = not reported, target 190.0, boost
	 * 5, superboost 10, battery 85 %, countdown 30 s (bytes added), mode
	 * 1, charger, settings: Fahrenheit + setpoint reached */
	sb_init(&st, SB_VENTY);
	CHECK(dec(&st, "00000001" SBT,
		"010000806c07050a551e0001000103000000000000") == 0, "venty pkt");
	CHECK(st.cur == SB_TEMP_NONE, "venty 0x8000 = not reported");
	CHECK(st.target == 1900 && st.boost == 5 && st.superboost == 10
		&& st.battery == 85 && st.countdown == 30 && st.heater == 1
		&& st.charger == 1 && st.fahrenheit == 1 && st.ready == 1,
		"venty fields");
	CHECK(dec(&st, "00000001" SBT, "0601070000") == 0
		&& st.brightness == 7, "venty settings");
	CHECK(dec(&st, "00000001" SBT, "01000080") == -1, "venty short pkt");

	/* Crafty: ready is project status bit 8 (0x0100) */
	sb_init(&st, SB_CRAFTY);
	CHECK(dec(&st, "00000011" CRT, "f906") == 0 && st.cur == 1785,
		"crafty current");
	CHECK(dec(&st, "00000093" CRT, "0001") == 0 && st.ready == 1,
		"crafty ready 0x0100");
	CHECK(dec(&st, "00000052" CRT, "00000000000000000100") == 0
		&& st.heater == 1, "crafty status (10 bytes)");
	CHECK(dec(&st, "00000093" CRT, "2000") == 0 && st.heater == 2
		&& st.ready == 0, "crafty boost, not ready");
	CHECK(dec(&st, "000001c3" CRT, "0400") == 0 && st.ready == 1,
		"crafty ready via reg 2 bit 2");
	CHECK(dec(&st, "00000041" CRT, "4b") == 0 && st.battery == 75,
		"crafty battery");
	CHECK(dec(&st, "00000083" CRT, "0100") == 0 && st.charger == 1,
		"crafty charger");

	/* Volcano: status register 1 bits, ready by comparison */
	sb_init(&st, SB_VOLCANO);
	CHECK(dec(&st, "1010000c" SBT, "2020") == 0 && st.heater == 1
		&& st.pump == 1, "volcano st1");
	CHECK(dec(&st, "10110001" SBT, "3a07") == 0
		&& dec(&st, "10110003" SBT, "3a070000") == 0 && st.ready == 1,
		"volcano ready");
	CHECK(dec(&st, "1010000d" SBT, "0002") == 0 && st.fahrenheit == 1,
		"volcano F");
	CHECK(dec(&st, "12345678" SBT, "00") == -1, "unknown characteristic");

	CHECK(sb_reads(SB_VOLCANO, r, 16) == 8, "volcano reads");
	CHECK(sb_reads(SB_CRAFTY, r, 16) == 11, "crafty reads");
	CHECK(sb_reads(SB_VENTY, r, 16) == 0, "venty: notifications");

	CHECK(sb_detect("S&B VOLCANO H") == SB_VOLCANO, "detect volcano");
	CHECK(sb_detect("S&B VZ 12345") == SB_VEAZY, "detect veazy");
	CHECK(sb_detect("Venty") == SB_VENTY, "detect venty");
	CHECK(sb_detect("S&B VY") == SB_VENTY, "detect venty short");
	CHECK(sb_detect("CRAFTY+") == SB_CRAFTY, "detect crafty");
	CHECK(sb_detect("Flipper") == SB_UNKNOWN, "detect other");
	CHECK(sizeof(struct sb_state) <= 64, "state <= 64 B");
}

int main(void)
{
	encode_tests();
	decode_tests();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
