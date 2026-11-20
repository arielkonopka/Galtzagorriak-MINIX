/* fstatfs(), statfs() - free space of a mounted file system  (spec O.5) */
#include <lib.h>
#include <string.h>
#include <sys/statfs.h>

PUBLIC int fstatfs(fd, buf)
int fd;
struct statfs *buf;
{
  message m;

  m.m1_i1 = fd;
  m.m1_p2 = (char *) buf;
  return(_syscall(FS, FSTATFS, &m));
}

PUBLIC int statfs(path, buf)
_CONST char *path;
struct statfs *buf;
{
  message m;

  m.m1_i1 = FSTATFS_PATH;
  m.m1_i2 = strlen(path) + 1;
  m.m1_p1 = (char *) path;
  m.m1_p2 = (char *) buf;
  return(_syscall(FS, FSTATFS, &m));
}
