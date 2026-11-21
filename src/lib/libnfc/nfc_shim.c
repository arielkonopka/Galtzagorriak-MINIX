/* nfc_shim.c - furi and the `Nfc` API for the vendored pollers (#404).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #404.  See nfc_shim.h.
 */
#include <furi.h>
#include <furi_hal_random.h>
#include <flipper_format/flipper_format.h>
#include <nfc/nfc.h>
#include <toolbox/bit_buffer.h>

#include "nfc_shim.h"

#define NFC_SHIM_HARD_STEPS 65536u

#undef malloc

/* ---- furi ---- */

size_t nfcshim_heap_bytes;

void* nfcshim_malloc(size_t size) {
    void* p = calloc(1, size ? size : 1);
    if(!p) nfcshim_crash("out of memory", __FILE__, __LINE__);
    nfcshim_heap_bytes += size;
    return p;
}

void nfcshim_crash(const char* msg, const char* file, int line) {
    fprintf(stderr, "libnfc: %s (%s:%d)\n", msg, file, line);
    abort();
}

void furi_delay_ms(uint32_t ms) { (void)ms; }
void furi_delay_us(uint32_t us) { (void)us; }
uint32_t furi_get_tick(void) { return 0; }

static uint32_t nfcshim_rng = 0x2545F491u;
uint32_t furi_hal_random_get(void) {
    nfcshim_rng = nfcshim_rng * 1103515245u + 12345u;
    return nfcshim_rng;
}
void furi_hal_random_fill_buf(uint8_t* buf, uint32_t len) {
    for(uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(furi_hal_random_get() >> 16);
}

/* FuriString: one heap buffer of NFCSHIM_STR_MAX + 1 bytes. */
FuriString* furi_string_alloc(void) {
    FuriString* s = nfcshim_malloc(sizeof *s);
    s->buf = nfcshim_malloc(NFCSHIM_STR_MAX + 1);
    return s;
}
void furi_string_free(FuriString* s) {
    if(!s) return;
    free(s->buf);
    free(s);
}
void furi_string_reset(FuriString* s) { s->len = 0; s->buf[0] = 0; }
const char* furi_string_get_cstr(const FuriString* s) { return s->buf; }
size_t furi_string_size(const FuriString* s) { return s->len; }
bool furi_string_empty(const FuriString* s) { return s->len == 0; }
char furi_string_get_char(const FuriString* s, size_t i) { return i < s->len ? s->buf[i] : 0; }
void furi_string_left(FuriString* s, size_t len) {
    if(len < s->len) { s->len = len; s->buf[len] = 0; }
}
void furi_string_cat_str(FuriString* s, const char* str) {
    size_t n = strlen(str);
    if(n > NFCSHIM_STR_MAX - s->len) n = NFCSHIM_STR_MAX - s->len;
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = 0;
}
void furi_string_set_str(FuriString* s, const char* str) {
    if(str == s->buf) return;
    furi_string_reset(s);
    furi_string_cat_str(s, str);
}
void furi_string_set_s(FuriString* s, const FuriString* src) { furi_string_set_str(s, src->buf); }
void furi_string_cat_s(FuriString* s, const FuriString* src) { furi_string_cat_str(s, src->buf); }
void furi_string_push_back(FuriString* s, char c) {
    char t[2] = {c, 0};
    furi_string_cat_str(s, t);
}
FuriString* furi_string_alloc_set_str(const char* str) {
    FuriString* s = furi_string_alloc();
    furi_string_cat_str(s, str);
    return s;
}
int furi_string_cmp_str(const FuriString* s, const char* str) { return strcmp(s->buf, str); }
bool furi_string_equal_str(const FuriString* s, const char* str) { return strcmp(s->buf, str) == 0; }
bool furi_string_equal_s(const FuriString* s, const FuriString* t) { return strcmp(s->buf, t->buf) == 0; }
int furi_string_cat_vprintf(FuriString* s, const char* fmt, va_list ap) {
    int n = vsnprintf(s->buf + s->len, NFCSHIM_STR_MAX + 1 - s->len, fmt, ap);
    if(n > 0) s->len += ((size_t)n > NFCSHIM_STR_MAX - s->len) ? NFCSHIM_STR_MAX - s->len : (size_t)n;
    return n;
}
int furi_string_vprintf(FuriString* s, const char* fmt, va_list ap) {
    furi_string_reset(s);
    return furi_string_cat_vprintf(s, fmt, ap);
}
int furi_string_printf(FuriString* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = furi_string_vprintf(s, fmt, ap);
    va_end(ap);
    return n;
}
int furi_string_cat_printf(FuriString* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = furi_string_cat_vprintf(s, fmt, ap);
    va_end(ap);
    return n;
}
FuriString* furi_string_alloc_printf(const char* fmt, ...) {
    FuriString* s = furi_string_alloc();
    va_list ap;
    va_start(ap, fmt);
    furi_string_vprintf(s, fmt, ap);
    va_end(ap);
    return s;
}

/* ---- FlipperFormat: not used (nfc_file.c does .nfc files); the read
 * side the ID-list parsers need is nfc_shim_ff.c (#412) ---- */
#define FF_NO(...) { (void)ff; (void)key; return false; }
bool flipper_format_read_hex(FlipperFormat* ff, const char* key, uint8_t* d, uint16_t n) FF_NO()
bool flipper_format_write_hex(FlipperFormat* ff, const char* key, const uint8_t* d, uint16_t n) FF_NO()
bool flipper_format_read_uint32(FlipperFormat* ff, const char* key, uint32_t* d, uint16_t n) FF_NO()
bool flipper_format_write_uint32(FlipperFormat* ff, const char* key, const uint32_t* d, uint16_t n) FF_NO()
bool flipper_format_write_string(FlipperFormat* ff, const char* key, FuriString* d) FF_NO()
bool flipper_format_write_string_cstr(FlipperFormat* ff, const char* key, const char* d) FF_NO()
bool flipper_format_write_comment_cstr(FlipperFormat* ff, const char* key) FF_NO()
bool flipper_format_write_comment(FlipperFormat* ff, FuriString* key) FF_NO()
bool flipper_format_key_exist(FlipperFormat* ff, const char* key) FF_NO()
bool flipper_format_get_value_count(FlipperFormat* ff, const char* key, uint32_t* d) FF_NO()
bool flipper_format_read_bool(FlipperFormat* ff, const char* key, bool* d, uint16_t n) FF_NO()
bool flipper_format_write_bool(FlipperFormat* ff, const char* key, const bool* d, uint16_t n) FF_NO()
bool flipper_format_read_hex_uint64(FlipperFormat* ff, const char* key, uint64_t* d, uint16_t n) FF_NO()
bool flipper_format_write_hex_uint64(FlipperFormat* ff, const char* key, const uint64_t* d, uint16_t n) FF_NO()

/* ---- Nfc over nfc_xfer ---- */

struct Nfc {
    const nfc_xfer_t* x;
    NfcTech tech;
    uint32_t fdt_poll_fc;
    unsigned max_steps;
    unsigned frames;
    unsigned answers; /* frames the card answered */
};

Nfc* nfc_alloc_xfer(const nfc_xfer_t* x) {
    furi_check(x);
    Nfc* n = nfcshim_malloc(sizeof *n);
    n->x = x;
    n->max_steps = 64;
    return n;
}

void nfc_shim_set_max_steps(Nfc* n, unsigned max_steps) { n->max_steps = max_steps ? max_steps : 1; }
unsigned nfc_shim_frames(const Nfc* n) { return n->frames; }

Nfc* nfc_alloc(void) {
    furi_crash("nfc_alloc: use nfc_alloc_xfer");
}

void nfc_free(Nfc* instance) { free(instance); }

static uint32_t nfc_shim_tech(NfcTech t) {
    switch(t) {
    case NfcTechIso14443b: return NFC_TECH_B;
    case NfcTechIso15693: return NFC_TECH_V;
    case NfcTechFelica: return NFC_TECH_F;
    default: return NFC_TECH_A;
    }
}

void nfc_config(Nfc* instance, NfcMode mode, NfcTech tech) {
    furi_check(instance);
    furi_check(mode == NfcModePoller); /* listening is the driver's (lane:local) */
    instance->tech = tech;
}

void nfc_set_fdt_poll_fc(Nfc* instance, uint32_t fdt) { instance->fdt_poll_fc = fdt; }
void nfc_set_fdt_listen_fc(Nfc* instance, uint32_t fdt) { (void)instance; (void)fdt; }
void nfc_set_mask_receive_time_fc(Nfc* instance, uint32_t t) { (void)instance; (void)t; }
void nfc_set_fdt_poll_poll_us(Nfc* instance, uint32_t t) { (void)instance; (void)t; }
void nfc_set_guard_time_us(Nfc* instance, uint32_t t) { (void)instance; (void)t; }

void nfc_start(Nfc* instance, NfcEventCallback callback, void* context) {
    furi_check(instance);
    furi_check(callback);
    uint32_t tech = nfc_shim_tech(instance->tech);
    (void)nfc_field(instance->x, 1, tech);
    /* max_steps counts the steps in a row in which the card answered no
     * frame (so a missing card ends the poll); NFC_SHIM_HARD_STEPS caps a
     * poll that keeps talking (a whole 4K read is a few hundred steps). */
    unsigned idle = 0, seen = instance->answers;
    for(unsigned step = 0; step < NFC_SHIM_HARD_STEPS && idle < instance->max_steps; step++) {
        NfcEvent event = {.type = NfcEventTypePollerReady};
        NfcCommand cmd = callback(event, context);
        if(cmd == NfcCommandStop) break;
        if(instance->answers != seen) {
            seen = instance->answers;
            idle = 0;
        } else
            idle++;
        if(cmd == NfcCommandReset) {
            (void)nfc_field(instance->x, 0, tech);
            (void)nfc_field(instance->x, 1, tech);
        }
    }
    (void)nfc_field(instance->x, 0, tech);
}

void nfc_stop(Nfc* instance) { furi_check(instance); }

static NfcError nfc_shim_err(int r) {
    switch(r) {
    case NFC_OK: return NfcErrorNone;
    case NFC_ERR_TIMEOUT: return NfcErrorTimeout;
    case NFC_ERR_CRC: return NfcErrorDataFormat;
    case NFC_ERR_PROTO: return NfcErrorIncompleteFrame;
    default: return NfcErrorInternal;
    }
}

static NfcError nfc_shim_trx(
    Nfc* n, const uint8_t* tx, size_t tx_bits, BitBuffer* rx, uint32_t fwt, uint32_t flags) {
    size_t room = bit_buffer_get_capacity_bytes(rx);
    uint8_t* buf = nfcshim_malloc(room + 1);
    size_t rx_bits = 0;
    n->frames++;
    int r = nfc_transceive(n->x, tx, tx_bits, buf, NFC_BITS(room), &rx_bits, fwt, flags);
    if(r == NFC_OK) {
        if(rx_bits > NFC_BITS(room)) r = NFC_ERR_OVERFLOW;
        else if(rx_bits == 0) r = NFC_ERR_TIMEOUT;
        else {
            bit_buffer_copy_bits(rx, buf, rx_bits);
            n->answers++;
        }
    }
    free(buf);
    return nfc_shim_err(r);
}

NfcError nfc_poller_trx(Nfc* instance, const BitBuffer* tx_buffer, BitBuffer* rx_buffer, uint32_t fwt) {
    furi_check(instance && tx_buffer && rx_buffer);
    return nfc_shim_trx(
        instance,
        bit_buffer_get_data(tx_buffer),
        bit_buffer_get_size(tx_buffer),
        rx_buffer,
        fwt,
        0);
}

NfcError nfc_iso14443a_poller_trx_short_frame(
    Nfc* instance, NfcIso14443aShortFrame frame, BitBuffer* rx_buffer, uint32_t fwt) {
    /* As stock's furi_hal_nfc does it: SensReq, the frame every 3A
     * activation sends, goes out as WUPA so a card the poller halted
     * (mf_classic halts after each sector) answers again; AllReqa is the
     * plain REQA.  A REQA here leaves a halted card silent and the poller
     * reports it lost. */
    uint8_t cmd = (frame == NfcIso14443aShortFrameAllReqa) ? 0x26 : 0x52;
    return nfc_shim_trx(instance, &cmd, 7, rx_buffer, fwt, NFC_XF_SHORT);
}

NfcError nfc_iso14443a_poller_trx_sdd_frame(
    Nfc* instance, const BitBuffer* tx_buffer, BitBuffer* rx_buffer, uint32_t fwt) {
    return nfc_poller_trx(instance, tx_buffer, rx_buffer, fwt);
}

NfcError nfc_iso14443a_poller_trx_custom_parity(
    Nfc* instance, const BitBuffer* tx_buffer, BitBuffer* rx_buffer, uint32_t fwt) {
    furi_check(instance && tx_buffer && rx_buffer);
    size_t n = bit_buffer_get_size_bytes(tx_buffer);
    size_t room = bit_buffer_get_capacity_bytes(rx_buffer);
    uint8_t* tx = nfcshim_malloc(n * 9 / 8 + 2);
    uint8_t* rx = nfcshim_malloc(room * 9 / 8 + 2);
    size_t bits = 0, rx_bits = 0;
    bit_buffer_write_bytes_with_parity(tx_buffer, tx, n * 9 / 8 + 2, &bits);
    instance->frames++;
    int r = nfc_transceive(instance->x, tx, bits, rx, room * 9, &rx_bits, fwt, NFC_XF_PARITY);
    if(r == NFC_OK) {
        if(rx_bits > room * 9) r = NFC_ERR_OVERFLOW;
        else if(rx_bits < 9) r = (rx_bits == 0) ? NFC_ERR_TIMEOUT : NFC_OK;
        if(r == NFC_OK && rx_bits) instance->answers++;
        if(r == NFC_OK && rx_bits >= 9) bit_buffer_copy_bytes_with_parity(rx_buffer, rx, rx_bits);
        else if(r == NFC_OK) bit_buffer_copy_bits(rx_buffer, rx, rx_bits); /* 4-bit ACK/NAK */
    }
    free(tx);
    free(rx);
    return nfc_shim_err(r);
}
