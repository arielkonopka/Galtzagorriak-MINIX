/* unaligned - S6/R1.5: a message pointer that is not word aligned gets
 * EFAULT instead of a fault in the kernel (which halted the system). */
#include "t.h"
#include <lib.h>
#include <minix/callnr.h>

static long space[16];

int main(void)
{
	message *m = (message *) ((char *) space + 1);
	int r;

	m->m_type = GETPID;
	r = _sendrec(MM, m);
	t_result("sendrec with an unaligned message", r == EFAULT || r == -EFAULT, NULL);
	m = (message *) ((char *) space + 2);
	r = _sendrec(MM, m);
	t_result("sendrec, message at +2", r == EFAULT || r == -EFAULT, NULL);
	t_result("system still running", 1, NULL);
	return t_fail;
}
