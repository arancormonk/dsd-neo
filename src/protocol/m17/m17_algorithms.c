// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Small deterministic M17 helpers kept separate from the main encoder/decoder
 * so spec vectors can exercise them directly.
 */

#include "m17_algorithms.h"

#include <stddef.h>

#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/m17/m17_tables.h>

uint16_t
m17_crc16(const uint8_t* in, uint16_t len) {
    uint32_t crc = 0xFFFFU;
    const uint16_t poly = 0x5935U;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint32_t)in[i] << 8U;
        for (uint8_t j = 0; j < 8U; j++) {
            crc <<= 1U;
            if ((crc & 0x10000U) != 0U) {
                crc = (crc ^ poly) & 0xFFFFU;
            }
        }
    }

    return (uint16_t)(crc & 0xFFFFU);
}

static size_t
m17_callsign_len_max9(const char* csd) {
    size_t len = 0U;
    if (csd == NULL) {
        return 0U;
    }
    while (len < 9U && csd[len] != '\0') {
        len++;
    }
    return len;
}

static char
m17_upper_ascii(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static uint8_t
m17_base40_value(char c) {
    const char uc = m17_upper_ascii(c);
    for (uint8_t i = 0U; i < 40U; i++) {
        if (uc == m17_base40_alphabet[i]) {
            return i;
        }
    }
    return 0U;
}

unsigned long long int
m17_encode_b40_callsign(unsigned long long int value, const char* csd) {
    if (value >= 0xEE6B28000000ULL || csd == NULL) {
        return value;
    }

    const size_t len = m17_callsign_len_max9(csd);
    for (size_t n = len; n > 0U; n--) {
        value = (value * 40ULL) + (unsigned long long int)m17_base40_value(csd[n - 1U]);
    }
    return value;
}

uint16_t
m17_prbs9_next_bit(uint16_t* lfsr) {
    if (lfsr == NULL) {
        return 0U;
    }

    uint16_t state = (uint16_t)(*lfsr & M17_PRBS9_MASK);
    if (state == 0U) {
        state = 1U;
    }
    const uint16_t bit = (uint16_t)(((state >> 8U) ^ (state >> 4U)) & 1U);
    *lfsr = (uint16_t)(((state << 1U) | bit) & M17_PRBS9_MASK);
    return bit;
}

void
m17_prbs9_fill_bits(uint16_t* lfsr, uint8_t* out_bits, uint16_t bit_count) {
    if (lfsr == NULL || out_bits == NULL) {
        return;
    }

    for (uint16_t i = 0U; i < bit_count; i++) {
        out_bits[i] = (uint8_t)m17_prbs9_next_bit(lfsr);
    }
}

void
m17_prbs9_rx_init(m17_prbs9_rx_state* rx, uint16_t initial_state) {
    if (rx == NULL) {
        return;
    }
    rx->lfsr = (uint16_t)(initial_state & M17_PRBS9_MASK);
    if (rx->lfsr == 0U) {
        rx->lfsr = 1U;
    }
    rx->lock_count = 0U;
    rx->window_bits = 0U;
    rx->window_errors = 0U;
    rx->total_bits = 0U;
    rx->total_errors = 0U;
    rx->resync_count = 0U;
    rx->locked = 0U;
}

int
m17_prbs9_rx_push_bit(m17_prbs9_rx_state* rx, uint8_t bit) {
    if (rx == NULL) {
        return 0;
    }

    bit &= 1U;
    if (rx->lfsr == 0U) {
        rx->lfsr = 1U;
    }

    if (rx->locked != 0U) {
        const uint16_t expected = m17_prbs9_next_bit(&rx->lfsr);
        const int error = (expected != bit);
        rx->total_bits++;
        rx->window_bits++;
        if (error) {
            rx->total_errors++;
            rx->window_errors++;
        }

        if (rx->window_bits >= M17_PRBS9_RESYNC_WINDOW_BITS) {
            if (rx->window_errors > M17_PRBS9_RESYNC_ERROR_THRESHOLD) {
                rx->locked = 0U;
                rx->lock_count = 0U;
                rx->resync_count++;
            }
            rx->window_bits = 0U;
            rx->window_errors = 0U;
        }
        return error ? -1 : 1;
    }

    const uint16_t expected = (uint16_t)(((rx->lfsr >> 8U) ^ (rx->lfsr >> 4U)) & 1U);
    rx->lock_count = (expected == bit) ? (uint16_t)(rx->lock_count + 1U) : 0U;
    rx->lfsr = (uint16_t)(((rx->lfsr << 1U) | bit) & M17_PRBS9_MASK);
    if (rx->lock_count >= M17_PRBS9_LOCK_BITS) {
        rx->locked = 1U;
        rx->window_bits = 0U;
        rx->window_errors = 0U;
    }
    return 0;
}

uint32_t
m17_scrambler_mask_for_subtype(uint8_t subtype) {
    switch (subtype) {
        case 0U: return M17_SCRAMBLER_8BIT_MASK;
        case 1U: return M17_SCRAMBLER_16BIT_MASK;
        case 2U: return M17_SCRAMBLER_24BIT_MASK;
        default: return 0U;
    }
}

uint8_t
m17_scrambler_next_bit(uint8_t subtype, uint32_t* lfsr) {
    if (lfsr == NULL) {
        return 0U;
    }

    const uint32_t mask = m17_scrambler_mask_for_subtype(subtype);
    uint32_t state = *lfsr & mask;
    if (mask == 0U || state == 0U) {
        *lfsr = state;
        return 0U;
    }

    uint32_t bit = 0U;
    switch (subtype) {
        case 0U: bit = ((state >> 7U) ^ (state >> 5U) ^ (state >> 4U) ^ (state >> 3U)) & 1U; break;
        case 1U: bit = ((state >> 15U) ^ (state >> 14U) ^ (state >> 12U) ^ (state >> 3U)) & 1U; break;
        case 2U: bit = ((state >> 23U) ^ (state >> 22U) ^ (state >> 21U) ^ (state >> 16U)) & 1U; break;
        default: bit = 0U; break;
    }

    state = ((state << 1U) | bit) & mask;
    *lfsr = state;
    return (uint8_t)bit;
}

int
m17_scrambler_advance_state(uint8_t subtype, uint32_t seed, uint32_t bit_count, uint32_t* out_state) {
    const uint32_t mask = m17_scrambler_mask_for_subtype(subtype);
    uint32_t state = seed & mask;
    if (mask == 0U || state == 0U || out_state == NULL) {
        return -1;
    }

    for (uint32_t i = 0U; i < bit_count; i++) {
        (void)m17_scrambler_next_bit(subtype, &state);
    }

    *out_state = state;
    return 0;
}

int
m17_scrambler_apply_bits(uint8_t subtype, uint32_t seed, uint16_t frame_number, const uint8_t* input_bits,
                         uint8_t* output_bits, uint16_t bit_count) {
    if (input_bits == NULL || output_bits == NULL) {
        return -1;
    }

    const uint32_t counter = (uint32_t)(frame_number & 0x7FFFU);
    uint32_t state = 0U;
    if (m17_scrambler_advance_state(subtype, seed, counter * (uint32_t)M17_STREAM_PAYLOAD_BITS, &state) != 0) {
        return -1;
    }

    for (uint16_t i = 0U; i < bit_count; i++) {
        output_bits[i] = (uint8_t)((input_bits[i] ^ m17_scrambler_next_bit(subtype, &state)) & 1U);
    }

    return 0;
}

uint8_t
m17_aes_key_bytes_for_subtype(uint8_t subtype) {
    switch (subtype) {
        case 0U: return M17_AES_128_KEY_BYTES;
        case 1U: return M17_AES_192_KEY_BYTES;
        case 2U: return M17_AES_256_KEY_BYTES;
        default: return 0U;
    }
}

void
m17_aes_build_counter(const uint8_t nonce[M17_AES_NONCE_BYTES], uint16_t frame_number,
                      uint8_t counter[M17_AES_COUNTER_BYTES]) {
    if (nonce == NULL || counter == NULL) {
        return;
    }

    for (int i = 0; i < M17_AES_NONCE_BYTES; i++) {
        counter[i] = nonce[i];
    }

    /*
     * gr-m17 and M17_Implementations both mask the transmitted EoT bit
     * before AES-CTR, so final frame 0xFFFF uses counter 0x7FFF.
     */
    const uint16_t fn = (uint16_t)(frame_number & 0x7FFFU);
    counter[14] = (uint8_t)((fn >> 8U) & 0x7FU);
    counter[15] = (uint8_t)(fn & 0xFFU);
}

int
m17_packet_frame_count_for_app_bytes(uint16_t app_bytes, uint8_t* last_frame_bytes) {
    if (app_bytes > M17_PACKET_MAX_APPLICATION_BYTES) {
        return -1;
    }

    const uint16_t total_bytes = (uint16_t)(app_bytes + M17_PACKET_CRC_BYTES);
    uint16_t frames = (uint16_t)((total_bytes + M17_PACKET_CHUNK_BYTES - 1U) / M17_PACKET_CHUNK_BYTES);
    if (frames < M17_PACKET_MIN_FRAMES) {
        frames = M17_PACKET_MIN_FRAMES;
    }
    if (frames > M17_PACKET_MAX_FRAMES) {
        return -1;
    }

    uint8_t last = (uint8_t)(total_bytes % M17_PACKET_CHUNK_BYTES);
    if (last == 0U) {
        last = M17_PACKET_CHUNK_BYTES;
    }
    if (last_frame_bytes != NULL) {
        *last_frame_bytes = last;
    }
    return (int)frames;
}

_Static_assert((((M17_PACKET_MAX_FRAMES - 1U) * M17_PACKET_CHUNK_BYTES) + M17_PACKET_CHUNK_BYTES - M17_PACKET_CRC_BYTES)
                   == M17_PACKET_MAX_APPLICATION_BYTES,
               "M17 packet frame bounds must match maximum application bytes");

int
m17_packet_app_bytes_from_eof(uint8_t full_frames, uint8_t last_frame_bytes, uint16_t* app_bytes) {
    if (full_frames >= M17_PACKET_MAX_FRAMES || last_frame_bytes == 0U || last_frame_bytes > M17_PACKET_CHUNK_BYTES) {
        return -1;
    }

    const uint16_t total_bytes =
        (uint16_t)(((uint16_t)full_frames * M17_PACKET_CHUNK_BYTES) + (uint16_t)last_frame_bytes);
    if (total_bytes < M17_PACKET_CRC_BYTES) {
        return -1;
    }

    const uint16_t app = (uint16_t)(total_bytes - M17_PACKET_CRC_BYTES);
    if (app_bytes != NULL) {
        *app_bytes = app;
    }
    return 0;
}

int
m17_packet_metadata_byte(uint8_t eof, uint8_t value, uint8_t* metadata_byte) {
    eof &= 1U;
    if ((eof == 0U && value > 31U) || (eof != 0U && (value == 0U || value > M17_PACKET_CHUNK_BYTES))) {
        return -1;
    }
    if (metadata_byte != NULL) {
        *metadata_byte = (uint8_t)((eof << 7U) | ((value & 0x1FU) << 2U));
    }
    return 0;
}

int
m17_packet_parse_metadata_byte(uint8_t metadata_byte, uint8_t* eof, uint8_t* value) {
    if ((metadata_byte & 0x03U) != 0U) {
        return -1;
    }

    const uint8_t parsed_eof = (uint8_t)((metadata_byte >> 7U) & 1U);
    const uint8_t parsed_value = (uint8_t)((metadata_byte >> 2U) & 0x1FU);
    if (parsed_eof != 0U && (parsed_value == 0U || parsed_value > M17_PACKET_CHUNK_BYTES)) {
        return -1;
    }

    if (eof != NULL) {
        *eof = parsed_eof;
    }
    if (value != NULL) {
        *value = parsed_value;
    }
    return 0;
}

void
m17_convolution_encode_bits(const uint8_t* input_bits, uint8_t* output_bits, uint16_t bit_count) {
    if (input_bits == NULL || output_bits == NULL) {
        return;
    }

    uint16_t out = 0U;
    uint8_t d1 = 0U;
    uint8_t d2 = 0U;
    uint8_t d3 = 0U;
    uint8_t d4 = 0U;

    for (uint16_t i = 0U; i < bit_count; i++) {
        const uint8_t d = (uint8_t)(input_bits[i] & 1U);
        output_bits[out++] = (uint8_t)((d + d3 + d4) & 1U);
        output_bits[out++] = (uint8_t)((d + d1 + d2 + d4) & 1U);

        d4 = d3;
        d3 = d2;
        d2 = d1;
        d1 = d;
    }
}

uint16_t
m17_puncture_bits(const uint8_t* input_bits, uint16_t input_bits_len, const uint8_t* puncture_pattern,
                  uint8_t pattern_len, uint8_t* output_bits, uint16_t output_capacity, uint16_t* consumed_bits) {
    if (input_bits == NULL || puncture_pattern == NULL || output_bits == NULL || pattern_len == 0U) {
        if (consumed_bits != NULL) {
            *consumed_bits = 0U;
        }
        return 0U;
    }

    uint16_t out = 0U;
    uint16_t consumed = 0U;
    for (uint16_t i = 0U; i < input_bits_len; i++) {
        consumed = (uint16_t)(i + 1U);
        if (puncture_pattern[i % pattern_len] != 0U && out < output_capacity) {
            output_bits[out++] = (uint8_t)(input_bits[i] & 1U);
        }
    }

    if (consumed_bits != NULL) {
        *consumed_bits = consumed;
    }
    return out;
}

void
m17_interleave_368_bits(const uint8_t* input_bits, uint8_t* output_bits) {
    if (input_bits == NULL || output_bits == NULL) {
        return;
    }
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        const int x = ((45 * i) + (92 * i * i)) % M17_PAYLOAD_BITS;
        output_bits[x] = (uint8_t)(input_bits[i] & 1U);
    }
}

void
m17_deinterleave_368_bits(const uint8_t* input_bits, uint8_t* output_bits) {
    if (input_bits == NULL || output_bits == NULL) {
        return;
    }
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        const int x = ((45 * i) + (92 * i * i)) % M17_PAYLOAD_BITS;
        output_bits[i] = (uint8_t)(input_bits[x] & 1U);
    }
}

void
m17_randomize_368_bits(const uint8_t* input_bits, uint8_t* output_bits) {
    if (input_bits == NULL || output_bits == NULL) {
        return;
    }
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        output_bits[i] = (uint8_t)((input_bits[i] ^ m17_scramble[i]) & 1U);
    }
}

void
m17_payload_encode_bits(const uint8_t decoded_bits[M17_PAYLOAD_BITS], uint8_t randomized_bits[M17_PAYLOAD_BITS]) {
    if (decoded_bits == NULL || randomized_bits == NULL) {
        return;
    }

    uint8_t interleaved[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        interleaved[i] = 0U;
        randomized_bits[i] = 0U;
    }

    m17_interleave_368_bits(decoded_bits, interleaved);
    m17_randomize_368_bits(interleaved, randomized_bits);
}

void
m17_payload_decode_bits(const uint8_t randomized_bits[M17_PAYLOAD_BITS], uint8_t decoded_bits[M17_PAYLOAD_BITS]) {
    if (randomized_bits == NULL || decoded_bits == NULL) {
        return;
    }

    uint8_t interleaved[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        interleaved[i] = 0U;
        decoded_bits[i] = 0U;
    }

    m17_randomize_368_bits(randomized_bits, interleaved);
    m17_deinterleave_368_bits(interleaved, decoded_bits);
}

void
m17_golay24_encode12_bits(const uint8_t* data_bits, uint8_t* encoded_bits) {
    if (data_bits == NULL || encoded_bits == NULL) {
        return;
    }

    unsigned char data[M17_LICH_GOLAY_DATA_BITS];
    unsigned char encoded[M17_LICH_GOLAY_CODE_BITS];
    for (int i = 0; i < M17_LICH_GOLAY_DATA_BITS; i++) {
        data[i] = (unsigned char)(data_bits[i] & 1U);
    }

    Golay_24_12_encode(data, encoded);

    for (int i = 0; i < M17_LICH_GOLAY_CODE_BITS; i++) {
        encoded_bits[i] = (uint8_t)(encoded[i] & 1U);
    }
}

int
m17_golay24_decode24_bits(const uint8_t* encoded_bits, uint8_t* decoded_bits) {
    if (encoded_bits == NULL || decoded_bits == NULL) {
        return -1;
    }

    unsigned char rx[M17_LICH_GOLAY_CODE_BITS];
    for (int i = 0; i < M17_LICH_GOLAY_CODE_BITS; i++) {
        rx[i] = (unsigned char)(encoded_bits[i] & 1U);
    }

    const int ok = Golay_24_12_decode(rx) ? 1 : 0;

    for (int i = 0; i < M17_LICH_GOLAY_DATA_BITS; i++) {
        decoded_bits[i] = (uint8_t)(rx[i] & 1U);
    }

    return ok != 0 ? 0 : -1;
}

int
m17_lich_build_content(const uint8_t* lsf_bits, uint8_t lich_cnt, uint8_t* lich_content) {
    if (lsf_bits == NULL || lich_content == NULL || lich_cnt >= M17_LICH_CHUNKS) {
        return -1;
    }

    const uint16_t lsf_offset = (uint16_t)lich_cnt * M17_LICH_CHUNK_BITS;
    for (int i = 0; i < M17_LICH_CHUNK_BITS; i++) {
        lich_content[i] = (uint8_t)(lsf_bits[lsf_offset + i] & 1U);
    }

    lich_content[40] = (uint8_t)((lich_cnt >> 2U) & 1U);
    lich_content[41] = (uint8_t)((lich_cnt >> 1U) & 1U);
    lich_content[42] = (uint8_t)(lich_cnt & 1U);
    for (int i = 0; i < M17_LICH_RESERVED_BITS; i++) {
        lich_content[43 + i] = 0U;
    }

    return 0;
}

int
m17_lich_parse_content(const uint8_t* lich_content, uint8_t* lich_cnt, uint8_t* reserved) {
    if (lich_content == NULL) {
        return -1;
    }

    const uint8_t cnt =
        (uint8_t)(((lich_content[40] & 1U) << 2U) | ((lich_content[41] & 1U) << 1U) | (lich_content[42] & 1U));
    uint8_t rsv = 0U;
    for (int i = 0; i < M17_LICH_RESERVED_BITS; i++) {
        rsv = (uint8_t)((rsv << 1U) | (lich_content[43 + i] & 1U));
    }

    if (lich_cnt != NULL) {
        *lich_cnt = cnt;
    }
    if (reserved != NULL) {
        *reserved = rsv;
    }

    return cnt < M17_LICH_CHUNKS ? 0 : -1;
}

void
m17_lich_encode_bits(const uint8_t* lich_content, uint8_t* encoded_bits) {
    if (lich_content == NULL || encoded_bits == NULL) {
        return;
    }

    for (int block = 0; block < M17_LICH_GOLAY_BLOCKS; block++) {
        const size_t data_offset = (size_t)block * (size_t)M17_LICH_GOLAY_DATA_BITS;
        const size_t code_offset = (size_t)block * (size_t)M17_LICH_GOLAY_CODE_BITS;
        m17_golay24_encode12_bits(&lich_content[data_offset], &encoded_bits[code_offset]);
    }
}

int
m17_lich_decode_bits(const uint8_t* encoded_bits, uint8_t* lich_content) {
    if (encoded_bits == NULL || lich_content == NULL) {
        return -1;
    }

    int err = 0;
    for (int block = 0; block < M17_LICH_GOLAY_BLOCKS; block++) {
        const size_t code_offset = (size_t)block * (size_t)M17_LICH_GOLAY_CODE_BITS;
        const size_t data_offset = (size_t)block * (size_t)M17_LICH_GOLAY_DATA_BITS;
        if (m17_golay24_decode24_bits(&encoded_bits[code_offset], &lich_content[data_offset]) != 0) {
            err = -1;
        }
    }

    return err;
}

uint16_t
m17_lsf_encode_type1_bits(const uint8_t type1_flush_bits[M17_LSF_TYPE1_FLUSH_BITS],
                          uint8_t randomized_bits[M17_PAYLOAD_BITS], uint16_t* consumed_bits) {
    if (type1_flush_bits == NULL || randomized_bits == NULL) {
        if (consumed_bits != NULL) {
            *consumed_bits = 0U;
        }
        return 0U;
    }

    uint8_t encoded[M17_LSF_TYPE2_BITS];
    uint8_t punctured[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_LSF_TYPE2_BITS; i++) {
        encoded[i] = 0U;
    }
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        punctured[i] = 0U;
        randomized_bits[i] = 0U;
    }

    m17_convolution_encode_bits(type1_flush_bits, encoded, M17_LSF_TYPE1_FLUSH_BITS);
    const uint16_t out = m17_puncture_bits(encoded, M17_LSF_TYPE2_BITS, m17_puncture_pattern_1, M17_PUNCTURE_P1_LEN,
                                           punctured, M17_PAYLOAD_BITS, consumed_bits);
    m17_payload_encode_bits(punctured, randomized_bits);
    return out;
}

void
m17_stream_build_type1_bits(uint16_t frame_number, const uint8_t* payload_bits, uint8_t* type1_flush_bits) {
    if (type1_flush_bits == NULL) {
        return;
    }

    for (int i = 0; i < M17_STREAM_TYPE1_FLUSH_BITS; i++) {
        type1_flush_bits[i] = 0U;
    }
    for (int i = 0; i < M17_STREAM_FN_BITS; i++) {
        type1_flush_bits[i] = (uint8_t)((frame_number >> (M17_STREAM_FN_BITS - 1U - i)) & 1U);
    }
    if (payload_bits == NULL) {
        return;
    }
    for (int i = 0; i < M17_STREAM_PAYLOAD_BITS; i++) {
        type1_flush_bits[M17_STREAM_FN_BITS + i] = (uint8_t)(payload_bits[i] & 1U);
    }
}

int
m17_stream_parse_type1_bits(const uint8_t* type1_bits, uint16_t* frame_number, uint8_t* payload_bits) {
    if (type1_bits == NULL) {
        return -1;
    }

    uint16_t fn = 0U;
    for (int i = 0; i < M17_STREAM_FN_BITS; i++) {
        fn = (uint16_t)((fn << 1U) | (type1_bits[i] & 1U));
    }
    if (frame_number != NULL) {
        *frame_number = fn;
    }
    if (payload_bits != NULL) {
        for (int i = 0; i < M17_STREAM_PAYLOAD_BITS; i++) {
            payload_bits[i] = (uint8_t)(type1_bits[M17_STREAM_FN_BITS + i] & 1U);
        }
    }
    return 0;
}

uint16_t
m17_stream_next_frame_counter(uint16_t frame_counter) {
    return (uint16_t)((frame_counter + 1U) & 0x7FFFU);
}

void
m17_stream_pack_payload_halves(const uint8_t* first_half_bits, const uint8_t* second_half_bits, uint8_t* payload_bits) {
    if (payload_bits == NULL) {
        return;
    }

    for (int i = 0; i < M17_STREAM_PAYLOAD_HALF_BITS; i++) {
        payload_bits[i] = (first_half_bits != NULL) ? (uint8_t)(first_half_bits[i] & 1U) : 0U;
        payload_bits[M17_STREAM_PAYLOAD_HALF_BITS + i] =
            (second_half_bits != NULL) ? (uint8_t)(second_half_bits[i] & 1U) : 0U;
    }
}

void
m17_stream_encode_type1_bits(const uint8_t* type1_flush_bits, uint8_t* punctured_bits) {
    if (type1_flush_bits == NULL || punctured_bits == NULL) {
        return;
    }

    uint8_t encoded[M17_STREAM_TYPE2_BITS];
    for (int i = 0; i < M17_STREAM_TYPE2_BITS; i++) {
        encoded[i] = 0U;
    }
    m17_convolution_encode_bits(type1_flush_bits, encoded, M17_STREAM_TYPE1_FLUSH_BITS);
    (void)m17_puncture_bits(encoded, M17_STREAM_TYPE2_BITS, m17_puncture_pattern_2, M17_PUNCTURE_P2_LEN, punctured_bits,
                            M17_STREAM_PUNCTURED_BITS, NULL);
}

void
m17_stream_combine_frame_bits(const uint8_t* lich_bits, const uint8_t* stream_punctured_bits, uint8_t* combined_bits) {
    if (lich_bits == NULL || stream_punctured_bits == NULL || combined_bits == NULL) {
        return;
    }

    for (int i = 0; i < M17_LICH_BITS; i++) {
        combined_bits[i] = (uint8_t)(lich_bits[i] & 1U);
    }
    for (int i = 0; i < M17_STREAM_PUNCTURED_BITS; i++) {
        combined_bits[M17_LICH_BITS + i] = (uint8_t)(stream_punctured_bits[i] & 1U);
    }
}

uint16_t
m17_bert_encode_type1_bits(const uint8_t* type1_flush_bits, uint8_t* randomized_bits, uint16_t* consumed_bits) {
    if (type1_flush_bits == NULL || randomized_bits == NULL) {
        if (consumed_bits != NULL) {
            *consumed_bits = 0U;
        }
        return 0U;
    }

    uint8_t encoded[M17_BERT_TYPE2_BITS];
    uint8_t punctured[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_BERT_TYPE2_BITS; i++) {
        encoded[i] = 0U;
    }
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        punctured[i] = 0U;
        randomized_bits[i] = 0U;
    }

    m17_convolution_encode_bits(type1_flush_bits, encoded, M17_BERT_TYPE1_FLUSH_BITS);
    const uint16_t out = m17_puncture_bits(encoded, M17_BERT_TYPE2_BITS, m17_puncture_pattern_2, M17_PUNCTURE_P2_LEN,
                                           punctured, M17_PAYLOAD_BITS, consumed_bits);
    m17_payload_encode_bits(punctured, randomized_bits);
    return out;
}

void
m17_packet_build_type1_bits(const uint8_t* chunk_bits, uint8_t metadata_byte, uint8_t* type1_flush_bits) {
    if (type1_flush_bits == NULL) {
        return;
    }

    for (int i = 0; i < M17_PACKET_TYPE1_FLUSH_BITS; i++) {
        type1_flush_bits[i] = 0U;
    }
    if (chunk_bits != NULL) {
        for (int i = 0; i < M17_PACKET_CHUNK_BITS; i++) {
            type1_flush_bits[i] = (uint8_t)(chunk_bits[i] & 1U);
        }
    }
    for (int i = 0; i < M17_PACKET_METADATA_BITS; i++) {
        type1_flush_bits[M17_PACKET_CHUNK_BITS + i] = (uint8_t)((metadata_byte >> (7U - (uint8_t)i)) & 1U);
    }
}

uint16_t
m17_packet_encode_type1_bits(const uint8_t* type1_flush_bits, uint8_t* randomized_bits, uint16_t* consumed_bits) {
    if (type1_flush_bits == NULL || randomized_bits == NULL) {
        if (consumed_bits != NULL) {
            *consumed_bits = 0U;
        }
        return 0U;
    }

    uint8_t encoded[M17_PACKET_TYPE2_BITS];
    uint8_t punctured[M17_PAYLOAD_BITS];
    for (int i = 0; i < M17_PACKET_TYPE2_BITS; i++) {
        encoded[i] = 0U;
    }
    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        punctured[i] = 0U;
        randomized_bits[i] = 0U;
    }

    m17_convolution_encode_bits(type1_flush_bits, encoded, M17_PACKET_TYPE1_FLUSH_BITS);
    const uint16_t out = m17_puncture_bits(encoded, M17_PACKET_TYPE2_BITS, m17_puncture_pattern_3, M17_PUNCTURE_P3_LEN,
                                           punctured, M17_PAYLOAD_BITS, consumed_bits);
    m17_payload_encode_bits(punctured, randomized_bits);
    return out;
}

int
m17_symbol_from_dibit(uint8_t dibit) {
    static const int symbol_map[4] = {+1, +3, -1, -3};
    return symbol_map[dibit & 0x3U];
}

static uint8_t
m17_dibit_from_word_pair(uint16_t word, int bit_index) {
    const uint8_t msb = (uint8_t)((word >> (15 - bit_index)) & 1U);
    const uint8_t lsb = (uint8_t)((word >> (14 - bit_index)) & 1U);
    return (uint8_t)((msb << 1U) | lsb);
}

void
m17_fill_repeating_16bit_dibits(uint16_t pattern, uint8_t output_dibits[M17_FRAME_SYMBOLS]) {
    if (output_dibits == NULL) {
        return;
    }
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        output_dibits[i] = m17_dibit_from_word_pair(pattern, (i * 2) % 16);
    }
}

void
m17_fill_sync_dibits_from_word(uint16_t sync_word, uint8_t output_dibits[M17_SYNC_SYMBOLS]) {
    if (output_dibits == NULL) {
        return;
    }
    for (int i = 0; i < M17_SYNC_SYMBOLS; i++) {
        output_dibits[i] = m17_dibit_from_word_pair(sync_word, i * 2);
    }
}

void
m17_frame_build_dibits(uint16_t sync_word, const uint8_t randomized_bits[M17_PAYLOAD_BITS],
                       uint8_t output_dibits[M17_FRAME_SYMBOLS]) {
    if (randomized_bits == NULL || output_dibits == NULL) {
        return;
    }

    m17_fill_sync_dibits_from_word(sync_word, output_dibits);
    for (int i = 0; i < M17_PAYLOAD_SYMBOLS; i++) {
        const uint8_t msb = (uint8_t)(randomized_bits[(i * 2) + 0] & 1U);
        const uint8_t lsb = (uint8_t)(randomized_bits[(i * 2) + 1] & 1U);
        output_dibits[M17_SYNC_SYMBOLS + i] = (uint8_t)((msb << 1U) | lsb);
    }
}
