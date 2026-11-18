/* mdmz.h - libmodem's optional compression (spec P.3a, gmodem -z):
 * heatshrink LZSS, window 256 B, lookahead 16 B, static buffers.
 *
 * mdm_zget() is a source (mdm_get_fn) that compresses another source;
 * mdm_zput() is a sink (mdm_put_fn) that decompresses into another sink,
 * and mdm_zend() flushes it at the end of the stream.  Frames of a
 * compressed stream carry MDM_F_Z.  RAM: 0.5 KB for the encoder, 0.3 KB
 * for the decoder, only while they are used (the caller's memory).
 *
 * The decoder takes data from the air: it never writes outside its own
 * window, and a back reference of 13 bits repeats at most 16 bytes, so a
 * forged stream makes at most about 10 times its size of output.
 */
#ifndef _MDMZ_H
#define _MDMZ_H

#include "modem.h"
#include "heatshrink/heatshrink_encoder.h"
#include "heatshrink/heatshrink_decoder.h"

struct mdm_zsrc {
	mdm_get_fn get;
	void *u;
	heatshrink_encoder enc;
	unsigned char in[64];
	int inlen, inpos, eof, done;
};
struct mdm_zsink {
	mdm_put_fn put;
	void *u;
	heatshrink_decoder dec;
};

void mdm_zsrc_init(struct mdm_zsrc *z, mdm_get_fn get, void *u);
int mdm_zget(void *z, unsigned char *buf, int max);
void mdm_zsink_init(struct mdm_zsink *z, mdm_put_fn put, void *u);
int mdm_zput(void *z, const unsigned char *buf, int n);
int mdm_zend(struct mdm_zsink *z);

#endif /* _MDMZ_H */
