/* zstream.c - compression for libmodem (spec P.3a): heatshrink as a
 * source and a sink, so it sits between a file and mdm_send/mdm_recv or
 * the carousel without either knowing. */
#include "mdmz.h"

void mdm_zsrc_init(struct mdm_zsrc *z, mdm_get_fn get, void *u)
{
	z->get = get;
	z->u = u;
	heatshrink_encoder_reset(&z->enc);
	z->inlen = z->inpos = z->eof = z->done = 0;
}

int mdm_zget(void *p, unsigned char *buf, int max)
{
	struct mdm_zsrc *z = p;
	size_t n, sunk;
	int r;

	for (;;) {
		n = 0;
		if (heatshrink_encoder_poll(&z->enc, buf, (size_t) max, &n) < 0)
			return -1;
		if (n > 0) return (int) n;
		if (z->done) return 0;
		if (z->inpos < z->inlen) {
			if (heatshrink_encoder_sink(&z->enc, z->in + z->inpos,
				(size_t) (z->inlen - z->inpos), &sunk) < 0) return -1;
			if (sunk == 0) return -1;	/* full and nothing out */
			z->inpos += (int) sunk;
		} else if (!z->eof) {
			r = z->get(z->u, z->in, (int) sizeof(z->in));
			if (r < 0) return r;
			if (r == 0) z->eof = 1;
			z->inlen = r;
			z->inpos = 0;
		} else if (heatshrink_encoder_finish(&z->enc) == HSER_FINISH_DONE) {
			z->done = 1;
		}
	}
}

void mdm_zsink_init(struct mdm_zsink *z, mdm_put_fn put, void *u)
{
	z->put = put;
	z->u = u;
	heatshrink_decoder_reset(&z->dec);
}

static int drain(struct mdm_zsink *z)
{
	unsigned char out[64];
	size_t m;
	HSD_poll_res pr;

	do {
		m = 0;
		pr = heatshrink_decoder_poll(&z->dec, out, sizeof(out), &m);
		if (pr < 0) return -1;
		if (m > 0 && z->put(z->u, out, (int) m) < 0) return -1;
	} while (pr == HSDR_POLL_MORE);
	return 0;
}

int mdm_zput(void *p, const unsigned char *buf, int n)
{
	struct mdm_zsink *z = p;
	size_t sunk;
	int off = 0;

	while (off < n) {
		if (heatshrink_decoder_sink(&z->dec, (uint8_t *) buf + off,
			(size_t) (n - off), &sunk) < 0) return -1;
		off += (int) sunk;
		if (drain(z) < 0) return -1;
		if (sunk == 0 && off < n) return -1;
	}
	return 0;
}

int mdm_zend(struct mdm_zsink *z)
{
	HSD_finish_res fr;
	int k;

	for (k = 0; k < 64; k++) {
		fr = heatshrink_decoder_finish(&z->dec);
		if (fr < 0) return -1;
		if (fr == HSDR_FINISH_DONE) return 0;
		if (drain(z) < 0) return -1;
	}
	return -1;
}
