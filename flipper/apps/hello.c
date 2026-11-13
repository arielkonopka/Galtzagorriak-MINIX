/* hello - the first Galtzagorriak app (an example package). */
#include <sys/types.h>
#include <sys/utsname.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	struct utsname u;

	if (uname(&u) < 0) u.nodename[0] = 0;
	printf("Kaixo! Hello from %s, a galtzagorri at your service.\n",
								u.nodename);
	if (argc > 1) printf("You asked for: %s\n", argv[1]);
	return 0;
}
