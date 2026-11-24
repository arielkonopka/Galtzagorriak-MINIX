/* ir.h - /dev/ir, the infrared receiver and transmitter (kernel/arm/flipdev.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A read gives one frame: 32-bit durations in microseconds, mark first.  A
 * frame ends after a silence, 30 ms by default: enough to tell one message
 * from the next.  IRIOCSGAP sets another silence in ms (10 to 250; 0: the
 * default again) for the reads that follow.  Learning a raw signal wants
 * 150 ms, as the stock firmware's raw receiver has
 * (INFRARED_RAW_RX_TIMING_DELAY_US): many remotes send a burst and repeat
 * bursts 40 to 110 ms apart, and all of them belong to the signal.  The
 * last close of /dev/ir sets the default again.
 */
#ifndef _MINIX_IR_H
#define _MINIX_IR_H

#include <sys/ioctl.h>

#define IRIOCSGAP	_IOW('I', 1, int)	/* ms of silence ending a frame */

#define IR_GAP_DEFAULT	30	/* ms */
#define IR_GAP_LEARN	150	/* ms, as stock's raw receiver */

#endif /* _MINIX_IR_H */
