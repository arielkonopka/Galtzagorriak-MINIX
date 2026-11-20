/* nfc_supported_cards.c - the parser table and the read/parse loops
 * (#404).  See nfc_supported_cards.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <furi.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <flipper_application.h>
#include <plugins/supported_cards/nfc_supported_card_plugin.h>

#include "nfc_supported_cards.h"
#include "nfc_supported_cards_list.h"

#define NSC_DECL(name) extern const FlipperAppPluginDescriptor* name##_plugin_ep(void);
NFC_SUPPORTED_CARDS(NSC_DECL)

typedef struct {
    const char* name;
    const FlipperAppPluginDescriptor* (*ep)(void);
} NscEntry;

#define NSC_ENTRY(name) {#name, name##_plugin_ep},
static const NscEntry nsc_table[] = {NFC_SUPPORTED_CARDS(NSC_ENTRY)};

size_t nfc_supported_cards_count(void) { return COUNT_OF(nsc_table); }

const char* nfc_supported_cards_name(size_t i) {
    return i < COUNT_OF(nsc_table) ? nsc_table[i].name : NULL;
}

static const NfcSupportedCardsPlugin* nsc_plugin(size_t i) {
    const FlipperAppPluginDescriptor* d = nsc_table[i].ep();
    furi_check(d && d->ep_api_version == NFC_SUPPORTED_CARD_PLUGIN_API_VERSION);
    furi_check(strcmp(d->appid, NFC_SUPPORTED_CARD_PLUGIN_APP_ID) == 0);
    return d->entry_point;
}

const void* nfc_supported_cards_plugin(size_t i) {
    return i < COUNT_OF(nsc_table) ? nsc_plugin(i) : NULL;
}

int nfc_supported_cards_read(Nfc* nfc, NfcDevice* device) {
    furi_check(nfc && device);
    NfcProtocol protocol = nfc_device_get_protocol(device);
    for(size_t i = 0; i < COUNT_OF(nsc_table); i++) {
        const NfcSupportedCardsPlugin* p = nsc_plugin(i);
        if(p->protocol != protocol || !p->read) continue;
        if(p->verify && !p->verify(nfc)) continue;
        if(p->read(nfc, device)) return (int)i;
    }
    return -1;
}

int nfc_supported_cards_parse(const NfcDevice* device, FuriString* out) {
    furi_check(device && out);
    NfcProtocol protocol = nfc_device_get_protocol(device);
    for(size_t i = 0; i < COUNT_OF(nsc_table); i++) {
        const NfcSupportedCardsPlugin* p = nsc_plugin(i);
        if(p->protocol != protocol || !p->parse) continue;
        furi_string_reset(out);
        if(p->parse(device, out)) return (int)i;
    }
    furi_string_reset(out);
    return -1;
}
