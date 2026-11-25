/* usbcan.c - the usb canary's verdict and trace summary (spec 11.2); see
 * usbcan.h.
 */
#include <string.h>
#include "usbcan.h"

#define REQ_GET_DESCRIPTOR	6
#define REQ_SET_ADDRESS		5
#define REQ_SET_CONFIGURATION	9
#define DT_DEVICE		1
#define DT_STRING		3
#define DT_QUALIFIER		6
#define DT_BOS			15

void uc_init(struct uc_sum *s)
{
	memset(s, 0, sizeof(*s));
}

static void up(struct uc_sum *s, int level, unsigned long *t, unsigned long now)
{
	if (s->level >= level) return;
	s->level = (unsigned char)level;
	*t = now ? now : 1;
}

void uc_attach(struct uc_sum *s, unsigned long now)
{
	uc_init(s);
	s->level = UC_POWER;	/* provisional: "power only" is final after 3 s */
	s->t_attach = now;
}

void uc_detach(struct uc_sum *s)
{
	uc_init(s);
}

void uc_reset(struct uc_sum *s, unsigned long now)
{
	if (s->level == UC_NONE) return;
	if (s->resets < 255) s->resets++;
	if (s->setups) {
		if (s->reenum < 255) s->reenum++;
		s->setups = 0;	/* a new enumeration starts */
		if (s->level == UC_ENUM) s->level = UC_ALIVE;
	}
	up(s, UC_ALIVE, &s->t_alive, now);
}

void uc_sof(struct uc_sum *s, unsigned long now)
{
	if (s->level == UC_NONE) return;
	up(s, UC_ALIVE, &s->t_alive, now);
}

void uc_suspend(struct uc_sum *s)
{
	s->flags |= UCF_SUSPEND;
}

static void str(struct uc_sum *s, int idx, int lang)
{
	s->strings |= (unsigned char)(1 << (idx > 7 ? 7 : idx));
	if (idx == 0xEE) s->flags |= UCF_MSOS;
	if (idx != 0 && lang) s->langid = (unsigned short)lang;
}

void uc_setup(struct uc_sum *s, unsigned long now, const unsigned char p[8])
{
	int type = p[0] & 0x60, req = p[1], wvalue = p[2] | p[3] << 8;
	int wlen = p[6] | p[7] << 8;

	if (s->level == UC_NONE) return;
	if (s->setups < 255) s->setups++;
	up(s, UC_ALIVE, &s->t_alive, now);	/* a SETUP means a live host */
	if (type == 0x40) {			/* vendor request */
		s->flags |= UCF_MSOS;
	} else if (type == 0x20) {		/* class request */
		s->flags |= UCF_CLASSREQ;
	} else if (!(p[0] & 0x60) && req == REQ_GET_DESCRIPTOR) {
		switch (wvalue >> 8) {
		case DT_DEVICE:
			if (!s->first_wlen)
				s->first_wlen = (unsigned char)(wlen > 255 ? 255 : wlen);
			if (s->level < UC_ENUM && (s->flags & UCF_ADDRESS))
				up(s, UC_ENUM, &s->t_enum, now);
			break;
		case DT_STRING:
			str(s, wvalue & 0xff, p[4] | p[5] << 8);
			break;
		case DT_QUALIFIER:
			s->flags |= UCF_QUALIFIER;
			break;
		case DT_BOS:
			s->flags |= UCF_BOS;
			break;
		}
	} else if (!(p[0] & 0x60) && req == REQ_SET_ADDRESS) {
		s->flags |= UCF_ADDRESS;
		up(s, UC_ENUM, &s->t_enum, now);	/* the address was set */
	} else if (!(p[0] & 0x60) && req == REQ_SET_CONFIGURATION) {
		up(s, UC_ENUM, &s->t_enum, now);
		if (wvalue & 0xff) up(s, UC_CONFIG, &s->t_config, now);
	}
}

void uc_data(struct uc_sum *s, unsigned long now)
{
	if (s->level == UC_NONE) return;
	up(s, UC_USED, &s->t_used, now);
}

void uc_tick(struct uc_sum *s, unsigned long now)
{
	if (s->level == UC_POWER && now - s->t_attach >= UC_POWER_MS)
		s->powered = 1;
}

const char *uc_level_name(int level)
{
	switch (level) {
	case UC_POWER: return "power only";
	case UC_ALIVE: return "host alive";
	case UC_ENUM: return "enumerates";
	case UC_CONFIG: return "configures";
	case UC_USED: return "uses the function";
	}
	return "no port";
}
