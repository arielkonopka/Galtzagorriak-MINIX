/* nfc_supported_cards.h - the vendored supported_cards parsers, linked in
 * (#404).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #404 (D.3).
 *
 * Stock loads each parser as a .fal plugin from the SD card.  Here they
 * are plain objects and this is their table: one entry per parser,
 * the plugin's own NfcSupportedCardsPlugin (protocol, verify, read,
 * parse) reached through its unchanged `<name>_plugin_ep()`.  Nothing
 * is shipped that the parser would read from files (ID lists, station
 * names); a parser that needs one is not in the table.
 *
 * The same two steps as stock's nfc app:
 *   read:  for each parser of the card's protocol, verify(nfc) and, when
 *          it says yes, read(nfc, device) with the parser's keys;
 *   parse: for each parser of the protocol, parse(device, out); the first
 *          one that knows the card wins.
 */
#ifndef _LIBNFC_NFC_SUPPORTED_CARDS_H
#define _LIBNFC_NFC_SUPPORTED_CARDS_H

#include <stdbool.h>
#include <stddef.h>

struct Nfc;
struct NfcDevice;
struct FuriString;

/* Parsers linked in, and the name of parser i (its file name). */
size_t nfc_supported_cards_count(void);
const char* nfc_supported_cards_name(size_t i);

/* Parser i's NfcSupportedCardsPlugin (as const void*: the type lives in
 * the vendored plugin header), for tests and listings. */
const void* nfc_supported_cards_plugin(size_t i);

/* Read the card with the first parser of the device's protocol whose
 * verify() accepts it.  The device must hold the protocol's data (from a
 * plain read); it is replaced by the parser's read.  Returns the parser's
 * index, or -1 if none read it. */
int nfc_supported_cards_read(struct Nfc* nfc, struct NfcDevice* device);

/* Parse the device's data into text.  Returns the index of the parser
 * that recognised the card, or -1 (out is then empty). */
int nfc_supported_cards_parse(const struct NfcDevice* device, struct FuriString* out);

#endif
