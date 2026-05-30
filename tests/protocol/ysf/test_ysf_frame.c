// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
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
                 "%s", "BUMBLEBEETUNA");
    assert(!dsd_ysf_event_text_should_print(&state));

    DSD_SNPRINTF(history[0].Event_History_Items[0].event_string, sizeof(history[0].Event_History_Items[0].event_string),
                 "%s", "YSF data event");
    assert(dsd_ysf_event_text_should_print(&state));
}

int
main(void) {
    test_ysf_soft_viterbi_matches_reference_offset();
    test_ysf_soft_viterbi_rejects_invalid_args();
    test_ysf_event_text_print_guard();
    return 0;
}
