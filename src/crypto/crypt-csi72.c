// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static size_t
csi_collect_hex_digits(const char* input, char* out, size_t out_cap) {
    if (!input || !out || out_cap < 2U) {
        return 0;
    }

    size_t w = 0;
    const char* p = input;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    for (; *p; ++p) {
        if (isspace((unsigned char)*p)) {
            continue;
        }
        if (!isxdigit((unsigned char)*p)) {
            return 0;
        }
        if (w + 1 >= out_cap) {
            return 0;
        }
        out[w++] = (char)toupper((unsigned char)*p);
    }
    out[w] = '\0';
    return w;
}

static int
csi_hex_nibble(char c, int* value) {
    if (!value) {
        return -1;
    }

    if (c >= '0' && c <= '9') {
        *value = c - '0';
        return 0;
    }
    if (c >= 'a' && c <= 'f') {
        *value = 10 + (c - 'a');
        return 0;
    }
    if (c >= 'A' && c <= 'F') {
        *value = 10 + (c - 'A');
        return 0;
    }
    return -1;
}

static int
csi_parse_hex_bytes(const char* hex, size_t nhex, uint8_t* out, size_t out_len) {
    if (!hex || !out || (out_len * 2U) != nhex) {
        return -1;
    }

    for (size_t i = 0; i < out_len; i++) {
        int hi = 0;
        int lo = 0;
        char c0 = hex[i * 2U];
        char c1 = hex[i * 2U + 1U];
        if (csi_hex_nibble(c0, &hi) != 0 || csi_hex_nibble(c1, &lo) != 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int
connect_systems_ee72_key_creation(dsd_state* state, const char* input) {
    if (!state || !input) {
        return -1;
    }

    char hex[32];
    const size_t nhex = csi_collect_hex_digits(input, hex, sizeof(hex));
    if (nhex != 18U) {
        DSD_FPRINTF(stderr, "DMR EE72 key parse failed: expected 18 hex characters, got %zu\n", nhex);
        return -1;
    }

    uint8_t key[9];
    DSD_MEMSET(key, 0, sizeof(key));
    if (csi_parse_hex_bytes(hex, nhex, key, sizeof(key)) != 0) {
        DSD_FPRINTF(stderr, "DMR EE72 key parse failed: invalid hex string\n");
        return -1;
    }

    DSD_MEMCPY(state->csi_ee_key, key, sizeof(key));
    state->csi_ee = 1;
    DSD_FPRINTF(stderr, "DMR Connect Systems EE72 72-bit key with forced application\n");
    return 0;
}

void
csi72_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24]) {
    if (!state || !state->csi_ee) {
        return;
    }

    char interleaved[72];
    DSD_MEMSET(interleaved, 0, sizeof(interleaved));

    const int *w = dmr_ambe_interleave_w, *x = dmr_ambe_interleave_x, *y = dmr_ambe_interleave_y,
              *z = dmr_ambe_interleave_z;
    for (int8_t i = 0; i < 36; i++) {
        interleaved[(i * 2) + 0] = ambe_fr[*w][*x];
        interleaved[(i * 2) + 1] = ambe_fr[*y][*z];
        w++;
        x++;
        y++;
        z++;
    }

    uint8_t ks_bytes[9];
    uint8_t ks_bits[72];
    DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));
    DSD_MEMSET(ks_bits, 0, sizeof(ks_bits));

    for (int i = 0; i < 9; i++) {
        ks_bytes[i] = state->csi_ee_key[8 - i];
    }
    unpack_byte_array_into_bit_array(ks_bytes, ks_bits, 9);

    for (int8_t i = 0; i < 72; i++) {
        interleaved[i] ^= (char)ks_bits[71 - i];
    }

    w = dmr_ambe_interleave_w;
    x = dmr_ambe_interleave_x;
    y = dmr_ambe_interleave_y;
    z = dmr_ambe_interleave_z;
    int k = 0;
    for (int8_t i = 0; i < 36; i++) {
        ambe_fr[*w][*x] = interleaved[k++];
        ambe_fr[*y][*z] = interleaved[k++];
        w++;
        x++;
        y++;
        z++;
    }
}
