/* libflipperpb host test (issue #388): nanopb decodes the golden frames
 * that Google's protobuf made (mkgolden.py) and encodes them back to the
 * same bytes; cut and damaged frames are refused without a crash or leak
 * (ASan, UBSan, LeakSanitizer). */
#include <stdio.h>
#include <string.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "flipper.pb.h"
#include "version.h"
#include "golden.h"

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
		__LINE__, #c); fails++; } } while (0)

static int decode(const unsigned char *b, size_t n, PB_Main *m)
{
  pb_istream_t s = pb_istream_from_buffer(b, n);
  PB_Main z = PB_Main_init_zero;

  *m = z;
  return pb_decode_ex(&s, PB_Main_fields, m, PB_DECODE_DELIMITED) ? 0 : -1;
}

static const struct golden *find(const char *name)
{
  unsigned i;

  for (i = 0; i < sizeof(goldens) / sizeof(goldens[0]); i++)
	if (strcmp(goldens[i].name, name) == 0) return &goldens[i];
  return NULL;
}

int main(void)
{
  unsigned char out[1200], bad[256];
  PB_Main m;
  unsigned i, k;
  const struct golden *g;

  for (i = 0; i < sizeof(goldens) / sizeof(goldens[0]); i++) {
	pb_ostream_t o = pb_ostream_from_buffer(out, sizeof(out));

	g = &goldens[i];
	if (decode(g->b, g->n, &m) != 0) {
		printf("FAIL decode %s\n", g->name);
		fails++;
		continue;
	}
	T(pb_encode_ex(&o, PB_Main_fields, &m, PB_ENCODE_DELIMITED));
	if (o.bytes_written != g->n || memcmp(out, g->b, g->n) != 0) {
		printf("FAIL %s: encodes to other bytes\n", g->name);
		fails++;
	}
	pb_release(PB_Main_fields, &m);
	/* every cut frame is refused */
	for (k = 0; k < g->n; k++) {
		if (decode(g->b, k, &m) == 0) {
			printf("FAIL %s cut at %u decoded\n", g->name, k);
			fails++;
		}
		pb_release(PB_Main_fields, &m);
	}
	/* damaged bytes: decoded or not, never a crash or a leak */
	for (k = 1; k < g->n && g->n <= sizeof(bad); k++) {
		unsigned v;

		for (v = 0; v < 256; v += 17) {
			memcpy(bad, g->b, g->n);
			bad[k] ^= (unsigned char) v | 1;
			(void) decode(bad, g->n, &m);
			pb_release(PB_Main_fields, &m);
		}
	}
  }

  /* fields, as the RPC will use them */
  g = find("ping_req");
  T(decode(g->b, g->n, &m) == 0 && m.command_id == 1
    && m.which_content == PB_Main_system_ping_request_tag
    && m.content.system_ping_request.data->size == 3
    && m.content.system_ping_request.data->bytes[2] == 3);
  pb_release(PB_Main_fields, &m);
  g = find("list_req");
  T(decode(g->b, g->n, &m) == 0
    && m.which_content == PB_Main_storage_list_request_tag
    && strcmp(m.content.storage_list_request.path, "/ext") == 0);
  pb_release(PB_Main_fields, &m);
  g = find("setdt_req");
  T(decode(g->b, g->n, &m) == 0
    && m.which_content == PB_Main_system_set_datetime_request_tag
    && m.content.system_set_datetime_request.datetime.year == 2026
    && m.content.system_set_datetime_request.datetime.weekday == 5);
  pb_release(PB_Main_fields, &m);
  g = find("notimpl_resp");
  T(decode(g->b, g->n, &m) == 0
    && m.command_status == PB_CommandStatus_ERROR_NOT_IMPLEMENTED
    && m.which_content == PB_Main_empty_tag);
  pb_release(PB_Main_fields, &m);
  g = find("pbver_resp");
  T(decode(g->b, g->n, &m) == 0
    && m.content.system_protobuf_version_response.major == FPB_MAJOR
    && m.content.system_protobuf_version_response.minor == FPB_MINOR);
  pb_release(PB_Main_fields, &m);

  if (fails) { printf("fpbtest: %d failed\n", fails); return 1; }
  printf("fpbtest: %u golden frames, all passed\n",
	 (unsigned) (sizeof(goldens) / sizeof(goldens[0])));
  return 0;
}
