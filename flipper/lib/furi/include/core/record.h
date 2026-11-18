/* SPDX-License-Identifier: GPL-3.0-or-later
 * libfuri (spec R5.1, D.1): records (stock furi/core/record.h): named
 * pointers shared by the threads of one process. furi_record_open()
 * waits until someone creates the record. libfuri's own services ("gui",
 * "storage", "notification", ...) come with R5.2-R5.4. */
#ifndef FURI_CORE_RECORD_H
#define FURI_CORE_RECORD_H

#include "core/base.h"

#ifdef __cplusplus
extern "C" {
#endif

bool furi_record_exists(const char* name);
void furi_record_create(const char* name, void* data);
/* false if someone still holds it open */
bool furi_record_destroy(const char* name);
void* furi_record_open(const char* name);
void furi_record_close(const char* name);

#ifdef __cplusplus
}
#endif

#endif
