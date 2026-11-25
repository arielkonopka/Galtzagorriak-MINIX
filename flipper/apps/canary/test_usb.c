/* Host tests for usbcan (task T35). */
#include <stdio.h>
#include <string.h>
#include "usbcan.h"

static int checks, fails;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s\n", m); } else printf("ok %s\n", m); } while (0)

static void setup(struct uc_sum *s, unsigned long t, int bm, int req, int val,
		  int idx, int len)
{
	unsigned char p[8] = { (unsigned char)bm, (unsigned char)req,
		(unsigned char)val, (unsigned char)(val >> 8),
		(unsigned char)idx, (unsigned char)(idx >> 8),
		(unsigned char)len, (unsigned char)(len >> 8) };
	uc_setup(s, t, p);
}

int main(void)
{
	struct uc_sum s;

	uc_init(&s);
	CHECK(s.level == UC_NONE && !strcmp(uc_level_name(s.level), "no port"),
								"nothing yet");
	uc_attach(&s, 100);
	uc_tick(&s, 2000);
	CHECK(s.level == UC_POWER && !s.powered, "power, not final before 3 s");
	uc_tick(&s, 3100);
	CHECK(s.level == UC_POWER && s.powered &&
	      !strcmp(uc_level_name(s.level), "power only"),
						"a charger: power only after 3 s");
	uc_detach(&s);
	CHECK(s.level == UC_NONE && !s.t_attach, "detach clears");

	/* a computer: reset, SOF, enumeration like Windows */
	uc_attach(&s, 1000);
	uc_reset(&s, 1100);
	CHECK(s.level == UC_ALIVE && s.t_alive == 1100, "reset: host alive");
	uc_sof(&s, 1101);
	CHECK(s.t_alive == 1100, "the first time stays");
	setup(&s, 1110, 0x80, 6, 0x0100, 0, 64);	/* device, 64 bytes */
	CHECK(s.first_wlen == 64 && s.level == UC_ALIVE, "first wLength 64");
	uc_reset(&s, 1200);
	CHECK(s.reenum == 1, "re-enumeration counted");
	setup(&s, 1210, 0x00, 5, 7, 0, 0);		/* SET_ADDRESS 7 */
	CHECK(s.level == UC_ENUM && s.t_enum == 1210 && (s.flags & UCF_ADDRESS),
							"address set: enumerates");
	setup(&s, 1220, 0x80, 6, 0x0100, 0, 18);
	CHECK(s.first_wlen == 64, "only the first wLength is kept");
	setup(&s, 1230, 0x80, 6, 0x0600, 0, 10);
	setup(&s, 1240, 0x80, 6, 0x0F00, 0, 5);
	setup(&s, 1250, 0x80, 6, 0x03EE, 0x0409, 18);
	setup(&s, 1260, 0x80, 6, 0x0302, 0x0409, 255);
	CHECK((s.flags & (UCF_QUALIFIER | UCF_BOS | UCF_MSOS)) ==
	      (UCF_QUALIFIER | UCF_BOS | UCF_MSOS) && s.langid == 0x0409 &&
	      (s.strings & 0x80) && (s.strings & 0x04), "qualifier, BOS, MS OS, strings");
	setup(&s, 1300, 0x00, 9, 1, 0, 0);		/* SET_CONFIGURATION 1 */
	CHECK(s.level == UC_CONFIG && s.t_config == 1300 &&
	      !strcmp(uc_level_name(s.level), "configures"), "configures");
	setup(&s, 1310, 0x21, 0x0A, 0, 0, 0);		/* HID SET_IDLE */
	CHECK(s.flags & UCF_CLASSREQ, "class request noted");
	uc_data(&s, 1400);
	CHECK(s.level == UC_USED && s.t_used == 1400, "traffic: uses the function");
	setup(&s, 1500, 0x00, 5, 9, 0, 0);
	CHECK(s.level == UC_USED, "the level never goes down");
	uc_suspend(&s);
	CHECK(s.flags & UCF_SUSPEND, "suspend");

	/* a host that powers the port but never enumerates */
	uc_attach(&s, 5000);
	uc_sof(&s, 5010);
	uc_tick(&s, 9000);
	CHECK(s.level == UC_ALIVE && !s.powered, "alive without SETUP");

	/* SET_CONFIGURATION 0 is not a configuration */
	uc_attach(&s, 0);
	setup(&s, 5, 0x00, 9, 0, 0, 0);
	CHECK(s.level == UC_ENUM, "configuration 0: not configured");

	/* events without an attach are ignored */
	uc_detach(&s);
	uc_reset(&s, 1); uc_sof(&s, 2); uc_data(&s, 3);
	setup(&s, 4, 0x00, 9, 1, 0, 0);
	CHECK(s.level == UC_NONE && !s.setups && !s.resets, "no port: ignored");

	/* counters stop at 255 */
	uc_attach(&s, 0);
	{ int i; for (i = 0; i < 300; i++) uc_reset(&s, (unsigned long)i + 1); }
	CHECK(s.resets == 255, "reset counter saturates");
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
