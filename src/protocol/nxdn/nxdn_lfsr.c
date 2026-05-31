// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN voice scrambler and AES IV LFSR helpers.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    if (BufferIn == NULL || BufferOut == NULL || state == NULL) {
        return;
    }

    int lfsr = (int)(state->payload_miN & 0x7FFFULL);

    for (int i = 0; i < 49; i++) {
        const int pn = lfsr & 0x1;
        const int bit = ((lfsr >> 1) ^ (lfsr >> 0)) & 1;
        lfsr = ((lfsr >> 1) | (bit << 14));
        BufferOut[i] = (char)(BufferIn[i] ^ pn);
    }

    state->payload_miN = (unsigned long long)(lfsr & 0x7FFF);
}

void
LFSR128n(dsd_state* state) {
    if (state == NULL) {
        return;
    }

    unsigned long long int lfsr = state->payload_miN;

    state->aes_iv[0] = (uint8_t)((lfsr >> 56) & 0xFFU);
    state->aes_iv[1] = (uint8_t)((lfsr >> 48) & 0xFFU);
    state->aes_iv[2] = (uint8_t)((lfsr >> 40) & 0xFFU);
    state->aes_iv[3] = (uint8_t)((lfsr >> 32) & 0xFFU);
    state->aes_iv[4] = (uint8_t)((lfsr >> 24) & 0xFFU);
    state->aes_iv[5] = (uint8_t)((lfsr >> 16) & 0xFFU);
    state->aes_iv[6] = (uint8_t)((lfsr >> 8) & 0xFFU);
    state->aes_iv[7] = (uint8_t)((lfsr >> 0) & 0xFFU);

    int x = 64;
    for (int cnt = 0; cnt < 64; cnt++) {
        const unsigned long long bit =
            ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1ULL;
        lfsr = (lfsr << 1) | bit;

        state->aes_iv[x / 8] = (uint8_t)((state->aes_iv[x / 8] << 1) + bit);
        x++;
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " IV(128): 0x");
    for (x = 0; x < 16; x++) {
        DSD_FPRINTF(stderr, "%02X", state->aes_iv[x]);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}
