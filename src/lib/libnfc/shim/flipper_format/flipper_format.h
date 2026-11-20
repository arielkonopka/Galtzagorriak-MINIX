/* libnfc shim (#404): FlipperFormat declared only so the vendored device
 * data files compile.  Their load/save is not used: `.nfc` files are read
 * and written by libnfc's nfc_file.c.  Every call fails (returns false). */
#pragma once
#include <furi.h>
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
