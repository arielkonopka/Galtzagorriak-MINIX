/* zones.h - the time zones people choose from (spec I.1, K.7, R18b.2):
 * a const table in flash, label -> POSIX TZ rule, shared by settings
 * time and the first-boot wizard (setup, Phase 18b).  No tz database on
 * the device.  /etc/TZ is the rule on its own line, with the label in a
 * '#' comment before it:
 *
 *	# Europe/Warsaw
 *	CET-1CEST,M3.5.0,M10.5.0/3
 *
 * Pure functions (no I/O), tested on the PC (tests/host/settimetest). */
#ifndef ZONES_H
#define ZONES_H

#define TZ_RULE_MAX	40	/* longest rule kept, '\0' included */
#define TZ_LABEL_MAX	32

int tz_count(void);		/* entries 0 .. tz_count() - 1 */
const char *tz_label(int i);	/* "other" past the end */
const char *tz_rule(int i);	/* "" past the end */

/* /etc/TZ's text (buf, '\0'-ended) into its rule and label ("" if none).
 * 0, or -1 when no rule line is there or it is too long. */
int tz_parse(const char *buf, char *rule, char *label);

/* The table entry for a label (first) or else a rule; tz_count() (the
 * "other" entry) when neither is in the table. */
int tz_find(const char *label, const char *rule);

/* /etc/TZ's text for table entry i into buf (n bytes): its length, or -1. */
int tz_format(char *buf, int n, int i);

/* Days in month m (1-12) of year y (Gregorian). */
int tz_mdays(int y, int m);

/* Local time without the libc's zone code: tz_off() the seconds to add to
 * UTC t under a POSIX TZ rule (east positive), tz_utc() the UTC of a local
 * time; tz_days() days since 1970-01-01 of a date, tz_date() back. */
long tz_off(const char *rule, long t);
long tz_utc(const char *rule, long local);
long tz_days(int y, int m, int d);
void tz_date(long days, int *y, int *m, int *d);

#endif
