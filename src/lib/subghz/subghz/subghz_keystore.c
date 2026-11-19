/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz: the KeeLoq manufacturer keystore (spec Phase 9, D.3).
 *
 * Own code with the interface of Unleashed lib/subghz/subghz_keystore.c
 * (DarkFlippers/unleashed-firmware 53818b18f63e105baaf37ed8eac9a7def58ec8b1).
 * D.3: the manufacturer keys are never shipped; the user's own file is
 * read from /home/flipper/subghz/assets/keeloq_mfcodes (and
 * keeloq_mfcodes_user).  The stock files are AES-256 encrypted with a key
 * held in the CPU2 enclave, which MINIX has no client for yet, so only
 * plain keystores load here:
 *
 *	Filetype: Flipper SubGhz Keystore File
 *	Version: 0
 *	Encryption: 0
 *	0123456789ABCDEF:1:Manufacturer_name
 *
 * (one KEY:TYPE:NAME line per key, as stock's decrypted lines).  An
 * encrypted file is refused (subghz_keystore_load() false, the reason in
 * subghz_keystore_error()).  At most SUBGHZ_KEYSTORE_MAX keys are kept,
 * each name in its own small heap string: the RAM is bounded.
 * Rainbow tables (Nice Flor-S, Alutech AT-4N): "Flipper SubGhz Keystore
 * RAW File", Version 0, Encryption 0, the bytes in hex as "Data:". */
#include "subghz_keystore.h"
#include "subghz_keystore_i.h"

#include <furi.h>
#include <furi_hal.h>
#include <toolbox/hex.h>
#include <flipper_format/flipper_format.h>

#define SUBGHZ_KEYSTORE_FILE_TYPE     "Flipper SubGhz Keystore File"
#define SUBGHZ_KEYSTORE_FILE_RAW_TYPE "Flipper SubGhz Keystore RAW File"
#define SUBGHZ_KEYSTORE_FILE_VERSION  0

#ifndef SUBGHZ_KEYSTORE_MAX
#define SUBGHZ_KEYSTORE_MAX 512
#endif

static const char* keystore_error = "";
static unsigned keystore_dropped;

const char* subghz_keystore_error(void) {
    return keystore_error;
}

unsigned subghz_keystore_dropped(void) {
    return keystore_dropped;
}

SubGhzKeystore* subghz_keystore_alloc(void) {
    SubGhzKeystore* instance = malloc(sizeof(SubGhzKeystore));

    SubGhzKeyArray_init(instance->data);
    subghz_keystore_reset_kl(instance);
    return instance;
}

void subghz_keystore_reset_kl(SubGhzKeystore* instance) {
    furi_assert(instance);
    instance->mfname = "";
    instance->kl_type = 0;
}

void subghz_keystore_free(SubGhzKeystore* instance) {
    furi_assert(instance);
    for
        M_EACH(manufacture_code, instance->data, SubGhzKeyArray_t) {
            furi_string_free(manufacture_code->name);
            manufacture_code->key = 0;
        }
    SubGhzKeyArray_clear(instance->data);
    free(instance);
}

/* KEY:TYPE:NAME, KEY 1..16 hex digits, TYPE decimal, NAME 1..64 chars. */
static bool subghz_keystore_process_line(SubGhzKeystore* instance, const char* line) {
    uint64_t key = 0;
    unsigned long type = 0;
    const char* p = line;
    size_t n = 0;
    uint8_t nib;

    while(n < 16 && hex_char_to_hex_nibble(*p, &nib)) {
        key = key << 4 | nib;
        p++;
        n++;
    }
    if(n == 0 || *p++ != ':') return false;
    if(*p < '0' || *p > '9') return false;
    while(*p >= '0' && *p <= '9' && type <= 0xFFFF)
        type = type * 10 + (unsigned long)(*p++ - '0');
    if(type > 0xFFFF || *p++ != ':') return false;
    n = strlen(p);
    while(n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
        n--;
    if(n == 0 || n > 64) return false;
    if(SubGhzKeyArray_size(instance->data) >= SUBGHZ_KEYSTORE_MAX) {
        keystore_dropped++;
        return true;
    }
    SubGhzKey* code = SubGhzKeyArray_push_raw(instance->data);
    code->name = furi_string_alloc();
    furi_string_set_str(code->name, p);
    if(furi_string_size(code->name) > n) {
        FuriString* t = furi_string_alloc();
        furi_string_set_n(t, code->name, 0, n);
        furi_string_set_s(code->name, t);
        furi_string_free(t);
    }
    code->key = key;
    code->type = (uint16_t)type;
    return true;
}

/* The header: type and version, "Encryption"; the stream is then at the
 * line after it.  Read line by line with stdio, never the whole file. */
static FILE* keystore_open(const char* file_name, const char* type, uint32_t* encryption) {
    char line[160];
    char want_type[64];
    bool have_type = false, have_version = false;
    FILE* f = fopen(file_name, "r");

    keystore_error = "cannot open";
    if(f == NULL) return NULL;
    snprintf(want_type, sizeof(want_type), "Filetype: %s", type);
    while(fgets(line, sizeof(line), f) != NULL) {
        size_t n = strlen(line);
        while(n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if(n == 0 || line[0] == '#') continue;
        if(!have_type) {
            if(strcmp(line, want_type) != 0) break;
            have_type = true;
        } else if(!have_version) {
            if(strcmp(line, "Version: 0") != 0) break;
            have_version = true;
        } else {
            if(strncmp(line, "Encryption: ", 12) != 0) break;
            *encryption = (uint32_t)strtoul(line + 12, NULL, 10);
            keystore_error = "";
            return f;
        }
    }
    keystore_error = "not a keystore of this type and version";
    fclose(f);
    return NULL;
}

bool subghz_keystore_load(SubGhzKeystore* instance, const char* file_name) {
    furi_assert(instance);
    char line[160];
    uint32_t encryption = 0;
    bool result = true;
    FILE* f = keystore_open(file_name, SUBGHZ_KEYSTORE_FILE_TYPE, &encryption);

    if(f == NULL) return false;
    if(encryption != 0) {
        keystore_error = "encrypted (stock): needs the CPU2 key enclave; "
                         "use a plain keystore (Encryption: 0)";
        fclose(f);
        return false;
    }
    while(fgets(line, sizeof(line), f) != NULL) {
        size_t n = strlen(line);
        if(n > 0 && line[n - 1] != '\n' && !feof(f)) {
            int c;
            while((c = getc(f)) != EOF && c != '\n')
                ; /* too long: skipped */
            result = false;
            continue;
        }
        while(n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if(n == 0 || line[0] == '#') continue;
        if(!subghz_keystore_process_line(instance, line)) result = false;
    }
    fclose(f);
    if(!result) keystore_error = "some lines are not KEY:TYPE:NAME";
    return result || SubGhzKeyArray_size(instance->data) > 0;
}

bool subghz_keystore_save(SubGhzKeystore* instance, const char* file_name, uint8_t* iv) {
    UNUSED(instance);
    UNUSED(file_name);
    UNUSED(iv);
    keystore_error = "saving an encrypted keystore needs the CPU2 key enclave";
    return false;
}

SubGhzKeyArray_t* subghz_keystore_get_data(SubGhzKeystore* instance) {
    furi_assert(instance);
    return &instance->data;
}

bool subghz_keystore_raw_encrypted_save(
    const char* input_file_name,
    const char* output_file_name,
    uint8_t* iv) {
    UNUSED(input_file_name);
    UNUSED(output_file_name);
    UNUSED(iv);
    return false;
}

bool subghz_keystore_raw_get_data(const char* file_name, size_t offset, uint8_t* data, size_t len) {
    char line[160];
    uint32_t encryption = 0;
    size_t pos = 0, got = 0;
    bool in_data = false;
    FILE* f;

    if(file_name == NULL || file_name[0] == 0) return false;
    f = keystore_open(file_name, SUBGHZ_KEYSTORE_FILE_RAW_TYPE, &encryption);
    if(f == NULL) return false;
    if(encryption != 0) {
        keystore_error = "encrypted (stock): needs the CPU2 key enclave";
        fclose(f);
        return false;
    }
    /* "Data: 0A1B..." (hex, spaces allowed), on one or more lines */
    while(got < len && fgets(line, sizeof(line), f) != NULL) {
        const char* p = line;
        uint8_t hi, lo;

        if(!in_data) {
            if(strncmp(line, "Data:", 5) != 0) continue;
            in_data = true;
            p += 5;
        }
        while(*p && got < len) {
            if(*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') {
                p++;
                continue;
            }
            if(!hex_char_to_hex_nibble(p[0], &hi) || !hex_char_to_hex_nibble(p[1], &lo)) {
                fclose(f);
                return false;
            }
            if(pos >= offset) data[got++] = (uint8_t)(hi << 4 | lo);
            pos++;
            p += 2;
        }
    }
    fclose(f);
    return got == len;
}
