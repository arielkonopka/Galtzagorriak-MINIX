/* nfc_file.c - the stock `.nfc` file format (spec Phase 14, AC14.1),
 * issue #387.  See nfc_file.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A line-at-a-time FlipperFormat reader/writer.  The whole file is never
 * held: each line is parsed into the growing nfc_device model and dropped.
 */
#include "nfc_file.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static int hexval(int c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Parse up to `max` space-separated hex bytes from `s` into `out`; "??"
 * (stock's unknown marker) reads as 0.  Returns the count, -1 on a bad
 * token. */
static int parse_bytes(const char *s, uint8_t *out, int max)
{
	int n = 0;
	while(*s && n < max) {
		while(*s == ' ' || *s == '\t') s++;
		if(*s == '\0' || *s == '\n' || *s == '\r' || *s == '#') break;
		if(s[0] == '?' && s[1] == '?') {
			out[n++] = 0;
			s += 2;
		} else {
			int hi = hexval((unsigned char)s[0]);
			int lo = (hi < 0) ? -1 : hexval((unsigned char)s[1]);
			if(hi < 0 || lo < 0) return -1;
			out[n++] = (uint8_t)(hi << 4 | lo);
			s += 2;
		}
	}
	return n;
}

/* Split "Key: value" -> key (in place, trimmed) and value pointer. */
static int split_kv(char *line, char **key, char **val)
{
	char *colon = strchr(line, ':');
	char *k, *end;
	if(!colon) return -1;
	*colon = '\0';
	k = line;
	while(*k == ' ' || *k == '\t') k++;
	end = colon - 1;
	while(end >= k && (*end == ' ' || *end == '\t')) *end-- = '\0';
	*key = k;
	*val = colon + 1;
	while(**val == ' ' || **val == '\t') (*val)++;
	return 0;
}

int nfc_file_read(nfc_device_t *d, FILE *fp, const char **err)
{
	char line[256];
	int seen_filetype = 0;
	uint16_t max_block = 0;

	memset(d, 0, sizeof *d);
	if(err) *err = NULL;

	while(fgets(line, sizeof line, fp)) {
		char *p = line, *key, *val;
		while(*p == ' ' || *p == '\t') p++;
		if(*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
		if(split_kv(p, &key, &val) != 0) continue;

		if(!strcmp(key, "Filetype")) {
			if(!strstr(val, "Flipper NFC")) {
				if(err) *err = "not a Flipper NFC file";
				return -1;
			}
			seen_filetype = 1;
		} else if(!strcmp(key, "Version")) {
			d->version = atoi(val);
		} else if(!strcmp(key, "Device type")) {
			if(strstr(val, "Mifare Classic"))
				d->type = NFC_DEV_MF_CLASSIC;
			else if(strstr(val, "Ultralight") || strstr(val, "NTAG")) {
				d->type = NFC_DEV_MF_ULTRALIGHT;
				strncpy(d->ul_subtype, val, sizeof d->ul_subtype - 1);
			} else
				d->type = NFC_DEV_ISO14443_3A;
		} else if(!strcmp(key, "UID")) {
			int n = parse_bytes(val, d->uid, sizeof d->uid);
			if(n < 0) { if(err) *err = "bad UID"; return -1; }
			d->uid_len = (uint8_t)n;
		} else if(!strcmp(key, "ATQA")) {
			parse_bytes(val, d->atqa, 2);
		} else if(!strcmp(key, "SAK")) {
			parse_bytes(val, &d->sak, 1);
		} else if(!strcmp(key, "Mifare Classic type")
			  || !strcmp(key, "Mifare Ultralight type")) {
			if(d->type == NFC_DEV_MF_CLASSIC) {
				if(strstr(val, "4K")) d->mfc.type = MF_TYPE_4K;
				else if(strstr(val, "MINI") || strstr(val, "Mini"))
					d->mfc.type = MF_TYPE_MINI;
				else d->mfc.type = MF_TYPE_1K;
			} else {
				strncpy(d->ul_subtype, val, sizeof d->ul_subtype - 1);
			}
		} else if(!strncmp(key, "Block ", 6)) {
			int b = atoi(key + 6);
			if(b >= 0 && b < MF_4K_BLOCKS) {
				uint8_t tmp[MF_BLOCK_SIZE];
				int n = parse_bytes(val, tmp, MF_BLOCK_SIZE);
				if(n == MF_BLOCK_SIZE) {
					memcpy(d->mfc.block[b].data, tmp, MF_BLOCK_SIZE);
					if(b < 64) d->mfc.block_read |= (uint64_t)1 << b;
					if(b + 1 > max_block) max_block = b + 1;
				}
			}
		} else if(!strncmp(key, "Page ", 5)) {
			int pg = atoi(key + 5);
			if(pg >= 0 && pg < NFC_UL_MAX_PAGES) {
				parse_bytes(val, d->ul_page[pg], 4);
				if(pg + 1 > d->ul_pages) d->ul_pages = pg + 1;
			}
		}
		/* unknown keys (Data format version, key maps, counters) are
		 * tolerated and skipped - forward compatible. */
	}

	if(!seen_filetype) { if(err) *err = "missing Filetype"; return -1; }
	if(d->type == NFC_DEV_MF_CLASSIC) {
		if(d->mfc.block_count == 0)
			d->mfc.block_count = max_block ? max_block : mf_block_count(d->mfc.type);
		memcpy(d->mfc.uid, d->uid, d->uid_len);
		d->mfc.uid_len = d->uid_len;
		d->mfc.atqa[0] = d->atqa[0];
		d->mfc.atqa[1] = d->atqa[1];
		d->mfc.sak = d->sak;
	}
	return 0;
}

int nfc_file_load(nfc_device_t *d, const char *path, const char **err)
{
	FILE *fp = fopen(path, "r");
	int r;
	if(!fp) { if(err) *err = "cannot open"; return -1; }
	r = nfc_file_read(d, fp, err);
	fclose(fp);
	return r;
}

static void write_bytes(FILE *fp, const uint8_t *b, int n)
{
	int i;
	for(i = 0; i < n; i++) fprintf(fp, "%s%02X", i ? " " : "", b[i]);
}

int nfc_file_write(const nfc_device_t *d, FILE *fp)
{
	int b;

	fprintf(fp, "Filetype: Flipper NFC device\n");
	fprintf(fp, "Version: %d\n", NFC_FILE_VERSION);
	fprintf(fp, "# Device type can be ISO14443-3A, ISO14443-4A, "
		    "Mifare Classic, Mifare Ultralight, etc.\n");
	switch(d->type) {
	case NFC_DEV_MF_CLASSIC: fprintf(fp, "Device type: Mifare Classic\n"); break;
	case NFC_DEV_MF_ULTRALIGHT:
		fprintf(fp, "Device type: %s\n",
			d->ul_subtype[0] ? d->ul_subtype : "NTAG/Ultralight");
		break;
	default: fprintf(fp, "Device type: ISO14443-3A\n"); break;
	}
	fprintf(fp, "# UID is common for all formats\n");
	fprintf(fp, "UID: "); write_bytes(fp, d->uid, d->uid_len); fprintf(fp, "\n");
	fprintf(fp, "ATQA: "); write_bytes(fp, d->atqa, 2); fprintf(fp, "\n");
	fprintf(fp, "SAK: %02X\n", d->sak);

	if(d->type == NFC_DEV_MF_CLASSIC) {
		const char *t = d->mfc.type == MF_TYPE_4K ? "4K"
			      : d->mfc.type == MF_TYPE_MINI ? "MINI" : "1K";
		fprintf(fp, "# Mifare Classic specific data\n");
		fprintf(fp, "Mifare Classic type: %s\n", t);
		fprintf(fp, "Data format version: 2\n");
		fprintf(fp, "# Mifare Classic blocks, '?\?' means unknown data\n");
		for(b = 0; b < d->mfc.block_count; b++) {
			fprintf(fp, "Block %d: ", b);
			if(b < 64 && !(d->mfc.block_read & ((uint64_t)1 << b)))
				fprintf(fp, "?? ?? ?? ?? ?? ?? ?? ?? "
					    "?? ?? ?? ?? ?? ?? ?? ??");
			else
				write_bytes(fp, d->mfc.block[b].data, MF_BLOCK_SIZE);
			fprintf(fp, "\n");
		}
	} else if(d->type == NFC_DEV_MF_ULTRALIGHT) {
		int pg;
		fprintf(fp, "# Mifare Ultralight specific data\n");
		fprintf(fp, "Data format version: 2\n");
		fprintf(fp, "Pages total: %d\n", d->ul_pages);
		for(pg = 0; pg < d->ul_pages; pg++) {
			fprintf(fp, "Page %d: ", pg);
			write_bytes(fp, d->ul_page[pg], 4);
			fprintf(fp, "\n");
		}
	}
	return ferror(fp) ? -1 : 0;
}

int nfc_file_save(const nfc_device_t *d, const char *path)
{
	FILE *fp = fopen(path, "w");
	int r;
	if(!fp) return -1;
	r = nfc_file_write(d, fp);
	if(fclose(fp) != 0) r = -1;
	return r;
}
