/* cfcat - print settings files: the SD card's copy, else the default in
 * /rom (Galtzagorriak MINIX, spec L.3; for shell scripts, like cfopen()).
 *
 * Usage:	cfcat path ...
 */
#include <sys/types.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	FILE *f;
	int i, c, r = 0;

	if (argc < 2) {
		fprintf(stderr, "Usage: cfcat path ...\n");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		if ((f = cfopen(argv[i], "r")) == NULL) {
			fprintf(stderr, "cfcat: %s: not on the card and no "
				"default\n", argv[i]);
			r = 1;
			continue;
		}
		while ((c = getc(f)) != EOF) putchar(c);
		fclose(f);
	}
	return r;
}
