/* cardmodel.h - software cards for the vendored-poller tests (#404).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #404.
 *
 * The models answer at the nfc_xfer level with the frames a real card puts
 * on the air: the vendored pollers add and check CRCs themselves (the shim
 * passes flags 0), so the models check the reader's CRC and append their
 * own.  The CRCs are written here again from ISO/IEC 14443-3 and 15693-3,
 * not taken from the vendored helpers, so a wrong helper cannot pass its
 * own test.
 */
#ifndef CARDMODEL_H
#define CARDMODEL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* CRC_A (ISO 14443-3 Annex B): x^16+x^12+x^5+1 reflected, init 0x6363.
 * CRC_B and ISO 15693: the same polynomial, init 0xFFFF, result inverted. */
static uint16_t cm_crc(const uint8_t* p, size_t n, uint16_t crc, int invert) {
    for(size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for(int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return invert ? (uint16_t)~crc : crc;
}
static int cm_crc_ok(const uint8_t* p, size_t n, uint16_t init, int inv) {
    if(n < 3) return 0;
    uint16_t c = cm_crc(p, n - 2, init, inv);
    return p[n - 2] == (c & 0xff) && p[n - 1] == (c >> 8);
}
static size_t cm_crc_add(uint8_t* p, size_t n, uint16_t init, int inv) {
    uint16_t c = cm_crc(p, n, init, inv);
    p[n] = c & 0xff;
    p[n + 1] = c >> 8;
    return n + 2;
}
#define CM_CRC_A 0x6363, 0
#define CM_CRC_B 0xFFFF, 1

/* ISO 14443-3A activation: REQA/WUPA, anticollision and SELECT (4- or
 * 7-byte UID) as the card answers them; HLTA halts it until WUPA.
 * Returns 1 and fills rx if the frame was a 3A one, 0 if it is not. */
typedef struct {
    uint8_t uid[10], uid_len, atqa[2], sak;
    int halted, active;
} cm_3a_t;

static int cm_3a(cm_3a_t* k, const uint8_t* tx, size_t bits, uint8_t* rx, size_t* rxb, int* err) {
    *err = 0;
    if(bits == 7 && (tx[0] == 0x26 || tx[0] == 0x52)) {
        if(k->halted && tx[0] == 0x26) { *err = -1; return 1; } /* silent */
        k->halted = 0;
        k->active = 0;
        rx[0] = k->atqa[0];
        rx[1] = k->atqa[1];
        *rxb = 16;
        return 1;
    }
    if(bits % 8) return 0;
    size_t n = bits / 8;
    if(n == 2 && tx[1] == 0x20 && (tx[0] == 0x93 || tx[0] == 0x95)) {
        int lvl = tx[0] == 0x93 ? 0 : 1;
        if(k->uid_len == 4 && lvl == 0) memcpy(rx, k->uid, 4);
        else if(k->uid_len == 7 && lvl == 0) { rx[0] = 0x88; memcpy(rx + 1, k->uid, 3); }
        else if(k->uid_len == 7 && lvl == 1) memcpy(rx, k->uid + 3, 4);
        else { *err = -1; return 1; }
        rx[4] = rx[0] ^ rx[1] ^ rx[2] ^ rx[3];
        *rxb = 40;
        return 1;
    }
    if(n == 9 && tx[1] == 0x70 && (tx[0] == 0x93 || tx[0] == 0x95)) {
        if(!cm_crc_ok(tx, n, CM_CRC_A)) { *err = -1; return 1; }
        int last = (k->uid_len == 4) || tx[0] == 0x95;
        rx[0] = last ? k->sak : 0x04;
        if(last) k->active = 1;
        *rxb = 8 * cm_crc_add(rx, 1, CM_CRC_A);
        return 1;
    }
    if(n == 4 && tx[0] == 0x50 && tx[1] == 0x00) { /* HLTA: no answer */
        k->halted = 1;
        k->active = 0;
        *err = -1;
        return 1;
    }
    return 0;
}

/* ISO 14443-4 (ISO-DEP) card on top of 3A or 3B: I-blocks with the
 * reader's block number, an S(WTX) before the first answer if wtx_left,
 * S(DESELECT) answered.  The application (an APDU or DESFire native
 * command) is the `app` callback: command in, response out (length). */
typedef size_t (*cm_app_fn)(void* app_ctx, const uint8_t* cmd, size_t n, uint8_t* resp);

typedef struct {
    int wtx_left;        /* S(WTX) requests still to send */
    int wtx_pending;     /* a WTX is out, the reader's answer is due */
    uint8_t pcb;         /* the pending I-block's PCB */
    uint8_t cmd[256];
    size_t cmd_len;
    cm_app_fn app;
    void* app_ctx;
    unsigned iblocks, wtx_seen;
    /* Chaining (#451), off when 0: answers longer than `chunk` bytes go
     * out as a chain of I-blocks, one per reader R(ACK); a reader's
     * chained command is gathered (R(ACK) per block). */
    size_t chunk;
    int chain_in;
    uint8_t out[300];
    size_t out_len, out_off;
    unsigned chains_in, chains_out;
    uint8_t last_bn;     /* the card's block number (its last I-block) */
    unsigned naks;
} cm_isodep_t;

/* The next piece of a chained answer, PCB block number bn. */
static void cm_isodep_piece(cm_isodep_t* k, uint8_t bn, uint8_t* rx, size_t* rx_n) {
    size_t c = k->out_len - k->out_off;
    int more = k->chunk && c > k->chunk;
    if(more) c = k->chunk;
    rx[0] = 0x02 | bn | (more ? 0x10 : 0);
    k->last_bn = bn;
    memcpy(rx + 1, k->out + k->out_off, c);
    k->out_off += c;
    *rx_n = 1 + c;
    if(more) k->chains_out++;
}

/* The application's answer to k->cmd, chained if long. */
static void cm_isodep_answer(cm_isodep_t* k, uint8_t* rx, size_t* rx_n) {
    k->last_bn = k->pcb & 1;
    if(!k->chunk) {
        rx[0] = k->pcb;
        *rx_n = 1 + k->app(k->app_ctx, k->cmd, k->cmd_len, rx + 1);
        return;
    }
    k->out_len = k->app(k->app_ctx, k->cmd, k->cmd_len, k->out);
    k->out_off = 0;
    cm_isodep_piece(k, k->pcb & 1, rx, rx_n);
}

/* tx without CRC (checked by the caller); answer without CRC. */
static int cm_isodep(cm_isodep_t* k, const uint8_t* tx, size_t n, uint8_t* rx, size_t* rx_n) {
    uint8_t pcb = tx[0];
    if((pcb & 0xC0) == 0xC0) {             /* S-block */
        if((pcb & 0x30) == 0x30) {         /* WTX response */
            if(!k->wtx_pending || n != 2) return -1;
            k->wtx_pending = 0;
            k->wtx_seen++;
            cm_isodep_answer(k, rx, rx_n);
            return 0;
        }
        rx[0] = pcb;                       /* DESELECT */
        *rx_n = 1;
        return 0;
    }
    if((pcb & 0xF6) == 0xB2 && n == 1 && (pcb & 1) != k->last_bn) {
        k->naks++;                         /* R(NAK): the reader's block */
        rx[0] = 0xA2 | k->last_bn;         /* never came: ACK (rule 13) */
        *rx_n = 1;
        return 0;
    }
    if((pcb & 0xF6) == 0xA2 && k->chunk && k->out_off < k->out_len) {
        cm_isodep_piece(k, pcb & 1, rx, rx_n); /* R(ACK): the next piece */
        return 0;
    }
    if((pcb & 0xE2) == 0x02) {             /* I-block */
        size_t hdr = 1 + ((pcb & 0x08) ? 1 : 0) + ((pcb & 0x04) ? 1 : 0);
        size_t base = k->chain_in ? k->cmd_len : 0;
        if(n < hdr || base + n - hdr > sizeof k->cmd) return -1;
        k->iblocks++;
        memcpy(k->cmd + base, tx + hdr, n - hdr);
        k->cmd_len = base + n - hdr;
        if(pcb & 0x10) {                   /* chained: ACK, wait for more */
            k->chain_in = 1;
            k->chains_in++;
            rx[0] = 0xA2 | (pcb & 1);
            *rx_n = 1;
            return 0;
        }
        k->chain_in = 0;
        k->pcb = pcb & 0xE3;               /* no CID/NAD in the answer */
        if(k->wtx_left > 0) {
            k->wtx_left--;
            k->wtx_pending = 1;
            rx[0] = 0xF2;
            rx[1] = 0x01;
            *rx_n = 2;
            return 0;
        }
        cm_isodep_answer(k, rx, rx_n);
        return 0;
    }
    return -1;
}

/* A whole ISO 14443-4A card: 3A activation, RATS -> ATS, ISO-DEP. */
typedef struct {
    cm_3a_t a;
    uint8_t ats[20];     /* TL first */
    int rats_done;
    unsigned rats_seen;
    cm_isodep_t dep;
} cm_4a_t;

static int cm_4a_xfer(void* ctx, const uint8_t* tx, size_t bits, uint8_t* rx, size_t room,
                      size_t* rxb, uint32_t fwt, uint32_t flags) {
    cm_4a_t* k = ctx;
    int err;
    (void)fwt;
    (void)flags;
    if(cm_3a(&k->a, tx, bits, rx, rxb, &err)) {
        if(bits == 7) k->rats_done = 0;
        return err ? -1 : 0;
    }
    size_t n = bits / 8;
    if(!k->a.active || bits % 8 || !cm_crc_ok(tx, n, CM_CRC_A)) return -1;
    n -= 2;
    size_t out;
    if(!k->rats_done && n == 2 && tx[0] == 0xE0) {
        k->rats_done = 1;
        k->dep.last_bn = 1;          /* the PICC starts at 1 */
        k->dep.chain_in = 0;
        k->rats_seen++;
        memcpy(rx, k->ats, k->ats[0]);
        out = k->ats[0];
    } else if(k->rats_done) {
        if(cm_isodep(&k->dep, tx, n, rx, &out)) return -1;
    } else
        return -1;
    if(8 * (out + 2) > room) return -5;
    *rxb = 8 * cm_crc_add(rx, out, CM_CRC_A);
    return 0;
}

/* A whole ISO 14443-4B card: REQB -> ATQB, ATTRIB -> answer, ISO-DEP.
 * CRC_B on every frame. */
typedef struct {
    uint8_t pupi[4], app_data[4], proto_info[3];
    int ready, halted;
    unsigned reqb_seen, attrib_seen;
    cm_isodep_t dep;
} cm_4b_t;

static int cm_4b_xfer(void* ctx, const uint8_t* tx, size_t bits, uint8_t* rx, size_t room,
                      size_t* rxb, uint32_t fwt, uint32_t flags) {
    cm_4b_t* k = ctx;
    (void)fwt;
    (void)flags;
    size_t n = bits / 8, out;
    if(bits % 8 || !cm_crc_ok(tx, n, CM_CRC_B)) return -1;
    n -= 2;
    if(n == 3 && tx[0] == 0x05) { /* REQB/WUPB */
        if(k->halted && !(tx[2] & 0x08)) return -1;
        k->reqb_seen++;
        k->halted = 0;
        k->ready = 0;
        rx[0] = 0x50;
        memcpy(rx + 1, k->pupi, 4);
        memcpy(rx + 5, k->app_data, 4);
        memcpy(rx + 9, k->proto_info, 3);
        out = 12;
    } else if(n == 9 && tx[0] == 0x1D && !memcmp(tx + 1, k->pupi, 4)) {
        k->attrib_seen++;
        k->ready = 1;
        rx[0] = tx[8] & 0x0F; /* MBLI 0, the CID given */
        out = 1;
    } else if(n == 5 && tx[0] == 0x50 && !memcmp(tx + 1, k->pupi, 4)) {
        k->halted = 1;
        k->ready = 0;
        rx[0] = 0x00;
        out = 1;
    } else if(k->ready) {
        if(cm_isodep(&k->dep, tx, n, rx, &out)) return -1;
    } else
        return -1;
    if(8 * (out + 2) > room) return -5;
    *rxb = 8 * cm_crc_add(rx, out, CM_CRC_B);
    return 0;
}

#endif
