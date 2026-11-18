/* hidfake - play the USB host from the shell (QEMU only; T28e, nattest).
 *
 * Usage:	hidfake STATE
 *
 * Sets the HID bus state as a host would (HIDIOCFAKE, <minix/hid.h>):
 * bits 0-7 the bus (1 attached, 2 configured, 4 suspended), 16-23 the
 * LED byte.  "hidfake 0" unplugs the cable, "hidfake 3" plugs it in and
 * configures us.  On the Flipper the ioctl is ENOTTY: exit status 1.
 */
#include "t.h"
#include <sys/ioctl.h>
#include <minix/hid.h>

int main(int argc, char **argv)
{
	int fd, v;

	if (argc != 2) {
		printf("usage: hidfake STATE\n");
		return 2;
	}
	v = (int) strtol(argv[1], (char **) NULL, 0);
	if ((fd = open("/dev/hidmouse", O_WRONLY)) < 0
	    || ioctl(fd, HIDIOCFAKE, &v) < 0) {
		printf("hidfake: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}
