/* rpcd core: the stock Flipper RPC as a transport-independent state
 * machine (spec R15.5, R15.7 profile "phone" steps 1-3; issue #388).
 *
 * Bytes from any transport (USB CDC, BLE serial, a pty in tests) go into
 * rpc_feed(); whole frames (varint length + PB.Main, libflipperpb) are
 * decoded and answered through ops->send, one frame per call.  Requests
 * not listed below get ERROR_NOT_IMPLEMENTED, never a disconnect.
 *
 * Answered: system ping, protobuf_version, device_info, power_info,
 * get/set_datetime; storage info, stat, list, read, only under /ext.  The
 * platform part (rpcd.c's main, the tests) gives the facts through ops;
 * the file system is reached through ops too, so rights are those of the
 * process (rpcd's user).
 */
#ifndef RPCD_H
#define RPCD_H

#include <stdint.h>

#define RPC_MAX_FRAME	1100	/* largest frame taken or sent: a 1024 B
				 * ping (stock's limit) plus the header */
#define RPC_READ_CHUNK	512	/* storage_read data per frame, as stock */
#define RPC_LIST_CHUNK	8	/* files per list frame, as stock */
#define RPC_PATH_MAX	255	/* storage.options */

struct rpc;

struct rpc_dt {			/* the stock DateTime, wall time */
  int year, month, day, hour, minute, second, weekday;
};

struct rpc_file {
  int dir;			/* 1: a directory */
  uint32_t size;
  char name[RPC_PATH_MAX + 1];
};

/* Statuses the callbacks return: PB_CommandStatus values (0 = OK). */
struct rpc_ops {
  void *ctx;
  /* one encoded frame out; 0 or -1 (the transport is gone) */
  int (*send)(void *ctx, const unsigned char *b, unsigned n);
  /* device_info / power_info: call rpc_kv() for each pair, in order */
  int (*device_info)(void *ctx, struct rpc *r);
  int (*power_info)(void *ctx, struct rpc *r);
  int (*get_time)(void *ctx, struct rpc_dt *dt);
  /* fields already checked (R15.7); the zone rules are the platform's */
  int (*set_time)(void *ctx, const struct rpc_dt *dt);
  /* storage, path already checked to be under /ext */
  int (*fs_info)(void *ctx, const char *path, uint64_t *total,
		 uint64_t *free);
  int (*fs_stat)(void *ctx, const char *path, struct rpc_file *f);
  /* call rpc_list_add() for each entry */
  int (*fs_list)(void *ctx, const char *path, struct rpc *r);
  /* read up to n bytes at off: bytes read (0: end) or -status */
  long (*fs_read)(void *ctx, const char *path, uint32_t off,
		  unsigned char *b, unsigned n);
};

struct rpc_state;			/* rpcd.c: the decoder's buffers */

struct rpc {
  const struct rpc_ops *ops;
  struct rpc_state *s;		/* rpc_init()'s second argument */
  int dead;			/* send failed: drop everything */
};

/* What the caller sets aside for one session (no malloc in the core;
 * nanopb mallocs the request's strings and frees them after the answer). */
unsigned rpc_state_size(void);

void rpc_init(struct rpc *r, const struct rpc_ops *ops, void *state);
/* Feed n bytes; answers every whole frame.  0, or -1 if r->dead. */
int rpc_feed(struct rpc *r, const unsigned char *b, unsigned n);

/* From device_info / power_info: one key/value frame. */
int rpc_kv(struct rpc *r, const char *key, const char *value);
/* From fs_list: one entry (sent in frames of RPC_LIST_CHUNK). */
int rpc_list_add(struct rpc *r, const struct rpc_file *f);

/* Helpers, exported for the tests. */
int rpc_path_ok(const char *p);		/* under /ext, no "..": 1 */
int rpc_dt_ok(const struct rpc_dt *dt);	/* R15.7 field check: 1 */
int rpc_weekday(int y, int m, int d);	/* 1 = Monday .. 7 = Sunday */

#endif
