/* beacon - the `beacon` canary's core (Galtzagorriak MINIX, spec M.0,
 * R16.5.7, task T35): recognise positioning beacons in one BLE advertising
 * payload and group them into deployments ("places", not people).
 *
 * Pure C: no I/O, no malloc, no writable statics; the caller owns all
 * state.  Observe only: there is no transmit path here or anywhere in the
 * canary (M.0).  Host-tested (make test); the scan (16.3, M7) and the UI
 * come later.
 */
#ifndef BEACON_H
#define BEACON_H

/* What bc_parse found (bc_frame.kind, and its return value). */
enum bc_kind {
	BC_NONE,	/* a well-formed payload that is not a beacon */
	BC_IBEACON,	/* manufacturer data 0x004C, type 0x02, length 0x15 */
	BC_EDDY_UID,	/* service data 0xFEAA, frame 0x00 */
	BC_EDDY_URL,	/* frame 0x10 */
	BC_EDDY_TLM,	/* frame 0x20 (telemetry; no stable ID) */
	BC_EDDY_EID,	/* frame 0x30 (ephemeral ID; no stable ID) */
	BC_ALTBEACON	/* manufacturer data, beacon code 0xBEAC */
};
#define BC_BAD		(-1)	/* a length byte runs past the payload, or a
				 * beacon structure has the wrong length or
				 * a bad field: nothing in it is trusted */

#define BC_NOTX		(-128)	/* bc_frame.tx: no calibrated power (TLM) */
#define BC_TEMP_NONE	(-32768) /* bc_frame.temp: TLM has no sensor */

struct bc_frame {
	unsigned char kind;	/* enum bc_kind */
	signed char tx;		/* calibrated power, dBm: iBeacon and AltBeacon
				 * at 1 m, Eddystone at 0 m; BC_NOTX */
	unsigned char idlen;	/* bytes of id[] used */
	unsigned char id[20];	/* iBeacon: UUID(16) major(2) minor(2), big
				 * endian; Eddystone-UID: namespace(10)
				 * instance(6); AltBeacon: beacon ID(20);
				 * Eddystone-EID: EID(8); Eddystone-URL:
				 * scheme(1) + encoded URL (0-17); TLM: none */
	unsigned short mfg;	/* iBeacon 0x004C; AltBeacon: manufacturer */
	unsigned char rsv;	/* AltBeacon: manufacturer reserved byte */
	unsigned char tlmver;	/* TLM: 0 plain, other: encrypted or unknown
				 * (the fields below stay "unknown") */
	unsigned short vbatt;	/* TLM: battery mV, 0 = not known */
	short temp;		/* TLM: 1/256 degree C, BC_TEMP_NONE */
	unsigned long advcnt;	/* TLM: advertising PDUs since power-up */
	unsigned long secs;	/* TLM: 0.1 s units since power-up */
};
#define BC_MAJOR(f)	((unsigned)(f)->id[16] << 8 | (f)->id[17])
#define BC_MINOR(f)	((unsigned)(f)->id[18] << 8 | (f)->id[19])

/* Parse one advertising (or scan response) payload: the AD structures,
 * len bytes at adv (legacy: at most 31; longer extended payloads work
 * too).  A length byte of 0 ends the payload (the rest is padding).
 * Every length byte is checked against len (I2).  Returns the first
 * beacon's enum bc_kind (and fills f), BC_NONE, or BC_BAD (f->kind is
 * then BC_NONE). */
int bc_parse(const unsigned char *adv, int len, struct bc_frame *f);

/* An Eddystone-URL frame as text (expansions applied), NUL-terminated in
 * buf (n > 0 bytes, cut if short).  Shown only, never opened.  Returns
 * the length written, or -1 if f is not a URL frame (buf = ""). */
int bc_url(const struct bc_frame *f, char *buf, int n);

/* ---- Deployments (R16.5.7) ----
 * Grouped by iBeacon UUID, Eddystone-UID namespace, the first 16 bytes of
 * an AltBeacon ID, and the host name of an Eddystone-URL.  TLM and EID
 * frames are "anonymous Eddystone".  Beacons inside a deployment: iBeacon
 * major/minor, Eddystone instance, AltBeacon ID bytes 16-19, URL and
 * anonymous ones by their BLE address.  Both tables are the caller's, of
 * fixed size; when one is full its least recently seen entry goes.  Other
 * devices the scan hears are only counted (privacy, M.1). */
#define BC_KEYLEN	18
struct bc_dep {
	unsigned long last;	/* bc_groups.clock when last seen */
	unsigned long frames;	/* beacon frames seen */
	unsigned short beacons;	/* beacons in bc_groups.bcn now */
	unsigned char kind;	/* BC_IBEACON, BC_EDDY_UID, BC_ALTBEACON,
				 * BC_EDDY_URL; BC_NONE = free slot */
	unsigned char klen;
	unsigned char key[BC_KEYLEN];	/* URL: www flag, then the host,
				 * encoded, A-Z folded to a-z */
	signed char best;	/* strongest RSSI, dBm */
};
#define BC_FREE		0xFE	/* bc_bcn.dep: free slot */
#define BC_ANON		0xFF	/* bc_bcn.dep: anonymous Eddystone */
struct bc_bcn {
	unsigned long last;
	unsigned char dep;	/* index in bc_groups.dep, BC_FREE, BC_ANON */
	unsigned char sub[6];
	signed char best;
};
struct bc_groups {
	struct bc_dep *dep;
	struct bc_bcn *bcn;
	int ndep, nbcn;		/* ndep at most 254, nbcn at most 65535 */
	unsigned long clock;	/* one tick per bc_observe */
	unsigned long anon;	/* anonymous Eddystone frames */
	unsigned long other;	/* payloads that were not beacons */
	unsigned long bad;	/* BC_BAD payloads */
};
struct bc_sum {
	int deps;		/* deployments */
	int beacons;		/* beacons in them */
	int anon;		/* anonymous Eddystone beacons */
	int best;		/* strongest RSSI of any beacon, BC_NOTX: none */
};

void bc_init(struct bc_groups *g, struct bc_dep *dep, int ndep,
					struct bc_bcn *bcn, int nbcn);
/* Count one parsed payload (bc_parse's return in kind, its frame in f),
 * heard from addr (6 bytes, may be 0) at rssi dBm.  Returns the
 * deployment's index, BC_ANON for anonymous Eddystone, -1 otherwise. */
int bc_observe(struct bc_groups *g, int kind, const struct bc_frame *f,
				const unsigned char *addr, int rssi);
void bc_summary(const struct bc_groups *g, struct bc_sum *s);
/* A deployment's name for the screen: the UUID / namespace / ID prefix in
 * hex, or the URL host as text.  Same contract as bc_url. */
int bc_dep_text(const struct bc_dep *d, char *buf, int n);

#endif
