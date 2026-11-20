/* rpcd - the stock Flipper RPC for the mobile app and qFlipper
 * (Galtzagorriak MINIX, spec R15.5, R15.7 profile "phone" steps 1-3;
 * issue #388).
 *
 * Usage:	rpcd [device]
 *
 * Serves one session on the device (default: standard input and output)
 * until it is closed or hung up.  The core (src/lib/rpcd) does the frames
 * and the dispatch; this file is the MINIX platform part: device_info,
 * /dev/power, the clock and the files under /ext, with the rights of the
 * user rpcd runs as.  Transports: a tty or pty today (tools/rpc.py over
 * the USB console, the QEMU serial port); the second USB CDC (R10.1) and
 * the BLE serial service (bled, R15.4) hand rpcd the same byte stream.
 *
 * Memory: the session (rpc_state_size(), about 6 KB) and nanopb's strings
 * come from the heap (CHMEM_rpcd in minix.mk); no flash writes.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <minix/gzversion.h>
#include "flipper.pb.h"
#include "version.h"
#include "rpcd.h"

/* The stock /ext is /home/flipper on MINIX (R5.3, as mtpd); $RPCD_EXT
 * names another directory (tests). */
static const char *ext = "/home/flipper";
static char rp[RPC_PATH_MAX + 80];
static int out = 1;

static const char *real(const char *p)	/* p starts with /ext (core) */
{
  if (strlen(ext) + strlen(p + 4) >= sizeof(rp)) return "/nonexistent";
  strcpy(rp, ext);
  strcat(rp, p + 4);
  return rp;
}

static int st_of(int e)
{
  return e == ENOENT || e == ENOTDIR ? PB_CommandStatus_ERROR_STORAGE_NOT_EXIST
	 : e == EACCES || e == EPERM ? PB_CommandStatus_ERROR_STORAGE_DENIED
	 : PB_CommandStatus_ERROR_STORAGE_INTERNAL;
}

static int m_send(void *c, const unsigned char *b, unsigned n)
{
  int k;

  (void) c;
  while (n > 0) {
	if ((k = write(out, b, n)) <= 0) return -1;
	b += k, n -= k;
  }
  return 0;
}

static int m_devinfo(void *c, struct rpc *r)
{
  char v[8];

  (void) c;
  rpc_kv(r, "format_major", "3");
  rpc_kv(r, "format_minor", "3");
  rpc_kv(r, "hardware_model", "Flipper Zero");
  rpc_kv(r, "firmware_version", GZ_RELEASE);
  rpc_kv(r, "firmware_origin", GZ_NAME " MINIX");
  rpc_kv(r, "firmware_base", GZ_BASE);
  sprintf(v, "%d", FPB_MAJOR);
  rpc_kv(r, "protobuf_version_major", v);
  sprintf(v, "%d", FPB_MINOR);
  return rpc_kv(r, "protobuf_version_minor", v);
}

static int m_power(void *c, struct rpc *r)
{
  char line[160], v[16];
  int fd, n, soc, mv, ma;

  (void) c;
  if ((fd = open("/dev/power", O_RDONLY)) < 0) return 0;	/* no pairs */
  n = read(fd, line, sizeof(line) - 1);
  close(fd);
  if (n <= 0) return 0;
  line[n] = 0;
  if (sscanf(line, "soc=%d mv=%d ma=%d", &soc, &mv, &ma) < 3) return 0;
  sprintf(v, "%d", soc);
  rpc_kv(r, "charge_level", v);
  sprintf(v, "%d", mv);
  rpc_kv(r, "battery_voltage", v);
  sprintf(v, "%d", ma);
  return rpc_kv(r, "battery_current", v);
}

static int m_time(void *c, struct rpc_dt *d)
{
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);

  (void) c;
  d->year = tm->tm_year + 1900, d->month = tm->tm_mon + 1;
  d->day = tm->tm_mday, d->hour = tm->tm_hour;
  d->minute = tm->tm_min, d->second = tm->tm_sec;
  d->weekday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
  return 0;
}

/* R15.7: the phone sends its local wall time; ours follows TZ.  Two
 * seconds or less: only logged.  A step close to a whole quarter hour of
 * at least 45 min: the phone is in another zone, a notice, no step.
 * Else stime() (only root may; others get ERROR). */
static int m_settime(void *c, const struct rpc_dt *d)
{
  struct tm tm;
  time_t now = time(NULL), t;
  long diff, q;

  (void) c;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = d->year - 1900, tm.tm_mon = d->month - 1;
  tm.tm_mday = d->day, tm.tm_hour = d->hour;
  tm.tm_min = d->minute, tm.tm_sec = d->second;
  tm.tm_isdst = -1;
  if ((t = mktime(&tm)) == (time_t) -1) return PB_CommandStatus_ERROR_INVALID_PARAMETERS;
  diff = (long) (t - now);
  if (diff >= -2 && diff <= 2) {
	fprintf(stderr, "rpcd: phone clock %+lds, not stepped\n", diff);
	return 0;
  }
  q = (diff < 0 ? -diff : diff) % 900;
  if ((diff <= -2700 || diff >= 2700) && (q <= 60 || q >= 840)) {
	fprintf(stderr, "rpcd: phone in another time zone (%+ld min), "
		"clock not changed\n", diff / 60);
	return 0;
  }
  if (stime(&t) < 0) return PB_CommandStatus_ERROR;
  fprintf(stderr, "rpcd: clock stepped %+lds from the phone\n", diff);
  return 0;
}

static int m_info(void *c, const char *p, uint64_t *t, uint64_t *f)
{
  struct statfs s;

  (void) c;
  if (statfs(real(p), &s) < 0) return st_of(errno);
  *t = (uint64_t) s.f_blocks * (uint64_t) s.f_bsize;
  *f = (uint64_t) s.f_bavail * (uint64_t) s.f_bsize;
  return 0;
}

static int m_stat(void *c, const char *p, struct rpc_file *f)
{
  const char *n = strrchr(p, '/');
  struct stat s;

  (void) c;
  if (stat(real(p), &s) < 0) return st_of(errno);
  memset(f, 0, sizeof(*f));
  f->dir = S_ISDIR(s.st_mode);
  f->size = (uint32_t) s.st_size;
  strncpy(f->name, n ? n + 1 : p, RPC_PATH_MAX);
  return 0;
}

static int m_list(void *c, const char *p, struct rpc *r)
{
  static char q[RPC_PATH_MAX + 64];
  static struct rpc_file f;
  DIR *d;
  struct dirent *e;
  struct stat s;

  (void) c;
  p = real(p);
  if ((d = opendir(p)) == NULL) return st_of(errno);
  while ((e = readdir(d)) != NULL) {
	if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
	if (strlen(p) + 1 + strlen(e->d_name) >= sizeof(q)) continue;
	sprintf(q, "%s/%s", p, e->d_name);
	if (stat(q, &s) < 0) continue;
	memset(&f, 0, sizeof(f));
	f.dir = S_ISDIR(s.st_mode);
	f.size = (uint32_t) s.st_size;
	strncpy(f.name, e->d_name, RPC_PATH_MAX);
	if (rpc_list_add(r, &f) < 0) break;
  }
  closedir(d);
  return 0;
}

static long m_read(void *c, const char *p, uint32_t off, unsigned char *b,
		   unsigned n)
{
  int fd, k;

  (void) c;
  if ((fd = open(real(p), O_RDONLY)) < 0) return -st_of(errno);
  if (lseek(fd, (off_t) off, SEEK_SET) < 0) k = -1;
  else k = read(fd, b, n);
  close(fd);
  return k < 0 ? -PB_CommandStatus_ERROR_STORAGE_INTERNAL : (long) k;
}

static const struct rpc_ops ops = { NULL, m_send, m_devinfo, m_power,
	m_time, m_settime, m_info, m_stat, m_list, m_read };

int main(int argc, char **argv)
{
  static struct rpc r;
  static unsigned char b[256];
  void *st;
  int in = 0, n;
  char *e = getenv("RPCD_EXT");

  if (e != NULL && *e != 0) ext = e;
  if (argc > 2) {
	fprintf(stderr, "usage: rpcd [device]\n");
	return 1;
  }
  if (argc == 2) {
	if ((in = open(argv[1], O_RDWR)) < 0) {
		fprintf(stderr, "rpcd: %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	out = in;
  }
  if ((st = malloc(rpc_state_size())) == NULL) {
	fprintf(stderr, "rpcd: out of memory\n");
	return 1;
  }
  rpc_init(&r, &ops, st);
  while ((n = read(in, b, sizeof(b))) > 0)
	if (rpc_feed(&r, b, (unsigned) n) < 0) break;
  return 0;
}
