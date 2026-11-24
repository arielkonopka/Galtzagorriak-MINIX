/* halt / reboot - halt or reboot system (depends on name)

   halt   - calling reboot() with RBT_HALT
   reboot - calling reboot() with RBT_REBOOT

   author: Edvard Tuinder   v892231@si.hhs.NL

   This program calls the library function reboot(2) which performs
   the system-call do_reboot. 

 */

#define _POSIX_SOURCE	1
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

void write_log _ARGS(( void ));
int main _ARGS(( int argc, char *argv[] ));

char *prog;

int
main(argc,argv)
int argc;
char **argv;
{
  int flag = RBT_HALT;		/* halting system is default */
  int fast = 0;			/* fast halt/reboot, don't kill all. */
  int dfu = 0;			/* reboot to the firmware loader */
  int fsck = 0;			/* reboot fsck: check and repair the card first */
  struct stat dummy;

  prog = strrchr(*argv,'/');
  if (prog == (char *)0)
    prog = *argv;
  else
    prog++;

  while (argc > 1) {
    if (strcmp(argv[1], "-f") == 0)
      fast = 1;
    else if (strcmp(argv[1], "-d") == 0)
      dfu = 1;		/* Flipper: to the ROM DFU loader (RBT_MONITOR) */
    else if (strcmp(argv[1], "fsck") == 0)
      fsck = 1;		/* reboot fsck: /etc/rc checks and repairs, restarts */
    else {
      fprintf(stderr, "Usage: %s [-f] [-d]%s\n", prog,
		strcmp(prog, "reboot") == 0 ? " [fsck]" : "");
      exit(1);
    }
    argc--, argv++;
  }

  if (fsck) {
    /* The request of /etc/fsckboot (issue #399): a running system has no
     * room to start fsck, so /etc/rc finds this file at the next start,
     * checks and repairs /usr and /home before anything else, restarts
     * once more and then starts normally. */
    int fd;

    if (strcmp(prog, "reboot") != 0 || dfu) {
      fprintf(stderr, "Usage: %s [-f] [-d]\n(fsck is for reboot only, not with -d)\n", prog);
      exit(1);
    }
    if ((fd = open("/etc/.fsck-boot", O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
      fprintf(stderr, "%s: cannot write /etc/.fsck-boot (root only)\n", prog);
      exit(1);
    }
    close(fd);
    sync();
    printf("Restarting to check and repair the card...\n");
  }

  /* Make sure that we don't die. */
  signal(SIGHUP, SIG_IGN);
  signal(SIGTERM, SIG_IGN);

  if (stat("/usr/bin", &dummy) < 0) {
    /* It seems that /usr isn't present, let's assume "-f." */
    fast = 1;
  }

  if (fast) {
    sleep(1);	/* Not too fast, people like to see it "do something". */
  } else {
    /* Tell init to stop spawning getty's. */
    kill(1, SIGTERM);
    /* Give everybody a chance to die peacefully. */
    kill(-1, SIGTERM);
    sleep(2);
  }

  if (strcmp(prog,"reboot"))
    flag=RBT_HALT;
  else
    flag=RBT_REBOOT;
  
  write_log();
  if (dfu)
    reboot(RBT_MONITOR, "", (size_t) 1);
  else
    reboot(flag);
  fprintf(stderr, "reboot call failed\n");
  return 1;
}
