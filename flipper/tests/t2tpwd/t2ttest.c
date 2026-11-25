/* Host test for t2t_pwd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "t2t_pwd.h"

static int checks, fails;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s\n", m); } else printf("ok %s\n", m); } while (0)

static int seq;
static unsigned char script[][4] = {
	{ 0xAA, 0xAA, 0xAA, 0xAA },	/* weak: refused */
	{ 0x12, 0x34, 0x56, 0x78 },	/* weak: refused */
	{ 0xC0, 0xFF, 0xEE, 0x01 },	/* good */
	{ 0xC0, 0xFF, 0xEE, 0x01 },	/* a second run: already known */
	{ 0xBA, 0x5C, 0x0D, 0x02 },	/* good */
};
static int rng(unsigned char *b, int n)
{
	memcpy(b, script[seq++ % 5], n);
	return 0;
}
static int badrng(unsigned char *b, int n) { (void)b; (void)n; return -1; }

static char *slurp(const char *p)
{
	static char b[512];
	FILE *f = fopen(p, "r");
	size_t n;

	if (!f) return NULL;
	n = fread(b, 1, sizeof(b) - 1, f);
	b[n] = 0;
	fclose(f);
	return b;
}

int main(void)
{
	const char *d = "dict.nfc", *l = "last.nfc";
	unsigned char uid[7] = { 4, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	unsigned char pwd[4], pack[2], q[4] = { 1, 2, 3, 4 };
	struct t2t_cfg c;
	FILE *f;

	remove(d); remove(l);
	memset(&c, 0, sizeof(c));
	c.dict = d; c.last = l; c.rng = rng; c.mode = T2T_RANDOM;
	c.note = "Tokia\nNTAG";

	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_OK, "random: ok");
	CHECK(!memcmp(pwd, script[2], 4), "random: weak ones skipped");
	CHECK(t2t_pwd_known(d, pwd) == 1, "in the dictionary before use");
	CHECK(strstr(slurp(d), "C0FFEE01  # Tokia NTAG\n") != NULL,
						"line with a one-line note");
	CHECK(strstr(slurp(l), "C0FFEE01\n") != NULL, "last file");
	CHECK(pack[0] == (unsigned char)(0xC0 ^ 0xEE ^ 4) &&
	      pack[1] == (unsigned char)(0xFF ^ 0x01 ^ 0x11), "pack");

	/* a known password is not made again */
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_OK &&
	      !memcmp(pwd, script[4], 4), "random: a known one is skipped");
	CHECK(t2t_pwd_known(d, pwd) == 1, "second one stored");

	/* previous */
	c.mode = T2T_PREVIOUS;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_OK &&
	      !memcmp(pwd, script[4], 4), "previous");

	/* dictionary entry */
	c.mode = T2T_DICT; c.index = 0;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_OK &&
	      !memcmp(pwd, script[2], 4), "dictionary entry 0");
	c.index = 1;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_OK &&
	      !memcmp(pwd, script[4], 4), "dictionary entry 1");
	c.index = 9;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_BADCFG, "no such entry");
	CHECK(strcmp(slurp(d), "C0FFEE01  # Tokia NTAG\nBA5C0D02  # Tokia NTAG\n") == 0,
							"dictionary: no duplicates");

	/* own */
	c.mode = T2T_OWN;
	memcpy(c.own, q, 4);
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_OK &&
	      !memcmp(pwd, q, 4) && t2t_pwd_known(d, q) == 1, "own, stored");
	memset(c.own, 0xFF, 4);
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_BADCFG,
						"own: FFFFFFFF refused");

	/* legacy: nothing is written */
	remove(d); remove(l);
	c.mode = T2T_LEGACY;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_NONE &&
	      !slurp(d) && !slurp(l), "legacy: nothing stored");

	/* errors */
	c.mode = T2T_RANDOM; c.rng = badrng;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_BADCFG, "rng fails");
	c.rng = NULL;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_BADCFG, "no rng");
	c.mode = T2T_PREVIOUS;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_BADCFG, "no previous");
	c.mode = T2T_RANDOM; c.rng = rng; c.dict = "/nonexistent/dir/d.nfc";
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_ERR,
					"dictionary not writable: no password");
	c.mode = 99;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_BADCFG, "bad mode");

	/* a dictionary with comments, blanks, junk and a long line */
	f = fopen(d, "w");
	fputs("# mine\n\nZZZZZZZZ\n  deadbeef  # x\n", f);
	{ int i; for (i = 0; i < 200; i++) fputc('A', f); fputc('\n', f); }
	fputs("0A0B0C0D\n", f);
	fclose(f);
	c.dict = d; c.mode = T2T_DICT; c.index = 1;
	CHECK(t2t_pwd_choose(&c, uid, pwd, pack) == T2T_OK &&
	      pwd[0] == 0x0A, "messy dictionary: entries by position");
	CHECK(t2t_pwd_known(d, (unsigned char *)"\xde\xad\xbe\xef") == 1,
						"lower case, indented");
	remove(d); remove(l);
	printf("t2t_pwd: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
