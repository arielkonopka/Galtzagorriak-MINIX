/* follow - the `ble` canary's "a tracker is following you" rule (Galtzagorriak
 * MINIX, spec M.2, R16.5, task T35).
 *
 * A device that a scan sees again and again over a long time, in many
 * separate scans, while you moved, is following you.  The caller feeds
 * one (id, time) per scan hit (id: a small hash of the address, 32 bits;
 * rotating random addresses of trackers are the caller's problem) and
 * `flw_check` names the ones that crossed the line.
 *
 * Pure C, no I/O, no malloc, no writable statics.  Host-tested.
 */
#ifndef FOLLOW_H
#define FOLLOW_H

#define FLW_MAX 16	/* candidates tracked at once */

struct flw_rule {
	unsigned long span;	/* ms from first to last sighting (15 min) */
	unsigned char sightings; /* distinct sightings needed (6) */
	unsigned long apart;	/* ms between two sightings that count (60 s) */
};
#define FLW_DEFAULT { 15UL * 60 * 1000, 6, 60UL * 1000 }

struct flw_ent {
	unsigned long id;
	unsigned long first, last;
	unsigned char n;	/* sightings counted (stops at 255) */
	unsigned char used, flagged;
};

struct flw {
	struct flw_ent e[FLW_MAX];
};

void flw_init(struct flw *f);
/* A sighting at now ms.  Returns 1 the first time this device crosses the
 * rule (it then stays flagged), else 0.  A full table drops the entry
 * seen longest ago, unless it is flagged. */
int flw_seen(const struct flw_rule *r, struct flw *f,
					unsigned long id, unsigned long now);
/* Forget devices not seen for 2 * span. */
void flw_expire(const struct flw_rule *r, struct flw *f, unsigned long now);
/* The number of flagged devices. */
int flw_flagged(const struct flw *f);

#endif
