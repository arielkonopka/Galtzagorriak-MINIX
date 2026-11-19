/*	dirent.h - Declarations for directory reading routines.
 *							Author: Kees J. Bot
 *								24 Apr 1989
 * 
 * Note: The directory entries used under Minix (16-byte V1/V2, 64-byte V3)
 * are transformed into a struct dirent with a d_name of 61 characters.
 */

#ifndef _DIRENT_H
#define _DIRENT_H

/* On-disk directory entries (spec O.4).  The FS returns them as they are;
 * readdir() turns them into a struct dirent.  The old "flex" format, never
 * on our disks, is gone.
 */

/* Version 7 / MINIX V1 and V2 directory entry, 16 bytes: */
struct _v7_direct {		
	unsigned short	d_ino;
	char		d_name[14];
};

/* MINIX V3 directory entry, 64 bytes: */
struct _v3_direct {
	unsigned long	d_ino;
	char		d_name[60];
};

struct dirent {
	ino_t		d_ino;		/* I-node number */
	char		d_name[61];	/* Null terminated name */
};

/* Definitions for the directory(3) routines: */
typedef struct {
	char		_fd;	/* Filedescriptor of open directory */
	char		_v7;	/* Entry size: 16 (V1/V2) or 64 (V3) */
	short		_count;	/* This many bytes left in buf */
	off_t		_pos;	/* Position in directory file */
	char		*_ptr;	/* Next entry in buf */
	char		_buf[1024];	/* One block of a directory file */
	struct dirent	_ent;	/* The entry readdir() returns */
} DIR;

/* Function Prototypes. */
#ifndef _ANSI_H
#include <ansi.h>
#endif

_PROTOTYPE( int closedir, (DIR *_dirp)					);
_PROTOTYPE( DIR *opendir, (const char *_dirname)			);
_PROTOTYPE( struct dirent *readdir, (DIR *_dirp)			);
_PROTOTYPE( void rewinddir, (DIR *_dirp)				);

#ifdef _MINIX
_PROTOTYPE( int seekdir, (DIR *_dirp, off_t _loc)			);
_PROTOTYPE( off_t telldir, (DIR *_dirp)					);
#endif

#endif /* _DIRENT_H */
