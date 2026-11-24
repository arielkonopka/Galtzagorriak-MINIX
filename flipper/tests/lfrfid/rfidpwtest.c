/* rfidpwtest: the copier's password and dictionary (issue #448,
 * flipper/cmds/rfid_pw.h) on the PC: the line format, the append before the
 * write, "previous", the modes, the saved choice. */
#include "../../cmds/rfid_pw.h"
#include <assert.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
	char dir[] = "/tmp/rfidpwXXXXXX", dict[96], conf[96], rnd[96];
	unsigned long w, x, v;
	int mode, made, i;
	FILE *f;

	CHECK(mkdtemp(dir) != NULL);
	sprintf(dict, "%s/lfrfid/assets/dict.lfrfid", dir);
	sprintf(conf, "%s/copier.conf", dir);
	sprintf(rnd, "%s/rnd", dir);

	CHECK(pw_line("1a2B3c4D # copy\n", &w) && w == 0x1A2B3C4DUL);
	CHECK(pw_line("  00000001\n", &w) && w == 1);
	CHECK(!pw_line("# comment\n", &w));
	CHECK(!pw_line("1234567\n", &w));
	CHECK(!pw_line("123456789\n", &w));
	CHECK(!pw_line("1234567G\n", &w));

	/* no file: legacy none, own makes one and records it first */
	f = fopen(rnd, "w"); fwrite("\xFF\x12\x34\x56\xFF\x12\x34\x57", 1, 8, f); fclose(f);
	CHECK(pw_previous(dict, &w) == 0);
	CHECK(pw_pick(PW_LEGACY, 0, dict, rnd, &w, &made) == 0 && w == 0 && !made);
	CHECK(pw_pick(PW_OWN, 0, dict, rnd, &w, &made) == 0 && made);
	CHECK(w == 0x7F123456UL);			/* bit 31 cleared */
	CHECK(pw_has(dict, w));
	CHECK(pw_previous(dict, &x) && x == w);
	/* the same random bytes again: already known, the next four are used */
	CHECK(pw_pick(PW_OWN, 0, dict, rnd, &v, &made) == 0 && made);
	CHECK(v == 0x7F123457UL || pw_has(dict, v));
	/* previous: the last one, nothing new written */
	CHECK(pw_count(dict) == 1 || pw_count(dict) == 2);
	i = pw_count(dict);
	CHECK(pw_pick(PW_PREVIOUS, 0, dict, rnd, &x, &made) == 0 && !made && x == v);
	CHECK(pw_count(dict) == i);
	/* dict: the chosen one if it is in the file, else a new one */
	CHECK(pw_pick(PW_DICT_PICK, w, dict, rnd, &x, &made) == 0 && !made && x == w);
	/* a failed save: no password may be used */
	CHECK(pw_record("/proc/nope/x", 5, 1) < 0);
	CHECK(pw_pick(PW_OWN, 0, "/proc/nope/x", rnd, &x, &made) < 0);
	/* a dictionary line that is not a copy is not "previous" */
	CHECK(pw_record(dict, 0x00ABCDEFUL, 0) == 0);
	CHECK(pw_previous(dict, &x) && x == v);
	CHECK(pw_nth(dict, 0, &x) && x == w);
	CHECK(!pw_nth(dict, 99, &x));

	pw_conf_load(conf, &mode, &x);
	CHECK(mode == PW_OWN && x == 0);		/* the default */
	for (i = 0; i < PW_MODES; i++) {
		CHECK(pw_conf_save(conf, i, 0x00ABCDEFUL) == 0);
		pw_conf_load(conf, &mode, &x);
		CHECK(mode == i && (i != PW_DICT_PICK || x == 0x00ABCDEFUL));
	}
	printf(fails ? "rfidpwtest: %d FAILED\n" : "rfidpwtest: all ok\n", fails);
	return fails != 0;
}
