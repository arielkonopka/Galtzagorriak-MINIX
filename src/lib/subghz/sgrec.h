/* SPDX-License-Identifier: GPL-3.0-or-later
 * sgrec - the Read RAW recorder's core (issue #419): received levels and
 * durations as a stock RAW .sub file ("Flipper SubGhz RAW File", RAW_Data
 * lines of up to SGREC_LINE signed values: on > 0, off < 0), given to an
 * output function a buffer at a time, so a recording never stays in RAM:
 * its length is bounded by where the output goes (the card) and by the
 * limits.  No I/O of its own: `subghz rec` gives it write(), the host
 * test a string.  RAM: one SgRec (SGREC_BUF + ~48 B). */
#ifndef SUBGHZ_SGREC_H
#define SUBGHZ_SGREC_H

#include <stdbool.h>
#include <stdint.h>

#define SGREC_LINE	512		/* values on a RAW_Data line (stock) */
#define SGREC_BUF	1024		/* bytes buffered before an output */

typedef struct {
    int (*out)(void* ctx, const char* buf, int n);	/* n, or < 0 */
    void* ctx;
    char buf[SGREC_BUF];
    int len, vals, on, level, err;
    uint32_t dur;
    unsigned long n, max;		/* values written; max: 0 none */
    unsigned long long us, max_us;	/* time recorded; max_us: 0 none */
} SgRec;

void sgrec_init(SgRec* r, int (*out)(void*, const char*, int), void* ctx,
                unsigned long max, unsigned long long max_us);
void sgrec_header(SgRec* r, unsigned long hz);
/* One level for us; the same level twice in a row is one value.
 * 1: a limit is reached or the output failed, stop. */
int sgrec_put(SgRec* r, bool level, uint32_t us);
/* The last level (unless -n ended it), the rest of the buffer.  0, or
 * the output's failure (-1). */
int sgrec_end(SgRec* r);
/* The RSSI threshold: the dBm of a /dev/subghz line ("rssi=-92 peak=-41
 * freq=..."; the peak when there is one).  1, or 0: none (off). */
int sgrec_rssi(const char* line, int* dbm);

#endif
