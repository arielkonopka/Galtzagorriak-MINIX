/* rpcd core, see rpcd.h (spec R15.5, R15.7; issue #388). */
#include <string.h>
#include <stdio.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "flipper.pb.h"
#include "version.h"
#include "rpcd.h"

struct rpc_state {
  unsigned char in[RPC_MAX_FRAME];	/* a frame being received */
  unsigned have;			/* bytes in in[] */
  unsigned long skip;			/* bytes of a too-big frame to drop */
  unsigned char out[RPC_MAX_FRAME];	/* the frame being sent */
  PB_Main req, ans;
  uint32_t id;				/* the request's command_id */
  struct rpc_file f[RPC_LIST_CHUNK];	/* list entries not sent yet */
  int nf;
  char kvk[64], kvv[96];		/* the pair held back (rpc_kv) */
  int kv_held, kv_tag;
  union {				/* data of the answer */
	PB_BYTES_ARRAY_T(RPC_READ_CHUNK) rd;
	char path[RPC_PATH_MAX + 1];
  } u;
};

unsigned rpc_state_size(void)
{
  return sizeof(struct rpc_state);
}

void rpc_init(struct rpc *r, const struct rpc_ops *ops, void *state)
{
  r->ops = ops;
  r->s = state;
  r->dead = 0;
  memset(r->s, 0, sizeof(*r->s));
}

/* Send s->ans (its content already set) with status st and has_next. */
static int send_ans(struct rpc *r, int st, int next)
{
  struct rpc_state *s = r->s;
  pb_ostream_t o = pb_ostream_from_buffer(s->out, sizeof(s->out));

  if (r->dead) return -1;
  s->ans.command_id = s->id;
  s->ans.command_status = (PB_CommandStatus) st;
  s->ans.has_next = next != 0;
  if (!pb_encode_ex(&o, PB_Main_fields, &s->ans, PB_ENCODE_DELIMITED)) {
	/* cannot happen with the sizes above; answer the bare status */
	memset(&s->ans, 0, sizeof(s->ans));
	return send_ans(r, PB_CommandStatus_ERROR, 0);
  }
  if (r->ops->send(r->ops->ctx, s->out, (unsigned) o.bytes_written) < 0) {
	r->dead = 1;
	return -1;
  }
  return 0;
}

static int send_status(struct rpc *r, int st)
{
  memset(&r->s->ans, 0, sizeof(r->s->ans));
  r->s->ans.which_content = PB_Main_empty_tag;
  return send_ans(r, st, 0);
}

/* Key/value streams (device_info, power_info): one pair per frame, as
 * stock; the last has has_next 0.  Which pair is last is known only when
 * the callback returns, so one pair is held back. */

static int kv_flush(struct rpc *r, int next)
{
  struct rpc_state *s = r->s;

  if (!r->s->kv_held) return 0;
  memset(&s->ans, 0, sizeof(s->ans));
  s->ans.which_content = (pb_size_t) r->s->kv_tag;
  if (r->s->kv_tag == PB_Main_system_device_info_response_tag) {
	s->ans.content.system_device_info_response.key = r->s->kvk;
	s->ans.content.system_device_info_response.value = r->s->kvv;
  } else {
	s->ans.content.system_power_info_response.key = r->s->kvk;
	s->ans.content.system_power_info_response.value = r->s->kvv;
  }
  r->s->kv_held = 0;
  return send_ans(r, PB_CommandStatus_OK, next);
}

int rpc_kv(struct rpc *r, const char *key, const char *value)
{
  if (kv_flush(r, 1) < 0) return -1;
  snprintf(r->s->kvk, sizeof(r->s->kvk), "%s", key);
  snprintf(r->s->kvv, sizeof(r->s->kvv), "%s", value);
  r->s->kv_held = 1;
  return 0;
}

static int kv_stream(struct rpc *r, int tag,
		     int (*fn)(void *, struct rpc *))
{
  int st;

  r->s->kv_held = 0;
  r->s->kv_tag = tag;
  if (fn == NULL) return send_status(r, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
  st = fn(r->ops->ctx, r);
  if (st != 0) { r->s->kv_held = 0; return send_status(r, st); }
  if (!r->s->kv_held) return send_status(r, PB_CommandStatus_OK);
  return kv_flush(r, 0);
}

/* Storage list: entries in frames of RPC_LIST_CHUNK, the last has
 * has_next 0 (an empty directory: one frame without files). */
static int list_flush(struct rpc *r, int next)
{
  struct rpc_state *s = r->s;
  int i;

  memset(&s->ans, 0, sizeof(s->ans));
  s->ans.which_content = PB_Main_storage_list_response_tag;
  for (i = 0; i < s->nf; i++) {
	PB_Storage_File *f = &s->ans.content.storage_list_response.file[i];

	f->type = s->f[i].dir ? PB_Storage_File_FileType_DIR
			      : PB_Storage_File_FileType_FILE;
	f->name = s->f[i].name;
	f->size = s->f[i].dir ? 0 : s->f[i].size;
  }
  s->ans.content.storage_list_response.file_count = (pb_size_t) s->nf;
  s->nf = 0;
  return send_ans(r, PB_CommandStatus_OK, next);
}

int rpc_list_add(struct rpc *r, const struct rpc_file *f)
{
  struct rpc_state *s = r->s;

  if (s->nf == RPC_LIST_CHUNK && list_flush(r, 1) < 0) return -1;
  s->f[s->nf++] = *f;
  return 0;
}

int rpc_path_ok(const char *p)
{
  const char *c;

  if (p == NULL || strncmp(p, "/ext", 4) != 0) return 0;
  if (p[4] != 0 && p[4] != '/') return 0;
  if (strlen(p) > RPC_PATH_MAX) return 0;
  for (c = p; *c; c++) {
	if ((unsigned char) *c < ' ') return 0;
	if (c[0] == '/' && c[1] == '.' && c[2] == '.'
	    && (c[3] == '/' || c[3] == 0)) return 0;
  }
  return 1;
}

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int rpc_dt_ok(const struct rpc_dt *d)
{
  static const unsigned char dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31,
					  30, 31, 30, 31 };

  if (d->year < 2000 || d->year > 2099) return 0;
  if (d->month < 1 || d->month > 12) return 0;
  if (d->day < 1 || d->day > dim[d->month - 1]
		    + (d->month == 2 && leap(d->year))) return 0;
  return d->hour >= 0 && d->hour <= 23 && d->minute >= 0
	 && d->minute <= 59 && d->second >= 0 && d->second <= 59;
}

int rpc_weekday(int y, int m, int d)	/* Sakamoto; 1 = Monday */
{
  static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  int w;

  if (m < 3) y--;
  w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;	/* 0 = Sun */
  return w == 0 ? 7 : w;
}

static int path_of(struct rpc *r, const char *p)
{
  if (!rpc_path_ok(p)) {
	send_status(r, PB_CommandStatus_ERROR_STORAGE_INVALID_NAME);
	return -1;
  }
  snprintf(r->s->u.path, sizeof(r->s->u.path), "%s", p);
  return 0;
}

static int do_read(struct rpc *r, const char *p)
{
  struct rpc_state *s = r->s;
  struct rpc_file st;
  uint32_t off = 0;
  long n;
  int e;
  char path[RPC_PATH_MAX + 1];

  if (path_of(r, p) < 0) return 0;
  strcpy(path, s->u.path);
  if (!r->ops->fs_stat || !r->ops->fs_read)
	return send_status(r, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
  if ((e = r->ops->fs_stat(r->ops->ctx, path, &st)) != 0)
	return send_status(r, e);
  if (st.dir) return send_status(r, PB_CommandStatus_ERROR_STORAGE_NOT_EXIST);
  for (;;) {
	n = r->ops->fs_read(r->ops->ctx, path, off, s->u.rd.bytes,
			    RPC_READ_CHUNK);
	if (n < 0) return send_status(r, (int) -n);
	memset(&s->ans, 0, sizeof(s->ans));
	s->ans.which_content = PB_Main_storage_read_response_tag;
	s->ans.content.storage_read_response.has_file = 1;
	s->u.rd.size = (pb_size_t) n;
	s->ans.content.storage_read_response.file.data =
				(pb_bytes_array_t *) &s->u.rd;
	off += (uint32_t) n;
	/* the last chunk: has_next 0 (stock sends size-limited chunks) */
	if (send_ans(r, PB_CommandStatus_OK, off < st.size && n > 0) < 0)
		return -1;
	if (off >= st.size || n == 0) return 0;
  }
}

static int dispatch(struct rpc *r)
{
  struct rpc_state *s = r->s;
  PB_Main *q = &s->req;
  struct rpc_dt dt;
  struct rpc_file f;
  uint64_t tot, fr;
  int e;

  s->id = q->command_id;
  switch (q->which_content) {
  case PB_Main_system_ping_request_tag:
	memset(&s->ans, 0, sizeof(s->ans));
	s->ans.which_content = PB_Main_system_ping_response_tag;
	s->ans.content.system_ping_response.data =
				q->content.system_ping_request.data;
	return send_ans(r, PB_CommandStatus_OK, 0);
  case PB_Main_system_protobuf_version_request_tag:
	memset(&s->ans, 0, sizeof(s->ans));
	s->ans.which_content = PB_Main_system_protobuf_version_response_tag;
	s->ans.content.system_protobuf_version_response.major = FPB_MAJOR;
	s->ans.content.system_protobuf_version_response.minor = FPB_MINOR;
	return send_ans(r, PB_CommandStatus_OK, 0);
  case PB_Main_system_device_info_request_tag:
	return kv_stream(r, PB_Main_system_device_info_response_tag,
			 r->ops->device_info);
  case PB_Main_system_power_info_request_tag:
	return kv_stream(r, PB_Main_system_power_info_response_tag,
			 r->ops->power_info);
  case PB_Main_system_get_datetime_request_tag:
	if (!r->ops->get_time)
		return send_status(r, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
	if ((e = r->ops->get_time(r->ops->ctx, &dt)) != 0)
		return send_status(r, e);
	memset(&s->ans, 0, sizeof(s->ans));
	s->ans.which_content = PB_Main_system_get_datetime_response_tag;
	s->ans.content.system_get_datetime_response.has_datetime = 1;
	{
		PB_System_DateTime *d =
			&s->ans.content.system_get_datetime_response.datetime;

		d->year = (uint16_t) dt.year, d->month = (uint8_t) dt.month;
		d->day = (uint8_t) dt.day, d->hour = (uint8_t) dt.hour;
		d->minute = (uint8_t) dt.minute;
		d->second = (uint8_t) dt.second;
		d->weekday = (uint8_t) rpc_weekday(dt.year, dt.month, dt.day);
	}
	return send_ans(r, PB_CommandStatus_OK, 0);
  case PB_Main_system_set_datetime_request_tag: {
	PB_System_DateTime *d =
			&q->content.system_set_datetime_request.datetime;

	if (!q->content.system_set_datetime_request.has_datetime)
		return send_status(r, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
	dt.year = d->year, dt.month = d->month, dt.day = d->day;
	dt.hour = d->hour, dt.minute = d->minute, dt.second = d->second;
	if (!rpc_dt_ok(&dt))
		return send_status(r, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
	dt.weekday = rpc_weekday(dt.year, dt.month, dt.day);	/* ignored */
	if (!r->ops->set_time)
		return send_status(r, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
	return send_status(r, r->ops->set_time(r->ops->ctx, &dt));
  }
  case PB_Main_storage_info_request_tag:
	if (path_of(r, q->content.storage_info_request.path) < 0) return 0;
	if (!r->ops->fs_info)
		return send_status(r, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
	if ((e = r->ops->fs_info(r->ops->ctx, s->u.path, &tot, &fr)) != 0)
		return send_status(r, e);
	memset(&s->ans, 0, sizeof(s->ans));
	s->ans.which_content = PB_Main_storage_info_response_tag;
	s->ans.content.storage_info_response.total_space = tot;
	s->ans.content.storage_info_response.free_space = fr;
	return send_ans(r, PB_CommandStatus_OK, 0);
  case PB_Main_storage_stat_request_tag:
	if (path_of(r, q->content.storage_stat_request.path) < 0) return 0;
	if (!r->ops->fs_stat)
		return send_status(r, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
	if ((e = r->ops->fs_stat(r->ops->ctx, s->u.path, &f)) != 0)
		return send_status(r, e);
	s->f[0] = f;
	memset(&s->ans, 0, sizeof(s->ans));
	s->ans.which_content = PB_Main_storage_stat_response_tag;
	s->ans.content.storage_stat_response.has_file = 1;
	s->ans.content.storage_stat_response.file.type = f.dir
		? PB_Storage_File_FileType_DIR : PB_Storage_File_FileType_FILE;
	s->ans.content.storage_stat_response.file.name = s->f[0].name;
	s->ans.content.storage_stat_response.file.size = f.dir ? 0 : f.size;
	return send_ans(r, PB_CommandStatus_OK, 0);
  case PB_Main_storage_list_request_tag:
	if (path_of(r, q->content.storage_list_request.path) < 0) return 0;
	if (!r->ops->fs_list)
		return send_status(r, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
	s->nf = 0;
	if ((e = r->ops->fs_list(r->ops->ctx, s->u.path, r)) != 0) {
		s->nf = 0;
		return send_status(r, e);
	}
	return list_flush(r, 0);
  case PB_Main_storage_read_request_tag:
	return do_read(r, q->content.storage_read_request.path);
  default:		/* R15.7: never a disconnect */
	return send_status(r, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
  }
}

static void frame(struct rpc *r, const unsigned char *b, unsigned n)
{
  struct rpc_state *s = r->s;
  pb_istream_t is = pb_istream_from_buffer(b, n);
  PB_Main z = PB_Main_init_zero;

  s->req = z;
  if (!pb_decode(&is, PB_Main_fields, &s->req)) {
	pb_release(PB_Main_fields, &s->req);
	s->id = 0;
	send_status(r, PB_CommandStatus_ERROR_DECODE);
	return;
  }
  dispatch(r);
  pb_release(PB_Main_fields, &s->req);
}

int rpc_feed(struct rpc *r, const unsigned char *b, unsigned n)
{
  struct rpc_state *s = r->s;
  unsigned long len;
  unsigned i, vl, c;

  while (n > 0 && !r->dead) {
	if (s->skip) {				/* rest of a too-big frame */
		c = s->skip < n ? (unsigned) s->skip : n;
		s->skip -= c, b += c, n -= c;
		continue;
	}
	c = sizeof(s->in) - s->have;
	if (c > n) c = n;
	memcpy(s->in + s->have, b, c);
	s->have += c, b += c, n -= c;
	for (;;) {				/* every whole frame in in[] */
		for (len = 0, vl = 0; vl < s->have && vl < 5; vl++) {
			len |= (unsigned long) (s->in[vl] & 0x7F) << (7 * vl);
			if (!(s->in[vl] & 0x80)) break;
		}
		if (vl == 5 || (vl < s->have && len > 0xFFFFFFUL)) {
			s->have = 0;		/* garbage: start again */
			s->id = 0;
			send_status(r, PB_CommandStatus_ERROR_DECODE);
			break;
		}
		if (vl >= s->have) break;	/* length not complete */
		vl++;
		if (vl + len > sizeof(s->in)) {	/* too big: drop it */
			s->skip = vl + len - s->have;
			s->have = 0;
			s->id = 0;
			send_status(r, PB_CommandStatus_ERROR_DECODE);
			break;
		}
		if (vl + len > s->have) break;	/* body not complete */
		frame(r, s->in + vl, (unsigned) len);
		i = vl + (unsigned) len;
		memmove(s->in, s->in + i, s->have - i);
		s->have -= i;
		if (r->dead) break;
	}
  }
  return r->dead ? -1 : 0;
}
