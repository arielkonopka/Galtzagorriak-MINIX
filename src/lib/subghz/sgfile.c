/* SPDX-License-Identifier: GPL-3.0-or-later
 * libsubghz: .sub files, the environment, one-line packets.  See sgfile.h. */
#include "sgfile.h"

#include <subghz/registry.h>
#include <subghz/blocks/custom_btn.h>
#include <subghz/blocks/generic.h>
#include <lib/flipper_format/flipper_format.h>

/* ---- .sub files ---- */

/* One header line "Key: value" (comments and blank lines skipped) into
 * key/val; false at the end or at RAW_Data (left unread). */
static bool sg_header_line(SgFile* s, char* key, size_t ksize, char* val, size_t vsize) {
    for(;;) {
        int c;
        size_t k = 0, v = 0;
        bool in_val = false;

        /* RAW_Data: stop before it (peek the first letter) */
        c = getc(s->f);
        if(c == EOF) return false;
        if(c == 'R') {
            char peek[9];
            int n = (int)fread(peek, 1, 8, s->f);
            peek[n < 0 ? 0 : n] = 0;
            if(strcmp(peek, "AW_Data:") == 0) return false;
            /* not RAW_Data: treat "R" + peek as the start of the line */
            key[k++] = 'R';
            for(int i = 0; i < n; i++) {
                char ch = peek[i];
                if(!in_val && ch == ':') {
                    in_val = true;
                    continue;
                }
                if(in_val) {
                    if(v == 0 && ch == ' ') continue;
                    if(v + 1 < vsize) val[v++] = ch;
                } else if(k + 1 < ksize)
                    key[k++] = ch;
            }
            c = getc(s->f);
        }
        while(c != EOF && c != '\n') {
            if(c != '\r') {
                if(!in_val && c == ':')
                    in_val = true;
                else if(in_val) {
                    if(!(v == 0 && c == ' ') && v + 1 < vsize) val[v++] = (char)c;
                } else if(k + 1 < ksize)
                    key[k++] = (char)c;
            }
            c = getc(s->f);
        }
        s->line++;
        key[k] = 0;
        val[v] = 0;
        while(v > 0 && val[v - 1] == ' ')
            val[--v] = 0;
        if(k == 0 || key[0] == '#') {
            if(c == EOF) return false;
            continue;
        }
        return true;
    }
}

int sg_open(SgFile* s, const char* path, const char** why) {
    char key[32], val[64];

    memset(s, 0, sizeof(*s));
    s->f = fopen(path, "r");
    if(s->f == NULL) {
        *why = "cannot open";
        return -1;
    }
    while(sg_header_line(s, key, sizeof(key), val, sizeof(val))) {
        if(strcmp(key, "Filetype") == 0)
            snprintf(s->filetype, sizeof(s->filetype), "%s", val);
        else if(strcmp(key, "Frequency") == 0)
            s->frequency = strtoul(val, NULL, 10);
        else if(strcmp(key, "Preset") == 0)
            snprintf(s->preset, sizeof(s->preset), "%s", val);
        else if(strcmp(key, "Protocol") == 0) {
            snprintf(s->protocol, sizeof(s->protocol), "%s", val);
            s->raw = strcmp(val, "RAW") == 0;
            if(!s->raw) break; /* a key file: read again through FlipperFormat */
        }
    }
    if(strncmp(s->filetype, "Flipper SubGhz", 14) != 0) {
        *why = "not a Flipper SubGhz file";
        sg_close(s);
        return -1;
    }
    if(s->protocol[0] == 0) {
        *why = "no Protocol line";
        sg_close(s);
        return -1;
    }
    return 0;
}

void sg_close(SgFile* s) {
    if(s->f) fclose(s->f);
    s->f = NULL;
}

int sg_raw_next(SgFile* s, bool* level, uint32_t* us) {
    for(;;) {
        int c = getc(s->f);
        long v = 0;
        bool neg = false;
        int digits = 0;

        if(c == EOF) return 0;
        if(c == ' ' || c == ',' || c == '\r' || c == '\t') continue;
        if(c == '\n') {
            s->line++;
            continue;
        }
        if(c == 'R') { /* "RAW_Data:" starts the next line of values */
            char word[9];
            if(fread(word, 1, 8, s->f) != 8 || memcmp(word, "AW_Data:", 8) != 0) return -1;
            continue;
        }
        if(c == '-') {
            neg = true;
            c = getc(s->f);
        }
        while(c >= '0' && c <= '9') {
            if(v < 100000000L) v = v * 10 + (c - '0');
            digits++;
            c = getc(s->f);
        }
        if(digits == 0) return -1;
        if(c == '\n') s->line++;
        else if(c != EOF && c != ' ' && c != ',' && c != '\r' && c != '\t') return -1;
        *level = !neg;
        *us = (uint32_t)v;
        return 1;
    }
}

/* ---- the environment ---- */

SubGhzEnvironment* sg_env_alloc(const char* assets, size_t* keys) {
    static char nice[96], alutech[96];
    char path[96];
    SubGhzEnvironment* env = subghz_environment_alloc();

    if(assets == NULL) assets = SG_ASSETS;
    subghz_environment_set_protocol_registry(env, (void*)&subghz_protocol_registry);
    snprintf(path, sizeof(path), "%s/keeloq_mfcodes", assets);
    subghz_environment_load_keystore(env, path);
    snprintf(path, sizeof(path), "%s/keeloq_mfcodes_user", assets);
    subghz_environment_load_keystore(env, path);
    snprintf(nice, sizeof(nice), "%s/nice_flor_s", assets);
    snprintf(alutech, sizeof(alutech), "%s/alutech_at_4n", assets);
    subghz_environment_set_nice_flor_s_rainbow_table_file_name(env, nice);
    subghz_environment_set_alutech_at_4n_rainbow_table_file_name(env, alutech);
    if(keys)
        *keys = SubGhzKeyArray_size(
            *subghz_keystore_get_data(subghz_environment_get_keystore(env)));
    return env;
}

void sg_env_free(SubGhzEnvironment* env) {
    subghz_environment_free(env);
}

/* ---- one line per packet ---- */

void sg_key_hex(SubGhzProtocolDecoderBase* d, FuriString* out) {
    FlipperFormat* ff = flipper_format_string_alloc();
    SubGhzRadioPreset preset;
    uint8_t key[8];
    uint32_t bits = 0;
    int i, ndig;

    furi_string_reset(out);
    preset.name = furi_string_alloc();
    furi_string_set_str(preset.name, "AM650");
    preset.frequency = 433920000;
    preset.data = NULL;
    preset.data_size = 0;
    if(subghz_protocol_decoder_base_serialize(d, ff, &preset) == SubGhzProtocolStatusOk) {
        flipper_format_rewind(ff);
        if(!flipper_format_read_uint32(ff, "Bit", &bits, 1)) bits = 64;
        flipper_format_rewind(ff);
        if(flipper_format_read_hex(ff, "Key", key, 8)) {
            /* the digits the bit count needs (at least one), as twoflip
             * and stock's "Key:" lines compare them */
            ndig = bits >= 64 || bits == 0 ? 16 : (int)(bits + 3) / 4;
            for(i = 16 - ndig; i < 16; i++)
                furi_string_cat_printf(
                    out, "%X", (key[i / 2] >> ((i & 1) ? 0 : 4)) & 0xF);
        }
    }
    furi_string_free(preset.name);
    flipper_format_free(ff);
}

void sg_describe(SubGhzProtocolDecoderBase* d, FuriString* out) {
    FuriString* key = furi_string_alloc();
    FlipperFormat* ff = flipper_format_string_alloc();
    SubGhzRadioPreset preset;
    uint32_t bits = 0;

    preset.name = furi_string_alloc();
    furi_string_set_str(preset.name, "AM650");
    preset.frequency = 433920000;
    preset.data = NULL;
    preset.data_size = 0;
    if(subghz_protocol_decoder_base_serialize(d, ff, &preset) == SubGhzProtocolStatusOk) {
        flipper_format_rewind(ff);
        flipper_format_read_uint32(ff, "Bit", &bits, 1);
    }
    sg_key_hex(d, key);
    furi_string_printf(out, "%s %lubit", d->protocol->name, (unsigned long)bits);
    if(!furi_string_empty(key)) furi_string_cat_printf(out, " Key:%s", furi_string_get_cstr(key));
    furi_string_free(preset.name);
    flipper_format_free(ff);
    furi_string_free(key);
}

/* ---- encoders ---- */

SubGhzTransmitter* sg_transmitter(SubGhzEnvironment* env, FlipperFormat* ff, const char** why) {
    FuriString* proto = furi_string_alloc();
    SubGhzTransmitter* t = NULL;

    /* Unleashed keeps the "custom button" and override state in globals
     * the decoders set; a key file is sent as it is (stock's app resets them
     * when it loads one) */
    subghz_custom_btns_reset();
    subghz_block_generic_global_reset(NULL);
    flipper_format_rewind(ff);
    if(!flipper_format_read_string(ff, "Protocol", proto)) {
        *why = "no Protocol line";
    } else if((t = subghz_transmitter_alloc_init(env, furi_string_get_cstr(proto))) == NULL) {
        *why = "no encoder for this protocol";
    } else if(subghz_transmitter_deserialize(t, ff) != SubGhzProtocolStatusOk) {
        *why = "the key file does not fit the protocol";
        subghz_transmitter_free(t);
        t = NULL;
    }
    furi_string_free(proto);
    return t;
}
