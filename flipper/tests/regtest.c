/* regtest - exec clears the registers of the old program (issue #160,
 * security review 2026-11-16 finding U1, fixed in #159).
 *
 *   regtest		fills r3-r8, r10-r12 and lr with a marker right at
 *			the exec trap and execs /usr/tests/regchild
 *   regchild		(entry regstart.S, no crtso) saves all registers
 *			before anything runs and reports
 *
 * Output: "regtest: ok" or "regtest: FAIL rN=..." (exit status 1).
 * r0-r2 carry the trap's arguments, r9 is the GOT base the kernel sets.
 */
#include <lib.h>
#include <minix/callnr.h>
#include <string.h>
#include <unistd.h>

#define MARK	0xA5A5A5A5UL
#define CHILD	"/usr/tests/regchild"

static void say(const char *s)
{
	(void) write(1, s, strlen(s));
}

/* regchild: regs[0..12] = r0..r12, regs[13] = lr as exec left them. */
void regcheck(unsigned long *regs)
{
	static const char hex[] = "0123456789abcdef";
	char line[48];
	int i, k, bad = 0;

	for (i = 0; i < 14; i++) {
		if (regs[i] != MARK) continue;
		strcpy(line, bad++ ? " " : "regtest: FAIL");
		strcat(line, i == 13 ? " lr=" : " r");
		if (i < 13) {
			k = strlen(line);
			if (i >= 10) line[k++] = '1';
			line[k++] = '0' + i % 10;
			line[k++] = '=';
			line[k] = 0;
		}
		k = strlen(line);
		line[k++] = hex[(regs[i] >> 28) & 15];
		line[k++] = hex[(regs[i] >> 24) & 15];
		line[k++] = '.'; line[k++] = '.'; line[k] = 0;
		say(line);
	}
	say(bad ? "\n" : "regtest: ok (r0-r12, lr clean after exec)\n");
	_exit(bad != 0);
}

int main(void)
{
	static char *argv[] = { "regchild", NULL };
	static char *envp[] = { NULL };
	message m;

	/* as libc's execve, for one 8-byte argument and no environment */
	memset(&m, 0, sizeof m);
	m.m_type = EXEC;
	m.m1_i1 = strlen(CHILD) + 1;
	m.m1_i2 = ((1 + 1 + 1 + 0 + 1) * 4 + 1 + 0 + 8 + 3) / 4 * 4;
	m.m1_i3 = (int) envp;
	m.m1_p1 = CHILD;
	m.m1_p2 = NULL;
	m.m1_p3 = (char *) argv;
	{
		register int r0 __asm__("r0") = MM;
		register message *r1 __asm__("r1") = &m;
		register int r2 __asm__("r2") = 3;	/* BOTH */

		__asm__ volatile (
			"ldr	r3, =0xA5A5A5A5\n\t"
			"mov	r4, r3\n\t"
			"mov	r5, r3\n\t"
			"mov	r6, r3\n\t"
			"mov	r7, r3\n\t"
			"mov	r8, r3\n\t"
			"mov	r10, r3\n\t"
			"mov	r11, r3\n\t"
			"mov	r12, r3\n\t"
			"mov	lr, r3\n\t"
			"svc	0\n\t"
			: "+r" (r0)
			: "r" (r1), "r" (r2)
			: "r3", "r4", "r5", "r6", "r7", "r8", "r10", "r11",
			  "r12", "lr", "memory");
	}
	say("regtest: exec of " CHILD " failed\n");
	return 2;
}
