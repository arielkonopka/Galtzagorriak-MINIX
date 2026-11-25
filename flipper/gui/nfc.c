/* SPDX-License-Identifier: GPL-3.0-or-later
 * nfc - the stock NFC app on Galtzagorriak MINIX (spec Phase 14; issue
 * #412 item 4), a window app (spec K.8): the launcher draws, the app only
 * describes ("Pine meets modern UX": one list a screen, OK opens, Back
 * goes up, the values on the right).
 *
 *	nfc		from the launcher (NFC:nobody:@nfc:=/bin/nfc)
 *	nfc -L		the same, with the loopback cards (QEMU, no driver)
 *	nfc -t [DICT]	no screen: read both loopback cards (the Classic one
 *			with the keys of DICT), save, reload and compare;
 *			prints "nfc: N ok, M bad", exit 0 when all ok
 *	nfc -c		may this user open /dev/nfc?  0 opened, 1 not in
 *			group nfc, 2 no device (AC7.3's rule for NFC)
 *	nfc scan [-L] [DICT]  the service scanner without a screen: SELECT
 *			every AID of DICT (default: yours, else the example)
 *			and print the ones the card answers to (#451)
 *
 * Screens: Read (poll, identify, MIFARE Classic keys from the streaming
 * dictionary, read, Save), Detect reader (a stub until /dev/nfc can
 * listen, Phase 13), Saved (the .nfc files in /home/flipper/nfc: Info,
 * Delete), Extra actions (MIFARE Classic keys: the dictionaries and how
 * many keys they hold), Add manually (type and UID, saved as a blank
 * card) and Scan services (#451: which applications an ISO 14443-4 card
 * answers SELECT for, from a dictionary file read a line at a time;
 * nothing but SELECT is sent).  Read shows a Type 4 Tag's NDEF record.  The reader side is libnfc (src/lib/libnfc) on one nfc_xfer: the
 * /dev/nfc ioctls (<minix/nfc.h>, driver lane:local) or nfcloop.c.
 *
 * /dev/nfc is group nfc (14); nfc is set-gid nfc and opens it only for a
 * member, then drops the group (as infrared, AC7.3).  No kernel change.
 *
 * RAM (AC14.3, at most 35 KB at the peak): the card (nfc_device_t, 5.3 KB)
 * and the keys found (0.6 KB) are malloc'd for a read or a file and freed
 * after; the loopback cards 1.6 KB only with -L; stdio's buffers while a
 * file or dictionary is read.  The whole image is data + bss + CHMEM_nfc
 * (m2test checks a_total).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <minix/nfc.h>
#include "gm.h"
#include "nfc_xfer.h"
#include "nfc_file.h"
#include "nfc_poller.h"
#include "mf_attack.h"
#include "nfcloop.h"
#include "iso_dep.h"
#include "aid_scan.h"
#include "t4t.h"

#define HOME	"/home/flipper"
#define NFCDIR	HOME "/nfc"
#define DEV_NFC	"/dev/nfc"
#define PATHMAX	128
#define PAGE	8
#define NAMEMAX	24
#define AID_EXAMPLE	"/usr/share/nfc/aid_scan.nfc"	/* base package */
#define T4NOTE	48

static int loopback;			/* -L, -t */
static int nfcfd = -1;			/* /dev/nfc */
static int devwhy;			/* 1 not a member, 2 no device */
static struct nfcloop *lb;		/* -L: the cards */
static int lbnext = NFCLOOP_MFC1K;	/* -L: the card the next Read finds */
static nfc_xfer_t xf;
static nfc_device_t *card;		/* the card read or opened */
static int keys_a, keys_b, nblk;	/* what the last read found */
static const char *dictnote;		/* "no dictionary" note, or NULL */
static const char *dict_t;		/* -t DICT */
static char *t4note;			/* a Type 4 Tag's NDEF (malloc'd) */
static aid_scan_t *scan;		/* the last scan (malloc'd) */
static const char *scandict;		/* the dictionary it used */
static char file[PATHMAX];		/* the saved file shown */
static char top;			/* window on top: m r s f c n x k a */
static int lfirst;
/* Add manually */
static int add_type;			/* 0 Classic 1K, 1 NTAG215 */
static char add_uid[21] = "04A1B2C3D4E5F6";
static char add_name[NAMEMAX + 1] = "Manual";

/* ---- group "nfc" (as infrared's ir_member, AC7.3) ---- */

static int field(const char *line, int f, char *out, int osz)
{
	int n = 0;

	while (f > 0 && *line) if (*line++ == ':') f--;
	if (f > 0) return -1;
	while (*line && *line != ':' && *line != '\n' && n < osz - 1)
		out[n++] = *line++;
	out[n] = 0;
	return 0;
}

static int nfc_member(void)
{
	char line[128], name[16], v[96], sb[128], *p;
	FILE *fp;
	long gid = -1;
	uid_t uid = getuid();
	int in = 0;

	if (uid == 0) return 1;
	name[0] = 0;
	if ((fp = fopen("/etc/passwd", "r")) != NULL) {
		setvbuf(fp, sb, _IOFBF, sizeof sb);
		while (fgets(line, sizeof line, fp))
			if (field(line, 2, v, sizeof v) == 0
				&& atol(v) == (long) uid) {
				field(line, 0, name, sizeof name);
				break;
			}
		fclose(fp);
	}
	if ((fp = fopen("/etc/group", "r")) == NULL) return 0;
	setvbuf(fp, sb, _IOFBF, sizeof sb);
	while (fgets(line, sizeof line, fp)) {
		if (field(line, 0, v, sizeof v) || strcmp(v, "nfc")) continue;
		field(line, 2, v, sizeof v);
		gid = atol(v);
		field(line, 3, v, sizeof v);
		for (p = strtok(v, ", \n"); p && name[0]; p = strtok(NULL, ", \n"))
			if (!strcmp(p, name)) in = 1;
		break;
	}
	fclose(fp);
	return in || (gid >= 0 && (long) getgid() == gid);
}

/* ---- the transceiver: /dev/nfc or the loopback ---- */

static int dev_xfer(void *ctx, const uint8_t *tx, size_t tx_bits,
		    uint8_t *rx, size_t rx_room, size_t *rx_bits,
		    uint32_t fwt, uint32_t flags)
{
	struct nfc_xfer_io io;

	(void) ctx;
	io.tx = (void *) tx;
	io.rx = rx;
	io.tx_bits = (unsigned) tx_bits;
	io.rx_bits = (unsigned) rx_room;
	io.fwt = fwt;
	io.flags = flags;
	if (ioctl(nfcfd, NFCIOCXFER, &io) < 0)
		return errno == EIO ? NFC_ERR_IO : NFC_ERR_TIMEOUT;
	*rx_bits = io.rx_bits;
	return NFC_OK;
}

static int dev_field(void *ctx, int on, uint32_t tech)
{
	int v = on ? (int) tech : 0;

	(void) ctx;
	return ioctl(nfcfd, NFCIOCFIELD, &v) < 0 ? NFC_ERR_IO : NFC_OK;
}

/* Open /dev/nfc as the user may, then drop the group "nfc" for good. */
static void nfc_open(void)
{
	if (!loopback) {
		if (!nfc_member()) devwhy = 1;
		else if ((nfcfd = open(DEV_NFC, O_RDWR)) < 0) devwhy = 2;
	}
	if (getegid() != getgid() && setgid(getgid()) < 0) exit(1);
	if (loopback) {
		if ((lb = nfcloop_new()) == NULL) exit(1);
		nfcloop_xfer(lb, &xf);
	} else if (nfcfd >= 0) {
		memset(&xf, 0, sizeof xf);
		xf.transceive = dev_xfer;
		xf.field = dev_field;
	}
}

static int have_reader(void)
{
	return loopback || nfcfd >= 0;
}

/* ---- small things ---- */

static void hex(char *out, const uint8_t *p, int n)
{
	static const char h[] = "0123456789ABCDEF";
	int i;

	for (i = 0; i < n; i++) {
		out[2 * i] = h[p[i] >> 4];
		out[2 * i + 1] = h[p[i] & 15];
	}
	out[2 * n] = 0;
}

static int unhex(const char *s, uint8_t *out, int max)
{
	int n = 0, v, i;

	while (*s && n < max) {
		for (v = 0, i = 0; i < 2; i++, s++) {
			int c = *s;

			if (c >= '0' && c <= '9') c -= '0';
			else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
			else if (c >= 'A' && c <= 'F') c -= 'A' - 10;
			else return -1;
			v = v << 4 | c;
		}
		out[n++] = (uint8_t) v;
	}
	return *s ? -1 : n;
}

static int ends(const char *s, const char *ext)
{
	size_t l = strlen(s), e = strlen(ext);

	return l > e && strcmp(s + l - e, ext) == 0;
}

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

static const char *type_name(const nfc_device_t *d)
{
	switch (d->type) {
	case NFC_DEV_MF_CLASSIC:
		return d->mfc.type == MF_TYPE_4K ? "MIFARE Classic 4K"
			: d->mfc.type == MF_TYPE_MINI ? "MIFARE Mini"
			: "MIFARE Classic 1K";
	case NFC_DEV_MF_ULTRALIGHT:
		return d->ul_subtype[0] ? d->ul_subtype : "Ultralight";
	case NFC_DEV_ISO14443_3A:
		return d->sak & 0x20 ? "ISO14443-4A" : "ISO14443-3A";
	default: return "Unknown";
	}
}

static int card_new(void)
{
	if (card == NULL && (card = malloc(sizeof *card)) == NULL) return -1;
	memset(card, 0, sizeof *card);
	return 0;
}

static void card_free(void)
{
	free(card);
	card = NULL;
	free(t4note);
	t4note = NULL;
}

/* An ISO 14443-4 card (SAK bit 0x20): RATS, then its NDEF message if it
 * is a Type 4 Tag, summed up in t4note.  Read only. */
static void read_t4(void)
{
	iso_dep_t *d = malloc(sizeof *d);
	t4t_t *t = malloc(sizeof *t);
	int r;

	t4note = malloc(T4NOTE);
	if (d == NULL || t == NULL || t4note == NULL) goto out;
	strcpy(t4note, "no ATS");
	if (iso_dep_activate(d, &xf) != NFC_OK) goto out;
	r = t4t_read(d, t);
	if (r == NFC_OK) {
		t4t_ndef_summary(t->ndef, t->got, t4note, T4NOTE);
		nblk = t->nlen;
	} else
		strcpy(t4note, r == NFC_ERR_NOTSUP ? "No NDEF application"
		       : "NDEF read failed");
	iso_dep_deselect(d);
out:
	free(d);
	free(t);
}

/* ---- reading a card ---- */

/* Poll, identify and read into card.  0, or -1 (no card). */
static int do_read(const char *dict)
{
	mf_found_t *f;
	long k = -1;
	int s;
	uint32_t uid;

	keys_a = keys_b = nblk = 0;
	dictnote = NULL;
	if (card_new() < 0) return -1;
	if (lb) {
		nfcloop_card(lb, lbnext);
		lbnext = lbnext == NFCLOOP_MFC1K ? NFCLOOP_NTAG215
			: lbnext == NFCLOOP_NTAG215 ? NFCLOOP_T4T : NFCLOOP_MFC1K;
	}
	(void) nfc_field(&xf, 1, NFC_TECH_A);
	if (nfc_poll_identify(&xf, card) != NFC_OK) {
		(void) nfc_field(&xf, 0, 0);
		card_free();
		return -1;
	}
	if (card->type == NFC_DEV_MF_ULTRALIGHT) {
		if (nfc_poll_read_ul(&xf, card) == NFC_OK) nblk = card->ul_pages;
	} else if (card->type == NFC_DEV_ISO14443_3A && (card->sak & 0x20)) {
		read_t4();
	} else if (card->type == NFC_DEV_MF_CLASSIC
		   && (f = malloc(sizeof *f)) != NULL) {
		mf_found_init(f, card->mfc.type);
		uid = (uint32_t) card->uid[0] << 24 | (uint32_t) card->uid[1] << 16
			| (uint32_t) card->uid[2] << 8 | card->uid[3];
		if (card->uid_len == 7)		/* the last 4 bytes, as stock */
			uid = (uint32_t) card->uid[3] << 24
				| (uint32_t) card->uid[4] << 16
				| (uint32_t) card->uid[5] << 8 | card->uid[6];
		if (dict) k = mf_attack_dict(&xf, uid, f, dict);
		else {
			long a = mf_attack_dict(&xf, uid, f, MF_DICT_USER);
			long b = mf_found_complete(f) ? 0
				: mf_attack_dict(&xf, uid, f, MF_DICT_SYS);

			k = a < 0 && b < 0 ? -1 : (a > 0 ? a : 0) + (b > 0 ? b : 0);
		}
		if (k < 0) dictnote = "No key dictionary";
		for (s = 0; s < f->sectors; s++) {
			keys_a += f->have_a[s];
			keys_b += f->have_b[s];
		}
		nblk = nfc_poll_read_mfc(&xf, card, f);
		free(f);
	}
	(void) nfc_field(&xf, 0, 0);
	return 0;
}

/* Save card as NFCDIR/<name>.nfc (name_2, _3 ... if taken). */
static int do_save(const char *name, char *out)
{
	int i;

	mkdir(HOME, 0755);
	mkdir(NFCDIR, 0755);
	for (i = 1; i < 100; i++) {
		if (i == 1) sprintf(out, "%s/%.24s.nfc", NFCDIR, name);
		else sprintf(out, "%s/%.24s_%d.nfc", NFCDIR, name, i);
		if (access(out, F_OK) != 0) break;
	}
	if (i == 100) return -1;
	return nfc_file_save(card, out);
}

/* The default name of a read card: its type and UID, as stock. */
static void card_name(char *out)
{
	char u[21];

	hex(u, card->uid, card->uid_len);
	sprintf(out, "%s_%s",
		card->type == NFC_DEV_MF_CLASSIC ? "MFC"
		: card->type == NFC_DEV_MF_ULTRALIGHT ? "NTAG" : "NFC", u);
}

/* ---- Add manually: a blank card from a type and a UID ---- */

static int make_card(void)
{
	uint8_t u[10];
	int n = unhex(add_uid, u, 10), b;

	if ((n != 4 && n != 7) || card_new() < 0) return -1;
	memcpy(card->uid, u, (size_t) n);
	card->uid_len = (uint8_t) n;
	if (add_type == 0) {			/* MIFARE Classic 1K */
		card->type = NFC_DEV_MF_CLASSIC;
		card->sak = 0x08;
		card->atqa[1] = n == 4 ? 0x04 : 0x44;
		card->mfc.type = MF_TYPE_1K;
		card->mfc.block_count = mf_block_count(MF_TYPE_1K);
		memcpy(card->mfc.uid, u, (size_t) n);
		card->mfc.uid_len = (uint8_t) n;
		card->mfc.sak = card->sak;
		memcpy(card->mfc.atqa, card->atqa, 2);
		memcpy(card->mfc.block[0].data, u, (size_t) n);
		if (n == 4) card->mfc.block[0].data[4] = u[0] ^ u[1] ^ u[2] ^ u[3];
		card->mfc.block[0].data[5] = 0x08;
		card->mfc.block[0].data[6] = card->atqa[1];
		for (b = 3; b < 64; b += 4) {
			uint8_t *t = card->mfc.block[b].data;

			memset(t, 0xFF, 16);
			t[6] = 0xFF; t[7] = 0x07; t[8] = 0x80; t[9] = 0x69;
		}
		card->mfc.block_read = ~(uint64_t) 0;
		return 0;
	}
	if (n != 7) { card_free(); return -1; }	/* NTAG215: 7 bytes */
	card->type = NFC_DEV_MF_ULTRALIGHT;
	card->atqa[1] = 0x44;
	strcpy(card->ul_subtype, "NTAG215");
	card->ul_pages = 135;
	card->ul_page[0][0] = u[0]; card->ul_page[0][1] = u[1];
	card->ul_page[0][2] = u[2];
	card->ul_page[0][3] = 0x88 ^ u[0] ^ u[1] ^ u[2];
	memcpy(card->ul_page[1], u + 3, 4);
	card->ul_page[2][0] = u[3] ^ u[4] ^ u[5] ^ u[6];
	card->ul_page[2][1] = 0x48;
	memcpy(card->ul_page[3], "\xE1\x10\x3E\x00", 4);
	memcpy(card->ul_page[4], "\x03\x00\xFE\x00", 4);
	memcpy(card->ul_page[130], "\x00\x00\x00\xBD", 4);
	memcpy(card->ul_page[131], "\x04\x00\x00\xFF", 4);
	memcpy(card->ul_page[132], "\x00\x05\x00\x00", 4);
	memcpy(card->ul_page[133], "\xFF\xFF\xFF\xFF", 4);
	return 0;
}

/* ---- the service scanner (#451) ---- */

/* Your dictionary if there is one, else the example. */
static const char *scan_dict(void)
{
	return access(AID_SCAN_DICT, R_OK) == 0 ? AID_SCAN_DICT : AID_EXAMPLE;
}

/* Scan the card in the field with dict.  The aid_scan result. */
static int do_scan(const char *dict)
{
	int r;

	if (scan == NULL && (scan = malloc(sizeof *scan)) == NULL)
		return NFC_ERR_IO;
	scandict = dict;
	(void) nfc_field(&xf, 1, NFC_TECH_A);
	r = aid_scan(&xf, dict, scan, NULL, NULL);
	(void) nfc_field(&xf, 0, 0);
	return r;
}

static void scan_free(void)
{
	free(scan);
	scan = NULL;
}

/* "Visa credit/debit" "Payments" "A0000000031010" "9000": a hit's text. */
static const char *sw_text(uint16_t sw)
{
	return sw == 0x6283 ? "blocked" : "selected";
}

/* ---- windows ---- */

static void w_main(void)
{
	gm_win("main", "list", loopback ? "NFC (loopback)" : "NFC", 0, 0);
	gm_item("read", "Read", "");
	gm_item("detect", "Detect reader", "");
	gm_item("saved", "Saved", "");
	gm_item("extra", "Extra actions", "");
	gm_item("add", "Add manually", "");
	gm_item("scan", "Scan services", "");
	gm_show();
	top = 'm';
}

static void w_note(const char *title, const char *l1, const char *l2)
{
	gm_win("note", "text", title, 0, 0);
	gm_line(l1);
	if (l2) gm_line(l2);
	gm_show();
	top = 'n';
}

static void no_reader(void)
{
	w_note("Read", devwhy == 1 ? "Not in group nfc" : "No NFC device",
	       devwhy == 1 ? "(ask root to add you)" : "/dev/nfc: driver to come");
}

/* The card's rows (read or opened); id "card" or "info". */
static void card_rows(void)
{
	char u[21], r[16];

	gm_item("i_type", "Type", type_name(card));
	hex(u, card->uid, card->uid_len);
	gm_item("i_uid", "UID", u);
	hex(u, card->atqa, 2);
	hex(r, &card->sak, 1);
	strcat(u, " ");
	strcat(u, r);
	gm_item("i_as", "ATQA SAK", u);
	if (card->type == NFC_DEV_MF_CLASSIC) {
		int s = card->mfc.block_count / 4, b, n = 0;

		if (card->mfc.type == MF_TYPE_4K) s = 40;
		for (b = 0; b < 64 && b < card->mfc.block_count; b++)
			n += (int) (card->mfc.block_read >> b & 1);
		if (top != 'f') {
			sprintf(r, "%d/%d", keys_a + keys_b, 2 * s);
			gm_item("i_keys", "Keys found", r);
		}
		sprintf(r, "%d/%d", n, card->mfc.block_count);
		gm_item("i_blk", "Blocks read", r);
	} else if (card->type == NFC_DEV_MF_ULTRALIGHT) {
		sprintf(r, "%d", card->ul_pages);
		gm_item("i_pg", "Pages", r);
	}
	if (t4note) {
		gm_item("i_ndef", "NDEF", "");
		gm_line(t4note);
	}
}

static void w_card(void)
{
	char name[NAMEMAX + 8];

	card_name(name);
	gm_win("card", "list", type_name(card), 0, 0);
	card_rows();
	if (dictnote) gm_line(dictnote);
	gm_item("save", "Save", name);
	gm_show();
	top = 'r';
}

static void w_read(void)
{
	if (!have_reader()) { no_reader(); return; }
	if (do_read(NULL) < 0) {
		w_note("Read", "No card found", "Hold a card to the back");
		return;
	}
	w_card();
}

static int entry(int i, char *out)	/* the i-th .nfc file, or the count */
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	if ((d = opendir(NFCDIR)) == NULL) return i < 0 ? 0 : -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || !ends(e->d_name, ".nfc")) continue;
		if (n++ == i) {
			strncpy(out, e->d_name, PATHMAX - 1);
			out[PATHMAX - 1] = 0;
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return i < 0 ? n : -1;
}

static void w_saved(int first)
{
	char key[6], name[PATHMAX];
	int n = entry(-1, NULL), i;

	if (first < 0 || first >= n) first = 0;
	lfirst = first;
	top = 's';
	if (n == 0) {
		gm_win("saved", "list", "Saved", 0, 0);
		gm_line("No saved cards");
		gm_show();
		return;
	}
	gm_win("saved", "list", "Saved", n, first);
	for (i = first; i < first + PAGE && i < n; i++) {
		if (entry(i, name) < 0) break;
		sprintf(key, "f%04d", i % 10000);
		name[strlen(name) - 4] = 0;	/* without .nfc */
		name[30] = 0;
		gm_item(key, name, "");
	}
	gm_show();
}

static void w_file(void)
{
	char name[NAMEMAX + 8];
	const char *err = NULL;

	strncpy(name, base(file), sizeof name - 1);
	name[sizeof name - 1] = 0;
	top = 'f';
	gm_win("file", "list", name, 0, 0);
	if (card_new() < 0 || nfc_file_load(card, file, &err) != 0) {
		gm_line("Cannot read the file");
		if (err) gm_line(err);
	} else card_rows();
	card_free();
	gm_item("del", "Delete", "");
	gm_show();
}

static void w_confirm(void)
{
	gm_win("delq", "confirm", "Delete", 0, 0);
	gm_line(base(file));
	gm_action("yes", "Delete");
	gm_show();
	top = 'c';
}

static void w_extra(void)
{
	gm_win("extra", "list", "Extra actions", 0, 0);
	gm_item("keys", "MIFARE Classic keys", "");
	gm_show();
	top = 'x';
}

static void dict_row(const char *key, const char *label, const char *path)
{
	mf_dict_t d;
	char r[16];

	if (mf_dict_open(&d, path) != 0) { gm_item(key, label, "none"); return; }
	sprintf(r, "%lu keys", mf_dict_count(&d));
	mf_dict_close(&d);
	gm_item(key, label, r);
}

static void w_keys(void)
{
	gm_win("keys", "list", "MIFARE Classic keys", 0, 0);
	dict_row("sys", "System dict", MF_DICT_SYS);
	dict_row("user", "User dict", MF_DICT_USER);
	gm_line("Your lists: nfc/assets/");
	gm_show();
	top = 'k';
}

/* The services a card answered to: ATQA/SAK and ATS first, then one row
 * per service (its name; its group on the right).  OK on one tells what
 * it is. */
static void w_scan(void)
{
	char r[48], k[6];
	unsigned i;

	gm_win("scan", "list", "Services", 0, 0);
	hex(r, scan->atqa, 2);
	sprintf(r + 4, " %02X", scan->sak);
	gm_item("s_as", "ATQA SAK", r);
	hex(r, scan->ats, scan->ats_len > 10 ? 10 : scan->ats_len);
	gm_item("s_ats", "ATS", r);
	for (i = 0; i < scan->nhits && i < AID_HITS_MAX; i++) {
		sprintf(k, "h%02u", i);
		gm_item(k, scan->hit[i].e.name[0] ? scan->hit[i].e.name : "?",
			scan->hit[i].e.group);
	}
	if (scan->nhits > AID_HITS_MAX) {
		sprintf(r, "+%u more (nfc scan)", scan->nhits - AID_HITS_MAX);
		gm_line(r);
	}
	if (scan->nhits == 0) gm_line("No service answered");
	sprintf(r, "%u AIDs tried%s", scan->tried, scan->lost ? ", card lost" : "");
	gm_line(r);
	gm_show();
	top = 'S';
}

/* What a found service is: its group's line from the dictionary. */
static void w_hit(unsigned i)
{
	aid_hit_t *h = &scan->hit[i];
	char d[AID_DESC_MAX], a[2 * AID_MAX + 1], *sp;

	gm_win("hit", "text", h->e.group[0] ? h->e.group : "Service", 0, 0);
	gm_line(h->e.name);
	if (aid_dict_group_desc(scandict, h->e.group, d, sizeof d) == 0) {
		/* two lines of about 30 characters, cut at a blank */
		sp = strlen(d) > 30 ? strchr(d + 20, ' ') : NULL;
		if (sp && sp - d <= 32) { *sp = 0; gm_line(d); gm_line(sp + 1); }
		else gm_line(d);
	}
	aid_hex(a, h->e.aid, h->e.aid_len);
	gm_line(a);
	sprintf(d, "SW %04X: %s", h->sw, sw_text(h->sw));
	gm_line(d);
	gm_show();
	top = 'h';
}

static void w_scanrun(void)
{
	char sak[24];
	int r;

	if (!have_reader()) { no_reader(); return; }
	if (lb) nfcloop_card(lb, NFCLOOP_T4T);	/* -L: the Type 4 card */
	r = do_scan(scan_dict());
	if (r == AID_SCAN_NODICT)
		w_note("Scan services", "No AID dictionary", "nfc/assets/aid_scan.nfc");
	else if (r == NFC_ERR_NOTSUP) {
		sprintf(sak, "(SAK %02X: no apps)", scan->sak);
		w_note("Scan services", "Not an ISO 14443-4 card", sak);
	} else if (r != NFC_OK)
		w_note("Scan services", "No card found", "Hold a card to the back");
	else {
		w_scan();
		return;
	}
	scan_free();
}

static void w_add(void)
{
	gm_win_live("add", "list", "Add manually", 1);
	gm_enum("type", "Type", add_type, "Classic 1K|NTAG215");
	gm_input("uid", "UID (hex)", add_uid, 20);
	gm_input("name", "Name", add_name, NAMEMAX);
	gm_item("make", "Save", "");
	gm_show();
	top = 'a';
}

static void redraw(void)
{
	switch (top) {
	case 's': w_saved(lfirst); break;
	case 'f': case 'c': w_file(); break;
	case 'x': w_extra(); break;
	case 'k': w_keys(); break;
	case 'a': w_add(); break;
	case 'S': case 'h': if (scan) { w_scan(); break; } w_main(); break;
	case 'r': if (card) { w_card(); break; } /* FALLTHROUGH */
	default: w_main();
	}
}

/* ---- events ---- */

static void ok(const char *win, const char *key)
{
	char p[PATHMAX], name[PATHMAX];

	if (!strcmp(win, "main")) {
		if (!strcmp(key, "read")) w_read();
		else if (!strcmp(key, "detect"))
			w_note("Detect reader", "Needs /dev/nfc to listen",
			       "(the driver, Phase 13)");
		else if (!strcmp(key, "saved")) w_saved(0);
		else if (!strcmp(key, "extra")) w_extra();
		else if (!strcmp(key, "add")) w_add();
		else if (!strcmp(key, "scan")) w_scanrun();
		else gm_busy(0);
	} else if (!strcmp(win, "scan") && key[0] == 'h' && scan
		   && (unsigned) atoi(key + 1) < scan->nhits
		   && atoi(key + 1) < AID_HITS_MAX) {
		w_hit((unsigned) atoi(key + 1));
	} else if (!strcmp(win, "card") && !strcmp(key, "save") && card) {
		card_name(name);
		if (do_save(name, p) == 0) w_note("Saved", base(p), NULL);
		else w_note("Save", "Cannot save in", NFCDIR);
	} else if (!strcmp(win, "saved") && key[0] == 'f') {
		if (entry(atoi(key + 1), name) < 0
		    || strlen(NFCDIR) + 2 + strlen(name) > PATHMAX) {
			w_saved(lfirst);
			return;
		}
		sprintf(file, "%s/%s", NFCDIR, name);
		w_file();
	} else if (!strcmp(win, "file") && !strcmp(key, "del")) w_confirm();
	else if (!strcmp(win, "delq") && !strcmp(key, "yes")) {
		(void) unlink(file);
		w_saved(lfirst);
	} else if (!strcmp(win, "extra") && !strcmp(key, "keys")) w_keys();
	else if (!strcmp(win, "add") && !strcmp(key, "make")) {
		if (make_card() < 0) {
			w_note("Add manually", "UID: 4 or 7 bytes in hex",
			       "(NTAG215: 7)");
			return;
		}
		if (do_save(add_name[0] ? add_name : "Manual", p) == 0)
			w_note("Saved", base(p), NULL);
		else w_note("Save", "Cannot save in", NFCDIR);
		card_free();
	} else gm_busy(0);
}

static void submit(char **f)
{
	char *k, *v;
	int i;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		if (!strcmp(k, "type")) add_type = atoi(v) ? 1 : 0;
		else if (!strcmp(k, "uid")) {
			strncpy(add_uid, v, sizeof add_uid - 1);
			add_uid[sizeof add_uid - 1] = 0;
		} else if (!strcmp(k, "name") && !strchr(v, '/') && v[0] != '.') {
			strncpy(add_name, v, NAMEMAX);
			add_name[NAMEMAX] = 0;
		}
	}
}

static void back(const char *win)
{
	if (!strcmp(win, "card")) { card_free(); top = 'm'; }
	else if (!strcmp(win, "file")) top = 's';
	else if (!strcmp(win, "delq")) top = 'f';
	else if (!strcmp(win, "keys")) top = 'x';
	else if (!strcmp(win, "hit")) top = 'S';
	else if (!strcmp(win, "scan")) { scan_free(); top = 'm'; }
	else if (!strcmp(win, "note")) top = card ? 'r' : 'm';
	else top = 'm';
}

/* ---- nfc scan: the scanner without a screen ---- */

static int cli_scan(const char *dict)
{
	char a[2 * AID_MAX + 1], u[21];
	unsigned i;
	int r;

	if (!have_reader()) {
		printf("nfc scan: %s\n", devwhy == 1 ? "not in group nfc"
		       : "no /dev/nfc");
		return 1;
	}
	if (lb) nfcloop_card(lb, NFCLOOP_T4T);
	r = do_scan(dict);
	if (r == AID_SCAN_NODICT) {
		printf("nfc scan: no dictionary %s\n", dict);
		return 1;
	}
	if (r != NFC_OK && r != NFC_ERR_NOTSUP) {
		printf("nfc scan: no card\n");
		return 1;
	}
	hex(u, scan->uid, scan->uid_len);
	printf("UID %s ATQA %02X%02X SAK %02X\n", u, scan->atqa[0],
	       scan->atqa[1], scan->sak);
	if (r == NFC_ERR_NOTSUP) {
		printf("nfc scan: not an ISO 14443-4 card\n");
		return 1;
	}
	hex(a, scan->ats, scan->ats_len);
	printf("ATS %s\n", a);
	for (i = 0; i < scan->nhits && i < AID_HITS_MAX; i++) {
		aid_hex(a, scan->hit[i].e.aid, scan->hit[i].e.aid_len);
		printf("%-12s %-32s %04X %s\n", scan->hit[i].e.group, a,
		       scan->hit[i].sw, scan->hit[i].e.name);
	}
	printf("nfc scan: %u AIDs tried, %u answered%s (%s)\n", scan->tried,
	       scan->nhits, scan->lost ? ", card lost" : "", dict);
	scan_free();
	return 0;
}

/* ---- nfc -t: the flow without a screen ---- */

static int nok, nbad;

static void t_check(int c, const char *what)
{
	printf("%s %s\n", c ? "ok  " : "BAD ", what);
	if (c) nok++; else nbad++;
}

static int selftest(void)
{
	static const char tmp[] = "/tmp/nfct.nfc";
	nfc_device_t *d2;
	const char *err = NULL;
	char *brk0 = sbrk(0);
	int r;

	r = do_read(dict_t);
	t_check(r == 0 && card->type == NFC_DEV_MF_CLASSIC
		&& card->mfc.type == MF_TYPE_1K && card->uid_len == 4,
		"loopback: MIFARE Classic 1K identified");
	t_check(r == 0 && keys_a == 16 && keys_b == 0,
		"Classic: key A of all 16 sectors from the dictionary, no key B");
	t_check(r == 0 && nblk == 64 && card->mfc.block[5].data[3] == 5 * 16 + 3,
		"Classic: 64 blocks read and decrypted");
	t_check(r == 0 && nfc_file_save(card, tmp) == 0,
		"Classic: saved as a .nfc file");
	if (r == 0 && (d2 = malloc(sizeof *d2)) != NULL) {
		t_check(nfc_file_load(d2, tmp, &err) == 0
			&& memcmp(d2->mfc.block, card->mfc.block, 64 * 16) == 0
			&& memcmp(d2->uid, card->uid, 4) == 0,
			"Classic: the file reads back the same");
		free(d2);
	}
	r = do_read(dict_t);
	t_check(r == 0 && card->type == NFC_DEV_MF_ULTRALIGHT
		&& !strcmp(card->ul_subtype, "NTAG215") && card->uid_len == 7,
		"loopback: NTAG215 identified (7-byte UID)");
	t_check(r == 0 && nblk == 135 && card->ul_page[3][0] == 0xE1,
		"NTAG215: 135 pages read");
	r = do_read(dict_t);
	t_check(r == 0 && card->type == NFC_DEV_ISO14443_3A
		&& card->sak == 0x20 && card->uid_len == 7,
		"loopback: ISO 14443-4A card identified (SAK 20)");
	t_check(r == 0 && t4note
		&& !strcmp(t4note, "URI https://example.org/minix/t"),
		"Type 4 Tag: NDEF URI read");
	{
		static const char sd[] = "/tmp/nfcscan.txt";
		FILE *fp = fopen(sd, "w");

		if (fp) {
			fputs("[Payments] Bank cards\nA0000000031010 Visa\n"
			      "A0000000041010 Mastercard\n[Access] Keys\n"
			      "A0000006472F0001 FIDO U2F\nzz bad line\n", fp);
			fclose(fp);
		}
		r = do_scan(sd);
		t_check(r == NFC_OK && scan->tried == 3 && scan->nhits == 2
			&& !strcmp(scan->hit[0].e.name, "Visa")
			&& !strcmp(scan->hit[1].e.group, "Access")
			&& scan->bad_lines == 1 && scan->ats_len == 5,
			"scan: Visa and FIDO U2F found, Mastercard not");
		scan_free();
		lbnext = NFCLOOP_MFC1K;
		r = do_read(dict_t);
		r = r == 0 ? do_scan(sd) : -1;
		t_check(r == NFC_ERR_NOTSUP,
			"scan: a MIFARE Classic has no applications");
		scan_free();
		unlink(sd);
	}
	nfcloop_card(lb, NFCLOOP_NONE);
	lbnext = NFCLOOP_NONE;
	t_check(do_read(dict_t) < 0 && card == NULL, "empty field: no card");
	strcpy(add_uid, "04112233445566");
	add_type = 1;
	t_check(make_card() == 0 && nfc_file_save(card, tmp) == 0
		&& nfc_file_load(card, tmp, &err) == 0 && card->ul_pages == 135
		&& card->uid[6] == 0x66, "Add manually: NTAG215 saved and read");
	add_type = 0;
	strcpy(add_uid, "XYZ");
	t_check(make_card() < 0, "Add manually: a bad UID refused");
	card_free();
	unlink(tmp);
	printf("nfc: heap peak %ld B\n", (long) ((char *) sbrk(0) - brk0));
	printf("nfc: %d ok, %d bad\n", nok, nbad);
	return nbad != 0;
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];

	if (argc > 1 && !strcmp(argv[1], "-c")) {
		nfc_open();
		if (nfcfd >= 0) { printf("nfc: %s opened\n", DEV_NFC); return 0; }
		printf("nfc: %s\n", devwhy == 1 ? "not in group nfc"
		       : "no /dev/nfc");
		return devwhy;
	}
	if (argc > 1 && !strcmp(argv[1], "-r")) {	/* read once, no screen */
		int r;

		nfc_open();
		if (nfcfd < 0) { printf("nfc: no %s\n", DEV_NFC); return 1; }
		r = do_read(NULL);
		if (r < 0 || card == NULL) {
			printf("nfc: no card\n");
			return 1;
		}
		/* the family as the stock scanner names it, then our name */
		/* SAK 00 with no answer to an Ultralight read: a plain
		 * ISO 14443-3A tag, as the stock scanner says */
		if (card->type == NFC_DEV_MF_ULTRALIGHT && nblk == 0)
			printf("nfc: ISO14443-3A, UID");
		else printf("nfc: %s%s, UID", card->type == NFC_DEV_MF_ULTRALIGHT
			? "NTAG/Ultralight " : "", type_name(card));
		for (r = 0; r < card->uid_len; r++) printf(" %02X", card->uid[r]);
		printf(", ATQA %02X%02X, SAK %02X\n", card->atqa[0],
			card->atqa[1], card->sak);
		if (card->type == NFC_DEV_MF_ULTRALIGHT && nblk > 0)
			printf("nfc: pages read %d\n", nblk);
		else if (card->type == NFC_DEV_MF_CLASSIC) {
			printf("nfc: read 0, keys %d/%d, blocks %d/%d\n",
			       keys_a + keys_b, 2 * (card->mfc.block_count / 4),
			       nblk, (int) card->mfc.block_count);
			printf("nfc: blocks read %08lx%08lx\n",
				(unsigned long) (card->mfc.block_read >> 32),
				(unsigned long) card->mfc.block_read);
		} else if (t4note != NULL) printf("nfc: %s\n", t4note);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "-t")) {
		loopback = 1;
		dict_t = argc > 2 ? argv[2] : MF_DICT_SYS;
		nfc_open();
		return selftest();
	}
	if (argc > 1 && !strcmp(argv[1], "scan")) {
		int a = 2;

		if (argc > a && !strcmp(argv[a], "-L")) { loopback = 1; a++; }
		nfc_open();
		return cli_scan(argc > a ? argv[a] : scan_dict());
	}
	if (argc > 1 && !strcmp(argv[1], "-L")) loopback = 1;
	else if (argc > 1) {
		fprintf(stderr, "Usage: nfc [-L | -t [DICT] | -c | -r | scan [-L] [DICT]]\n");
		return 2;
	}
	nfc_open();
	if (gm_init(buf, sizeof buf) < 0) {
		fprintf(stderr, "nfc: start it from the launcher\n");
		return 1;
	}
	w_main();
	for (;;) {
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK: ok(f[1], f[2]); break;
		case GM_SUBMIT: submit(f); break;
		case GM_BACK: back(f[1]); break;
		case GM_PAGE: if (top == 's') w_saved(atoi(f[2])); break;
		case GM_REDRAW: redraw(); break;
		case GM_CLOSE: return 0;
		}
	}
}
