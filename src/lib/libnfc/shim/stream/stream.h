/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc shim (#404): the Stream/Storage names mf_classic_poller.c uses
 * for its nested-attack log.  No file system here: opening fails, so the
 * vendored code takes its own "could not save" path.  Only the dictionary
 * attack mode reaches it; libnfc reads with known keys (mf_classic_shim.c)
 * and runs its own attacks (mf_attack.c). */
#pragma once
#include <furi.h>

typedef struct Stream Stream;
typedef struct Storage Storage;
#define RECORD_STORAGE "storage"
typedef enum { FSAM_READ = 1, FSAM_WRITE = 2, FSAM_READ_WRITE = 3 } FS_AccessMode;
typedef enum { FSOM_OPEN_EXISTING = 1, FSOM_OPEN_ALWAYS = 2, FSOM_OPEN_APPEND = 4,
               FSOM_CREATE_NEW = 8, FSOM_CREATE_ALWAYS = 16 } FS_OpenMode;

void* furi_record_open(const char* name);
void furi_record_close(const char* name);
void stream_free(Stream* stream);
size_t stream_write_string(Stream* stream, FuriString* string);
