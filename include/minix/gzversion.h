/* gzversion.h - the version of Galtzagorriak (spec J.2)
 *
 * The only place the numbers are written; a release changes them here and
 * adds its CHANGELOG.md section in the same PR (make versioncheck).  The
 * build id (commits since the tag, hash) is in the generated gzbuild.h.
 * OS_RELEASE / OS_VERSION in <minix/config.h> still mean the MINIX base.
 */
#ifndef _MINIX_GZVERSION_H
#define _MINIX_GZVERSION_H

#define GZ_NAME		"Galtzagorriak"
#define GZ_MAJOR	0
#define GZ_MINOR	1
#define GZ_PATCH	0
#define GZ_RELEASE	"0.1.0"
#define GZ_ABI		1	/* binary compatibility of programs (J.3) */
#define GZ_BASE		"MINIX 2.0.0"
#define GZ_BASE_VER	"M2.0.0"	/* uname -v */

#endif /* _MINIX_GZVERSION_H */
