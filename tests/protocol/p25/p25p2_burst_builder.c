// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Test support: P25 Phase 2 windows as a site sends them (issue #804). The layouts are the ones p25p2_frame.c gathers
   FACCH and SACCH bits by. */

#include "p25p2_burst_builder.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/ez.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <stddef.h>
#include <stdint.h>
#include "p25p2_frame_internal.h"

enum {
    BB_TIMESLOT_BITS = 360,
    BB_BURST_BITS = 320,
    BB_SUPERFRAME_BITS = 4320,
    BB_FACCH_PAYLOAD_BITS = 156,
    BB_FACCH_PARITY_BITS = 114,
    BB_SACCH_PAYLOAD_BITS = 180,
    BB_SACCH_PARITY_BITS = 132,
};

typedef enum { BB_KIND_NONE = 0, BB_KIND_FACCH, BB_KIND_SACCH, BB_KIND_LCCH } bb_kind;

static const int k_duid_offsets[8] = {0, 1, 74, 75, 244, 245, 318, 319};
static const uint8_t k_duid_codewords[16] = {
    0x00U, 0x17U, 0x2EU, 0x39U, 0x4BU, 0x5CU, 0x65U, 0x72U, 0x8DU, 0x9AU, 0xA3U, 0xB4U, 0xC6U, 0xD1U, 0xE8U, 0xFFU,
};

static const uint64_t k_s_isch = 0x575D57F7FFULL;
/* I-ISCH codewords (the (40,9) code of ez.cpp's table) for channel 0 and channel 1, ISCH location 0..2, no free or
   UF-count bits. */
static const uint64_t k_i_isch[2][3] = {
    {0x184229d461ULL, 0x1146a44f13ULL, 0x1a4a2e239eULL},
    {0x1442f705efULL, 0x1d467a9e9dULL, 0x164af0f210ULL},
};

static bb_kind
bb_kind_of(int duid) {
    switch (duid) {
        case 9:
        case 15: return BB_KIND_FACCH;
        case 3:
        case 12: return BB_KIND_SACCH;
        case 4:
        case 13: return BB_KIND_LCCH;
        default: return BB_KIND_NONE;
    }
}

static int
bb_duid_scrambled(int duid) {
    return duid == 0 || duid == 3 || duid == 4 || duid == 6 || duid == 9 || duid == 10;
}

static int
bb_payload_offset(bb_kind kind, int k) {
    if (kind == BB_KIND_FACCH) {
        if (k < 72) {
            return 2 + k;
        }
        return k < 134 ? 76 + (k - 72) : 180 + (k - 134);
    }
    return k < 72 ? 2 + k : 76 + (k - 72);
}

static int
bb_parity_offset(bb_kind kind, int k) {
    if (kind == BB_KIND_FACCH) {
        return k < 42 ? 202 + k : 246 + (k - 42);
    }
    return k < 60 ? 184 + k : 246 + (k - 60);
}

/* CRC-12 as p25_crc.c checks it: x^12 + x^11 + x^7 + x^4 + x^2 + x + 1, zero initial value, inverted. */
static uint16_t
bb_crc12(const int* bits, int len) {
    uint16_t reg = 0;
    for (int i = 0; i < len; i++) {
        const int feedback = ((reg >> 11) & 1) ^ (bits[i] & 1);
        reg = (uint16_t)((reg << 1) & 0xFFF);
        if (feedback) {
            reg ^= 0x897;
        }
    }
    return (uint16_t)(reg ^ 0xFFF);
}

static uint16_t
bb_crc16(const int* bits, int len) {
    uint8_t buf[BB_SACCH_PAYLOAD_BITS];
    for (int i = 0; i < len; i++) {
        buf[i] = (uint8_t)bits[i];
    }
    return ComputeCrcCCITT16b(buf, (unsigned int)len);
}

static void
bb_fill_crc(bb_kind kind, int* payload) {
    if (kind == BB_KIND_LCCH) {
        const uint16_t crc = bb_crc16(payload, 164);
        for (int i = 0; i < 16; i++) {
            payload[164 + i] = (crc >> (15 - i)) & 1;
        }
        return;
    }
    const int len = kind == BB_KIND_FACCH ? 144 : 168;
    const uint16_t crc = bb_crc12(payload, len);
    for (int i = 0; i < 12; i++) {
        payload[len + i] = (crc >> (11 - i)) & 1;
    }
}

static void
bb_write_acch(const p25p2_bb_slot* slot, bb_kind kind, uint8_t burst[BB_BURST_BITS]) {
    int payload[BB_SACCH_PAYLOAD_BITS] = {0};
    int parity[BB_SACCH_PARITY_BITS] = {0};
    const int n_payload = kind == BB_KIND_FACCH ? BB_FACCH_PAYLOAD_BITS : BB_SACCH_PAYLOAD_BITS;
    const int n_parity = kind == BB_KIND_FACCH ? BB_FACCH_PARITY_BITS : BB_SACCH_PARITY_BITS;
    for (int k = 0; k < n_payload; k++) {
        payload[k] = (slot->mac[k / 8] >> (7 - (k % 8))) & 1;
    }
    if (slot->fill_crc) {
        bb_fill_crc(kind, payload);
    }
    if (kind == BB_KIND_FACCH) {
        ez_rs28_facch_encode(payload, parity);
    } else {
        ez_rs28_sacch_encode(payload, parity);
    }
    for (int k = 0; k < n_payload; k++) {
        burst[bb_payload_offset(kind, k)] = (uint8_t)payload[k];
    }
    for (int k = 0; k < n_parity; k++) {
        burst[bb_parity_offset(kind, k)] = (uint8_t)parity[k];
    }
}

static uint64_t
bb_isch_after(int superframe_slot) {
    switch (superframe_slot % 4) {
        case 0: return k_i_isch[1][superframe_slot / 4];
        case 3: return k_i_isch[0][((superframe_slot + 1) / 4) % 3];
        default: return k_s_isch;
    }
}

void
p25p2_bb_window(uint64_t code, int t0, const p25p2_bb_slot slots[4], uint8_t out_bits[1440]) {
    uint8_t sequence[BB_SUPERFRAME_BITS];
    p25p2_generate_scramble_bits(code >> 24, (code >> 12) & 0xFFFU, code & 0xFFFU, sequence, BB_SUPERFRAME_BITS);
    DSD_MEMSET(out_bits, 0, (size_t)4U * BB_TIMESLOT_BITS);
    for (int ts = 0; ts < 4; ts++) {
        const int superframe_slot = (((t0 + ts) % 12) + 12) % 12;
        const int duid = slots[ts].duid & 0xF;
        uint8_t* burst = out_bits + (ptrdiff_t)ts * BB_TIMESLOT_BITS;
        const bb_kind kind = bb_kind_of(duid);
        if (kind != BB_KIND_NONE) {
            bb_write_acch(&slots[ts], kind, burst);
        }
        if (code != 0U && bb_duid_scrambled(duid)) {
            for (int b = 0; b < BB_BURST_BITS; b++) {
                burst[b] ^= sequence[(20 + (BB_TIMESLOT_BITS * superframe_slot) + b) % BB_SUPERFRAME_BITS];
            }
        }
        for (int i = 0; i < 8; i++) {
            burst[k_duid_offsets[i]] = (uint8_t)((k_duid_codewords[duid] >> (7 - i)) & 1U);
        }
        const uint64_t isch = bb_isch_after(superframe_slot);
        for (int i = 0; i < 40; i++) {
            burst[BB_BURST_BITS + i] = (uint8_t)((isch >> (39 - i)) & 1U);
        }
    }
}

int
p25p2_bb_symbol_offsets(int duid, int rs_index, int offsets[6]) {
    const bb_kind kind = bb_kind_of(duid);
    if (kind == BB_KIND_NONE) {
        return 0;
    }
    const int first_payload = kind == BB_KIND_FACCH ? 9 : 5;
    const int last_parity = kind == BB_KIND_FACCH ? 53 : 56;
    if (rs_index < first_payload || rs_index > last_parity) {
        return 0;
    }
    for (int i = 0; i < 6; i++) {
        offsets[i] = rs_index < 35 ? bb_payload_offset(kind, ((rs_index - first_payload) * 6) + i)
                                   : bb_parity_offset(kind, ((rs_index - 35) * 6) + i);
    }
    return 1;
}

void
p25p2_bb_flip_symbol(uint8_t bits[1440], int timeslot, int rs_index) {
    if (timeslot < 0 || timeslot > 3) {
        return;
    }
    uint8_t* burst = bits + (ptrdiff_t)timeslot * BB_TIMESLOT_BITS;
    uint8_t received = 0;
    for (int i = 0; i < 8; i++) {
        received = (uint8_t)((received << 1) | (burst[k_duid_offsets[i]] & 1U));
    }
    for (int duid = 0; duid < 16; duid++) {
        int offsets[6];
        if (k_duid_codewords[duid] == received && p25p2_bb_symbol_offsets(duid, rs_index, offsets)) {
            for (int i = 0; i < 6; i++) {
                burst[offsets[i]] ^= 1U;
            }
            return;
        }
    }
}
