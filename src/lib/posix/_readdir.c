/*	readdir()					Author: Kees J. Bot
 *								24 Apr 1989
 */
#define nil 0
#include <lib.h>
#define read	_read
#define lseek	_lseek
#define readdir	_readdir
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

struct dirent *readdir(DIR *dp)
/* Return the next entry in a directory: 16-byte V1/V2 entries (14-character
 * names) or 64-byte V3 entries (60 characters), as opendir() found.
 */
{
	struct dirent *e;
	int size, nlen;

	if (dp == nil) { errno= EBADF; return nil; }
	e= &dp->_ent;
	if (dp->_v7 == (char)-1) {
		/* The entry format (spec O.4), once, from the directory's first
		 * entry, always ".": a V3 entry has ".\0" at bytes 4-5; a V1/V2
		 * entry has its name at byte 2, zero-padded, so byte 4 is 0.
		 * Only name bytes are looked at, never the inode number, so any
		 * 32-bit inode number works. */
		char h[8];
		off_t here= lseek(dp->_fd, (off_t) 0, SEEK_CUR);

		dp->_v7= 16;
		if (here != -1 && lseek(dp->_fd, (off_t) 0, SEEK_SET) == 0
			&& read(dp->_fd, h, sizeof(h)) == sizeof(h)
			&& h[4] == '.' && h[5] == 0)
			dp->_v7= 64;
		if (here != -1) (void) lseek(dp->_fd, here, SEEK_SET);
	}
	size= dp->_v7 == 64 ? 64 : 16;
	nlen= size == 64 ? 60 : 14;

	do {
		if (dp->_count < size) {
			/* Read the next directory block. */
			dp->_count= read(dp->_fd, dp->_buf, sizeof(dp->_buf));
			if (dp->_count < size) { dp->_count= 0; return nil; }
			dp->_ptr= dp->_buf;
		}
		if (size == 64) {
			e->d_ino= ((struct _v3_direct *) dp->_ptr)->d_ino;
			memcpy(e->d_name, dp->_ptr + 4, 60);
		} else {
			e->d_ino= ((struct _v7_direct *) dp->_ptr)->d_ino;
			memcpy(e->d_name, dp->_ptr + 2, 14);
		}
		e->d_name[nlen]= 0;
		dp->_ptr+= size;
		dp->_count-= size;
		dp->_pos+= size;
	} while (e->d_ino == 0);
	return e;
}
