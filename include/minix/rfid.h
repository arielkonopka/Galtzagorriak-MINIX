/* rfid.h - holding the 125 kHz field up across a command and its reply,
 * /dev/rfid (spec R6.5, R8.4; kernel/arm/flipdev.c).  Group `rfid`.
 *
 * A read of /dev/rfid (minor RFID) turns the field on, captures the
 * antenna comparator's edge times and turns it off: one EM4100-style
 * reading, the first level unknown.  A write plays field-gap durations
 * (us, field on first) to program a T5577.  Both end with the field off.
 *
 * A T5577 block read needs the field to stay up between the read command
 * (gaps, a write) and the tag's reply (edge times, a read): dropping the
 * carrier resets the tag to its default broadcast.  RFIDIOCFIELD with a
 * non-zero argument holds the field up; a write then leaves it on and a
 * read captures without raising it afresh.  A zero argument, a close or a
 * cancel drops it.  While held, the usual EBUSY arbitration with the IR
 * transmitter and the ADC stream still applies (shared TIM1/TIM2).
 *
 * The decoding - the comparator bias, Manchester, the word offset, the
 * password sweep - is the caller's (lib t5577, as the stock firmware's
 * lib/lfrfid/tools/t5577.c): the kernel only keeps the field up and the
 * timing honest, and reports why a step could not run.
 */
#ifndef _MINIX_RFID_H
#define _MINIX_RFID_H

#include <sys/ioctl.h>

#define RFIDIOCFIELD	_IOW('r', 1, int)	/* !=0 hold the field, 0 drop */

/* RFIDIOCEMU: pretend to be a tag (R6.5 part 4, R8.4).  The *reader* makes
 * the field here: the kernel counts its carrier and loads the antenna in
 * step with it, so the pattern plays at the reader's own clock and cannot
 * drift.  `dur` and `pulse` are arrays of `n` 32-bit words in the caller's
 * memory, which stays pinned and must not be freed while this runs: dur[i]
 * is a segment in carrier periods minus one, pulse[i] how many of them the
 * antenna is loaded (0 none, dur[i] + 1 all).  The list repeats for as long
 * as emulation is on, with no CPU work; `n` = 0 (or a close, or a cancel)
 * stops it.  Exclusive with reading, writing and a held field (one timer).
 *
 * EM4100 at RF/64: a bit is 64 carrier periods, Manchester, so every half
 * bit is a 32-period segment that is either loaded or not.
 */
struct rfid_emu {
	void *dur;		/* segment lengths, carrier periods - 1 */
	void *pulse;		/* of each, how long the antenna is loaded */
	unsigned n;		/* words in each (0 stops; at most 512) */
};
#define RFIDIOCEMU	_IOW('r', 2, struct rfid_emu)

/* RFIDIOCCARRIER (spec R9.0.4): the field a read raises, for the next
 * reads, until set again or the device is closed (then 125 kHz again).
 * PSK tags are read with the stock firmware's 62.5 kHz at 25 % duty,
 * FDX-B animal tags with 134.2 kHz (Unleashed's ASK134 mode).  A T5577
 * block read with the field held (RFIDIOCFIELD) is always 125 kHz. */
#define RFID_CARRIER_125	0	/* 125 kHz, 50 %: ASK and FSK tags */
#define RFID_CARRIER_PSK	1	/* 62.5 kHz, 25 %: PSK tags */
#define RFID_CARRIER_134	2	/* 134.2 kHz, 50 %: FDX-B */
#define RFIDIOCCARRIER	_IOW('r', 3, int)

/* RFIDIOCSTREAM (spec R9.0.5): capture without end.  The field comes on
 * (RFIDIOCCARRIER's) and the DMA writes the tag signal's edges as 32-bit
 * timestamps (us, a free-running counter) round and round into `ring`,
 * `words` long (even, 64..65534), in the caller's memory, pinned and not
 * to be freed while it runs.  read() then blocks until a half of the ring
 * is full and returns 4 bytes: 0 (words 0..words/2-1 are new) or 1 (the
 * second half).  Use the half before the next one is full (a ring of
 * 4096 words gives at least 32 ms at FSK's fastest).  A reader that fell
 * more than a half behind gets -1 with EOVERFLOW once, and the next whole
 * half after it: an edge is never lost silently.  poll() is readable when
 * a half is waiting.  words 0, close or the caller's exit stop it.  The
 * durations are the differences of the timestamps (mod 2^32). */
struct rfid_stream {
	void *ring;		/* 32-bit timestamps, us */
	unsigned words;		/* even, 64..65534; 0 stops */
};
#define RFIDIOCSTREAM	_IOW('r', 4, struct rfid_stream)

/* RFIDIOCEMURUN (spec R9.0.6): emulation of long frames (FSK, PSK) as a
 * run list.  A run is `count` equal segments of `period_m1` + 1 carrier
 * periods with the antenna loaded for `pulse` of them (an FSK bit is one
 * run, a PSK bit one or two); the list repeats.  `ring` is room for
 * 2 * ring_n words in the caller's memory (ring_n even, 16..512; 128 is a
 * good size), pinned while it runs: the DMA plays it at the reader's clock
 * and the kernel refills each half from the runs as soon as it has been
 * played (a short interrupt, no work per carrier period).  The run list
 * stays the caller's too and must not change meanwhile.  Stop as
 * RFIDIOCEMU (n = 0 there, a close or a cancel).  At most 512 runs. */
struct rfid_emu_run {
	unsigned short period_m1;	/* segment length, carrier periods - 1 */
	unsigned short pulse;		/* periods loaded (0 .. period_m1 + 1) */
	unsigned short count;		/* segments in a row (>= 1) */
	unsigned short pad;
};
struct rfid_emu_runs {
	struct rfid_emu_run *runs;
	unsigned nruns;			/* 1..512 */
	void *ring;			/* 2 * ring_n 32-bit words */
	unsigned ring_n;		/* even, 16..512 */
};
#define RFIDIOCEMURUN	_IOW('r', 5, struct rfid_emu_runs)

#endif /* _MINIX_RFID_H */
