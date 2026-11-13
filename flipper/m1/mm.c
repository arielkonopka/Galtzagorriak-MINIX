/* MM stand-in: answers pings and fetches the uptime for INIT (users may only
 * talk to MM and FS, not to tasks such as CLOCK). */
#include "m1.h"
#include <signal.h>

static int init_fails = -1;

static void say(const char *s)
{
	tty_write(MM_PROC_NR, s);
}

static void report_signal(int proc, unsigned long map)
{
	unsigned long bad = (1UL << SIGSEGV) | (1UL << SIGBUS);
	int ok = proc == INIT_PROC_NR && (map & bad) != 0;

	say(ok ? "  ok   " : "  FAIL ");
	say("fault became a signal for INIT, kernel survived\n");
	if (init_fails != 0) ok = 0;
	say(ok ? "M1 PASS\n" : "M1 FAIL\n");
	semihost_exit(ok ? 0 : 1);
}

int main(void)
{
	message m, c;
	int who;

	tty_write(MM_PROC_NR, "MM stand-in: up\n");
	for (;;) {
		if (_receive(ANY, &m) != OK) continue;
		who = m.m_source;
		switch (m.m_type) {
		case M1_PING:
			m.m_type = OK;
			break;
		case M1_RESULT:
			init_fails = m.m1_i1;
			m.m_type = OK;
			break;
		case KSIG:		/* from the kernel: no reply */
			report_signal(m.SIG_PROC, m.SIG_MAP);
			continue;
		case M1_UPTIME:
			c.m_type = GET_UPTIME;
			if (_sendrec(CLOCK, &c) != OK) {
				m.m_type = EIO;
				break;
			}
			m.m_type = OK;
			m.m2_l1 = c.NEW_TIME;
			break;
		default:
			m.m_type = EINVAL;
		}
		_send(who, &m);
	}
}
