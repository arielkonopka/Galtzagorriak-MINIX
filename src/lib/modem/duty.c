/* duty.c - the duty-cycle counter of the Sub-GHz modem (spec RP.7).
 *
 * EN 300 220 counts the duty cycle over one hour.  Transmit time is kept in
 * 10 ms units in 7 slots of 10 minutes ([0] the slot `now` is in).  Any
 * 60-minute window that ends now lies inside those 7 slots, so allowing a
 * packet only while their sum plus its airtime stays within the limit keeps
 * every window within it.  14 bytes of counts; the kernel keeps the binding
 * copy (R8.2), the library its own to tell the user how long to wait.
 */
#include "modem.h"

void mdm_duty_init(struct mdm_duty *d)
{
	int i;

	d->slot0 = 0;
	for (i = 0; i < MDM_DUTY_SLOTS; i++) d->slot[i] = 0;
}

static void roll(struct mdm_duty *d, unsigned long now_s)
{
	unsigned long s = now_s / MDM_DUTY_SLOT_S, k;
	int i;

	if (s <= d->slot0) return;		/* same slot (or the clock went back) */
	k = s - d->slot0;
	if (k >= MDM_DUTY_SLOTS) {
		for (i = 0; i < MDM_DUTY_SLOTS; i++) d->slot[i] = 0;
	} else {
		for (i = MDM_DUTY_SLOTS - 1; i >= 0; i--)
			d->slot[i] = i >= (int) k ? d->slot[i - k] : 0;
	}
	d->slot0 = s;
}

unsigned long mdm_duty_used(struct mdm_duty *d, unsigned long now_s)
{
	unsigned long sum = 0;
	int i;

	roll(d, now_s);
	for (i = 0; i < MDM_DUTY_SLOTS; i++) sum += d->slot[i];
	return sum;
}

long mdm_duty_wait(struct mdm_duty *d, unsigned long now_s, int duty_pm,
	unsigned air_10ms)
{
	/* the limit in 10 ms units: duty_pm / 1000 of 3600 s */
	unsigned long limit = (unsigned long) duty_pm * 360UL, sum;
	int k;

	if (duty_pm >= 1000) return 0;		/* no limit in this band */
	if (air_10ms > limit) return -1;
	sum = mdm_duty_used(d, now_s);
	if (sum + air_10ms <= limit) return 0;
	/* the oldest slots leave the window one by one, at slot boundaries */
	for (k = MDM_DUTY_SLOTS - 1; k > 0; k--) {
		sum -= d->slot[k];
		if (sum + air_10ms <= limit)
			return (long) ((d->slot0 + (MDM_DUTY_SLOTS - k))
				* MDM_DUTY_SLOT_S - now_s);
	}
	/* only the current slot is left and it is full: wait it out */
	return (long) ((d->slot0 + MDM_DUTY_SLOTS) * MDM_DUTY_SLOT_S - now_s);
}

void mdm_duty_add(struct mdm_duty *d, unsigned long now_s, unsigned air_10ms)
{
	unsigned long v;

	roll(d, now_s);
	v = (unsigned long) d->slot[0] + air_10ms;
	d->slot[0] = (unsigned short) (v > 0xFFFFUL ? 0xFFFFUL : v);
}
