/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc shim (#404): the plugin descriptor the supported_cards parsers
 * export.  No loader: nfc_supported_cards.c links them in and lists
 * their entry points in a table. */
#pragma once
#include <furi.h>

typedef struct {
    const char* appid;
    uint32_t ep_api_version;
    const void* entry_point;
} FlipperAppPluginDescriptor;
