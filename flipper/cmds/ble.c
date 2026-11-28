/* ble - start and use the Bluetooth LE stack on CPU2 (T30b, issue #390).
 *
 *	ble up [NAME]	start the stack (opening /dev/ble does it), set the
 *			address, and advertise under NAME: the argument,
 *			else "name=..." in /etc/ble.conf, else
 *			"Galtzagorriak MINIX" (26 characters at most)
 *	ble down	stop advertising
 *	ble scan [SECS]	listen for SECS seconds (10) and write what the
 *			stack reports, as HCI events, to standard output:
 *				ble scan 10 | blescan -t
 *	ble beep [NAME [SECS]]	make a key finder beep: look for the device
 *			that advertises NAME ("iTAG"), connect, and set its
 *			Immediate Alert (service 1802, characteristic 2A06)
 *			to "high" for SECS seconds (3), then to "off"
 *
 * /dev/ble takes one HCI command a write and gives one event a read (H4
 * packets), EAGAIN when none waits: this waits in poll().  Root only (the
 * device, and /dev/mem for the address, made from the chip's unique
 * number as ST's examples do).  The stack keeps what "up" set until the
 * next reset.
 * Exit status: 0, 1 an error, 2 usage.
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NAME_MAX_BLE	26
static int fd;

static void say(const char *s)
{
	write(2, "ble: ", 5);
	write(2, s, strlen(s));
	write(2, "\n", 1);
}

static void nap(int ms)
{
	(void) poll((struct pollfd *) 0, 0, ms);
}

/* One command; its status (0 fine), or -1 when it could not be sent or no
 * answer came.  The answer's parameters after the status go to rsp. */
static int cmd(unsigned op, const unsigned char *par, int n,
					unsigned char *rsp, int rmax)
{
	unsigned char pk[84];
	int r, i, t;

	if (n > 80) return -1;
	pk[0] = 0x01; pk[1] = (unsigned char) op;
	pk[2] = (unsigned char) (op >> 8); pk[3] = (unsigned char) n;
	if (n > 0) memcpy(pk + 4, par, n);
	if (write(fd, pk, 4 + n) != 4 + n) return -1;
	for (t = 0; t < 50; t++) {
		if ((r = read(fd, pk, sizeof pk)) < 3) {
			if (r < 0 && errno != EAGAIN) return -1;
			nap(20);
			continue;
		}
		if (pk[1] == 0x0E && r >= 7
		    && (unsigned) (pk[4] | pk[5] << 8) == op) {
			for (i = 0; i < rmax && 7 + i < r; i++) rsp[i] = pk[7 + i];
			return pk[6];
		}
		if (pk[1] == 0x0F && r >= 7
		    && (unsigned) (pk[5] | pk[6] << 8) == op) return pk[3];
	}
	return -1;
}

static int step(const char *what, unsigned op, const unsigned char *par, int n)
{
	static const char hex[] = "0123456789abcdef";
	char m[48];
	int st = cmd(op, par, n, (unsigned char *) 0, 0), l;

	if (st == 0) return 0;
	strcpy(m, what);
	l = strlen(m);
	if (st < 0) strcpy(m + l, ": no answer");
	else {
		strcpy(m + l, ": status ");
		l = strlen(m);
		m[l] = hex[st >> 4 & 15]; m[l + 1] = hex[st & 15]; m[l + 2] = 0;
	}
	say(m);
	return -1;
}

static void conf_name(char *name)
{
	char b[80];
	int f, n, i;

	if ((f = open("/etc/ble.conf", O_RDONLY)) < 0) return;
	n = read(f, b, sizeof b - 1);
	close(f);
	if (n < 6 || strncmp(b, "name=", 5) != 0) return;
	for (i = 5; i < n && b[i] != '\n' && i - 5 < NAME_MAX_BLE; i++)
		name[i - 5] = b[i];
	if (i > 5) name[i - 5] = 0;
}

static int up(const char *arg)
{
	char name[NAME_MAX_BLE + 1];
	unsigned char p[48], u[8];
	int f, l;

	strcpy(name, "Galtzagorriak MINIX");
	conf_name(name);
	if (arg != (char *) 0) {
		if (strlen(arg) > NAME_MAX_BLE || arg[0] == 0) {
			say("the name: 1 to 26 characters");
			return 1;
		}
		strcpy(name, arg);
	}
	l = strlen(name);
	/* the public address, as ST's examples: the unique number's low
	 * bytes, the device id, ST's company id */
	if ((f = open("/dev/mem", O_RDONLY)) < 0
	    || lseek(f, (off_t) 0x1FFF7580L, SEEK_SET) == (off_t) -1
	    || read(f, u, 8) != 8) {
		say("cannot read the chip's number");
		return 1;
	}
	close(f);
	if (step("reset", 0x0C03, p, 0) < 0) return 1;
	p[0] = 0; p[1] = 6;
	p[2] = u[0]; p[3] = u[1]; p[4] = u[4]; p[5] = u[5]; p[6] = u[6]; p[7] = u[7];
	if (step("address", 0xFC0C, p, 8) < 0) return 1;
	if (step("GATT", 0xFD01, p, 0) < 0) return 1;
	p[0] = 0x0F; p[1] = 0; p[2] = (unsigned char) l;	/* every role */
	if (step("GAP", 0xFC8A, p, 3) < 0) return 1;
	p[0] = 0;				/* connectable, undirected */
	p[1] = 0xA0; p[2] = 0; p[3] = 0xA0; p[4] = 0;	/* every 100 ms */
	p[5] = 0; p[6] = 0;			/* public address, no filter */
	p[7] = (unsigned char) (l + 1);
	p[8] = 0x09;				/* the complete local name */
	memcpy(p + 9, name, l);
	memset(p + 9 + l, 0, 5);		/* no services, no intervals */
	if (step("advertising", 0xFC83, p, 14 + l) < 0) return 1;
	return 0;
}

static int scan(int secs)
{
	static const unsigned char go[6] = { 0x10, 0, 0x10, 0, 0, 0 };
	unsigned char pk[84], two = 2;
	time_t end = time((time_t *) 0) + secs;
	int r;

	if (step("scan (run \"ble up\" first)", 0xFC97, go, 6) < 0) return 1;
	while (time((time_t *) 0) < end) {
		if ((r = read(fd, pk, sizeof pk)) < 3) {
			if (r < 0 && errno != EAGAIN) break;
			nap(20);
			continue;
		}
		if (pk[1] == 0x3E) write(1, pk, r);
		else if (pk[1] == 0xFF && r >= 5 && pk[3] == 0x07 && pk[4] == 0x04)
			(void) cmd(0xFC97, go, 6, (unsigned char *) 0, 0);
						/* the procedure ended: again */
	}
	(void) cmd(0xFC9D, &two, 1, (unsigned char *) 0, 0);	/* stop it */
	return 0;
}

/* One event (not an answer) within ms; its length, 0 when none came. */
static int event(unsigned char *pk, int max, int ms)
{
	int r;

	for (;;) {
		if ((r = read(fd, pk, max)) >= 3) return r;
		if (r < 0 && errno != EAGAIN) return 0;
		if (ms <= 0) return 0;
		nap(20);
		ms -= 20;
	}
}

/* ble beep: a key finder's alarm, over a connection.  The full stack has
 * the host on CPU2, so ST's GAP and GATT commands do it all: no data
 * channel of ours.  Nothing is paired or kept. */
static int beep(const char *name, int secs)
{
	static const unsigned char go[6] = { 0x10, 0, 0x10, 0, 0, 0 };
	unsigned char pk[84], p[24], two = 2;
	unsigned conn = 0, vh = 0;
	int r, i, n, found = 0, l = strlen(name);
	time_t end;

	/* 1: its address, from what it advertises (10 s at most) */
	if (step("scan (run \"ble up\" first)", 0xFC97, go, 6) < 0) return 1;
	end = time((time_t *) 0) + 10;
	while (!found && time((time_t *) 0) < end) {
		if ((r = event(pk, sizeof pk, 200)) < 15 || pk[1] != 0x3E
		    || pk[3] != 0x02) continue;
		n = pk[13];				/* the data's length */
		if (14 + n > r) continue;
		for (i = 14; i + 1 < 14 + n && pk[i] != 0; i += pk[i] + 1)
			if ((pk[i + 1] == 0x09 || pk[i + 1] == 0x08)
			    && pk[i] - 1 == l && i + 2 + l <= 14 + n
			    && memcmp(pk + i + 2, name, l) == 0) found = 1;
		if (found) memcpy(p + 4, pk + 6, 7);	/* type, address */
	}
	(void) cmd(0xFC9D, &two, 1, (unsigned char *) 0, 0);
	for (i = 0; i < 20; i++)		/* until the procedure has ended */
		if ((r = event(pk, sizeof pk, 100)) >= 5 && pk[1] == 0xFF
		    && pk[3] == 0x07 && pk[4] == 0x04) break;
	if (!found) {
		say("no such device near (is it on?)");
		return 1;
	}
	/* 2: connect: scan 10 ms of 10, our public address, interval
	 * 50-75 ms, no latency, timeout 4 s */
	p[0] = 0x10; p[1] = 0; p[2] = 0x10; p[3] = 0;
	p[11] = 0;
	p[12] = 0x28; p[13] = 0; p[14] = 0x3C; p[15] = 0;
	p[16] = 0; p[17] = 0; p[18] = 0x90; p[19] = 0x01;
	p[20] = 0; p[21] = 0; p[22] = 0; p[23] = 0;
	if (step("connect", 0xFC9C, p, 24) < 0) return 1;
	for (i = 0; i < 50 && conn == 0; i++)
		if ((r = event(pk, sizeof pk, 200)) >= 7 && pk[1] == 0x3E
		    && (pk[3] == 0x01 || pk[3] == 0x0A)) {
			if (pk[4] != 0) break;
			conn = (pk[5] | pk[6] << 8) + 1u;
		}
	if (conn == 0) {
		(void) cmd(0xFC9D, (const unsigned char *) "\100", 1,
			   (unsigned char *) 0, 0);	/* stop connecting */
		say("it did not answer the connection");
		return 1;
	}
	conn--;
	/* 3: where its alert level is: the characteristic 2A06 */
	p[0] = (unsigned char) conn; p[1] = (unsigned char) (conn >> 8);
	p[2] = 1; p[3] = 0; p[4] = 0xFF; p[5] = 0xFF;
	p[6] = 1; p[7] = 0x06; p[8] = 0x2A;
	r = 1;
	if (step("find the alert", 0xFD16, p, 9) == 0) {
		for (i = 0; i < 50; i++) {
			if ((n = event(pk, sizeof pk, 200)) < 5 || pk[1] != 0xFF)
				continue;
			if (pk[3] == 0x12 && pk[4] == 0x0C && n >= 13)
				vh = pk[11] | pk[12] << 8;  /* its value's handle */
			if (pk[3] == 0x10 && pk[4] == 0x0C) break;
		}
		if (vh == 0) say("it has no Immediate Alert");
	}
	/* 4: high for a while, then off */
	if (vh != 0) {
		p[2] = (unsigned char) vh; p[3] = (unsigned char) (vh >> 8);
		p[4] = 1; p[5] = 2;
		if (step("alert", 0xFD23, p, 6) == 0) {
			r = 0;
			sleep(secs);
			p[5] = 0;
			(void) step("alert off", 0xFD23, p, 6);
			nap(300);
		}
	}
	p[2] = 0x13;				/* the user ended it */
	(void) cmd(0xFC93, p, 3, (unsigned char *) 0, 0);
	(void) event(pk, sizeof pk, 500);	/* its disconnection */
	return r;
}

int main(int argc, char **argv)
{
	int t;

	if (argc < 2 || argc > 4
	    || (strcmp(argv[1], "up") != 0 && strcmp(argv[1], "down") != 0
		&& strcmp(argv[1], "scan") != 0 && strcmp(argv[1], "beep") != 0)
	    || (argc == 3 && argv[1][0] == 'd')
	    || (argc == 4 && argv[1][0] != 'b')) {
		say("usage: ble up [NAME] | down | scan [SECS] | beep [NAME [SECS]]");
		return 2;
	}
	for (t = 0; (fd = open("/dev/ble", O_RDWR)) < 0; t++) {
		if (errno != EAGAIN || t == 60) {	/* CPU2 starts: ~3 s */
			say(errno == ENXIO ? "no BLE stack on CPU2 (see radio)"
			  : errno == EAGAIN ? "CPU2 did not start"
			  : "cannot open /dev/ble");
			return 1;
		}
		nap(50);
	}
	if (argv[1][0] == 'u') return up(argc == 3 ? argv[2] : (char *) 0);
	if (argv[1][0] == 'b') {
		t = argc == 4 ? atoi(argv[3]) : 3;
		return beep(argc >= 3 ? argv[2] : "iTAG",
			    t < 1 ? 1 : t > 60 ? 60 : t);
	}
	if (argv[1][0] == 'd')
		return step("advertising off", 0xFC81, (unsigned char *) 0, 0) < 0;
	t = argc == 3 ? atoi(argv[2]) : 10;
	return scan(t < 1 ? 1 : t > 3600 ? 3600 : t);
}
