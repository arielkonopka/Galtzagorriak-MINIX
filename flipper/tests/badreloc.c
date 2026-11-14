/* badreloc - S2/R1.3: relocation entries outside the new image or not
 * word aligned must not be applied; the exec fails or the new process is
 * killed before it runs.  Uses a copy of /bin/echo (position independent:
 * the relocation table follows header, text and data). */
#include "t.h"

#define PROG	"/tmp/badreloc.x"

static long table_at;

static void run(void)
{
	execl(PROG, "echo", "RAN", (char *) 0);
	_exit(2);				/* exec refused: fine */
}

static void try(char *what, unsigned char *table, int len)
{
	int st;

	/* group-writable: never put in the flash text cache (no wear) */
	if (t_copy("/bin/echo", PROG, 0775) < 0
	    || t_patch(PROG, table_at, table, len) < 0) {
		t_result(what, 0, "cannot make the test program");
		return;
	}
	st = t_child(run);
	t_result(what, st != -1 && (WIFSIGNALED(st) || WEXITSTATUS(st) == 2),
							t_status(st));
}

int main(void)
{
	struct t_hdr h;
	unsigned char t[300];
	long first;
	int i;

	if (t_header("/bin/echo", &h) < 0) {
		t_result("read /bin/echo", 0, NULL);
		return 1;
	}
	table_at = h.hdrlen + h.text + h.data;

	first = 0x7FFFFFF0L;			/* far outside */
	memcpy(t, &first, 4); t[4] = 0;
	try("first relocation far outside the image", t, 5);

	first = 3;				/* data + 2: unaligned */
	memcpy(t, &first, 4); t[4] = 0;
	try("unaligned first relocation", t, 5);

	first = 1;				/* data + 0, then walk off */
	memcpy(t, &first, 4);
	for (i = 0; i < 280; i++) t[4 + i] = 1;	/* + 254 each */
	t[284] = 2; t[285] = 0;
	try("relocation stepping past the data", t, 286);

	first = -0x1000L;			/* below the data */
	memcpy(t, &first, 4); t[4] = 0;
	try("relocation below the data", t, 5);

	unlink(PROG);
	t_result("system still running", 1, NULL);
	return t_fail;
}
