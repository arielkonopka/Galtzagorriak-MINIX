/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz: FlipperFormat, the stock "Key: value" text files (.sub key
 * files, keystores), as a bounded text buffer in memory.  The calls and
 * their meaning are stock's (lib/flipper_format/flipper_format.h): a read
 * looks for its key from the current position on, a write goes in at the
 * current position, an update rewinds and replaces the key's line.
 *
 * Key files are small (a few hundred bytes); a file over
 * FLIPPER_FORMAT_MAX bytes is refused, never loaded whole (RAW captures
 * are not read through here: `subghz` streams their RAW_Data lines).
 * A Stream (stock: the file under the format) is the same text here:
 * stream_clean() empties it. */
#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FLIPPER_FORMAT_MAX
#define FLIPPER_FORMAT_MAX 4096
#endif

typedef struct FlipperFormat FlipperFormat;
typedef FlipperFormat Stream;

FlipperFormat* flipper_format_string_alloc(void);
FlipperFormat* flipper_format_file_alloc(Storage* storage);
void flipper_format_free(FlipperFormat* ff);
/* Load a file (<= FLIPPER_FORMAT_MAX bytes) / write the text back. */
bool flipper_format_file_open_existing(FlipperFormat* ff, const char* path);
bool flipper_format_file_open_always(FlipperFormat* ff, const char* path);
bool flipper_format_file_close(FlipperFormat* ff);
bool flipper_format_save(FlipperFormat* ff, const char* path);
/* The text (for tests and for writing back), its length. */
const char* flipper_format_text(FlipperFormat* ff, size_t* len);
bool flipper_format_set_text(FlipperFormat* ff, const char* text, size_t len);
bool flipper_format_changed(FlipperFormat* ff);

bool flipper_format_rewind(FlipperFormat* ff);
bool flipper_format_seek_to_end(FlipperFormat* ff);
bool flipper_format_key_exist(FlipperFormat* ff, const char* key);

bool flipper_format_read_header(FlipperFormat* ff, FuriString* filetype, uint32_t* version);
bool flipper_format_read_string(FlipperFormat* ff, const char* key, FuriString* data);
bool flipper_format_read_uint32(FlipperFormat* ff, const char* key, uint32_t* data, uint16_t n);
bool flipper_format_read_int32(FlipperFormat* ff, const char* key, int32_t* data, uint16_t n);
bool flipper_format_read_hex(FlipperFormat* ff, const char* key, uint8_t* data, uint16_t n);
bool flipper_format_read_bool(FlipperFormat* ff, const char* key, bool* data, uint16_t n);
bool flipper_format_get_value_count(FlipperFormat* ff, const char* key, uint32_t* count);

bool flipper_format_write_header_cstr(FlipperFormat* ff, const char* filetype, uint32_t version);
bool flipper_format_write_string_cstr(FlipperFormat* ff, const char* key, const char* data);
bool flipper_format_write_string(FlipperFormat* ff, const char* key, FuriString* data);
bool flipper_format_write_uint32(FlipperFormat* ff, const char* key, const uint32_t* data, uint16_t n);
bool flipper_format_write_int32(FlipperFormat* ff, const char* key, const int32_t* data, uint16_t n);
bool flipper_format_write_hex(FlipperFormat* ff, const char* key, const uint8_t* data, uint16_t n);
bool flipper_format_write_bool(FlipperFormat* ff, const char* key, const bool* data, uint16_t n);
bool flipper_format_write_comment_cstr(FlipperFormat* ff, const char* data);

bool flipper_format_update_string_cstr(FlipperFormat* ff, const char* key, const char* data);
bool flipper_format_update_uint32(FlipperFormat* ff, const char* key, const uint32_t* data, uint16_t n);
bool flipper_format_update_hex(FlipperFormat* ff, const char* key, const uint8_t* data, uint16_t n);
bool flipper_format_insert_or_update_uint32(
    FlipperFormat* ff, const char* key, const uint32_t* data, uint16_t n);
bool flipper_format_insert_or_update_hex(
    FlipperFormat* ff, const char* key, const uint8_t* data, uint16_t n);

Stream* flipper_format_get_raw_stream(FlipperFormat* ff);
bool stream_clean(Stream* stream);

#ifdef __cplusplus
}
#endif
