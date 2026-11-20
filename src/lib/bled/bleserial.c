/* bled's GATT serial service state machine, see bleserial.h. */
#include <string.h>
#include "bleserial.h"

void bs_init(struct bs *s, const struct bs_ops *ops)
{
  memset(s, 0, sizeof(*s));
  s->ops = ops;
  s->mtu = BS_MTU_MIN;
}

unsigned bs_chunk(const struct bs *s)
{
  unsigned c = s->mtu - 3;		/* ATT header of a notification */

  return c > BS_CHAR_MAX ? BS_CHAR_MAX : c;
}

static void put32(unsigned char *b, uint32_t v)
{
  b[0] = (unsigned char) (v >> 24), b[1] = (unsigned char) (v >> 16);
  b[2] = (unsigned char) (v >> 8), b[3] = (unsigned char) v;
}

/* One step: the next TX chunk, else a pending FLOW notification. */
static void pump(struct bs *s)
{
  unsigned char f[4];
  unsigned c;

  if (!s->connected || s->tx_wait) return;
  if (s->txn > 0) {
	c = bs_chunk(s);
	if (c > s->txn) c = s->txn;
	if (s->ops->update(s->ops->ctx, BS_CH_TX, s->tx, c) != 0) return;
	s->tx += c, s->txn -= c;
	s->tx_wait = 1;
	return;
  }
  if (s->flow_dirty) {
	put32(f, s->credit);
	if (s->ops->update(s->ops->ctx, BS_CH_FLOW, f, 4) == 0)
		s->flow_dirty = 0;
  }
}

void bs_connect(struct bs *s)
{
  const struct bs_ops *o = s->ops;

  bs_init(s, o);
  s->connected = 1;
  s->credit = BS_RXBUF;
  s->flow_dirty = 1;		/* the phone reads FLOW first */
  pump(s);
}

void bs_disconnect(struct bs *s)
{
  const struct bs_ops *o = s->ops;

  bs_init(s, o);		/* drops what was being sent */
}

void bs_mtu(struct bs *s, unsigned mtu)
{
  if (mtu < BS_MTU_MIN) mtu = BS_MTU_MIN;
  s->mtu = mtu;
}

void bs_write(struct bs *s, int ch, const unsigned char *b, unsigned n)
{
  if (!s->connected || ch != BS_CH_RX || n == 0) return;
  if (n > s->credit) {		/* over the credit: keep what fits */
	s->dropped += n - s->credit;
	n = s->credit;
  }
  s->credit -= n;
  s->held += n;
  if (n) s->ops->rx(s->ops->ctx, b, n);
}

void bs_consumed(struct bs *s, unsigned n)
{
  if (!s->connected) return;
  if (n > s->held) n = s->held;
  s->held -= n;
  s->credit += n;
  if (n) {
	s->flow_dirty = 1;
	pump(s);
  }
}

void bs_tx_done(struct bs *s)
{
  s->tx_wait = 0;
  pump(s);
}

int bs_send(struct bs *s, const unsigned char *b, unsigned n)
{
  if (!s->connected || bs_busy(s)) return -1;
  s->tx = b;
  s->txn = n;
  pump(s);
  return 0;
}

int bs_busy(const struct bs *s)
{
  return s->txn > 0 || s->tx_wait;
}

void bs_rpc(struct bs *s, int on)
{
  unsigned char v[4];

  s->rpc_on = on != 0;
  if (!s->connected) return;
  put32(v, (uint32_t) s->rpc_on);
  (void) s->ops->update(s->ops->ctx, BS_CH_RPC, v, 4);
}
