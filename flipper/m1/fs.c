/* FS stand-in: like the real FS it is the only way for a user process to
 * reach the terminal.  It prints on behalf of INIT (TTY copies straight from
 * INIT's memory), and reads console input lines and echoes them (tests the
 * receive interrupt and REVIVE replies). */
#include "m1.h"

static char line[40];

static void post_read(void)
{
	message m;

	m.m_type = DEV_READ;
	m.TTY_LINE = 0;
	m.PROC_NR = FS_PROC_NR;
	m.ADDRESS = line;
	m.COUNT = sizeof line - 1;
	m.TTY_FLAGS = 0;
	if (_sendrec(TTY, &m) != OK || (m.REP_STATUS != SUSPEND
						&& m.REP_STATUS < 0))
		tty_write(FS_PROC_NR, "FS stand-in: read request failed\n");
}

int main(void)
{
	message m;
	int n, who;

	if (tty_write(FS_PROC_NR, "FS stand-in: hello via TTY\n") != 27)
		tty_write(FS_PROC_NR, "FS stand-in: short write!\n");
	post_read();

	for (;;) {
		if (_receive(ANY, &m) != OK) continue;
		who = m.m_source;
		switch (m.m_type) {
		case REVIVE:		/* our console read completed */
			n = m.REP_STATUS;
			if (n < 0) n = 0;
			line[n] = 0;
			tty_write(FS_PROC_NR, "FS stand-in: read: ");
			tty_write(FS_PROC_NR, line);
			post_read();
			break;
		case M1_PRINT:		/* write(1, buf, n) for a user */
			m.m_type = tty_write_for(who, m.ADDRESS, m.COUNT);
			_send(who, &m);
			break;
		default:
			m.m_type = EINVAL;
			_send(who, &m);
		}
	}
}
