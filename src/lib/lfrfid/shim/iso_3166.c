/* SPDX-License-Identifier: GPL-3.0-or-later
 * liblfrfid (spec R9.0.1): Unleashed lib/lfrfid/tools/iso_3166.c without
 * flipper_format.  Looks the country up in the same resource file
 * (EXT_PATH("lfrfid/assets/iso3166.lfrfid"), "Flipper LFRFID resources"
 * version 1, lines "NNNN: AF AFG Afghanistan"), with stdio.  No file: not
 * found, and FDX-B prints "Unknown" as the stock app does then. */
#include <lfrfid/tools/iso_3166.h>

#define RESOURCE_FILE_PATH (EXT_PATH("lfrfid/assets/iso3166.lfrfid"))

static bool lfrfid_search_data(uint16_t country_code, char* out, size_t size) {
    char line[96], key[8];
    FILE* f = fopen(RESOURCE_FILE_PATH, "r");
    bool found = false;
    size_t n = 0;

    if(f == NULL) return false;
    snprintf(key, sizeof(key), "%04u:", (unsigned)country_code);
    if(fgets(line, sizeof(line), f) == NULL ||
       strncmp(line, "Filetype: Flipper LFRFID resources", 34) != 0)
        goto out;
    while(fgets(line, sizeof(line), f) != NULL) {
        const char* p = line + 5;

        if(strncmp(line, key, 5) != 0) continue;
        for(; *p && *p != '\n' && n + 1 < size; p++)
            if(*p != ' ') out[n++] = *p; /* as stock: spaces removed */
        out[n] = 0;
        found = true;
        break;
    }
out:
    fclose(f);
    return found;
}

static bool part(uint16_t code, FuriString* out, size_t from, size_t len) {
    char line[96];

    if(!lfrfid_search_data(code, line, sizeof(line))) return false;
    if(strlen(line) < from + (len ? len : 1)) return false;
    if(len) line[from + len] = 0;
    furi_string_set_str(out, line + from);
    return true;
}

bool iso_3166_get_two_letter(Storage* storage, uint16_t country_code, FuriString* out) {
    (void)storage;
    return part(country_code, out, 0, 2);
}

bool iso_3166_get_three_letter(Storage* storage, uint16_t country_code, FuriString* out) {
    (void)storage;
    return part(country_code, out, 2, 3);
}

bool iso_3166_get_full_name(Storage* storage, uint16_t country_code, FuriString* out) {
    (void)storage;
    return part(country_code, out, 5, 0);
}
