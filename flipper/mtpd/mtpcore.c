/* mtpcore.c - the MTP container parser and operation set (spec R10.4, T29a).
 *
 * The attack surface from the USB host: every length is checked against
 * the container header and the one 1 KB buffer before a byte is copied;
 * a container that does not fit is read and thrown away, never stored.
 * Names come in as UTF-16 and go to the files as UTF-8, bounded, with no
 * '/', no "." or "..", no control characters and at most MTP_NAME_MAX
 * bytes: a longer name is an error, never cut.  Anything not in the R10.4
 * set gets Operation_Not_Supported, never a stall or a disconnect.
 *
 * Pure C: the only I/O goes through tr_* (the transport) and fs_* (the
 * files), so the same file builds on the PC under the fuzzer.
 *
 * RAM: the 1 KB buffer, the parameters and a few words of state.
 */
#include <string.h>
#include "mtp.h"

#ifndef MTP_VERSION
#define MTP_VERSION	"1.0"
#endif
#ifndef MTP_MODEL
#define MTP_MODEL	"Flipper Zero"
#endif

static mu8 buf[MTP_BUF];	/* the one data buffer */
static unsigned wn;		/* bytes built in buf */
static int wover;		/*   it did not fit */
static unsigned rp, rn;		/* the reader over buf */
static int rbad;		/*   it ran past rn */

static mu16 code;		/* the command's operation */
static mu32 tid;		/*   transaction id */
static mu32 par[MTP_MAXPAR];	/*   parameters (missing ones are 0) */
static int npar;

static int sess;		/* a session is open */
static int pend;		/* SendObjectInfo made a file, SendObject due */
static mu32 pend_h;		/*   its handle */
static int locked;		/* the PIN lock was on at the last command */
static long chg_last;		/* the last StorageInfoChanged */
static int chg_owed;		/*   one more is due after the 2 s */

/* What the device announces in GetDeviceInfo (const, in flash). */
static const mu16 ops[] = {
	OP_GetDeviceInfo, OP_OpenSession, OP_CloseSession, OP_GetStorageIDs,
	OP_GetStorageInfo, OP_GetNumObjects, OP_GetObjectHandles,
	OP_GetObjectInfo, OP_GetObject, OP_DeleteObject, OP_SendObjectInfo,
	OP_SendObject, OP_GetDevicePropDesc, OP_GetDevicePropValue,
	OP_MoveObject, OP_GetPartialObject, OP_GetObjectPropsSupported,
	OP_GetObjectPropDesc, OP_GetObjectPropValue, OP_SetObjectPropValue,
	OP_GetObjectPropList
};
static const mu16 events[] = {
	EV_ObjectAdded, EV_ObjectRemoved, EV_StoreAdded, EV_StorageInfoChanged
};
static const mu16 props[] = {
	PR_StorageID, PR_ObjectFormat, PR_ProtectionStatus, PR_ObjectSize,
	PR_ObjectFileName, PR_DateModified, PR_ParentObject, PR_PersistentUID,
	PR_Name
};
#define NELEM(a)	(sizeof(a) / sizeof((a)[0]))

/*===========================================================================*
 *				little-endian				     *
 *===========================================================================*/
static void st16(mu8 *p, mu16 v)
{
	p[0] = v; p[1] = v >> 8;
}

static void st32(mu8 *p, mu32 v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static mu16 ld16(const mu8 *p)
{
	return p[0] | p[1] << 8;
}

static mu32 ld32(const mu8 *p)
{
	return p[0] | p[1] << 8 | (mu32) p[2] << 16 | (mu32) p[3] << 24;
}

/*===========================================================================*
 *				the stream				     *
 *===========================================================================*/
/* Exactly n bytes from the host: 1, or TR_EOF/TR_ERR/TR_CANCEL. */
static int get(void *p, unsigned n)
{
	mu8 *q = p;
	int r;

	while (n > 0) {
		r = tr_read(q, n);
		if (r <= 0) return r == 0 ? TR_EOF : r;
		if ((unsigned) r > n) return TR_ERR;	/* a broken transport */
		q += r;
		n -= r;
	}
	return 1;
}

/* Read and drop n bytes (a container that does not fit). */
static int skip(mu32 n)
{
	int r;

	while (n > 0) {
		r = tr_read(buf, n < MTP_BUF ? n : MTP_BUF);
		if (r <= 0) return r == 0 ? TR_EOF : r;
		if ((mu32) r > n) return TR_ERR;
		n -= r;
	}
	return 1;
}

static int send_hdr(mu32 len, mu16 type)
{
	mu8 h[MTP_HDR];

	st32(h, len);
	st16(h + 4, type);
	st16(h + 6, code);
	st32(h + 8, tid);
	return tr_write(h, MTP_HDR);
}

static int respond(mu16 rc, int np, mu32 p0, mu32 p1, mu32 p2)
{
	mu8 r[MTP_HDR + 12];

	st32(r, MTP_HDR + 4 * np);
	st16(r + 4, CT_RESPONSE);
	st16(r + 6, rc);
	st32(r + 8, tid);
	st32(r + 12, p0);
	st32(r + 16, p1);
	st32(r + 20, p2);
	return tr_write(r, MTP_HDR + 4 * np);
}

static void event(mu16 ev, mu32 p0)
{
	mu8 e[MTP_HDR + 4];

	st32(e, sizeof(e));
	st16(e + 4, CT_EVENT);
	st16(e + 6, ev);
	st32(e + 8, tid);
	st32(e + 12, p0);
	(void) tr_event(e, sizeof(e));
}

/*===========================================================================*
 *				building a dataset			     *
 *===========================================================================*/
static void w8(unsigned v)
{
	if (wn + 1 > MTP_BUF) { wover = 1; return; }
	buf[wn++] = v;
}

static void w16(mu16 v)
{
	if (wn + 2 > MTP_BUF) { wover = 1; return; }
	st16(buf + wn, v);
	wn += 2;
}

static void w32(mu32 v)
{
	if (wn + 4 > MTP_BUF) { wover = 1; return; }
	st32(buf + wn, v);
	wn += 4;
}

static void w64(mu32 hi, mu32 lo)
{
	w32(lo);
	w32(hi);
}

static void warr16(const mu16 *a, unsigned n)
{
	w32(n);
	while (n-- > 0) w16(*a++);
}

/* The next code point of UTF-8 s (advanced); a bad byte is U+FFFD. */
static mu32 utf8_next(const unsigned char **s)
{
	const unsigned char *p = *s;
	mu32 c = *p++;
	int n, i;

	if (c < 0x80) n = 0;
	else if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; }
	else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; }
	else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; }
	else { *s = p; return 0xFFFD; }
	for (i = 0; i < n; i++) {
		if ((p[i] & 0xC0) != 0x80) { *s = p; return 0xFFFD; }
		c = c << 6 | (p[i] & 0x3F);
	}
	*s = p + n;
	if ((n == 1 && c < 0x80) || (n == 2 && c < 0x800)
	    || (n == 3 && c < 0x10000) || c > 0x10FFFF
	    || (c >= 0xD800 && c < 0xE000))
		return 0xFFFD;
	return c;
}

/* A PTP string from UTF-8: a count (with the NUL), UTF-16LE, a NUL. */
static void wstr(const char *s)
{
	const unsigned char *p = (const unsigned char *) s;
	unsigned at = wn, n = 0;
	mu32 c;

	w8(0);
	if (*p == 0) return;			/* the empty string: 0 */
	while (*p != 0 && n < 252) {
		c = utf8_next(&p);
		if (c >= 0x10000) {
			c -= 0x10000;
			w16(0xD800 | c >> 10);
			w16(0xDC00 | (c & 0x3FF));
			n += 2;
		} else {
			w16(c);
			n++;
		}
	}
	w16(0);
	if (!wover) buf[at] = n + 1;
}

/* Send buf[0..wn) as the data phase. */
static int send_buf(void)
{
	if (wover) return -1;
	if (send_hdr(MTP_HDR + wn, CT_DATA) < 0) return -1;
	return tr_write(buf, wn);
}

/*===========================================================================*
 *				reading a dataset			     *
 *===========================================================================*/
static unsigned r8(void)
{
	if (rp + 1 > rn) { rbad = 1; return 0; }
	return buf[rp++];
}

static mu16 r16(void)
{
	mu16 v;

	if (rp + 2 > rn) { rbad = 1; return 0; }
	v = ld16(buf + rp);
	rp += 2;
	return v;
}

static mu32 r32(void)
{
	mu32 v;

	if (rp + 4 > rn) { rbad = 1; return 0; }
	v = ld32(buf + rp);
	rp += 4;
	return v;
}

#define NAME_BAD	1
#define NAME_LONG	2

/* A PTP string into out as a file name (UTF-8): 0, NAME_BAD or NAME_LONG.
 * The whole string is read even when the name is refused. */
static int rname(char *out)
{
	unsigned n, i, k = 0;
	mu32 c, d;
	int bad = 0, len;

	n = r8();
	if (rbad) return NAME_BAD;
	if (n == 0) bad = NAME_BAD;		/* empty */
	for (i = 0; i < n; i++) {
		c = r16();
		if (rbad) return NAME_BAD;
		if (i == n - 1) {
			if (c != 0) bad = NAME_BAD;	/* no NUL at the end */
			break;
		}
		if (c >= 0xD800 && c < 0xDC00) {	/* a surrogate pair */
			if (++i >= n - 1) { bad = NAME_BAD; continue; }
			d = r16();
			if (rbad) return NAME_BAD;
			if (d < 0xDC00 || d >= 0xE000) { bad = NAME_BAD; continue; }
			c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
		} else if (c >= 0xDC00 && c < 0xE000) {
			bad = NAME_BAD;
			continue;
		}
		if (c < 0x20 || c == 0x7F || c == '/') { bad = NAME_BAD; continue; }
		len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
		if (k + len > MTP_NAME_MAX) {
			if (!bad) bad = NAME_LONG;
			continue;
		}
		if (bad) continue;
		if (len == 1) out[k++] = c;
		else if (len == 2) {
			out[k++] = 0xC0 | c >> 6;
			out[k++] = 0x80 | (c & 0x3F);
		} else if (len == 3) {
			out[k++] = 0xE0 | c >> 12;
			out[k++] = 0x80 | (c >> 6 & 0x3F);
			out[k++] = 0x80 | (c & 0x3F);
		} else {
			out[k++] = 0xF0 | c >> 18;
			out[k++] = 0x80 | (c >> 12 & 0x3F);
			out[k++] = 0x80 | (c >> 6 & 0x3F);
			out[k++] = 0x80 | (c & 0x3F);
		}
	}
	out[k] = 0;
	if (bad) return bad;
	if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0) return NAME_BAD;
	return 0;
}

/* Skip a PTP string. */
static void rskipstr(void)
{
	unsigned n = r8();

	if (rbad) return;
	if (rp + 2 * n > rn) { rbad = 1; return; }
	rp += 2 * n;
}

/* The data phase of a command that takes one: the whole container into
 * buf when it fits (rn = its length), else read and dropped (rn = 0,
 * rbad).  1 read, or TR_EOF/TR_ERR/TR_CANCEL. */
static int get_data(void)
{
	mu8 h[MTP_HDR];
	mu32 len;
	int r;

	rp = rn = 0;
	rbad = 1;
	if ((r = get(h, MTP_HDR)) != 1) return r;
	len = ld32(h);
	if (ld16(h + 4) != CT_DATA || len < MTP_HDR) {
		/* Not a data container: drop what it claims to carry.  The
		 * stream may be out of step now; the next header puts it
		 * right or the host resets. */
		if (len > MTP_HDR && (r = skip(len - MTP_HDR)) != 1) return r;
		return 1;
	}
	len -= MTP_HDR;
	if (len > MTP_BUF) return (r = skip(len)) != 1 ? r : 1;
	if ((r = get(buf, len)) != 1) return r;
	rn = len;
	rbad = 0;
	return 1;
}

/*===========================================================================*
 *				objects					     *
 *===========================================================================*/
/* A handle the host sent: 0 / 0xFFFFFFFF are not objects. */
static int obj(mu32 h, struct mtp_obj *o)
{
	if (h == 0 || h == MTP_ROOT || h == fs_root()) return FS_NOOBJ;
	return fs_stat(h, o);
}

/* A parent the host sent: the storage root (0 or 0xFFFFFFFF) or a folder;
 * its inode, or 0. */
static mu32 parent_of(mu32 p)
{
	struct mtp_obj o;

	if (p == 0 || p == MTP_ROOT) return fs_root();
	if (obj(p, &o) != FS_OK || !o.dir) return 0;
	return p;
}

static mu16 fs_rc(int e)
{
	switch (e) {
	case FS_OK:		return RC_OK;
	case FS_NOOBJ:		return RC_Invalid_ObjectHandle;
	case FS_DENIED:		return RC_Access_Denied;
	case FS_FULL:		return RC_Store_Full;
	case FS_EXIST:		return RC_Invalid_Parameter;
	case FS_PARTIAL:	return RC_Partial_Deletion;
	case FS_BADPARENT:	return RC_Invalid_ParentObject;
	default:		return RC_General_Error;
	}
}

static mu16 obj_format(const struct mtp_obj *o)
{
	return o->dir ? FMT_Association : FMT_Undefined;
}

static int prop_known(mu32 p)
{
	unsigned i;

	for (i = 0; i < NELEM(props); i++)
		if (props[i] == p) return 1;
	return 0;
}

static mu16 prop_type(mu32 p)
{
	switch (p) {
	case PR_ObjectFormat:
	case PR_ProtectionStatus:	return DT_UINT16;
	case PR_ObjectSize:		return DT_UINT64;
	case PR_PersistentUID:		return DT_UINT128;
	case PR_ObjectFileName:
	case PR_DateModified:
	case PR_Name:			return DT_STR;
	default:			return DT_UINT32;
	}
}

/* One property's value into buf. */
static void wprop(const struct mtp_obj *o, mu32 p)
{
	char date[16];

	switch (p) {
	case PR_StorageID:	w32(MTP_STORAGE); break;
	case PR_ObjectFormat:	w16(obj_format(o)); break;
	case PR_ProtectionStatus: w16(0); break;
	case PR_ObjectSize:	w64(0, o->dir ? 0 : o->size); break;
	case PR_ObjectFileName:
	case PR_Name:		wstr(o->name); break;
	case PR_DateModified:	fs_date(o->mtime, date); wstr(date); break;
	case PR_ParentObject:	w32(o->parent); break;
	case PR_PersistentUID:	/* the inode, unique in the storage */
		w32(o->handle); w32(MTP_STORAGE); w32(0); w32(0); break;
	}
}

/*===========================================================================*
 *				operations				     *
 *===========================================================================*/
static int op_device_info(void)
{
	static const mu16 dprops[] = { DP_DeviceFriendlyName };
	static const mu16 play[] = { FMT_Undefined, FMT_Association };

	wn = 0; wover = 0;
	w16(100);			/* PTP 1.00 */
	w32(6);				/* Microsoft's vendor extension: MTP */
	w16(100);
	wstr("microsoft.com: 1.0; ");
	w16(0);				/* functional mode */
	warr16(ops, NELEM(ops));
	warr16(events, NELEM(events));
	warr16(dprops, NELEM(dprops));
	warr16(0, 0);			/* capture formats */
	warr16(play, NELEM(play));
	wstr("Galtzagorriak");
	wstr(MTP_MODEL);
	wstr(MTP_VERSION);
	wstr(fs_label());		/* serial number */
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

static int op_storage_ids(void)
{
	wn = 0; wover = 0;
	if (locked) w32(0);		/* the PIN lock: no storage */
	else { w32(1); w32(MTP_STORAGE); }
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

static int op_storage_info(void)
{
	mu32 ch, cl, fh, fl;

	if (par[0] != MTP_STORAGE) return respond(RC_Invalid_StorageID, 0, 0, 0, 0);
	if (fs_space(&ch, &cl, &fh, &fl) < 0) ch = cl = fh = fl = 0xFFFFFFFFUL;
	wn = 0; wover = 0;
	w16(0x0004);			/* removable RAM: the SD card */
	w16(0x0002);			/* generic hierarchical */
	w16(0x0000);			/* read-write */
	w64(ch, cl);
	w64(fh, fl);
	w32(0xFFFFFFFFUL);		/* free objects: not known */
	wstr("SD card");
	wstr(fs_label());
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

/* The format filter of GetObjectHandles/GetNumObjects. */
static int fmt_ok(mu32 fmt, const struct mtp_obj *o)
{
	return fmt == 0 || fmt == obj_format(o);
}

/* GetObjectHandles and GetNumObjects. Two passes for the handles: the
 * count first (the container's length goes first), then the handles,
 * a buffer at a time; a folder that changed in between is padded with 0
 * or cut to the count. */
static int op_handles(int count_only)
{
	struct mtp_obj o;
	mu32 dir, n = 0, k;
	int r;

	if (par[0] != MTP_STORAGE && par[0] != MTP_ROOT)
		return respond(RC_Invalid_StorageID, 0, 0, 0, 0);
	/* 0 would be "every object in the storage": we list the root only
	 * (a host asks folder by folder). */
	if ((dir = parent_of(par[2])) == 0)
		return respond(RC_Invalid_ParentObject, 0, 0, 0, 0);
	if (fs_list_open(dir) != FS_OK)
		return respond(RC_Invalid_ParentObject, 0, 0, 0, 0);
	while ((r = fs_list_next(&o)) > 0)
		if (fmt_ok(par[1], &o)) n++;
	fs_list_close();
	if (count_only) return respond(RC_OK, 1, n, 0, 0);

	if (send_hdr(n > (0xFFFFFFFFUL - 16) / 4 ? 0xFFFFFFFFUL : MTP_HDR + 4 + 4 * n,
		     CT_DATA) < 0)
		return -1;
	wn = 0; wover = 0;
	w32(n);
	k = 0;
	if (fs_list_open(dir) == FS_OK) {
		while (k < n && fs_list_next(&o) > 0) {
			if (!fmt_ok(par[1], &o)) continue;
			w32(o.handle);
			k++;
			if (wn == MTP_BUF) {
				if (tr_write(buf, wn) < 0) { fs_list_close(); return -1; }
				wn = 0;
			}
		}
		fs_list_close();
	}
	for (; k < n; k++) {			/* the folder shrank */
		w32(0);
		if (wn == MTP_BUF) {
			if (tr_write(buf, wn) < 0) return -1;
			wn = 0;
		}
	}
	if (wn > 0 && tr_write(buf, wn) < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

static int op_object_info(void)
{
	struct mtp_obj o;
	char date[16];
	int e;

	if ((e = obj(par[0], &o)) != FS_OK) return respond(fs_rc(e), 0, 0, 0, 0);
	wn = 0; wover = 0;
	w32(MTP_STORAGE);
	w16(obj_format(&o));
	w16(0);				/* protection: none */
	w32(o.dir ? 0 : o.size);
	w16(0); w32(0); w32(0); w32(0);	/* no thumbnail */
	w32(0); w32(0); w32(0);		/* no image */
	w32(o.parent);
	w16(o.dir ? 0x0001 : 0x0000);	/* association: a generic folder */
	w32(0);
	w32(0);				/* sequence */
	wstr(o.name);
	fs_date(o.mtime, date);
	wstr(date);			/* capture date */
	wstr(date);			/* modification date */
	wstr("");			/* keywords */
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

/* GetObject (partial = 0) and GetPartialObject. */
static int op_get(int partial)
{
	struct mtp_obj o;
	mu32 off = 0, n, left, len;
	mu16 rc = RC_OK;
	int e, r;

	if ((e = obj(par[0], &o)) != FS_OK) return respond(fs_rc(e), 0, 0, 0, 0);
	if (o.dir) return respond(RC_Invalid_ObjectHandle, 0, 0, 0, 0);
	n = o.size;
	if (partial) {
		off = par[1];
		n = off >= o.size ? 0 : o.size - off;
		if (n > par[2]) n = par[2];
	}
	if ((e = fs_open_read(par[0], off)) != FS_OK)
		return respond(fs_rc(e), 0, 0, 0, 0);
	len = n > 0xFFFFFFFFUL - MTP_HDR ? 0xFFFFFFFFUL : MTP_HDR + n;
	if (send_hdr(len, CT_DATA) < 0) { fs_close(); return -1; }
	for (left = n; left > 0; left -= r) {
		r = left < MTP_BUF ? left : MTP_BUF;
		if (rc == RC_OK) {
			e = fs_read(buf, r);
			if (e < r) {		/* the file shrank: zeros */
				if (e < 0) e = 0;
				memset(buf + e, 0, r - e);
				rc = RC_Incomplete_Transfer;
			}
		} else {
			memset(buf, 0, r);
		}
		if (tr_write(buf, r) < 0) { fs_close(); return -1; }
	}
	fs_close();
	return respond(rc, partial, n, 0, 0);
}

/* Drop the file SendObjectInfo made when SendObject did not follow. */
static void drop_pending(void)
{
	if (!pend) return;
	fs_abandon();
	pend = 0;
	event(EV_ObjectRemoved, pend_h);
}

static int op_send_info(void)
{
	char name[MTP_NAME_MAX + 1];
	mu32 dir, size, h;
	mu16 fmt;
	int r, e;

	if ((r = get_data()) != 1) return r == TR_CANCEL ?
		respond(RC_Transaction_Cancelled, 0, 0, 0, 0) : -1;
	drop_pending();
	if (locked) return respond(RC_Device_Busy, 0, 0, 0, 0);
	if (rbad) return respond(RC_Invalid_Dataset, 0, 0, 0, 0);
	if (par[0] != MTP_STORAGE && par[0] != 0)
		return respond(RC_Invalid_StorageID, 0, 0, 0, 0);
	if ((dir = parent_of(par[1])) == 0)
		return respond(RC_Invalid_ParentObject, 0, 0, 0, 0);

	(void) r32();			/* storage */
	fmt = r16();
	(void) r16();			/* protection */
	size = r32();
	(void) r16(); (void) r32(); (void) r32(); (void) r32();	/* thumb */
	(void) r32(); (void) r32(); (void) r32();		/* image */
	(void) r32();			/* parent: the parameter counts */
	(void) r16(); (void) r32();	/* association */
	(void) r32();			/* sequence */
	e = rname(name);
	rskipstr(); rskipstr(); rskipstr();	/* dates, keywords */
	if (rbad) return respond(RC_Invalid_Dataset, 0, 0, 0, 0);
	if (e == NAME_LONG) return respond(RC_Invalid_ObjectProp_Value, 0, 0, 0, 0);
	if (e != 0) return respond(RC_Invalid_Parameter, 0, 0, 0, 0);
	if (fmt != FMT_Association && size == 0xFFFFFFFFUL)
		return respond(RC_Object_Too_Large, 0, 0, 0, 0);

	e = fs_create(dir, name, fmt == FMT_Association, &h);
	if (e != FS_OK) return respond(fs_rc(e), 0, 0, 0, 0);
	if (fmt != FMT_Association) { pend = 1; pend_h = h; }
	if (respond(RC_OK, 3, MTP_STORAGE,
		    dir == fs_root() ? MTP_ROOT : dir, h) < 0)
		return -1;
	event(EV_ObjectAdded, h);
	return 0;
}

static int op_send(void)
{
	mu8 h[MTP_HDR];
	mu32 left;
	int r, e = FS_OK;

	if ((r = get(h, MTP_HDR)) != 1) {
		drop_pending();
		return r == TR_CANCEL ?
			respond(RC_Transaction_Cancelled, 0, 0, 0, 0) : -1;
	}
	left = ld32(h);
	if (ld16(h + 4) != CT_DATA || left < MTP_HDR) {
		drop_pending();
		if (left > MTP_HDR && (r = skip(left - MTP_HDR)) != 1) return -1;
		return respond(RC_Invalid_Dataset, 0, 0, 0, 0);
	}
	left -= MTP_HDR;
	if (!pend) e = FS_NOOBJ;
	else if (ld32(h) == 0xFFFFFFFFUL) e = FS_FULL;	/* over 4 GiB */
	while (left > 0) {
		r = tr_read(buf, left < MTP_BUF ? left : MTP_BUF);
		if (r <= 0 || (mu32) r > left) {	/* gone or cancelled */
			drop_pending();
			if (r == TR_CANCEL)
				return respond(RC_Transaction_Cancelled, 0, 0, 0, 0);
			return -1;
		}
		left -= r;
		if (e == FS_OK && fs_write(buf, r) < 0) e = FS_FULL;
	}
	if (!pend) return respond(0x2015, 0, 0, 0, 0);	/* No_Valid_ObjectInfo */
	if (e != FS_OK) {
		drop_pending();
		return respond(e == FS_FULL ? RC_Store_Full : RC_General_Error,
			       0, 0, 0, 0);
	}
	fs_close();
	pend = 0;
	return respond(RC_OK, 0, 0, 0, 0);
}

static int op_delete(void)
{
	struct mtp_obj o;
	int e;

	if (par[1] != 0 && par[1] != FMT_Undefined && par[1] != FMT_Association)
		return respond(RC_Invalid_ObjectFormatCode, 0, 0, 0, 0);
	if (pend && par[0] == pend_h) {
		drop_pending();
		return respond(RC_OK, 0, 0, 0, 0);
	}
	if ((e = obj(par[0], &o)) != FS_OK) return respond(fs_rc(e), 0, 0, 0, 0);
	e = fs_delete(par[0]);
	if (e == FS_OK) {
		if (respond(RC_OK, 0, 0, 0, 0) < 0) return -1;
		event(EV_ObjectRemoved, par[0]);
		return 0;
	}
	return respond(fs_rc(e), 0, 0, 0, 0);
}

static int op_move(void)
{
	struct mtp_obj o;
	mu32 dir;
	int e;

	if ((e = obj(par[0], &o)) != FS_OK) return respond(fs_rc(e), 0, 0, 0, 0);
	if (par[1] != MTP_STORAGE) return respond(RC_Invalid_StorageID, 0, 0, 0, 0);
	if ((dir = parent_of(par[2])) == 0)
		return respond(RC_Invalid_ParentObject, 0, 0, 0, 0);
	return respond(fs_rc(fs_move(par[0], dir)), 0, 0, 0, 0);
}

static int op_props_supported(void)
{
	if (par[0] != 0 && par[0] != FMT_Undefined && par[0] != FMT_Association)
		return respond(RC_Invalid_ObjectFormatCode, 0, 0, 0, 0);
	wn = 0; wover = 0;
	warr16(props, NELEM(props));
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

static int op_prop_desc(void)
{
	mu16 t;

	if (!prop_known(par[0])) return respond(RC_Invalid_ObjectPropCode, 0, 0, 0, 0);
	t = prop_type(par[0]);
	wn = 0; wover = 0;
	w16(par[0]);
	w16(t);
	w8(par[0] == PR_ObjectFileName);	/* get/set: only the name */
	switch (t) {				/* the default value: 0, "" */
	case DT_UINT16:	w16(0); break;
	case DT_UINT32:	w32(0); break;
	case DT_UINT64:	w64(0, 0); break;
	case DT_UINT128: w32(0); w32(0); w32(0); w32(0); break;
	default:	w8(0); break;
	}
	w32(0);					/* group */
	w8(par[0] == PR_DateModified ? 0x03 : 0x00);	/* form: DateTime */
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

static int op_prop_value(void)
{
	struct mtp_obj o;
	int e;

	if ((e = obj(par[0], &o)) != FS_OK) return respond(fs_rc(e), 0, 0, 0, 0);
	if (!prop_known(par[1])) return respond(RC_Invalid_ObjectPropCode, 0, 0, 0, 0);
	wn = 0; wover = 0;
	wprop(&o, par[1]);
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

static int op_set_prop(void)
{
	char name[MTP_NAME_MAX + 1];
	struct mtp_obj o;
	int r, e;

	if ((r = get_data()) != 1) return r == TR_CANCEL ?
		respond(RC_Transaction_Cancelled, 0, 0, 0, 0) : -1;
	if (locked) return respond(RC_Device_Busy, 0, 0, 0, 0);
	if ((e = obj(par[0], &o)) != FS_OK) return respond(fs_rc(e), 0, 0, 0, 0);
	if (!prop_known(par[1])) return respond(RC_Invalid_ObjectPropCode, 0, 0, 0, 0);
	if (par[1] != PR_ObjectFileName) return respond(RC_Access_Denied, 0, 0, 0, 0);
	if (rbad) return respond(RC_Invalid_ObjectProp_Format, 0, 0, 0, 0);
	e = rname(name);
	if (rbad || rp != rn) return respond(RC_Invalid_ObjectProp_Format, 0, 0, 0, 0);
	if (e != 0) return respond(RC_Invalid_ObjectProp_Value, 0, 0, 0, 0);
	return respond(fs_rc(fs_rename(par[0], name)), 0, 0, 0, 0);
}

static int op_prop_list(void)
{
	struct mtp_obj o;
	unsigned i, n = 0, at;
	int e;

	/* handle, format, property (0xFFFFFFFF all), group, depth */
	if (par[4] != 0) return respond(RC_Spec_By_Depth_Unsupported, 0, 0, 0, 0);
	if (par[2] == 0 && par[3] != 0)
		return respond(RC_Spec_By_Group_Unsupported, 0, 0, 0, 0);
	if ((e = obj(par[0], &o)) != FS_OK) return respond(fs_rc(e), 0, 0, 0, 0);
	if (par[2] != 0xFFFFFFFFUL && !prop_known(par[2]))
		return respond(RC_Invalid_ObjectPropCode, 0, 0, 0, 0);
	wn = 0; wover = 0;
	at = wn;
	w32(0);
	for (i = 0; i < NELEM(props); i++) {
		if (par[2] != 0xFFFFFFFFUL && par[2] != props[i]) continue;
		w32(o.handle);
		w16(props[i]);
		w16(prop_type(props[i]));
		wprop(&o, props[i]);
		n++;
	}
	if (!wover) st32(buf + at, n);
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

static int op_dev_prop(int desc)
{
	if (par[0] != DP_DeviceFriendlyName)
		return respond(RC_DeviceProp_Not_Supported, 0, 0, 0, 0);
	wn = 0; wover = 0;
	if (desc) {
		w16(DP_DeviceFriendlyName);
		w16(DT_STR);
		w8(0);				/* get only */
		wstr(fs_label());		/* default */
	}
	wstr(fs_label());			/* current */
	if (desc) w8(0);			/* no form */
	if (send_buf() < 0) return -1;
	return respond(RC_OK, 0, 0, 0, 0);
}

/*===========================================================================*
 *				the loop				     *
 *===========================================================================*/
void mtp_init(void)
{
	sess = pend = locked = chg_owed = 0;
	chg_last = 0;
	pend_h = 0;
}

/* Operations that need the storage (Device_Busy under the PIN lock). */
static int object_op(mu16 c)
{
	switch (c) {
	case OP_GetStorageInfo: case OP_GetNumObjects: case OP_GetObjectHandles:
	case OP_GetObjectInfo: case OP_GetObject: case OP_DeleteObject:
	case OP_MoveObject: case OP_GetPartialObject: case OP_GetObjectPropValue:
	case OP_GetObjectPropList: case OP_SendObject:
		return 1;
	}
	return 0;
}

/* Does the operation carry a data phase from the host? */
static int data_in(mu16 c)
{
	return c == OP_SendObjectInfo || c == OP_SendObject
		|| c == OP_SetObjectPropValue;
}

void mtp_changed(long now)
{
	if (!sess) return;
	if (now - chg_last >= 2 || now < chg_last) {
		chg_last = now;
		chg_owed = 0;
		event(EV_StorageInfoChanged, MTP_STORAGE);
	} else {
		chg_owed = 1;			/* sent by the next step after 2 s */
	}
}

void mtp_end(void)
{
	drop_pending();
	sess = 0;
}

int mtp_step(void)
{
	mu8 h[MTP_HDR + 4 * MTP_MAXPAR];
	mu32 len;
	mu16 type;
	int r, i, was;

	if ((r = get(h, MTP_HDR)) != 1) {
		if (r == TR_CANCEL) return 1;	/* nothing to cancel */
		mtp_end();
		return 0;
	}
	len = ld32(h);
	type = ld16(h + 4);
	code = ld16(h + 6);
	tid = ld32(h + 8);
	if (len < MTP_HDR) len = MTP_HDR;	/* nothing more to read */
	if (type != CT_COMMAND) {
		/* A stray data container (or junk): drop it whole. */
		if (skip(len - MTP_HDR) != 1) { mtp_end(); return 0; }
		return 1;
	}
	npar = 0;
	memset(par, 0, sizeof(par));
	if (len - MTP_HDR > 4 * MTP_MAXPAR) {
		/* More parameters than a command has: drop it, answer. */
		if (skip(len - MTP_HDR) != 1) { mtp_end(); return 0; }
		/* its data phase (if any) follows: drop that too */
		if (data_in(code) && get_data() != 1) { mtp_end(); return 0; }
		return respond(RC_Invalid_Parameter, 0, 0, 0, 0) < 0 ? 0 : 1;
	}
	if ((r = get(h + MTP_HDR, len - MTP_HDR)) != 1) {
		if (r == TR_CANCEL) return 1;
		mtp_end();
		return 0;
	}
	npar = (len - MTP_HDR) / 4;
	for (i = 0; i < npar; i++) par[i] = ld32(h + MTP_HDR + 4 * i);

	was = locked;
	locked = fs_locked();
	if (was && !locked && sess) event(EV_StoreAdded, MTP_STORAGE);
	if (chg_owed && sess) mtp_changed(fs_now());

	if (code != OP_GetDeviceInfo && code != OP_OpenSession && !sess) {
		if (data_in(code) && get_data() != 1) { mtp_end(); return 0; }
		return respond(RC_Session_Not_Open, 0, 0, 0, 0) < 0 ? 0 : 1;
	}
	if (locked && object_op(code) && code != OP_SendObject) {
		return respond(RC_Device_Busy, 0, 0, 0, 0) < 0 ? 0 : 1;
	}
	switch (code) {
	case OP_GetDeviceInfo:	r = op_device_info(); break;
	case OP_OpenSession:
		if (sess) { r = respond(RC_Session_Already_Open, 1, 1, 0, 0); break; }
		if (par[0] == 0) { r = respond(RC_Invalid_Parameter, 0, 0, 0, 0); break; }
		sess = 1;
		r = respond(RC_OK, 0, 0, 0, 0);
		break;
	case OP_CloseSession:
		drop_pending();
		sess = 0;
		r = respond(RC_OK, 0, 0, 0, 0);
		break;
	case OP_GetStorageIDs:	r = op_storage_ids(); break;
	case OP_GetStorageInfo:	r = op_storage_info(); break;
	case OP_GetNumObjects:	r = op_handles(1); break;
	case OP_GetObjectHandles: r = op_handles(0); break;
	case OP_GetObjectInfo:	r = op_object_info(); break;
	case OP_GetObject:	r = op_get(0); break;
	case OP_GetPartialObject: r = op_get(1); break;
	case OP_SendObjectInfo:	r = op_send_info(); break;
	case OP_SendObject:	r = op_send(); break;
	case OP_DeleteObject:	r = op_delete(); break;
	case OP_MoveObject:	r = op_move(); break;
	case OP_GetObjectPropsSupported: r = op_props_supported(); break;
	case OP_GetObjectPropDesc: r = op_prop_desc(); break;
	case OP_GetObjectPropValue: r = op_prop_value(); break;
	case OP_SetObjectPropValue: r = op_set_prop(); break;
	case OP_GetObjectPropList: r = op_prop_list(); break;
	case OP_GetDevicePropDesc: r = op_dev_prop(1); break;
	case OP_GetDevicePropValue: r = op_dev_prop(0); break;
	default:
		/* Unknown: never a stall.  A data phase we cannot know of
		 * would come next; the host waits for our answer first. */
		r = respond(RC_Operation_Not_Supported, 0, 0, 0, 0);
		break;
	}
	if (r < 0) { mtp_end(); return 0; }
	return 1;
}
