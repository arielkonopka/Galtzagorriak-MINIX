/* cat - concatenates files  		Author: Andy Tanenbaum */

/* 30 March 90 - Slightly modified for efficiency by Norbert Schlenker. */


#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <minix/minlib.h>
#include <stdio.h>

/* One 1 KB buffer (one file system block) instead of a 4 KB input and a
 * 4 KB output buffer: reads fill it and it is written out when full, so
 * small reads (pipes, the terminal) are still collected (R2a.5).
 */
#define CHUNK_SIZE	1024

static int unbuffered;
static char buf[CHUNK_SIZE];
static size_t fill;

_PROTOTYPE(int main, (int argc, char **argv));
_PROTOTYPE(void copyfile, (int ifd, int ofd));
_PROTOTYPE(void flush, (void));
_PROTOTYPE(void fatal, (void));

int excode = 0;

int main(argc, argv)
int argc;
char *argv[];
{
  int i, fd;

  i = 1;
  /* Check for the -u flag -- unbuffered operation. */
  if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'u' && argv[1][2] == 0) {
	unbuffered = 1;
	i = 2;
  }
  if (i >= argc) {
	copyfile(STDIN_FILENO, STDOUT_FILENO);
  } else {
	for ( ; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] == 0) {
			copyfile(STDIN_FILENO, STDOUT_FILENO);
		} else {
			fd = open(argv[i], O_RDONLY);
			if (fd < 0) {
				std_err("cat: cannot open ");
				std_err(argv[i]);
				std_err(": ");
				std_err(strerror (errno));
				std_err("\n");
				excode = 1;
			} else {
				copyfile(fd, STDOUT_FILENO);
				close(fd);
			}
		}
	}
  }
  flush();
  return(excode);
}

void copyfile(ifd, ofd)
int ifd, ofd;
{
  int n;

  while (1) {
	n = read(ifd, buf + fill, CHUNK_SIZE - fill);
	if (n < 0) fatal();
	if (n == 0) return;
	fill += n;
	if (unbuffered || fill == CHUNK_SIZE) {
		if (write(ofd, buf, fill) != (ssize_t) fill) fatal();
		fill = 0;
	}
  }
}

void flush()
{
  if (fill != 0)
	if (write(STDOUT_FILENO, buf, fill) <= 0) fatal();
}

void fatal()
{
  perror("cat");
  exit(1);
}
