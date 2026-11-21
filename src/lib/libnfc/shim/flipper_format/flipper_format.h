/* libnfc shim (#404): FlipperFormat declared so the vendored device data
 * files compile.  Their load/save is not used: `.nfc` files are read and
 * written by libnfc's nfc_file.c, and those calls fail (return false).
 * The read side the parsers' lookup files need (file_alloc,
 * file_open_existing, read_header, read_string, rewind) works:
 * nfc_shim_ff.c (#412). */
#pragma once
#include <furi.h>
#include <storage/storage.h> /* RECORD_STORAGE, as stock's header pulls in */
typedef struct FlipperFormat FlipperFormat;
bool flipper_format_read_hex(FlipperFormat*, const char*, uint8_t*, uint16_t);
bool flipper_format_write_hex(FlipperFormat*, const char*, const uint8_t*, uint16_t);
bool flipper_format_read_uint32(FlipperFormat*, const char*, uint32_t*, uint16_t);
bool flipper_format_write_uint32(FlipperFormat*, const char*, const uint32_t*, uint16_t);
bool flipper_format_read_string(FlipperFormat*, const char*, FuriString*);
bool flipper_format_write_string(FlipperFormat*, const char*, FuriString*);
bool flipper_format_write_string_cstr(FlipperFormat*, const char*, const char*);
bool flipper_format_write_comment_cstr(FlipperFormat*, const char*);
bool flipper_format_write_comment(FlipperFormat*, FuriString*);
bool flipper_format_key_exist(FlipperFormat*, const char*);
bool flipper_format_get_value_count(FlipperFormat*, const char*, uint32_t*);
bool flipper_format_read_bool(FlipperFormat*, const char*, bool*, uint16_t);
bool flipper_format_write_bool(FlipperFormat*, const char*, const bool*, uint16_t);
bool flipper_format_rewind(FlipperFormat*);
bool flipper_format_read_hex_uint64(FlipperFormat*, const char*, uint64_t*, uint16_t);
bool flipper_format_write_hex_uint64(FlipperFormat*, const char*, const uint64_t*, uint16_t);
bool flipper_format_write_empty_line(FlipperFormat*);
/* nfc_device_save/_load (vendored nfc_device.c): no files here, opening
 * fails, so both return false. */
typedef struct Storage Storage;
FlipperFormat* flipper_format_buffered_file_alloc(Storage*);
bool flipper_format_buffered_file_open_always(FlipperFormat*, const char*);
bool flipper_format_buffered_file_open_existing(FlipperFormat*, const char*);
void flipper_format_free(FlipperFormat*);
bool flipper_format_read_header(FlipperFormat*, FuriString*, uint32_t*);
bool flipper_format_write_header_cstr(FlipperFormat*, const char*, uint32_t);
/* Read-only text files (#412, nfc_shim_ff.c): the ID lists and EMV tables
 * the parsers look names up in.  "/ext/..." paths map to the user's home
 * (nfcshim_ext_dir, "/home/flipper/" by default). */
FlipperFormat* flipper_format_file_alloc(Storage*);
bool flipper_format_file_open_existing(FlipperFormat*, const char*);
extern const char* nfcshim_ext_dir;
/* The path a "/ext/..." name maps to (into buf; returns buf). */
const char* nfcshim_ext_path(const char* path, char* buf, size_t size);
