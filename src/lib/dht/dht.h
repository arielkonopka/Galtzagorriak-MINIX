/* dht.h - the AM2301 / DHT21 / DHT22 temperature and humidity sensor
 * (Galtzagorriak MINIX, issue #358): the frame decoder, and the frame
 * encoder the host test uses, as a small library.
 *
 * The sensor has its own single-wire protocol (not Dallas 1-Wire).  The
 * host pulls the line low for at least 1 ms and lets go; the sensor
 * answers with
 *	low  ~80 us, high ~80 us				the response
 *	40 x ( low ~50 us, high ~27 us for a 0 or ~70 us for a 1 )	the bits
 *	low  ~50 us, then it lets go (the line idles high)
 * The 40 bits, most significant first, are: humidity (16 bits, tenths of
 * a percent), temperature (16 bits: bit 15 is the sign, the other 15 are
 * tenths of a degree, a magnitude, not two's complement) and a checksum
 * (the low 8 bits of the sum of the four bytes before it).  The older
 * DHT11 counts whole units and is not meant here.
 *
 * A "segment" is one level on the line and how long it lasted (us), as a
 * capture of edges gives them; segments alternate.  The decoder looks
 * only at the high levels, so it does not matter where the capture
 * starts or whether it begins with the host's own pulse.
 *
 * No malloc, no I/O.
 */
#ifndef DHT_H
#define DHT_H

#define DHT_BITS	40
#define DHT_MAXFRAME	86	/* segments of one answer (dht_frame) */

#define DHT_OK		0
#define DHT_SHORT	(-1)	/* too few pulses: no sensor, or cut off */
#define DHT_TIMING	(-2)	/* a pulse that is none of the sensor's */
#define DHT_CHECKSUM	(-3)

struct dht_reading {
	int temp_x10;		/* tenths of a degree Celsius, signed */
	int rh_x10;		/* tenths of a percent relative humidity */
};

/* Decode one capture: n segments of us[] microseconds each, the first at
 * level first_level (0 or 1).  DHT_OK, or a DHT_* error. */
int dht_decode(const unsigned short *us, int n, int first_level,
	       struct dht_reading *r);

/* The sensor's answer for a reading, as segments starting low (the
 * response).  Returns the number of segments (<= DHT_MAXFRAME).  bad_sum
 * is added to the checksum (0: a good frame).  For tests. */
int dht_frame(const struct dht_reading *r, int bad_sum, unsigned short *us);

/* "temp=23.4 rh=45.1" (the shape `dht` prints), NUL-terminated; buf has
 * room for 24 bytes.  Returns its length. */
int dht_format(const struct dht_reading *r, char *buf);

#endif /* DHT_H */
