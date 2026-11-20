/* GATT serial service host test (issue #388): a fake controller, then
 * rpcd behind it, the golden ping over the fake BLE link. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bleserial.h"
#include "rpcd.h"
#include "../flipperpb/golden.h"

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
		__LINE__, #c); fails++; } } while (0)

/* the fake controller: what went out on each characteristic */
static unsigned char txbuf[4096];
static unsigned txn, ntx, nflow, nrpc, busy;
static uint32_t flow, rpcv;
static unsigned char rxbuf[4096];
static unsigned rxn;
static struct bs s;

static uint32_t get32(const unsigned char *b)
{
  return (uint32_t) b[0] << 24 | (uint32_t) b[1] << 16 | b[2] << 8 | b[3];
}

static int c_update(void *c, int ch, const unsigned char *b, unsigned n)
{
  (void) c;
  if (busy) { busy--; return -1; }
  if (ch == BS_CH_TX) {
	if (n > bs_chunk(&s)) { printf("FAIL chunk %u > %u\n", n, bs_chunk(&s)); fails++; }
	memcpy(txbuf + txn, b, n), txn += n, ntx++;
  } else if (ch == BS_CH_FLOW) { T(n == 4); flow = get32(b); nflow++; }
  else if (ch == BS_CH_RPC) { T(n == 4); rpcv = get32(b); nrpc++; }
  return 0;
}
static void c_rx(void *c, const unsigned char *b, unsigned n)
{ (void) c; memcpy(rxbuf + rxn, b, n); rxn += n; }
static const struct bs_ops bops = { NULL, c_update, c_rx };

/* rpcd behind the link */
static struct rpc r;
static int r_send(void *c, const unsigned char *b, unsigned n)
{
  (void) c;
  if (bs_send(&s, b, n) < 0) return -1;
  while (bs_busy(&s)) bs_tx_done(&s);	/* the phone confirms each one */
  return 0;
}
static void r_rx(void *c, const unsigned char *b, unsigned n)
{
  (void) c;
  rpc_feed(&r, b, n);
  bs_consumed(&s, n);
}
static const struct bs_ops bops2 = { NULL, c_update, r_rx };
static const struct rpc_ops rops = { NULL, r_send, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL };

int main(void)
{
  unsigned char data[1500];
  unsigned i;
  void *st;

  for (i = 0; i < sizeof(data); i++) data[i] = (unsigned char) i;
  bs_init(&s, &bops);
  T(bs_send(&s, data, 10) == -1);		/* not connected */
  bs_connect(&s);
  T(nflow == 1 && flow == BS_RXBUF);		/* the first credit */
  T(bs_chunk(&s) == 20);
  /* 50 B at MTU 23: 20 + 20 + 10, one indication at a time */
  T(bs_send(&s, data, 50) == 0 && ntx == 1 && bs_busy(&s));
  T(bs_send(&s, data, 5) == -1);		/* still busy */
  bs_tx_done(&s); T(ntx == 2);
  bs_tx_done(&s); T(ntx == 3 && txn == 50 && bs_busy(&s));
  bs_tx_done(&s); T(!bs_busy(&s) && !memcmp(txbuf, data, 50));
  /* big MTU: capped at 243 */
  bs_mtu(&s, 517); T(bs_chunk(&s) == 243);
  bs_mtu(&s, 100); T(bs_chunk(&s) == 97);
  bs_mtu(&s, 5); T(bs_chunk(&s) == 20);
  /* controller busy: retried at the next tx_done */
  txn = ntx = 0;
  busy = 1;
  T(bs_send(&s, data, 30) == 0 && ntx == 0);
  bs_tx_done(&s); T(ntx == 1);
  bs_tx_done(&s); bs_tx_done(&s); T(txn == 30 && !bs_busy(&s));
  /* credit: the phone may send 1024; more is dropped */
  bs_write(&s, BS_CH_RX, data, 600);
  T(rxn == 600 && s.credit == 424);
  bs_write(&s, BS_CH_RX, data, 600);
  T(rxn == 1024 && s.credit == 0 && s.dropped == 176);
  bs_write(&s, BS_CH_TX, data, 10);		/* not writable: ignored */
  T(rxn == 1024);
  nflow = 0;
  bs_consumed(&s, 500);
  T(nflow == 1 && flow == 500);
  bs_consumed(&s, 9999);			/* never more than held */
  T(flow == 1024 && s.held == 0);
  /* FLOW waits behind an indication */
  bs_send(&s, data, 5);
  nflow = 0;
  bs_write(&s, BS_CH_RX, data, 10);
  bs_consumed(&s, 10);
  T(nflow == 0);
  bs_tx_done(&s);
  T(nflow == 1 && flow == 1024);
  bs_rpc(&s, 1); T(nrpc == 1 && rpcv == 1);
  /* disconnect drops everything; reconnect starts again */
  bs_send(&s, data, 100);
  bs_disconnect(&s);
  T(!bs_busy(&s) && !s.connected && s.mtu == BS_MTU_MIN);
  bs_write(&s, BS_CH_RX, data, 10);		/* ignored */
  T(rxn == 1034);

  /* rpcd over the fake link: the golden ping, cut in 20 B writes */
  st = malloc(rpc_state_size());
  rpc_init(&r, &rops, st);
  bs_init(&s, &bops2);
  bs_connect(&s);
  txn = 0;
  {
	const struct golden *g = &goldens[0], *h = &goldens[1];

	T(!strcmp(g->name, "ping_req") && !strcmp(h->name, "ping_resp"));
	for (i = 0; i < g->n; i += 3)
		bs_write(&s, BS_CH_RX, g->b + i, g->n - i < 3 ? g->n - i : 3);
	T(txn == h->n && !memcmp(txbuf, h->b, h->n));
	T(s.credit == BS_RXBUF);
	/* an unknown request over BLE: NOT_IMPLEMENTED, link stays up */
	txn = 0;
	{	/* id 7, system_reboot_request (field 31) */
		static const unsigned char f[] = { 0x05, 0x08, 0x07, 0xfa, 0x01, 0x00 };
		const struct golden *n = &goldens[5];

		bs_write(&s, BS_CH_RX, f, sizeof(f));
		T(!strcmp(n->name, "notimpl_resp") && txn == n->n
		  && !memcmp(txbuf, n->b, n->n) && s.connected);
	}
  }
  free(st);
  if (fails) { printf("bstest: %d failed\n", fails); return 1; }
  printf("bstest: all passed (struct bs %u B)\n", (unsigned) sizeof(struct bs));
  return 0;
}
