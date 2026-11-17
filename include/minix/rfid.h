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

#endif /* _MINIX_RFID_H */
