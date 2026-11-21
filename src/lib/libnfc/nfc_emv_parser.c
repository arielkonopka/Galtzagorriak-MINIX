/* nfc_emv_parser.c - EMV AID / country / currency names from the user's
 * lookup files (#412).  See shim/helpers/nfc_emv_parser.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #412.  As stock's helper:
 * keys are upper-case hex (AID bytes, or the 2-byte code as %04X).
 */
#include <flipper_format/flipper_format.h>
#include <helpers/nfc_emv_parser.h>

static bool emv_lookup(Storage* storage, const char* file, const char* key, FuriString* name) {
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* type = furi_string_alloc();
    uint32_t version = 0;
    bool ok = flipper_format_file_open_existing(ff, file) &&
              flipper_format_read_header(ff, type, &version) &&
              !strcmp(furi_string_get_cstr(type), "Flipper EMV resources") && version == 1 &&
              flipper_format_read_string(ff, key, name);
    furi_string_free(type);
    flipper_format_free(ff);
    return ok;
}

bool nfc_emv_parser_get_aid_name(Storage* storage, const uint8_t* aid, uint8_t aid_len, FuriString* name) {
    char key[2 * 16 + 1];
    if(!aid || aid_len == 0 || aid_len > 16) return false;
    for(uint8_t i = 0; i < aid_len; i++) snprintf(key + 2 * i, 3, "%02X", aid[i]);
    return emv_lookup(storage, EXT_PATH("nfc/assets/aid.nfc"), key, name);
}

bool nfc_emv_parser_get_country_name(Storage* storage, uint16_t country_code, FuriString* name) {
    char key[5];
    snprintf(key, sizeof key, "%04X", country_code);
    return emv_lookup(storage, EXT_PATH("nfc/assets/country_code.nfc"), key, name);
}

bool nfc_emv_parser_get_currency_name(Storage* storage, uint16_t currency_code, FuriString* name) {
    char key[5];
    snprintf(key, sizeof key, "%04X", currency_code);
    return emv_lookup(storage, EXT_PATH("nfc/assets/currency_code.nfc"), key, name);
}
