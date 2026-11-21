/* SPDX-License-Identifier: GPL-3.0-or-later
 * irfile - the stock Flipper infrared files, read and written as a stream
 * (spec Phase 7, AC7.1; issue #389).  Ours, not stock's (stock reads them
 * with FlipperFormat, which keeps a whole line of up to 1024 timings in a
 * string): here a file is read one key at a time with getc, the timings of
 * a raw signal go straight into the caller's buffer, and nothing of the
 * file stays in memory.  So the universal remote databases
 * (/home/flipper/infrared/assets/ *.ir, 100-250 KB) are searched without
 * ever being loaded whole.
 *
 * Formats (stock documentation/file_formats/InfraredFileFormats.md):
 *	Filetype: IR signals file	a remote (Saved remotes)
 *	Filetype: IR library file	a universal remote database
 *	Filetype: IR tests file		the stock unit tests' vectors
 *	Version: 1
 * then signals, each "name:", "type:" (parsed, raw, parsed_array) and the
 * fields of its type.  Lines starting with '#' are comments.
 */
#ifndef IRFILE_H
#define IRFILE_H

#include <stdio.h>
#include "encoder_decoder/infrared.h"

#define IRF_NAME_MAX	40		/* longest signal name kept */
#define IRF_DUTY_ONE	1000000UL	/* duty cycles in millionths */
#define IRF_RAW_MAX	1024		/* stock's limit of timings per signal */

enum { IRF_SIGNALS, IRF_LIBRARY, IRF_TESTS };	/* irf.kind */
enum { IRF_PARSED, IRF_RAW, IRF_ARRAY };		/* irf_sig.type */

struct irf {
	FILE *fp;
	int kind;
	unsigned long line;		/* for error messages */
	char next[IRF_NAME_MAX + 1];	/* a name read ahead */
	unsigned char havenext;
	unsigned char eof;
};

struct irf_sig {
	char name[IRF_NAME_MAX + 1];
	unsigned char type;		/* IRF_PARSED, IRF_RAW, IRF_ARRAY */
	InfraredMessage msg;		/* IRF_PARSED */
	unsigned long freq, duty;	/* IRF_RAW: Hz, millionths */
	uint32_t *data;			/* IRF_RAW: the caller's buffer, */
	size_t cap;			/*   its size in timings, */
	size_t n;			/*   timings read */
	unsigned long count;		/* IRF_ARRAY: messages that follow */
};

/* Read the header.  0, or -1 when fp is not an infrared file of version 1. */
int irf_open(struct irf *f, FILE *fp);
/* The next signal.  1: got one; 0: end of file; -1: a bad signal (it is
 * skipped; the next call goes on with the one after it).  A raw signal
 * with more timings than s->cap is bad.  For IRF_ARRAY, read s->count
 * messages with irf_msg next. */
int irf_next(struct irf *f, struct irf_sig *s);
/* The next signal called name (as stock: exact, case matters).  1, or 0
 * when there is none before the end of the file. */
int irf_find(struct irf *f, const char *name, struct irf_sig *s);
/* One message of a parsed_array: protocol, address, command, repeat. */
int irf_msg(struct irf *f, InfraredMessage *m);

/* Writing, byte for byte as stock writes (so stock reads our files). */
int irf_write_header(FILE *fp, int kind);
int irf_write(FILE *fp, const struct irf_sig *s);

/* A parsed message is valid for its protocol (address and command fit). */
int irf_msg_valid(const InfraredMessage *m);
/* A name stock accepts: 1..IRF_NAME_MAX printable ASCII characters. */
int irf_name_ok(const char *name);

#endif /* IRFILE_H */
