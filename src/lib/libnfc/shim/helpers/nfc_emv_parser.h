/* helpers/nfc_emv_parser.h - the EMV name lookups of stock's NFC app
 * (applications/main/nfc/helpers), which the emv parser includes (#412).
 * Own code: nfc_emv_parser.c.  SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <furi.h>
#include <storage/storage.h>

/* Look the name up in the user's /home/flipper/nfc/assets/{aid,
 * currency_code,country_code}.nfc ("Flipper EMV resources" v1, "KEY:
 * name" lines).  False if the file or the key is missing. */
bool nfc_emv_parser_get_aid_name(Storage* storage, const uint8_t* aid, uint8_t aid_len, FuriString* name);
bool nfc_emv_parser_get_country_name(Storage* storage, uint16_t country_code, FuriString* name);
bool nfc_emv_parser_get_currency_name(Storage* storage, uint16_t currency_code, FuriString* name);
