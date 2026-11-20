/* bled's GATT serial service: the stock Flipper serial service as a pure
 * state machine over an abstract controller link (spec R15.4, R15.7, P.7;
 * issue #388).  No CPU2 access: the ACI calls are the caller's (ops), so
 * the same code runs over ST's stack on the device and over a fake
 * controller in the host test (flipper/tests/bled).
 *
 * Characteristics (stock serial_service_uuid.inc; to be confirmed with the
 * mobile app on the device):
 *   RX    phone -> Flipper, write			...62fe0000
 *   TX    Flipper -> phone, indicate		...61fe0000
 *   FLOW  free bytes of our RX buffer, read + notify	...63fe0000
 *   RPC   1 while an RPC session runs, read + notify	...64fe0000
 * FLOW is a 32-bit big-endian count: the phone never sends more than it
 * says (the credit); we add back what the RPC consumed and notify.
 */
#ifndef BLESERIAL_H
#define BLESERIAL_H

#include <stdint.h>

#define BS_SVC_UUID	"8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000"
#define BS_TX_UUID	"19ed82ae-ed21-4c9d-4145-228e61fe0000"
#define BS_RX_UUID	"19ed82ae-ed21-4c9d-4145-228e62fe0000"
#define BS_FLOW_UUID	"19ed82ae-ed21-4c9d-4145-228e63fe0000"
#define BS_RPC_UUID	"19ed82ae-ed21-4c9d-4145-228e64fe0000"

#define BS_RXBUF	1024	/* our RX buffer: the first credit */
#define BS_CHAR_MAX	243	/* largest characteristic value (stock) */
#define BS_MTU_MIN	23	/* ATT default */

enum { BS_CH_TX, BS_CH_RX, BS_CH_FLOW, BS_CH_RPC };

struct bs_ops {
  void *ctx;
  /* send a value: TX by indication, FLOW and RPC by notification; 0, or
   * -1 when the controller is busy (try again on bs_tx_done) */
  int (*update)(void *ctx, int ch, const unsigned char *b, unsigned n);
  /* bytes the phone wrote, for rpcd (rpc_feed) */
  void (*rx)(void *ctx, const unsigned char *b, unsigned n);
};

struct bs {
  const struct bs_ops *ops;
  int connected, rpc_on;
  unsigned mtu;			/* ATT MTU agreed */
  uint32_t credit;		/* bytes the phone may still send */
  uint32_t held;		/* received, not consumed by rpcd yet */
  const unsigned char *tx;	/* data being sent, not yet indicated */
  unsigned txn;
  int tx_wait;			/* an indication not confirmed yet */
  int flow_dirty;		/* FLOW to notify when the link is free */
  unsigned long dropped;	/* bytes the phone sent over its credit */
};

void bs_init(struct bs *s, const struct bs_ops *ops);
/* controller events */
void bs_connect(struct bs *s);
void bs_disconnect(struct bs *s);
void bs_mtu(struct bs *s, unsigned mtu);
void bs_write(struct bs *s, int ch, const unsigned char *b, unsigned n);
void bs_tx_done(struct bs *s);	/* an indication was confirmed */
/* from rpcd */
void bs_consumed(struct bs *s, unsigned n);	/* n RX bytes processed */
/* Send n bytes (b stays valid until bs_busy() is 0): -1 if not
 * connected or a send is still running. */
int bs_send(struct bs *s, const unsigned char *b, unsigned n);
int bs_busy(const struct bs *s);
void bs_rpc(struct bs *s, int on);	/* session start / end */
unsigned bs_chunk(const struct bs *s);	/* bytes per indication */

#endif
