// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/dmr_keystream.h>

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_const.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
csi_parse_hex_bytes(const char* hex, size_t nhex, uint8_t* out, size_t out_len) {
    if (!hex || !out || (out_len * 2U) != nhex) {
        return -1;
    }

    for (size_t i = 0; i < out_len; i++) {
        unsigned int v = 0;
        if (sscanf(&hex[i * 2U], "%2X", &v) != 1) {
            return -1;
        }
        out[i] = (uint8_t)v;
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
        fprintf(stderr, "DMR EE72 key parse failed: expected 18 hex characters, got %zu\n", nhex);
        return -1;
    }

    uint8_t key[9];
    memset(key, 0, sizeof(key));
    if (csi_parse_hex_bytes(hex, nhex, key, sizeof(key)) != 0) {
        fprintf(stderr, "DMR EE72 key parse failed: invalid hex string\n");
        return -1;
    }

    memcpy(state->csi_ee_key, key, sizeof(key));
    state->csi_ee = 1;
    fprintf(stderr, "DMR Connect Systems EE72 72-bit key with forced application\n");
    return 0;
}

void
csi72_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24]) {
    if (!state || !state->csi_ee) {
        return;
    }

    char interleaved[72];
    memset(interleaved, 0, sizeof(interleaved));

    const int *w = rW, *x = rX, *y = rY, *z = rZ;
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
    memset(ks_bytes, 0, sizeof(ks_bytes));
    memset(ks_bits, 0, sizeof(ks_bits));

    for (int i = 0; i < 9; i++) {
        ks_bytes[i] = state->csi_ee_key[8 - i];
    }
    unpack_byte_array_into_bit_array(ks_bytes, ks_bits, 9);

    for (int8_t i = 0; i < 72; i++) {
        interleaved[i] ^= (char)ks_bits[71 - i];
    }

    w = rW;
    x = rX;
    y = rY;
    z = rZ;
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
