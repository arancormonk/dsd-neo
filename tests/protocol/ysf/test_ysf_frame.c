// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "ysf_frame.h"

static int
parity32(uint32_t value) {
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value &= 0xFU;
    return (0x6996U >> value) & 1U;
}

static void
encode_k5_bits_to_dibits(const uint8_t* bits, size_t bit_count, uint8_t* dibits) {
    uint32_t reg = 0U;

    for (size_t i = 0; i < bit_count; i++) {
        reg = (reg << 1U) | (uint32_t)(bits[i] & 1U);
        uint8_t b0 = (uint8_t)parity32(reg & 0x19U);
        uint8_t b1 = (uint8_t)parity32(reg & 0x17U);
        dibits[i] = (uint8_t)((b0 << 1U) | b1);
    }
}

static int
bits_equal(const uint8_t* lhs, const uint8_t* rhs, size_t bit_count) {
    for (size_t i = 0; i < bit_count; i++) {
        if ((lhs[i] & 1U) != (rhs[i] & 1U)) {
            return 0;
        }
    }
    return 1;
}

static void
pack_bits_to_bytes(const uint8_t* input, uint8_t* output, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = 0U;
        for (size_t bit = 0; bit < 8U; bit++) {
            byte = (uint8_t)((byte << 1U) | (input[(i * 8U) + bit] & 1U));
        }
        output[i] = byte;
    }
}

static uint8_t
expected_ysf_pn95_bit(size_t bit_index) {
    uint16_t lfsr = 0x1C9U;
    bit_index %= 512U;

    for (size_t i = 0; i < bit_index; i++) {
        uint16_t feedback = (uint16_t)(((lfsr >> 4U) ^ lfsr) & 1U);
        lfsr = (uint16_t)((lfsr >> 1U) | (feedback << 8U));
    }

    return (uint8_t)(lfsr & 1U);
}

static void
test_ysf_vd2_interleave_matrix(void) {
    assert(dsd_ysf_vd2_interleave_index(0U) == 0U);
    assert(dsd_ysf_vd2_interleave_index(1U) == 26U);
    assert(dsd_ysf_vd2_interleave_index(2U) == 52U);
    assert(dsd_ysf_vd2_interleave_index(3U) == 78U);
    assert(dsd_ysf_vd2_interleave_index(4U) == 1U);
    assert(dsd_ysf_vd2_interleave_index(99U) == 102U);
    assert(dsd_ysf_vd2_interleave_index(100U) == 25U);
    assert(dsd_ysf_vd2_interleave_index(103U) == 103U);
}

static void
test_ysf_pn95_seed_bit_order_and_reset(void) {
    static const uint8_t expected_prefix[16] = {1U, 0U, 0U, 1U, 0U, 0U, 1U, 1U, 1U, 1U, 0U, 1U, 0U, 1U, 1U, 1U};
    uint8_t dch2_bits[80];
    uint8_t dch_bits[160];
    uint8_t roundtrip_bits[160];

    for (size_t i = 0; i < sizeof(expected_prefix); i++) {
        assert(dsd_ysf_pn95_bit(i) == expected_prefix[i]);
    }

    for (size_t i = 0; i < 512U; i++) {
        assert(dsd_ysf_pn95_bit(i) == expected_ysf_pn95_bit(i));
    }
    assert(dsd_ysf_pn95_bit(512U) == expected_prefix[0]);

    for (size_t i = 0; i < sizeof(dch2_bits); i++) {
        dch2_bits[i] = (uint8_t)((i + 1U) & 1U);
    }
    for (size_t i = 0; i < sizeof(dch_bits); i++) {
        dch_bits[i] = (uint8_t)(((i * 3U) + 1U) & 1U);
        roundtrip_bits[i] = dch_bits[i];
    }

    dsd_ysf_dewhiten_bits(dch2_bits, sizeof(dch2_bits));
    dsd_ysf_dewhiten_bits(dch_bits, sizeof(dch_bits));
    dsd_ysf_dewhiten_bits(roundtrip_bits, sizeof(roundtrip_bits));
    dsd_ysf_dewhiten_bits(roundtrip_bits, sizeof(roundtrip_bits));

    for (size_t i = 0; i < sizeof(dch2_bits); i++) {
        assert(dch2_bits[i] == (uint8_t)(((i + 1U) & 1U) ^ expected_ysf_pn95_bit(i)));
    }
    for (size_t i = 0; i < sizeof(dch_bits); i++) {
        assert(dch_bits[i] == (uint8_t)((((i * 3U) + 1U) & 1U) ^ expected_ysf_pn95_bit(i)));
        assert(roundtrip_bits[i] == (uint8_t)(((i * 3U) + 1U) & 1U));
    }
}

static void
test_ysf_soft_viterbi_matches_reference_offset(void) {
    enum {
        decoded_bit_count = 100,
        payload_bit_count = 96,
        decoded_byte_count = 13,
    };

    uint8_t decoded[decoded_bit_count];
    uint8_t dibits[decoded_bit_count];
    uint8_t out_bits[payload_bit_count + 8];
    uint8_t out_bytes[payload_bit_count / 8];
    uint8_t expected_bytes[12];

    for (size_t i = 0; i < decoded_bit_count; i++) {
        decoded[i] = (uint8_t)(((i * 5U) + 1U) & 1U);
    }
    decoded[96] = 0U;
    decoded[97] = 0U;
    decoded[98] = 0U;
    decoded[99] = 0U;

    encode_k5_bits_to_dibits(decoded, decoded_bit_count, dibits);

    DSD_MEMSET(out_bits, 0xA5, sizeof(out_bits));
    uint32_t error = dsd_ysf_soft_viterbi_decode(dibits, decoded_bit_count, decoded_byte_count, 8U, payload_bit_count,
                                                 out_bits, out_bytes);
    assert(error != UINT32_MAX);
    assert(bits_equal(out_bits, decoded, payload_bit_count));
    for (size_t i = payload_bit_count; i < sizeof(out_bits); i++) {
        assert(out_bits[i] == 0xA5);
    }

    DSD_MEMSET(expected_bytes, 0, sizeof(expected_bytes));
    pack_bits_to_bytes(decoded, expected_bytes, 12);
    assert(memcmp(out_bytes, expected_bytes, sizeof(expected_bytes)) == 0);

    dibits[25] ^= 1U;
    DSD_MEMSET(out_bits, 0xA5, sizeof(out_bits));
    DSD_MEMSET(out_bytes, 0, sizeof(out_bytes));
    error = dsd_ysf_soft_viterbi_decode(dibits, decoded_bit_count, decoded_byte_count, 8U, payload_bit_count, out_bits,
                                        out_bytes);
    assert(error != UINT32_MAX);
    assert(bits_equal(out_bits, decoded, payload_bit_count));
    for (size_t i = payload_bit_count; i < sizeof(out_bits); i++) {
        assert(out_bits[i] == 0xA5);
    }
}

static void
test_ysf_soft_viterbi_full_rate_reference_offset(void) {
    enum {
        decoded_bit_count = 180,
        payload_bit_count = 176,
        decoded_byte_count = 23,
    };

    uint8_t decoded[decoded_bit_count];
    uint8_t dibits[decoded_bit_count];
    uint8_t out_bits[payload_bit_count + 8];
    uint8_t out_bytes[payload_bit_count / 8];
    uint8_t expected_bytes[22];

    for (size_t i = 0; i < decoded_bit_count; i++) {
        decoded[i] = (uint8_t)(((i * 7U) + 3U) & 1U);
    }
    decoded[176] = 0U;
    decoded[177] = 0U;
    decoded[178] = 0U;
    decoded[179] = 0U;

    encode_k5_bits_to_dibits(decoded, decoded_bit_count, dibits);

    DSD_MEMSET(out_bits, 0xA5, sizeof(out_bits));
    uint32_t error = dsd_ysf_soft_viterbi_decode(dibits, decoded_bit_count, decoded_byte_count, 8U, payload_bit_count,
                                                 out_bits, out_bytes);
    assert(error != UINT32_MAX);
    assert(bits_equal(out_bits, decoded, payload_bit_count));
    for (size_t i = payload_bit_count; i < sizeof(out_bits); i++) {
        assert(out_bits[i] == 0xA5);
    }

    DSD_MEMSET(expected_bytes, 0, sizeof(expected_bytes));
    pack_bits_to_bytes(decoded, expected_bytes, 22);
    assert(memcmp(out_bytes, expected_bytes, sizeof(expected_bytes)) == 0);
}

static void
test_ysf_soft_viterbi_rejects_invalid_args(void) {
    uint8_t dibits[100];
    uint8_t out_bits[96];
    uint8_t out_bytes[12];

    DSD_MEMSET(dibits, 0, sizeof(dibits));
    DSD_MEMSET(out_bits, 0, sizeof(out_bits));
    DSD_MEMSET(out_bytes, 0, sizeof(out_bytes));

    assert(dsd_ysf_soft_viterbi_decode(NULL, 100U, 13U, 8U, 96U, out_bits, out_bytes) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 100U, 13U, 8U, 96U, NULL, out_bytes) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 100U, 13U, 8U, 96U, out_bits, NULL) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 245U, 13U, 8U, 96U, out_bits, out_bytes) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 100U, 0U, 8U, 96U, out_bits, out_bytes) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 100U, 64U, 8U, 96U, out_bits, out_bytes) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 100U, 13U, 8U, 0U, out_bits, out_bytes) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 100U, 13U, 8U, 7U, out_bits, out_bytes) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 100U, 13U, 512U, 96U, out_bits, out_bytes) == UINT32_MAX);
    assert(dsd_ysf_soft_viterbi_decode(dibits, 100U, 13U, 105U, 8U, out_bits, out_bytes) == UINT32_MAX);
}

static void
test_ysf_event_text_print_guard(void) {
    static dsd_state state;
    static Event_History_I history[2];

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(history, 0, sizeof(history));

    assert(!dsd_ysf_event_text_should_print(NULL));
    assert(!dsd_ysf_event_text_should_print(&state));

    state.event_history_s = history;
    assert(!dsd_ysf_event_text_should_print(&state));

    DSD_SNPRINTF(history[0].Event_History_Items[0].text_message, sizeof(history[0].Event_History_Items[0].text_message),
                 "%s", "hello ysf");
    DSD_SNPRINTF(history[0].Event_History_Items[0].event_string, sizeof(history[0].Event_History_Items[0].event_string),
                 "%s", "YSF data event");
    assert(dsd_ysf_event_text_should_print(&state));
}

static void
test_ysf_sync_constants_and_synctypes(void) {
    _Static_assert(sizeof(FUSION_SYNC) == 21U, "FUSION_SYNC length");
    _Static_assert(sizeof(INV_FUSION_SYNC) == 21U, "INV_FUSION_SYNC length");
    assert(strcmp(FUSION_SYNC, INV_FUSION_SYNC) != 0);
    _Static_assert(DSD_SYNC_IS_YSF(DSD_SYNC_YSF_POS), "YSF positive synctype");
    _Static_assert(DSD_SYNC_IS_YSF(DSD_SYNC_YSF_NEG), "YSF negative synctype");
    _Static_assert(!DSD_SYNC_IS_YSF(DSD_SYNC_NXDN_POS), "NXDN is not YSF");
    _Static_assert(!DSD_SYNC_IS_YSF(DSD_SYNC_P25P2_POS), "P25P2 is not YSF");
}

static void
test_ysf_full_rate_imbe_unpack_consumes_144_bits(void) {
    uint8_t imbe_raw[144];
    uint8_t imbe_vch[144];
    char imbe_fr[8][23];
    uint8_t seen[144];
    int k = 0;

    for (int i = 0; i < 144; i++) {
        imbe_raw[i] = (uint8_t)i;
    }
    DSD_MEMSET(imbe_vch, 0, sizeof(imbe_vch));
    DSD_MEMSET(imbe_fr, 0x55, sizeof(imbe_fr));
    DSD_MEMSET(seen, 0, sizeof(seen));

    dsd_ysf_unpack_full_rate_imbe(imbe_raw, imbe_vch, imbe_fr);

    for (int i = 0; i < 144; i++) {
        assert(imbe_vch[i] < 144U);
        assert(seen[imbe_vch[i]] == 0U);
        seen[imbe_vch[i]] = 1U;
    }

    for (int n = 0; n < 4; n++) {
        for (int m = 22; m >= 0; m--) {
            assert((uint8_t)imbe_fr[n][m] == imbe_vch[k]);
            k++;
        }
    }
    for (int n = 4; n < 7; n++) {
        for (int m = 14; m >= 0; m--) {
            assert((uint8_t)imbe_fr[n][m] == imbe_vch[k]);
            k++;
        }
    }
    for (int m = 6; m >= 0; m--) {
        assert((uint8_t)imbe_fr[7][m] == imbe_vch[k]);
        k++;
    }

    assert(k == 144);
    assert((uint8_t)imbe_fr[7][7] == 0x55U);
}

int
main(void) {
    test_ysf_vd2_interleave_matrix();
    test_ysf_pn95_seed_bit_order_and_reset();
    test_ysf_soft_viterbi_matches_reference_offset();
    test_ysf_soft_viterbi_full_rate_reference_offset();
    test_ysf_soft_viterbi_rejects_invalid_args();
    test_ysf_event_text_print_guard();
    test_ysf_sync_constants_and_synctypes();
    test_ysf_full_rate_imbe_unpack_consumes_144_bits();
    return 0;
}
