/* mtp.h - mtpd, files to a PC over MTP (spec R10.4, T29a).
 *
 * Three layers, each in its own file:
 *   mtpcore.c	the container parser and the R10.4 operation set; pure C,
 *		its only I/O goes through the two interfaces below, so it
 *		builds on the PC for the fuzzer (AC10.4.6).
 *   mtpfs.c	the files: object handles = inode numbers, the parent ring
 *		(<= 256 x 8 B) and the path rebuild by directory scan.
 *   mtpfd.c	the transport: a framed byte stream on a file descriptor
 *		(a pipe on the PC, a serial line on QEMU); T29b adds USB.
 */
#ifndef MTP_H
#define MTP_H

#ifdef _MINIX
#include <sys/types.h>
typedef u8_t  mu8;
typedef u16_t mu16;
typedef u32_t mu32;
#else
#include <stdint.h>
#include <sys/types.h>
typedef uint8_t  mu8;
typedef uint16_t mu16;
typedef uint32_t mu32;
#endif

/* The one data buffer (spec: one 1 KB buffer, one FS block per step). */
#define MTP_BUF		1024
#define MTP_HDR		12		/* container header */
#define MTP_MAXPAR	5		/* parameters in a command */

/* Longest name the storage takes, in bytes of UTF-8 (V2 14, V3 60).
 * The PC build defaults to 60, the V3 rule (spec O). */
#ifndef MTP_NAME_MAX
#include <limits.h>
#if defined(NAME_MAX) && NAME_MAX < 255
#define MTP_NAME_MAX	NAME_MAX
#else
#define MTP_NAME_MAX	60
#endif
#endif
#define MTP_PATH_MAX	255		/* MINIX PATH_MAX */

/* Container types */
#define CT_COMMAND	1
#define CT_DATA		2
#define CT_RESPONSE	3
#define CT_EVENT	4

/* Operations (PTP 1.1, MTP 1.1) */
#define OP_GetDeviceInfo		0x1001
#define OP_OpenSession			0x1002
#define OP_CloseSession			0x1003
#define OP_GetStorageIDs		0x1004
#define OP_GetStorageInfo		0x1005
#define OP_GetNumObjects		0x1006
#define OP_GetObjectHandles		0x1007
#define OP_GetObjectInfo		0x1008
#define OP_GetObject			0x1009
#define OP_DeleteObject			0x100B
#define OP_SendObjectInfo		0x100C
#define OP_SendObject			0x100D
#define OP_GetDevicePropDesc		0x1014
#define OP_GetDevicePropValue		0x1015
#define OP_MoveObject			0x1019
#define OP_GetPartialObject		0x101B
#define OP_GetObjectPropsSupported	0x9801
#define OP_GetObjectPropDesc		0x9802
#define OP_GetObjectPropValue		0x9803
#define OP_SetObjectPropValue		0x9804
#define OP_GetObjectPropList		0x9805

/* Responses */
#define RC_OK				0x2001
#define RC_General_Error		0x2002
#define RC_Session_Not_Open		0x2003
#define RC_Operation_Not_Supported	0x2005
#define RC_Parameter_Not_Supported	0x2006
#define RC_Incomplete_Transfer		0x2007
#define RC_Invalid_StorageID		0x2008
#define RC_Invalid_ObjectHandle		0x2009
#define RC_DeviceProp_Not_Supported	0x200A
#define RC_Invalid_ObjectFormatCode	0x200B
#define RC_Store_Full			0x200C
#define RC_Access_Denied		0x200F
#define RC_Partial_Deletion		0x2012
#define RC_Invalid_Code_Format		0x2016
#define RC_Device_Busy			0x2019
#define RC_Invalid_ParentObject		0x201A
#define RC_Invalid_Parameter		0x201D
#define RC_Session_Already_Open		0x201E
#define RC_Transaction_Cancelled	0x201F
#define RC_Invalid_ObjectPropCode	0xA801
#define RC_Invalid_ObjectProp_Format	0xA802
#define RC_Invalid_ObjectProp_Value	0xA803
#define RC_Invalid_Dataset		0xA806
#define RC_Spec_By_Group_Unsupported	0xA807
#define RC_Spec_By_Depth_Unsupported	0xA808
#define RC_Object_Too_Large		0xA809

/* Events */
#define EV_ObjectAdded			0x4002
#define EV_ObjectRemoved		0x4003
#define EV_StoreAdded			0x4004
#define EV_StorageInfoChanged		0x400C

/* Object formats */
#define FMT_Undefined			0x3000
#define FMT_Association			0x3001	/* a folder */

/* Object properties */
#define PR_StorageID			0xDC01
#define PR_ObjectFormat			0xDC02
#define PR_ProtectionStatus		0xDC03
#define PR_ObjectSize			0xDC04
#define PR_ObjectFileName		0xDC07
#define PR_DateModified			0xDC09
#define PR_ParentObject			0xDC0B
#define PR_PersistentUID		0xDC41
#define PR_Name				0xDC44

#define DP_DeviceFriendlyName		0xD402

/* Data types */
#define DT_UINT16	0x0004
#define DT_UINT32	0x0006
#define DT_UINT64	0x0008
#define DT_UINT128	0x000A
#define DT_STR		0xFFFF

#define MTP_STORAGE	0x00010001UL	/* the one storage: /home/flipper */
#define MTP_ROOT	0xFFFFFFFFUL	/* "the storage root" as a parent */

/*===========================================================================*
 * The transport (mtpfd.c; the USB one in T29b)				     *
 *===========================================================================*/
#define TR_EOF		0		/* tr_read: the host has gone */
#define TR_ERR		(-1)
#define TR_CANCEL	(-2)		/* the host cancelled the transfer */

/* Up to n bytes from the host (bulk OUT); TR_EOF, TR_ERR, TR_CANCEL. */
int tr_read(void *buf, unsigned n);
/* n bytes to the host (bulk IN); 0 or TR_ERR. */
int tr_write(const void *buf, unsigned n);
/* One event container to the host (interrupt IN); 0 or TR_ERR. */
int tr_event(const void *buf, unsigned n);

/*===========================================================================*
 * The files (mtpfs.c)							     *
 *===========================================================================*/
struct mtp_obj {
	mu32 handle;			/* inode number */
	mu32 parent;			/* parent's inode, 0 = storage root */
	mu32 size;			/* bytes (files <= 4 GiB - 1, O.3) */
	long mtime;
	int dir;			/* a folder */
	char name[MTP_NAME_MAX + 1];
};

#define FS_OK		0
#define FS_NOOBJ	(-1)		/* no such handle (ring miss) */
#define FS_DENIED	(-2)
#define FS_FULL		(-3)
#define FS_EXIST	(-4)
#define FS_IO		(-5)
#define FS_PARTIAL	(-6)
#define FS_BADPARENT	(-7)

int fs_init(const char *root);	/* 0 or -1 */
mu32 fs_root(void);		/* the storage root's inode */
int fs_stat(mu32 h, struct mtp_obj *o);
/* Directory listing: fs_list_open(dir) then fs_list_next() until 0. */
int fs_list_open(mu32 dir);
int fs_list_next(struct mtp_obj *o);	/* 1 an object, 0 the end */
void fs_list_close(void);
int fs_open_read(mu32 h, mu32 off);	/* 0 or FS_*; one file at a time */
int fs_read(void *buf, unsigned n);
int fs_create(mu32 dir, const char *name, int folder, mu32 *h);
int fs_write(const void *buf, unsigned n);
void fs_close(void);
void fs_abandon(void);			/* close and remove the new file */
int fs_delete(mu32 h);
int fs_rename(mu32 h, const char *name);
int fs_move(mu32 h, mu32 dir);
int fs_space(mu32 *cap_hi, mu32 *cap_lo, mu32 *free_hi, mu32 *free_lo);
const char *fs_label(void);
int fs_locked(void);		/* the desktop PIN lock (stub until it exists) */
long fs_now(void);
void fs_date(long t, char *buf);	/* "YYYYMMDDThhmmss" */

/*===========================================================================*
 * The core (mtpcore.c)							     *
 *===========================================================================*/
void mtp_init(void);
/* Serve one command; 1 go on, 0 the transport closed. */
int mtp_step(void);
/* A file under the storage changed on the Flipper: StorageInfoChanged,
 * at most one per 2 s (rate-limited here). */
void mtp_changed(long now);
void mtp_end(void);		/* the transport closed: drop a partial file */

#endif /* MTP_H */
