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

/* The ID-list parsers (#412, D.3): plantain, sev_tk, sk_tk, szppk_so and
 * skylanders name stations or figures from the user's own lists in
 * /home/flipper/nfc/assets (none shipped).  The list file parser i reads
 * ("nfc/assets/sz_id.nfc", relative to the home), or NULL. */
const char* nfc_supported_cards_list_file(size_t i);
/* After nfc_supported_cards_parse(): append to out a note for each list
 * the card's protocol could have used and that is missing, so the user
 * sees why no names are shown:
 *   parsed (r >= 0, a list parser):  "No ID list found (<path>): codes
 *                                     shown instead of names."
 *   not parsed (r < 0):               "No ID list found (<path>),
 *                                     showing the raw card."
 * Returns how many notes were added. */
int nfc_supported_cards_list_notes(const struct NfcDevice* device, int r, struct FuriString* out);

#endif
