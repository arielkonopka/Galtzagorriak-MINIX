/* rpcd on the PC (issue #388): the core over stdin/stdout, /ext mapped to
 * the directory $RPCD_EXT, for tools/rpc.py and the tests (rpcclient.sh).
 * The device's rpcd has the same core with its own platform part. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "flipper.pb.h"
#include "rpcd.h"

static char ext[512];

static const char *real(const char *p, char *b, size_t n)
{
  snprintf(b, n, "%s%s", ext, p + 4);		/* p starts with /ext */
  return b;
}
static int st_of(int e)
{
  return e == ENOENT || e == ENOTDIR ? PB_CommandStatus_ERROR_STORAGE_NOT_EXIST
	 : e == EACCES || e == EPERM ? PB_CommandStatus_ERROR_STORAGE_DENIED
	 : PB_CommandStatus_ERROR_STORAGE_INTERNAL;
}
static int h_send(void *c, const unsigned char *b, unsigned n)
{
  (void) c;
  return fwrite(b, 1, n, stdout) == n && fflush(stdout) == 0 ? 0 : -1;
}
static int h_devinfo(void *c, struct rpc *r)
{
  (void) c;
  rpc_kv(r, "format_major", "3");
  rpc_kv(r, "format_minor", "3");
  rpc_kv(r, "hardware_model", "Flipper Zero");
  rpc_kv(r, "hardware_name", "Baserri");
  rpc_kv(r, "firmware_version", "host");
  rpc_kv(r, "firmware_origin", "Galtzagorriak MINIX");
  rpc_kv(r, "protobuf_version_major", "0");
  return rpc_kv(r, "protobuf_version_minor", "25");
}
static int h_power(void *c, struct rpc *r)
{
  (void) c;
  rpc_kv(r, "charge_level", "77");
  return rpc_kv(r, "battery_voltage", "3.95");
}
static int h_time(void *c, struct rpc_dt *d)
{
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);

  (void) c;
  d->year = tm->tm_year + 1900, d->month = tm->tm_mon + 1;
  d->day = tm->tm_mday, d->hour = tm->tm_hour;
  d->minute = tm->tm_min, d->second = tm->tm_sec;
  return 0;
}
static int h_settime(void *c, const struct rpc_dt *d)
{
  (void) c;		/* the PC's clock is not ours to step: log only */
  fprintf(stderr, "rpcdhost: set_datetime %04d-%02d-%02d %02d:%02d:%02d\n",
	  d->year, d->month, d->day, d->hour, d->minute, d->second);
  return 0;
}
static int h_info(void *c, const char *p, uint64_t *t, uint64_t *f)
{
  char b[1024];
  struct statvfs v;

  (void) c;
  if (statvfs(real(p, b, sizeof(b)), &v) < 0) return st_of(errno);
  *t = (uint64_t) v.f_blocks * v.f_frsize;
  *f = (uint64_t) v.f_bavail * v.f_frsize;
  return 0;
}
static int h_stat(void *c, const char *p, struct rpc_file *f)
{
  char b[1024];
  const char *n = strrchr(p, '/');
  struct stat s;

  (void) c;
  if (stat(real(p, b, sizeof(b)), &s) < 0) return st_of(errno);
  memset(f, 0, sizeof(*f));
  f->dir = S_ISDIR(s.st_mode);
  f->size = (uint32_t) s.st_size;
  snprintf(f->name, sizeof(f->name), "%s", n ? n + 1 : p);
  return 0;
}
static int h_list(void *c, const char *p, struct rpc *r)
{
  char b[1024], q[1024];
  DIR *d;
  struct dirent *e;
  struct rpc_file f;
  struct stat s;

  (void) c;
  if ((d = opendir(real(p, b, sizeof(b)))) == NULL) return st_of(errno);
  while ((e = readdir(d)) != NULL) {
	if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
	snprintf(q, sizeof(q), "%s/%s", b, e->d_name);
	if (stat(q, &s) < 0) continue;
	memset(&f, 0, sizeof(f));
	f.dir = S_ISDIR(s.st_mode);
	f.size = (uint32_t) s.st_size;
	snprintf(f.name, sizeof(f.name), "%s", e->d_name);
	if (rpc_list_add(r, &f) < 0) break;
  }
  closedir(d);
  return 0;
}
static long h_read(void *c, const char *p, uint32_t off, unsigned char *buf,
		   unsigned n)
{
  char b[1024];
  int fd;
  ssize_t k;

  (void) c;
  if ((fd = open(real(p, b, sizeof(b)), O_RDONLY)) < 0) return -st_of(errno);
  k = pread(fd, buf, n, off);
  close(fd);
  return k < 0 ? -PB_CommandStatus_ERROR_STORAGE_INTERNAL : (long) k;
}

static const struct rpc_ops ops = { NULL, h_send, h_devinfo, h_power, h_time,
	h_settime, h_info, h_stat, h_list, h_read };

int main(void)
{
  static struct rpc r;
  unsigned char b[256];
  void *st = malloc(rpc_state_size());
  ssize_t n;
  const char *e = getenv("RPCD_EXT");

  snprintf(ext, sizeof(ext), "%s", e ? e : ".");
  rpc_init(&r, &ops, st);
  while ((n = read(0, b, sizeof(b))) > 0)
	if (rpc_feed(&r, b, (unsigned) n) < 0) break;
  free(st);
  return 0;
}
