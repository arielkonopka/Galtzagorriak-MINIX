/* region - the radio region: which country's Sub-GHz rules apply
 * (spec section Q, RQ.1, RQ.2, RQ.4, RQ.5; issue #292, T32a).
 *
 *	region			the region set, and the kernel's
 *	region list		the countries with a table
 *	region show [CC]	a country's table (default: the region set)
 *	region check CC MHZ [-p PRESET] [-P DBM] [-c] [-a MS]
 *				(defaults: std, -30 dBm, data, 1 s)
 *				whether a transmit would be allowed; never
 *				sends anything
 *	region compile CC FILE	write CC's record (RQ.2) to FILE
 *	region set [-y] [-f] CC	(root) check the table and its SHA-256
 *				against the shipped list (RQ.5; a custom table
 *				needs -f), compile the record, ask, write the
 *				code to /etc/flipper/region and log the change
 *				to /usr/adm/messages; /etc/rc gives the code
 *				to the kernel at the next boot
 *
 * The tables are /usr/share/region/<CC>.reg (Q.2), read with libmodem's
 * parser, so `show` and `check` print what the library allows.  `set`
 * compiles the table into the record of RQ.2 (16 B + 16 B per active row,
 * <= 528 B); MM writes it into flash with T32b, once per change of region.
 * Until then the kernel keeps its own const band set per country (R8.2).
 * RAM: one table (~2.7 KB, malloc'd) while it runs; nothing resident.
 */
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <minix/subghz.h>
#include "modem.h"

#define DIR_RG	"/usr/share/region"
#define FILE_RG	"/etc/flipper/region"
#define LOG_DIR	"/usr/adm"
#define LOG_RG	"/usr/adm/messages"

static struct mdm_region *rg;
static unsigned char rec[MDM_RGB_MAX];	/* the record of RQ.2 */

static int usage(void)
{
	fprintf(stderr, "usage: region [list | show [CC] | set [-y] [-f] CC |\n"
		"              compile CC FILE |\n"
		"              check CC MHZ [-p PRESET] [-P DBM] [-c] [-a MS]]\n");
	return 2;
}

static void current(char *cc)
{
	int fd, n;

	cc[0] = 0;
	if ((fd = open(FILE_RG, O_RDONLY)) < 0) return;
	n = (int) read(fd, cc, 2);
	close(fd);
	cc[n == 2 ? 2 : 0] = 0;
}

static int load(const char *cc)
{
	char err[96];

	if (mdm_rg_load(DIR_RG, cc, rg, err, (int) sizeof(err)) < 0) {
		fprintf(stderr, "region: %s\n", err);
		return -1;
	}
	return 0;
}

static void pmhz(unsigned long hz)
{
	printf("%3lu.%03lu", hz / 1000000UL, hz / 1000UL % 1000UL);
}

static const char *use_text[] = {
	"any use",
	"remote buttons only, <= 5 s",
	"short bursts <= 1 s with a pause",
	"frequency-hopping link only",
};

static int show(const char *cc)
{
	struct mdm_rgrow *r;
	int i;

	if (load(cc) < 0) return 1;
	printf("%s %s%s%s\n", rg->code, rg->name, rg->base[0] ? ", base " : "",
		rg->base);
	printf("  MHz              dBm  PA  duty   bw kHz  use\n");
	for (i = 0; i < rg->n; i++) {
		r = &rg->row[i];
		printf("%c ", r->flags & MDM_RG_UNCHECKED ? '?' : ' ');
		pmhz(r->lo);
		printf("-");
		pmhz(r->hi);
		printf("  %3d %3d  ", r->dbm, mdm_pa_step(r->dbm));
		if (r->duty_pm >= 1000) printf("none ");
		else printf("%2d.%d%%", r->duty_pm / 10, r->duty_pm % 10);
		if (r->bw_khz) printf("  %5u", r->bw_khz);
		else printf("      -");
		printf("  %s\n      %s\n", use_text[r->use], r->ref);
	}
	printf("? = not checked: never used.  You are responsible for using "
		"the radio legally where you are.\n");
	return 0;
}

static int list(void)
{
	DIR *d;
	struct dirent *e;
	size_t n;

	if ((d = opendir(DIR_RG)) == NULL) {
		fprintf(stderr, "region: %s: %s\n", DIR_RG, strerror(errno));
		return 1;
	}
	while ((e = readdir(d)) != NULL) {
		n = strlen(e->d_name);
		if (n != 6 || strcmp(e->d_name + 2, ".reg") != 0) continue;
		e->d_name[2] = 0;
		if (load(e->d_name) == 0)
			printf("%s  %s\n", rg->code, rg->name);
	}
	closedir(d);
	return 0;
}

static unsigned long parse_mhz(const char *s)
{
	unsigned long m = 0, k = 0;
	int d = 0;

	while (*s >= '0' && *s <= '9') m = m * 10 + (unsigned long) (*s++ - '0');
	if (*s == '.')
		for (s++; *s >= '0' && *s <= '9' && d < 3; d++)
			k = k * 10 + (unsigned long) (*s++ - '0');
	while (d++ < 3) k *= 10;
	return m * 1000000UL + k * 1000UL;
}

static int check(int argc, char **argv)
{
	static struct mdm_rgstate st;
	const struct mdm_preset *p = mdm_preset("std");
	struct mdm_cc1101 c;
	unsigned long f;
	long wait, ms = 100;
	int i, dbm = -30, mode = MDM_SG_DATA, row, rc;

	if (argc < 4) return usage();
	for (i = 4; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) mode = MDM_SG_CTL;
		else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			p = mdm_preset(argv[++i]);
		else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc)
			dbm = atoi(argv[++i]);
		else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
			ms = atol(argv[++i]);
		else return usage();
	}
	if (p == NULL) {
		fprintf(stderr, "region: no such preset (gmodem info)\n");
		return 2;
	}
	if (load(argv[2]) < 0) return 1;
	f = parse_mhz(argv[3]);
	if (mdm_cc1101_regs(f, &p->phy, &c) < 0) {
		printf("refused: the CC1101 cannot send there\n");
		return 1;
	}
	mdm_rg_state_init(&st);
	rc = mdm_rg_tx(rg, &st, c.freq, mdm_occupied(&c), dbm, mode,
		(unsigned) ((ms + 9) / 10), 0, &row, &wait);
	if (rc != MDM_SG_OK) {
		printf("%s: %s\n", strerror(EPERM), mdm_rg_msg(rc));
		return 1;
	}
	printf("allowed in %s: ", rg->code);
	pmhz(rg->row[row].lo);
	printf("-");
	pmhz(rg->row[row].hi);
	printf(" MHz, up to %d dBm (PA %d), ", rg->row[row].dbm,
		mdm_pa_step(rg->row[row].dbm));
	if (rg->row[row].duty_pm >= 1000) printf("no duty limit");
	else printf("duty %d.%d %%", rg->row[row].duty_pm / 10,
		rg->row[row].duty_pm % 10);
	printf(", %s; %s\n", use_text[rg->row[row].use], rg->row[row].ref);
	return 0;
}

static void hex(char *out, const unsigned char *h, int n)
{
	static const char x[] = "0123456789abcdef";
	int i;

	for (i = 0; i < n; i++) {
		out[2 * i] = x[h[i] >> 4];
		out[2 * i + 1] = x[h[i] & 15];
	}
	out[2 * n] = 0;
}

/* RQ.5: is dir/cc.reg the shipped file?  1 yes, 0 custom (said why),
 * -1 unreadable.  Its hash in sum (65 B). */
static int shipped(const char *cc, char *sum)
{
	unsigned char h[32];
	char want[65];
	int same;

	if (mdm_rg_sha_file(DIR_RG, cc, h) < 0) {
		fprintf(stderr, "region: %s/%s.reg: %s\n", DIR_RG, cc,
			strerror(errno));
		return -1;
	}
	hex(sum, h, 32);
	if (mdm_rg_sha_find(mdm_rg_shipped, mdm_rg_nshipped, cc, h, &same))
		return 1;
	printf("custom table: %s.reg, sha256 %s\n", cc, sum);
	if (same >= 0) {
		hex(want, mdm_rg_shipped[same].sha, 32);
		printf("  differs from the shipped %s.reg, sha256 %s\n", cc, want);
	} else {
		printf("  no %s.reg was shipped with this firmware\n", cc);
	}
	return 0;
}

static int compile(const char *cc, unsigned char *b)
{
	int n;

	if (load(cc) < 0) return -1;
	if ((n = mdm_rg_compile(rg, b, MDM_RGB_MAX)) < 0
	    || mdm_rg_blob_check(b, n) != n) {
		fprintf(stderr, "region: %s: no active row, or more than %d\n",
			cc, MDM_RG_ROWS);
		return -1;
	}
	return n;
}

static int compile_to(int argc, char **argv)
{
	unsigned char *b = rec;
	int n, fd;

	if (argc != 4) return usage();
	if ((n = compile(argv[2], b)) < 0) return 1;
	if ((fd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0
	    || write(fd, b, (size_t) n) != n || close(fd) < 0) {
		fprintf(stderr, "region: %s: %s\n", argv[3], strerror(errno));
		return 1;
	}
	printf("%s: record %d B, CRC %08lx\n", argv[2], n, mdm_rg_blob_crc(b));
	return 0;
}

static void logit(const char *cc, const char *base, int custom,
	const char *sum, const char *bsum, int n, unsigned long crc)
{
	char t[32];
	time_t now = time(NULL);
	FILE *fp;

	strftime(t, sizeof(t), "%b %d %H:%M:%S", localtime(&now));
	mkdir(LOG_DIR, 0755);
	if ((fp = fopen(LOG_RG, "a")) == NULL) {
		fprintf(stderr, "region: %s: %s\n", LOG_RG, strerror(errno));
		return;
	}
	fprintf(fp, "%s region: set %s by uid %d, %s table, %s.reg sha256 %s",
		t, cc, (int) getuid(), custom ? "custom" : "shipped", cc, sum);
	if (base[0]) fprintf(fp, ", %s.reg sha256 %s", base, bsum);
	fprintf(fp, ", record %d B CRC %08lx\n", n, crc);
	fclose(fp);
}

static int set(int argc, char **argv)
{
	unsigned char *b = rec;
	char line[16], sum[65], bsum[65], base[3];
	int yes = 0, force = 0, fd, i = 2, n, ok;

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-y") == 0) yes = 1;
		else if (strcmp(argv[i], "-f") == 0) force = 1;
		else return usage();
	}
	if (i + 1 != argc) return usage();
	if (geteuid() != 0) {
		fprintf(stderr, "region: set needs root\n");
		return 1;
	}
	if (mdm_rg_bandset(argv[i]) == NULL) {
		fprintf(stderr, "region: no table for %s; transmit stays off\n",
			argv[i]);
		return 1;
	}
	if (show(argv[i]) != 0) return 1;
	memcpy(base, rg->base, 3);
	if ((n = compile(argv[i], b)) < 0) return 1;
	/* RQ.5: the file and its base, against the list made at build time */
	if ((ok = shipped(argv[i], sum)) < 0) return 1;
	bsum[0] = 0;
	if (base[0]) {
		int k = shipped(base, bsum);

		if (k < 0) return 1;
		ok = ok && k;
	}
	if (!ok && !force) {
		fprintf(stderr, "region: a custom table needs -f; you are "
			"responsible for it\n");
		return 1;
	}
	printf("record: %d B, CRC %08lx (written to flash once, with T32b)\n",
		n, mdm_rg_blob_crc(b));
	if (!yes) {
		printf("I will use this device under the rules of %s [y/N] ",
			argv[i]);
		fflush(stdout);
		if (fgets(line, sizeof(line), stdin) == NULL || line[0] != 'y')
			return 1;
	}
	if ((fd = open(FILE_RG, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0
	    || write(fd, argv[i], 2) != 2 || write(fd, "\n", 1) != 1) {
		fprintf(stderr, "region: %s: %s\n", FILE_RG, strerror(errno));
		return 1;
	}
	close(fd);
	logit(argv[i], base, !ok, sum, bsum, n, mdm_rg_blob_crc(b));
	printf("region %s: applies from the next boot\n", argv[i]);
	return 0;
}

int main(int argc, char **argv)
{
	char cc[3];
	int fd, v = 0;

	if ((rg = malloc(sizeof(*rg))) == NULL) {
		fprintf(stderr, "region: out of memory\n");
		return 1;
	}
	if (argc < 2) {
		current(cc);
		printf("%s: %s\n", FILE_RG, cc[0] ? cc : "none");
		if ((fd = open("/dev/subghz", O_RDONLY)) >= 0
		    && ioctl(fd, SGIOCGREGION, &v) == 0) {
			if (v) printf("kernel: %c%c\n", v >> 8 & 0xFF, v & 0xFF);
			else printf("kernel: none (no transmit)\n");
		}
		return 0;
	}
	if (strcmp(argv[1], "list") == 0) return list();
	if (strcmp(argv[1], "show") == 0) {
		if (argc > 2) return show(argv[2]);
		current(cc);
		if (!cc[0]) {
			fprintf(stderr, "region: none set (region set CC)\n");
			return 1;
		}
		return show(cc);
	}
	if (strcmp(argv[1], "check") == 0) return check(argc, argv);
	if (strcmp(argv[1], "set") == 0) return set(argc, argv);
	if (strcmp(argv[1], "compile") == 0) return compile_to(argc, argv);
	return usage();
}
