/* SPDX-License-Identifier: GPL-3.0-or-later
 * u2fstore - the U2F app's files, see u2fstore.h.  No stdio, no malloc:
 * each file is small and read whole into a buffer on the stack. */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>		/* rename() only */
#include <string.h>
#include <unistd.h>
#include "u2f.h"
#include "u2fstore.h"

#define PATHMAX		128
#define TXTMAX		384

#define T_CERTKEY	"Flipper U2F Certificate Key File"
#define T_DEVKEY	"Galtzagorriak U2F Device Key File"
#define T_COUNTER	"Galtzagorriak U2F Counter File"

const char *u2fs_error(int e)
{
	switch (e) {
	case U2FS_NOCERT:    return "no assets/cert.der";
	case U2FS_NOKEY:     return "no assets/cert_key.u2f";
	case U2FS_ENCRYPTED: return "cert_key.u2f is encrypted (stock)";
	case U2FS_DEVKEY:    return "minix.key cannot be used";
	case U2FS_COUNTER:   return "minix.cnt cannot be read";
	case U2FS_WRITE:     return "cannot write to the card";
	}
	return "";
}

static int path(char *p, const char *dir, const char *name)
{
	if (strlen(dir) + 1 + strlen(name) + 5 > PATHMAX) return -1;
	strcpy(p, dir);
	strcat(p, "/");
	strcat(p, name);
	return 0;
}

/* The whole file into b (at most max - 1 bytes, NUL ended): its length,
 * -1 when it does not exist, -2 when it cannot be read or is too big. */
static int slurp(const char *p, char *b, int max)
{
	int fd, n = 0, k = 0;

	if ((fd = open(p, O_RDONLY)) < 0) return -1;
	while (k < max && (n = (int) read(fd, b + k, (size_t) (max - k))) > 0)
		k += n;
	close(fd);
	if (n < 0 || k >= max) return -2;
	b[k] = 0;
	return k;
}

/* The line "key: value" of text: a pointer to the value, or NULL. */
static const char *value(const char *t, const char *key)
{
	size_t kl = strlen(key);

	while (*t) {
		if (strncmp(t, key, kl) == 0 && t[kl] == ':') {
			t += kl + 1;
			while (*t == ' ') t++;
			return t;
		}
		while (*t && *t != '\n') t++;
		if (*t) t++;
	}
	return NULL;
}

static int hexd(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int u2fs_hex(const char *t, const char *key, unsigned char *out, unsigned n)
{
	const char *v = value(t, key);
	unsigned i;
	int a, b;

	if (v == NULL) return -1;
	for (i = 0; i < n; i++) {
		while (*v == ' ') v++;
		if ((a = hexd(v[0])) < 0 || (b = hexd(v[1])) < 0) return -1;
		out[i] = (unsigned char) (a << 4 | b);
		v += 2;
	}
	while (*v == ' ' || *v == '\r') v++;
	return *v == 0 || *v == '\n' ? 0 : -1;
}

int u2fs_num(const char *t, const char *key, unsigned long *r)
{
	const char *v = value(t, key);
	unsigned long x = 0;
	int k = 0;

	if (v == NULL) return -1;
	while (*v >= '0' && *v <= '9') {
		if (++k > 10) return -1;
		x = x * 10 + (unsigned long) (*v++ - '0');
	}
	while (*v == ' ' || *v == '\r') v++;
	if (k == 0 || (*v != 0 && *v != '\n') || x > 0xFFFFFFFFUL) return -1;
	*r = x;
	return 0;
}

static int header(const char *t, const char *type)
{
	const char *v = value(t, "Filetype");
	unsigned long ver;
	size_t l = strlen(type);

	return v != NULL && strncmp(v, type, l) == 0
	       && (v[l] == '\n' || v[l] == '\r' || v[l] == 0)
	       && u2fs_num(t, "Version", &ver) == 0 && ver == 1 ? 0 : -1;
}

/* text into dir/name: name.tmp, mode 0600, sync, rename. */
static int put(const char *dir, const char *name, const char *text)
{
	char p[PATHMAX], tmp[PATHMAX];
	int fd, n = (int) strlen(text), ok;

	if (path(p, dir, name) < 0) return -1;
	strcpy(tmp, p);
	strcat(tmp, ".tmp");
	(void) unlink(tmp);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) return -1;
	ok = write(fd, text, (size_t) n) == n;
	if (close(fd) < 0) ok = 0;
	if (ok) sync();
	if (!ok || rename(tmp, p) < 0) {
		(void) unlink(tmp);
		return -1;
	}
	return 0;
}

static char *ultoa10(char *e, unsigned long v)	/* digits end at e */
{
	*e = 0;
	do *--e = (char) ('0' + v % 10); while ((v /= 10) > 0);
	return e;
}

int u2fs_counter_save(const char *dir, unsigned long c)
{
	char t[96], d[12];

	strcpy(t, "Filetype: " T_COUNTER "\nVersion: 1\nCounter: ");
	strcat(t, ultoa10(d + 11, c));
	strcat(t, "\n");
	return put(dir, "minix.cnt", t) < 0 ? -1 : 0;
}

static int devkey_save(const char *dir, const unsigned char *k)
{
	static const char hx[] = "0123456789ABCDEF";
	char t[160], *p;
	int i;

	strcpy(t, "Filetype: " T_DEVKEY "\nVersion: 1\nData:");
	p = t + strlen(t);
	for (i = 0; i < 32; i++) {
		*p++ = ' ';
		*p++ = hx[k[i] >> 4];
		*p++ = hx[k[i] & 15];
	}
	*p++ = '\n';
	*p = 0;
	i = put(dir, "minix.key", t);
	memset(t, 0, sizeof t);
	return i;
}

int u2fs_load(const char *dir, struct u2f *u, unsigned char *cert,
	      void (*rnd)(unsigned char *b, unsigned n))
{
	char p[PATHMAX], t[TXTMAX];
	unsigned long type, c;
	int n, fd;

	/* the certificate: DER, a SEQUENCE */
	if (path(p, dir, "assets/cert.der") < 0) return U2FS_NOCERT;
	if ((fd = open(p, O_RDONLY)) < 0) return U2FS_NOCERT;
	n = (int) read(fd, cert, U2F_MAXCERT);
	if (n == U2F_MAXCERT && read(fd, t, 1) == 1) n = -1;	/* too big */
	close(fd);
	if (n < 16 || cert[0] != 0x30) return U2FS_NOCERT;
	u->cert = cert;
	u->cert_len = (unsigned) n;

	/* its key */
	if (path(p, dir, "assets/cert_key.u2f") < 0
	    || slurp(p, t, sizeof t) < 0 || header(t, T_CERTKEY) < 0
	    || u2fs_num(t, "Type", &type) < 0)
		return U2FS_NOKEY;
	if (type != 2) return U2FS_ENCRYPTED;
	if (u2fs_hex(t, "Data", u->cert_key, 32) < 0) return U2FS_NOKEY;

	/* the device key: made once */
	if (path(p, dir, "minix.key") < 0) return U2FS_DEVKEY;
	n = slurp(p, t, sizeof t);
	if (n == -1) {
		rnd(u->device_key, 32);
		if (devkey_save(dir, u->device_key) < 0) return U2FS_WRITE;
	} else if (n < 0 || header(t, T_DEVKEY) < 0
		   || u2fs_hex(t, "Data", u->device_key, 32) < 0)
		return U2FS_DEVKEY;

	/* the counter: 0 only when there is no file */
	if (path(p, dir, "minix.cnt") < 0) return U2FS_COUNTER;
	n = slurp(p, t, sizeof t);
	if (n == -1) c = 0;
	else if (n < 0 || header(t, T_COUNTER) < 0
		 || u2fs_num(t, "Counter", &c) < 0)
		return U2FS_COUNTER;
	u->counter = c;
	memset(t, 0, sizeof t);
	return U2FS_OK;
}
