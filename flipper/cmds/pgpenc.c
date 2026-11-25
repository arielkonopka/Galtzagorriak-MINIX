/* pgpenc - encrypt a file to an OpenPGP public key (issue: user request
 * 2026-11-25).  Only the PUBLIC key is on the Flipper; the file can be
 * opened on a PC only with the secret key (for a YubiKey: the card and its
 * PIN): `gpg -d FILE.gpg`.
 *
 *	pgpenc [-a] -k KEYFILE [-o OUT] FILE
 *
 *	-k KEYFILE	the recipient's public key, as `gpg --export` writes it
 *			(binary or armored); the last RSA subkey is used (an
 *			encryption subkey), else the primary key.  Export just
 *			the one you need to keep it small.  RSA 1024-4096 bits;
 *			ECC keys are not supported yet.
 *	-o OUT		the output file (default: FILE.gpg, or FILE.asc with -a)
 *	-a		ASCII armor instead of binary
 *	-i		print the key id and the fingerprint, and exit (no FILE)
 *
 * The file is read and written in 512 byte pieces: any size, a few KB of
 * RAM.  The random numbers (session key, padding) come from /dev/random.
 * Reading the key from the YubiKey over NFC will write the same KEYFILE
 * (a later step: the NFC part is the device lane's).
 * Exit status: 0, 1 an error, 2 usage.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "pgpenc.h"

#ifdef HOST
#define RANDOM_DEV "/dev/urandom"
#else
#define RANDOM_DEV "/dev/random"
#endif

static int rd(void *arg, uint8_t *b, size_t n)
{
	return read(*(int *) arg, b, n);
}

static int wr(void *arg, const uint8_t *b, size_t n)
{
	int fd = *(int *) arg;
	size_t o = 0;
	int r;

	while (o < n) {
		if ((r = write(fd, b + o, n - o)) <= 0) return -1;
		o += (size_t) r;
	}
	return 0;
}

static int rng(void *arg, uint8_t *b, size_t n)
{
	size_t o = 0;
	int r;

	while (o < n) {
		if ((r = read(*(int *) arg, b + o, n - o)) <= 0) return -1;
		o += (size_t) r;
	}
	return 0;
}

struct ends { int in, out, rnd; };

static int e_rd(void *a, uint8_t *b, size_t n) { return rd(&((struct ends *) a)->in, b, n); }
static int e_wr(void *a, const uint8_t *b, size_t n) { return wr(&((struct ends *) a)->out, b, n); }
static int e_rng(void *a, uint8_t *b, size_t n) { return rng(&((struct ends *) a)->rnd, b, n); }

static int bits(const struct pgp_pub *k)
{
	int b = k->nlen * 8, t;

	for (t = k->n[0]; t < 128; t <<= 1) b--;
	return b;
}

int main(int argc, char **argv)
{
	struct pgp_pub key;
	static uint8_t kb[3072];
	struct ends e;
	struct pgp_io io;
	const char *keyf = NULL, *outf = NULL, *file = NULL, *base;
	char oname[128];
	int i, armor = 0, info = 0, kfd, n, r;
	struct stat st;
	uint32_t mtime;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-a") == 0) armor = 1;
		else if (strcmp(argv[i], "-i") == 0) info = 1;
		else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) keyf = argv[++i];
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outf = argv[++i];
		else if (argv[i][0] != '-' && file == NULL) file = argv[i];
		else goto usage;
	}
	if (keyf == NULL || (file == NULL && !info)) goto usage;
	if ((kfd = open(keyf, O_RDONLY)) < 0) { perror(keyf); return 1; }
	n = read(kfd, kb, sizeof kb);
	close(kfd);
	if (n <= 0 || pgp_key_parse(kb, (size_t) n, &key) < 0) {
		fprintf(stderr, "pgpenc: %s: no usable RSA public key (binary or armored,"
			" at most %d bytes: `gpg --export-options export-minimal`)\n",
			keyf, (int) sizeof kb);
		return 1;
	}
	if (info) {
		printf("key id ");
		for (i = 0; i < 8; i++) printf("%02X", key.keyid[i]);
		printf("\nfingerprint ");
		for (i = 0; i < 20; i++) printf("%02X", key.fpr[i]);
		printf("\nRSA %d bits, e %lu\n", bits(&key), (unsigned long) key.e);
		return 0;
	}
	if (outf == NULL) {
		if (strlen(file) + 5 > sizeof oname) goto usage;
		strcpy(oname, file);
		strcat(oname, armor ? ".asc" : ".gpg");
		outf = oname;
	}
	if ((e.in = open(file, O_RDONLY)) < 0) { perror(file); return 1; }
	if ((e.rnd = open(RANDOM_DEV, O_RDONLY)) < 0) { perror(RANDOM_DEV); return 1; }
	if ((e.out = open(outf, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) {
		perror(outf);
		return 1;
	}
	base = strrchr(file, '/');
	base = base != NULL ? base + 1 : file;
	io.arg = &e;
	io.read = e_rd;
	io.write = e_wr;
	io.rng = e_rng;
	mtime = fstat(e.in, &st) == 0 && st.st_mtime > 0 ? (uint32_t) st.st_mtime : 0;
	r = armor ? pgp_encrypt_armor(&key, &io, base, mtime)
		  : pgp_encrypt(&key, &io, base, mtime);
	close(e.in);
	close(e.rnd);
	if (close(e.out) < 0) r = -1;
	if (r < 0) {
		fprintf(stderr, "pgpenc: failed (read or write error)\n");
		unlink(outf);
		return 1;
	}
	return 0;
usage:
	fprintf(stderr, "usage: pgpenc [-a] -k KEYFILE [-o OUT] FILE\n"
			"       pgpenc -k KEYFILE -i\n");
	return 2;
}
