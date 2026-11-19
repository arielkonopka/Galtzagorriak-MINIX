/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz (spec Phase 9, issue #302): the MINIX side of the Unleashed
 * Sub-GHz protocols, for `subghz` and the host test.
 *
 * - .sub files read a line at a time (never the whole file): the header
 *   (Filetype, Frequency, Preset, Protocol), then for RAW files the
 *   RAW_Data values one by one (positive: carrier on that many us,
 *   negative: off), as stock's subghz_file_encoder_worker gives them;
 * - an environment with every protocol and the user's keystore
 *   (/home/flipper/subghz/assets, spec D.3: never shipped);
 * - one line per decoded packet: "Protocol 24bit Key:6AB234".
 */
#ifndef SUBGHZ_SGFILE_H
#define SUBGHZ_SGFILE_H

#include <stdio.h>
#include <furi.h>
#include <subghz/environment.h>
#include <subghz/receiver.h>
#include <subghz/transmitter.h>
#include <subghz/protocols/base.h>
#include <subghz/protocols/protocol_items.h>

#define SG_ASSETS "/home/flipper/subghz/assets"

typedef struct {
    FILE* f;
    unsigned long frequency;
    char filetype[40];
    char preset[48];
    char protocol[32];
    bool raw;           /* Protocol: RAW */
    unsigned long line; /* for messages */
} SgFile;

/* Open a .sub file and read its header up to Protocol (RAW: up to the
 * first RAW_Data).  0, or -1 with a message in *why. */
int sg_open(SgFile* s, const char* path, const char** why);
void sg_close(SgFile* s);
/* The next RAW value: 1 (*level, *us), 0 at the end, -1 bad data. */
int sg_raw_next(SgFile* s, bool* level, uint32_t* us);

/* All protocols; the keystores and rainbow tables from assets (NULL: the
 * default SG_ASSETS) if there.  *keys: the number of manufacturer keys
 * loaded (may be NULL). */
SubGhzEnvironment* sg_env_alloc(const char* assets, size_t* keys);
void sg_env_free(SubGhzEnvironment* env);

/* "Protocol 24bit Key:6AB234" for a decoder that just fired (the key from
 * its serialize(), as in a saved .sub file); the line has no newline. */
void sg_describe(SubGhzProtocolDecoderBase* d, FuriString* out);
/* Only the key's hex digits ("6AB234"), or "" if it has none. */
void sg_key_hex(SubGhzProtocolDecoderBase* d, FuriString* out);

/* Encoders: a transmitter set up from a key file's text (in ff); NULL
 * and *why if the protocol has no encoder or the file is bad. */
SubGhzTransmitter* sg_transmitter(SubGhzEnvironment* env, FlipperFormat* ff, const char** why);

#endif
