/* subghz.h - Sub-GHz transmit, packets, region (spec R8.1, R8.2, Phase 9;
 * issue #302; kernel/arm/flipdev.c).  Group `subghz`.
 *
 * /dev/subghz (minor 11): write a frequency in Hz to listen there, "0" to
 * stop; a read gives "rssi=... peak=... freq=..." (R8.1 part 1).
 *
 * /dev/subghzraw (minor 14): a read waits for one frame the receiver
 * demodulates: durations in us as unsigned 32-bit words, carrier on
 * first, then off, on, ...  A write sends such a frame at the frequency
 * /dev/subghz tuned (R8.2): the kernel reads the frequency back from the
 * chip and refuses
 *	EIO	not listening (tune with /dev/subghz first),
 *	EPERM	no region set, or the frequency is outside its bands,
 *	EAGAIN	the band's duty cycle for this hour is used up,
 *	EINVAL	a duration of 0 or over 1 s, or not whole words,
 *	ENOTSUP	no transmitter (the Flipper until the CC1101 TX path is
 *		written; QEMU prints the frame on the console).
 *
 * /dev/subghzpkt (minor 20): one packet (1..SG_PKT_MAX bytes) per write
 * or read, through the chip's FIFO, as stock's `subghz chat`; the same
 * checks on a write.  A read never waits: EAGAIN when none has come.
 *
 * The region (two letters, "EU", "US", "JP") is set once after boot
 * (/etc/rc, from /etc/flipper/region), by the superuser only (FS checks
 * the uid, issue #326): later SGIOCREGION calls with another region, and
 * any call by another user, fail with EPERM.  The band table is const in
 * the kernel.
 */
#ifndef _MINIX_SUBGHZ_H
#define _MINIX_SUBGHZ_H

#include <sys/ioctl.h>

#define SG_PKT_MAX	60		/* bytes in one packet */
#define SG_DURATION_MAX	1000000UL	/* us, one level of a frame */
#define SG_REGION(a, b)	((a) << 8 | (b))	/* SG_REGION('E', 'U') */

#define SGIOCREGION	_IOW('Q', 1, int)	/* set the region (once) */
#define SGIOCGREGION	_IOR('Q', 2, int)	/* the region, 0 none */
/* The air time this hour (ms) and the band's limit (ms) at the tuned
 * frequency; EPERM outside the region's bands. */
#define SGIOCAIR	_IOR('Q', 3, struct sg_air)

/* Test hooks, QEMU only (ENOTTY on the Flipper): SGIOCFAKE makes the next
 * write on this kind of file a received frame or packet (the waiting
 * reader of /dev/subghzraw gets it; /dev/subghzpkt's next read gives it);
 * SGIOCLINE gives the FNV-1a hash of the last frame or packet QEMU printed,
 * as one line: "tx 433920000 3 350 -700 350" or "pkt 433920000 68 69". */
#define SGIOCFAKE	_IOW('Q', 4, int)
#define SGIOCLINE	_IOR('Q', 5, long)

#define SG_FAKE_RAW	1
#define SG_FAKE_PKT	2

struct sg_air {
	unsigned long used_ms;		/* sent this hour (all bands) */
	unsigned long limit_ms;		/* the tuned band's duty cycle */
};

#endif /* _MINIX_SUBGHZ_H */
