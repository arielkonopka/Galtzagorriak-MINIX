/* rpcd core host test (issue #388, R15.5, R15.7 steps 1-3): a fake
 * platform, requests from the golden frames and from nanopb. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "flipper.pb.h"
#include "rpcd.h"
#include "../flipperpb/golden.h"

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
		__LINE__, #c); fails++; } } while (0)

/* what rpcd sent */
static unsigned char sent[64][RPC_MAX_FRAME];
static unsigned sentn[64], nsent;
static int send_fail;
static struct rpc_dt now = { 2026, 11, 20, 21, 5, 0, 0 }, setto;
static int nset;
static unsigned char file[1300];

static int f_send(void *c, const unsigned char *b, unsigned n)
{
  (void) c;
  if (send_fail) return -1;
  if (nsent < 64) { memcpy(sent[nsent], b, n); sentn[nsent] = n; }
  nsent++;
  return 0;
}
static int f_devinfo(void *c, struct rpc *r)
{
  (void) c;
  rpc_kv(r, "hardware_model", "Flipper Zero");
  rpc_kv(r, "firmware_origin", "Galtzagorriak MINIX");
  return rpc_kv(r, "firmware_version", "0.1.0");
}
static int f_time(void *c, struct rpc_dt *d) { (void) c; *d = now; return 0; }
static int f_settime(void *c, const struct rpc_dt *d)
{ (void) c; setto = *d; nset++; return 0; }
static int f_info(void *c, const char *p, uint64_t *t, uint64_t *f)
{ (void) c; (void) p; *t = 8ULL << 30; *f = 5ULL << 30; return 0; }
static int f_stat(void *c, const char *p, struct rpc_file *f)
{
  (void) c;
  memset(f, 0, sizeof(*f));
  if (strcmp(p, "/ext/big") == 0) { strcpy(f->name, "big"); f->size = sizeof(file); return 0; }
  if (strcmp(p, "/ext") == 0 || strcmp(p, "/ext/d") == 0) { f->dir = 1; return 0; }
  return PB_CommandStatus_ERROR_STORAGE_NOT_EXIST;
}
static int f_list(void *c, const char *p, struct rpc *r)
{
  struct rpc_file f;
  int i;

  (void) c;
  if (strcmp(p, "/ext/d") == 0) return 0;		/* empty */
  if (strcmp(p, "/ext") != 0) return PB_CommandStatus_ERROR_STORAGE_NOT_EXIST;
  for (i = 0; i < 10; i++) {
	memset(&f, 0, sizeof(f));
	snprintf(f.name, sizeof(f.name), "f%d", i);
	f.dir = i == 9;
	f.size = (uint32_t) i;
	rpc_list_add(r, &f);
  }
  return 0;
}
static long f_read(void *c, const char *p, uint32_t off, unsigned char *b,
		   unsigned n)
{
  (void) c; (void) p;
  if (off >= sizeof(file)) return 0;
  if (n > sizeof(file) - off) n = sizeof(file) - off;
  memcpy(b, file + off, n);
  return n;
}

static const struct rpc_ops ops = { NULL, f_send, f_devinfo, NULL, f_time,
	f_settime, f_info, f_stat, f_list, f_read };
static struct rpc r;
static void *st;

static void reset(void) { nsent = 0; rpc_init(&r, &ops, st); }

static void feed(const unsigned char *b, unsigned n, int bytewise)
{
  unsigned i;

  if (!bytewise) { rpc_feed(&r, b, n); return; }
  for (i = 0; i < n; i++) rpc_feed(&r, b + i, 1);
}

static void req(PB_Main *m, int bytewise)
{
  unsigned char b[RPC_MAX_FRAME];
  pb_ostream_t o = pb_ostream_from_buffer(b, sizeof(b));

  if (!pb_encode_ex(&o, PB_Main_fields, m, PB_ENCODE_DELIMITED)) {
	printf("FAIL encoding a request\n"); fails++; return;
  }
  feed(b, (unsigned) o.bytes_written, bytewise);
}

static int ans(unsigned i, PB_Main *m)		/* decode sent[i] */
{
  pb_istream_t s = pb_istream_from_buffer(sent[i], sentn[i]);
  PB_Main z = PB_Main_init_zero;

  *m = z;
  return pb_decode_ex(&s, PB_Main_fields, m, PB_DECODE_DELIMITED) ? 0 : -1;
}

static const struct golden *gold(const char *n)
{
  unsigned i;

  for (i = 0; i < sizeof(goldens) / sizeof(goldens[0]); i++)
	if (!strcmp(goldens[i].name, n)) return &goldens[i];
  return NULL;
}

int main(void)
{
  const struct golden *g, *h;
  PB_Main m, a;
  unsigned i;
  unsigned char big[2000];

  st = malloc(rpc_state_size());
  for (i = 0; i < sizeof(file); i++) file[i] = (unsigned char) (i * 7);
  printf("rpcd state %u B, max frame %u B\n", rpc_state_size(), RPC_MAX_FRAME);

  /* golden: ping and protobuf_version, whole and byte by byte */
  for (i = 0; i < 2; i++) {
	reset();
	g = gold("ping_req"), h = gold("ping_resp");
	feed(g->b, g->n, (int) i);
	T(nsent == 1 && sentn[0] == h->n && !memcmp(sent[0], h->b, h->n));
	g = gold("pbver_req"), h = gold("pbver_resp");
	feed(g->b, g->n, (int) i);
	T(nsent == 2 && sentn[1] == h->n && !memcmp(sent[1], h->b, h->n));
  }
  /* two frames in one write */
  reset();
  memcpy(big, gold("ping_req")->b, gold("ping_req")->n);
  memcpy(big + gold("ping_req")->n, gold("pbver_req")->b, gold("pbver_req")->n);
  feed(big, gold("ping_req")->n + gold("pbver_req")->n, 0);
  T(nsent == 2);

  /* unknown request: ERROR_NOT_IMPLEMENTED with its id, and the session
   * goes on (golden notimpl_resp has id 7) */
  reset();
  m = (PB_Main) PB_Main_init_zero;
  m.command_id = 7;
  m.which_content = PB_Main_system_reboot_request_tag;
  req(&m, 0);
  h = gold("notimpl_resp");
  T(nsent == 1 && sentn[0] == h->n && !memcmp(sent[0], h->b, h->n));
  m.which_content = PB_Main_gui_start_screen_stream_request_tag;
  req(&m, 0);
  T(nsent == 2 && ans(1, &a) == 0
    && a.command_status == PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
  g = gold("ping_req");
  feed(g->b, g->n, 0);
  T(nsent == 3);

  /* device_info: one pair per frame, has_next on all but the last */
  reset();
  m = (PB_Main) PB_Main_init_zero;
  m.command_id = 3;
  m.which_content = PB_Main_system_device_info_request_tag;
  req(&m, 0);
  T(nsent == 3);
  T(ans(0, &a) == 0 && a.has_next && a.command_id == 3
    && !strcmp(a.content.system_device_info_response.key, "hardware_model"));
  h = gold("devinfo_resp");		/* the same bytes as Google's */
  T(sentn[0] == h->n && !memcmp(sent[0], h->b, h->n));
  pb_release(PB_Main_fields, &a);
  T(ans(2, &a) == 0 && !a.has_next
    && !strcmp(a.content.system_device_info_response.value, "0.1.0"));
  pb_release(PB_Main_fields, &a);
  /* power_info without a callback: not implemented */
  m.which_content = PB_Main_system_power_info_request_tag;
  req(&m, 0);
  T(nsent == 4 && ans(3, &a) == 0
    && a.command_status == PB_CommandStatus_ERROR_NOT_IMPLEMENTED);

  /* datetime */
  reset();
  m = (PB_Main) PB_Main_init_zero;
  m.which_content = PB_Main_system_get_datetime_request_tag;
  req(&m, 0);
  T(ans(0, &a) == 0 && a.content.system_get_datetime_response.datetime.year == 2026
    && a.content.system_get_datetime_response.datetime.weekday == 5);
  g = gold("setdt_req");
  feed(g->b, g->n, 0);
  T(nset == 1 && setto.year == 2026 && setto.hour == 20 && setto.weekday == 5);
  T(ans(1, &a) == 0 && a.command_status == PB_CommandStatus_OK && a.command_id == 5);
  m = (PB_Main) PB_Main_init_zero;
  m.which_content = PB_Main_system_set_datetime_request_tag;
  m.content.system_set_datetime_request.has_datetime = 1;
  {
	PB_System_DateTime d = { 12, 0, 0, 30, 2, 2026, 3 };	/* Feb 30 */
	m.content.system_set_datetime_request.datetime = d;
	req(&m, 0);
	d.day = 29, d.year = 2028;			/* leap: fine */
	m.content.system_set_datetime_request.datetime = d;
	req(&m, 0);
	d.year = 2100;
	m.content.system_set_datetime_request.datetime = d;
	req(&m, 0);
	d.year = 2030, d.day = 1, d.hour = 24;
	m.content.system_set_datetime_request.datetime = d;
	req(&m, 0);
  }
  T(nset == 2 && setto.year == 2028 && setto.weekday == 2);
  T(ans(2, &a) == 0 && a.command_status == PB_CommandStatus_ERROR_INVALID_PARAMETERS);
  T(ans(4, &a) == 0 && a.command_status == PB_CommandStatus_ERROR_INVALID_PARAMETERS);
  T(ans(5, &a) == 0 && a.command_status == PB_CommandStatus_ERROR_INVALID_PARAMETERS);

  /* storage: list in frames of 8, empty dir, outside /ext refused */
  reset();
  g = gold("list_req");
  feed(g->b, g->n, 0);
  T(nsent == 2);
  T(ans(0, &a) == 0 && a.has_next
    && a.content.storage_list_response.file_count == 8);
  pb_release(PB_Main_fields, &a);
  T(ans(1, &a) == 0 && !a.has_next
    && a.content.storage_list_response.file_count == 2
    && a.content.storage_list_response.file[1].type == PB_Storage_File_FileType_DIR);
  pb_release(PB_Main_fields, &a);
  {
	static const char *badp[] = { "/etc", "/ext/../etc", "/extra",
				      "/ext/a/..", "ext", "/ext/\001" };
	for (i = 0; i < 6; i++) {
		reset();
		m = (PB_Main) PB_Main_init_zero;
		m.which_content = PB_Main_storage_list_request_tag;
		m.content.storage_list_request.path = (char *) badp[i];
		req(&m, 0);
		T(nsent == 1 && ans(0, &a) == 0 && a.command_status
		  == PB_CommandStatus_ERROR_STORAGE_INVALID_NAME);
	}
  }
  reset();
  m = (PB_Main) PB_Main_init_zero;
  m.which_content = PB_Main_storage_list_request_tag;
  m.content.storage_list_request.path = "/ext/d";
  req(&m, 0);
  T(nsent == 1 && ans(0, &a) == 0 && !a.has_next
    && a.content.storage_list_response.file_count == 0);
  m.content.storage_list_request.path = "/ext/none";
  req(&m, 0);
  T(ans(1, &a) == 0 && a.command_status == PB_CommandStatus_ERROR_STORAGE_NOT_EXIST);
  /* stat, info */
  reset();
  m = (PB_Main) PB_Main_init_zero;
  m.which_content = PB_Main_storage_stat_request_tag;
  m.content.storage_stat_request.path = "/ext/big";
  req(&m, 0);
  T(ans(0, &a) == 0 && a.content.storage_stat_response.has_file
    && a.content.storage_stat_response.file.size == sizeof(file));
  pb_release(PB_Main_fields, &a);
  m.which_content = PB_Main_storage_info_request_tag;
  m.content.storage_info_request.path = "/ext";
  req(&m, 0);
  T(ans(1, &a) == 0 && a.content.storage_info_response.free_space == 5ULL << 30);
  /* read: 512 + 512 + 276, has_next 1 1 0, the bytes right */
  reset();
  m = (PB_Main) PB_Main_init_zero;
  m.command_id = 9;
  m.which_content = PB_Main_storage_read_request_tag;
  m.content.storage_read_request.path = "/ext/big";
  req(&m, 0);
  T(nsent == 3);
  {
	unsigned off = 0;
	for (i = 0; i < 3 && i < nsent; i++) {
		T(ans(i, &a) == 0 && a.command_id == 9);
		T(a.has_next == (i < 2));
		pb_bytes_array_t *d = a.content.storage_read_response.file.data;
		T(d && d->size == (i < 2 ? 512 : 276)
		  && !memcmp(d->bytes, file + off, d->size));
		off += d ? d->size : 0;
		pb_release(PB_Main_fields, &a);
	}
  }
  m.content.storage_read_request.path = "/ext/d";		/* a dir */
  req(&m, 0);
  T(ans(3, &a) == 0 && a.command_status == PB_CommandStatus_ERROR_STORAGE_NOT_EXIST);

  /* framing: a too-big frame is dropped with ERROR_DECODE, the next
   * frame works; garbage gives ERROR_DECODE, never a hang */
  reset();
  big[0] = 0xD0, big[1] = 0x0F;			/* 2000 B */
  memset(big + 2, 0x55, sizeof(big) - 2);
  feed(big, sizeof(big), 0);
  feed(big + 2, 2002 - sizeof(big), 0);
  T(nsent == 1 && ans(0, &a) == 0
    && a.command_status == PB_CommandStatus_ERROR_DECODE);
  g = gold("ping_req");
  feed(g->b, g->n, 1);
  T(nsent == 2 && sentn[1] == gold("ping_resp")->n);
  reset();
  memset(big, 0xFF, 8);
  feed(big, 8, 0);
  T(nsent >= 1 && ans(0, &a) == 0
    && a.command_status == PB_CommandStatus_ERROR_DECODE);
  reset();
  big[0] = 3, big[1] = 0xFF, big[2] = 0xFF, big[3] = 0xFF;	/* bad body */
  feed(big, 4, 0);
  T(nsent == 1 && ans(0, &a) == 0
    && a.command_status == PB_CommandStatus_ERROR_DECODE);
  /* random bytes: no crash, no leak (ASan) */
  reset();
  srand(1);
  for (i = 0; i < 20000; i++) { big[0] = (unsigned char) rand(); feed(big, 1, 0); }
  /* transport gone */
  reset();
  send_fail = 1;
  T(rpc_feed(&r, g->b, g->n) == -1);
  send_fail = 0;
  T(rpc_feed(&r, g->b, g->n) == -1);		/* stays dead */

  T(rpc_weekday(2026, 11, 20) == 5 && rpc_weekday(2000, 1, 2) == 7);
  free(st);
  if (fails) { printf("rpctest: %d failed\n", fails); return 1; }
  printf("rpctest: all passed\n");
  return 0;
}
