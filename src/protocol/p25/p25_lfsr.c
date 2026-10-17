// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static unsigned long long
p25_lfsr_advance_64(unsigned long long lfsr) {
    for (int cnt = 0; cnt < 64; cnt++) {
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        unsigned long long bit =
            ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | bit;
    }
    return lfsr;
}

//LFSR code courtesy of https://github.com/mattames/LFSR/
void
LFSRP(dsd_state* state) {
    if (state == NULL) {
        return;
    }

    //rework for P2 TDMA support
    unsigned long long int lfsr = 0;
    if (state->currentslot == 0) {
        lfsr = state->payload_miP;
    }

    if (state->currentslot == 1) {
        lfsr = state->payload_miN;
    }

    lfsr = p25_lfsr_advance_64(lfsr);

    if (state->currentslot == 0) {
        state->payload_miP = lfsr;
    }

    if (state->currentslot == 1) {
        state->payload_miN = lfsr;
    }

    //print current ENC identifiers already known and new calculated MI
    DSD_FPRINTF(stderr, "%s", KYEL);
    if (state->currentslot == 0) {
        DSD_FPRINTF(stderr, "\n LDU2/ESS_B FEC ERR - ALG: 0x%02X KEY ID: 0x%04X LFSR MI: 0x%016llX",
                    state->payload_algid, state->payload_keyid, state->payload_miP);
    }
    if (state->currentslot == 1) {
        DSD_FPRINTF(stderr, "\n LDU2/ESS_B FEC ERR - ALG: 0x%02X KEY ID: 0x%04X LFSR MI: 0x%016llX",
                    state->payload_algidR, state->payload_keyidR, state->payload_miN);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25_lfsr128_store_seed_bytes(unsigned long long lfsr, uint8_t iv[16]) {
    iv[0] = (lfsr >> 56) & 0xFF;
    iv[1] = (lfsr >> 48) & 0xFF;
    iv[2] = (lfsr >> 40) & 0xFF;
    iv[3] = (lfsr >> 32) & 0xFF;
    iv[4] = (lfsr >> 24) & 0xFF;
    iv[5] = (lfsr >> 16) & 0xFF;
    iv[6] = (lfsr >> 8) & 0xFF;
    iv[7] = (lfsr >> 0) & 0xFF;
}

static void
p25_lfsr128_store_feedback_bytes(unsigned long long lfsr, uint8_t iv[16]) {
    int x = 64;
    //polynomial P(x) = 1 + X15 + X27 + X38 + X46 + X62 + X64
    for (int cnt = 0; cnt < 64; cnt++) {
        //63,61,45,37,27,14
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        unsigned long long bit =
            ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | bit;

        // Continue packing aes_iv
        iv[x / 8] = (uint8_t)((iv[x / 8] << 1) + bit);
        x++;
    }
}

static void
p25_lfsr128_print_slot(const dsd_state* state, int slot) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    if (state->dmrburstL != 27) {
        DSD_FPRINTF(stderr, "\n");
    }
    DSD_FPRINTF(stderr, "     ");
    if (slot == 0) {
        DSD_FPRINTF(stderr, " ALG ID: 0x%02X KEY ID: 0x%04X MI(128): 0x", state->payload_algid, state->payload_keyid);
        for (int x = 0; x < 16; x++) {
            DSD_FPRINTF(stderr, "%02X", state->aes_iv[x]);
        }
    } else {
        DSD_FPRINTF(stderr, " ALG ID: 0x%02X KEY ID: 0x%04X MI(128): 0x", state->payload_algidR, state->payload_keyidR);
        for (int x = 0; x < 16; x++) {
            DSD_FPRINTF(stderr, "%02X", state->aes_ivR[x]);
        }
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

void
p25_lfsr128_slot(dsd_state* state, int slot) {
    //generate a 128-bit IV from a 64-bit IV for AES blocks
    if (state == NULL || (slot != 0 && slot != 1)) {
        return;
    }

    unsigned long long lfsr = (slot == 0) ? state->payload_miP : state->payload_miN;
    uint8_t* iv = (slot == 0) ? state->aes_iv : state->aes_ivR;

    //start packing aes_iv
    p25_lfsr128_store_seed_bytes(lfsr, iv);
    p25_lfsr128_store_feedback_bytes(lfsr, iv);
    p25_lfsr128_print_slot(state, slot);
}

void
LFSR128(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    p25_lfsr128_slot(state, state->currentslot);
}
