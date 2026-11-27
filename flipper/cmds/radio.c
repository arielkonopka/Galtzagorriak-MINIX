/* radio - the CPU2 stack images on the SD card (spec R16.0.2, R2b.1a;
 * task T30a, issue #388).
 *
 *	radio			flash layout: SFSA, programs, cache, slot
 *	radio list [-F X.Y.Z]	the images in /home/flipper/radio, checked
 *	radio add [-F X.Y.Z] NAME|DIR
 *				(root) check /home/flipper/radio/NAME (manifest,
 *				size, SHA-256, FUS, R2b.1a limits), then
 *				rewrite /etc/radio.slot
 *	radio rm NAME		(root) delete /home/flipper/radio/NAME, rewrite the
 *				slot
 *	radio write NAME	(root; T30b) check NAME again, write its image
 *				at the manifest's address and read it back.
 *				Only with FUS alone in CPU2's flash (the old
 *				stack deleted): the kernel refuses any other
 *				place (the radio window)
 *	radio wtest		(root) try the writer on the 16 KB guard
 *				under CPU2's flash: a pattern, read back
 *
 * /etc/radio.slot holds one number: the lowest ST install address of the
 * images on the card that pass the checks (missing: no slot).  The flash
 * layout keeps installs below it (R2b.1a).  -F gives the FUS version the
 * device has; until the FUS client (T30b) reads it, it is not checked
 * without -F.  `radio use`, `repair` and `fus` are T30b's (lane:local):
 * nothing here talks to CPU2 or writes flash.
 * RAM: one manifest (~170 B) and a 512 B read buffer on the stack while
 * it runs; nothing resident.
 */
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "radio.h"
#include "modem.h"
#ifndef HOST
#include <lib.h>
#include <minix/type.h>
#endif

/* The tree stock calls /ext is /home/flipper here (as rpcd, R15.5): the
 * spec's /ext/radio.  It was "/ext/radio", which no card has. */
#define DIR_RD	"/home/flipper/radio"
#define FILE_SLOT "/etc/radio.slot"

static char root[128];			/* HOST tests: RADIO_ROOT prefix */
static char imgpath[192];		/* check(): the image it hashed */
static const char *fus;			/* -F */

static const char *path(char *buf, size_t bs, const char *a, const char *b,
			const char *c)
{
	snprintf(buf, bs, "%s%s%s%s%s%s", root, a, b ? "/" : "", b ? b : "",
		 c ? "/" : "", c ? c : "");
	return buf;
}

static int usage(void)
{
	fprintf(stderr, "usage: radio [list [-F X.Y.Z] | add [-F X.Y.Z] NAME |"
		" rm NAME]\n");
	return 2;
}

static unsigned long slot_read(void)
{
	char p[192], b[24];
	int fd, n;

	if ((fd = open(path(p, sizeof(p), FILE_SLOT, NULL, NULL), O_RDONLY))
									< 0)
		return 0;
	n = (int) read(fd, b, sizeof(b) - 1);
	close(fd);
	b[n > 0 ? n : 0] = 0;
	return strtoul(b, NULL, 0);
}

/* Read and check /home/flipper/radio/<name>: 0, RD_E* (+100: no manifest or
 * image).  *m gets the manifest. */
static int check(const char *name, struct rd_man *m)
{
	char p[192], buf[512], img[64];
	struct mdm_sha256 c;
	unsigned char sha[32];
	unsigned long size = 0;
	DIR *d;
	struct dirent *e;
	int fd, n, line, r;

	if ((fd = open(path(p, sizeof(p), DIR_RD, name, "manifest"), O_RDONLY))
									< 0)
		return 100;
	n = (int) read(fd, buf, sizeof(buf));
	close(fd);
	if (n < 0 || n == (int) sizeof(buf)) return RD_EVALUE;
	if ((r = rd_parse(buf, (unsigned long) n, m, &line)) != RD_OK)
		return r;
	if (strcmp(m->name, name) != 0) return RD_ENAME;
	img[0] = 0;			/* the image: the one *.bin */
	if ((d = opendir(path(p, sizeof(p), DIR_RD, name, NULL))) == NULL)
		return 100;
	while ((e = readdir(d)) != NULL) {
		size_t l = strlen(e->d_name);

		if (l > 4 && l < sizeof(img)
		    && strcmp(e->d_name + l - 4, ".bin") == 0) {
			if (img[0]) { closedir(d); return RD_EVALUE; }
			strcpy(img, e->d_name);
		}
	}
	closedir(d);
	if (!img[0]) return 100;
	if ((fd = open(path(p, sizeof(p), DIR_RD, name, img), O_RDONLY)) < 0)
		return 100;
	strcpy(imgpath, p);
	mdm_sha_init(&c);
	while ((n = (int) read(fd, buf, sizeof(buf))) > 0) {
		mdm_sha_add(&c, buf, (unsigned long) n);
		size += (unsigned long) n;
	}
	close(fd);
	if (n < 0) return 100;
	mdm_sha_done(&c, sha);
	return rd_check(m, sha, size, fus);
}

static const char *why(int r)
{
	return r == 100 ? "no manifest or image" : rd_err(r);
}

/* The slot of the good images on the card, written to /etc/radio.slot
 * (removed when none).  The file is on the card's root file system: no
 * internal flash is written. */
static int slot_write(void)
{
	char p[192], b[24];
	struct rd_man m;
	DIR *d;
	struct dirent *e;
	unsigned long s = 0, old = slot_read();
	int fd, n;

	if ((d = opendir(path(p, sizeof(p), DIR_RD, NULL, NULL))) != NULL) {
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			if (check(e->d_name, &m) == RD_OK
			    && (s == 0 || m.addr < s)) s = m.addr;
		}
		closedir(d);
	}
	path(p, sizeof(p), FILE_SLOT, NULL, NULL);
	if (s == 0) {
		if (unlink(p) < 0 && errno != ENOENT) goto err;
	} else {
		n = snprintf(b, sizeof(b), "0x%08lX\n", s);
		if ((fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
			goto err;
		if (write(fd, b, (size_t) n) != n) { close(fd); goto err; }
		if (close(fd) < 0) goto err;
	}
	if (s != old) {
		if (s) printf("radio slot: 0x%08lX (%lu KB below SFSA "
			      "0x%08lX at most)\n", s, (RD_FLASH_END - s) / 1024,
			      RD_FLASH_END);
		else printf("radio slot: none\n");
		printf("takes effect at the next boot\n");
	}
	return 0;
err:
	fprintf(stderr, "radio: %s: %s\n", p, strerror(errno));
	return 1;
}

static int list(void)
{
	char p[192];
	struct rd_man m;
	DIR *d;
	struct dirent *e;
	int r, any = 0;

	if ((d = opendir(path(p, sizeof(p), DIR_RD, NULL, NULL))) == NULL) {
		printf("no images (%s: %s)\n", DIR_RD, strerror(errno));
		return 0;
	}
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		if (!any++) printf("name                 type            "
				   "version  fus    addr       KB\n");
		r = check(e->d_name, &m);
		if (r == RD_OK || m.name[0])
			printf("%-20s %-15s %-8s %-6s 0x%08lX %4lu", m.name,
			       m.type, m.version, m.fus, m.addr,
			       (m.size + 1023) / 1024);
		else printf("%-20s", e->d_name);
		if (r != RD_OK) printf("  (%s)", why(r));
		printf("\n");
	}
	closedir(d);
	if (!any) printf("no images in %s\n", DIR_RD);
	if (!fus) printf("FUS version unknown: give it with -F to check\n");
	return 0;
}

static int add(const char *arg)
{
	const char *name = strrchr(arg, '/');
	struct rd_man m;
	int r;

	name = name ? name + 1 : arg;
	if (!*name) return usage();
	if ((r = check(name, &m)) != RD_OK) {
		fprintf(stderr, "radio: %s: %s\n", name, why(r));
		if (r == RD_EFUS)
			fprintf(stderr, "radio: needs FUS %s; update FUS "
				"first (radio fus)\n", m.fus);
		return 1;
	}
	printf("%s: %s %s at 0x%08lX, %lu KB: ok\n", m.name, m.type,
	       m.version, m.addr, (m.size + 1023) / 1024);
	return slot_write();
}

static int rm(const char *name)
{
	char p[192];
	DIR *d;
	struct dirent *e;

	if (!*name || strchr(name, '/') || name[0] == '.') return usage();
	if ((d = opendir(path(p, sizeof(p), DIR_RD, name, NULL))) == NULL) {
		fprintf(stderr, "radio: %s: %s\n", p, strerror(errno));
		return 1;
	}
	while ((e = readdir(d)) != NULL) {
		char f[192];

		if (e->d_name[0] == '.') continue;
		if (unlink(path(f, sizeof(f), DIR_RD, name, e->d_name)) < 0) {
			fprintf(stderr, "radio: %s: %s\n", f, strerror(errno));
			closedir(d);
			return 1;
		}
	}
	closedir(d);
	if (rmdir(p) < 0) {
		fprintf(stderr, "radio: %s: %s\n", p, strerror(errno));
		return 1;
	}
	return slot_write();
}

#ifndef HOST
/* One piece (8-256 bytes, a multiple of 8) into the radio window, through
 * MM and the kernel's flash path; a piece at a page start erases the page. */
static int piece(unsigned long addr, unsigned char *b, int n)
{
	message m;

	m.m1_i1 = n;
	m.m1_i2 = MI_RADIOWR;
	m.m1_i3 = (int) addr;
	m.m1_p1 = (char *) b;
	return _syscall(MM, MEMINFO, &m);
}

/* size bytes to addr: from the file fd, or (fd < 0) a pattern; then read
 * back through /dev/mem and compared.  0, or 1 with a message. */
static int put(int fd, unsigned long addr, unsigned long size)
{
	unsigned char b[256], c[256];
	unsigned long done;
	int n, i, mem, pass;

	for (pass = 0; pass < 2; pass++) {
		if (fd >= 0 && lseek(fd, (off_t) 0, SEEK_SET) < 0) return 1;
		if (pass == 1 && ((mem = open("/dev/mem", O_RDONLY)) < 0
		    || lseek(mem, (off_t) addr, SEEK_SET) == (off_t) -1)) {
			fprintf(stderr, "radio: /dev/mem: %s\n", strerror(errno));
			return 1;
		}
		for (done = 0; done < size; done += (unsigned long) n) {
			n = size - done > sizeof(b) ? (int) sizeof(b)
						    : (int) (size - done);
			if (fd >= 0) {
				if (read(fd, b, (size_t) n) != n) {
					fprintf(stderr, "radio: the image: %s\n",
						strerror(errno));
					return 1;
				}
			} else for (i = 0; i < n; i++)
				b[i] = (unsigned char) ((done + i) * 7 + 3);
			/* The tail of an image up to a double word is ZEROS, as
			 * stock's furi_hal_flash_program_page and psdb write
			 * it.  With 0xFF (left erased) FUS answered 10.03,
			 * image not authentic: ST's signature covers the
			 * padded length (found on the device, 2026-11-27). */
			while (n % 8 != 0) b[n++] = 0x00;
			if (pass == 0) {
				if (piece(addr + done, b, n) < 0) {
					fprintf(stderr, "radio: write at 0x%08lX: "
						"%s\n", addr + done, strerror(errno));
					return 1;
				}
			} else if (read(mem, c, (size_t) n) != n
				   || memcmp(b, c, (size_t) n) != 0) {
				fprintf(stderr, "radio: 0x%08lX does not read "
					"back\n", addr + done);
				close(mem);
				return 1;
			}
			if ((done & 0x3FFF) == 0) {
				putchar(pass == 0 ? '.' : '+');
				fflush(stdout);
			}
		}
	}
	close(mem);
	printf("\n%lu bytes at 0x%08lX written and read back\n", size, addr);
	return 0;
}

static int wr(const char *name)
{
	struct rd_man m;
	int r, fd;

	if (!*name || strchr(name, '/') || name[0] == '.') return usage();
	if ((r = check(name, &m)) != 0) {
		fprintf(stderr, "radio: %s: %s\n", name, r >= 100
			? "no manifest or image" : rd_err(r));
		return 1;
	}
	if ((fd = open(imgpath, O_RDONLY)) < 0) {
		fprintf(stderr, "radio: %s: %s\n", imgpath, strerror(errno));
		return 1;
	}
	printf("%s %s: %lu bytes to 0x%08lX\n", m.name, m.version, m.size,
		m.addr);
	r = put(fd, m.addr, m.size);
	close(fd);
	return r;
}

static int wtest(void)
{
	struct fcinfo fi;
	message m;

	m.m1_p1 = (char *) &fi;
	m.m1_i2 = MI_FCACHE;
	if (_syscall(MM, MEMINFO, &m) < 0) return 1;
	return put(-1, fi.sfsa - 4 * RD_PAGE, 4 * RD_PAGE);
}

/* Erase pages of the radio window (a piece of ones at each page start):
 * what a try of the writer, or an image FUS did not take, left there. */
static int wclear(const char *a, const char *n)
{
	unsigned char b[8];
	unsigned long addr = strtoul(a, NULL, 16);
	int pages = atoi(n);

	memset(b, 0xFF, sizeof(b));
	if (addr % RD_PAGE != 0 || pages < 1 || pages > 64) return usage();
	for (; pages > 0; pages--, addr += RD_PAGE)
		if (piece(addr, b, 8) < 0) {
			fprintf(stderr, "radio: erase at 0x%08lX: %s\n", addr,
				strerror(errno));
			return 1;
		}
	return 0;
}
#endif

static int status(void)
{
	struct rd_layout l;
	unsigned long slot = slot_read();
#ifndef HOST
	struct fcinfo fi;
	message m;

	m.m1_p1 = (char *) &fi;
	m.m1_i2 = MI_FCACHE;
	if (_syscall(MM, MEMINFO, &m) == 0) {
		printf("CPU2 flash from 0x%08lX; programs 0x%08lX %lu KB, "
		       "cache 0x%08lX\n", fi.sfsa, fi.inst_base,
		       fi.inst_size / 1024, fi.cache_base);
		/* QEMU's flash is at 0: show ST addresses */
		if (fi.sfsa < RD_FLASH) fi.sfsa += RD_FLASH;
		if (rd_layout(fi.sfsa, slot, &l) == 0)
			printf("with the slot (R2b.1a): programs up to "
			       "0x%08lX, cache 0x%08lX-0x%08lX\n",
			       l.inst_end, l.cache, l.cache_end);
	}
#else
	(void) l;
#endif
	if (slot) printf("radio slot: 0x%08lX (%s)\n", slot, FILE_SLOT);
	else printf("radio slot: none\n");
	printf("stack: not probed yet (R2b.10, T30b)\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *r = getenv("RADIO_ROOT");
	int i = 2;

#ifdef HOST
	if (r) snprintf(root, sizeof(root), "%s", r);
#else
	(void) r;
#endif
	if (argc < 2) return status();
	if (argc > 3 && strcmp(argv[2], "-F") == 0) {
		fus = argv[3];
		i = 4;
		if (rd_vercmp(fus, "0.0.0") < 0) return usage();
	}
	if (strcmp(argv[1], "list") == 0 && argc == i) return list();
#ifndef HOST
	if (geteuid() != 0 && (strcmp(argv[1], "add") == 0
			       || strcmp(argv[1], "rm") == 0
			       || strcmp(argv[1], "write") == 0
			       || strcmp(argv[1], "wtest") == 0)) {
		fprintf(stderr, "radio: %s: only root\n", argv[1]);
		return 1;
	}
	if (strcmp(argv[1], "write") == 0 && argc == 3) return wr(argv[2]);
	if (strcmp(argv[1], "wtest") == 0 && argc == 2) return wtest();
	if (strcmp(argv[1], "wclear") == 0 && argc == 4)
		return geteuid() == 0 ? wclear(argv[2], argv[3]) : 1;
#endif
	if (strcmp(argv[1], "add") == 0 && argc == i + 1) return add(argv[i]);
	if (strcmp(argv[1], "rm") == 0 && argc == 3) return rm(argv[2]);
	return usage();
}
