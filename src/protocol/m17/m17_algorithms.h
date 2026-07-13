// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_M17_M17_ALGORITHMS_H_
#define DSD_NEO_SRC_PROTOCOL_M17_M17_ALGORITHMS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M17_SYMBOL_RATE_SPS              4800
#define M17_BIT_RATE_BPS                 9600
#define M17_DEVIATION_INDEX_NUM          1
#define M17_DEVIATION_INDEX_DEN          3
#define M17_DEVIATION_STEP_HZ            800
#define M17_DEVIATION_MAX_HZ             2400
#define M17_OCCUPIED_BANDWIDTH_HZ        9000
#define M17_MIN_CHANNEL_SPACING_HZ       12500
#define M17_FRAME_BITS                   384
#define M17_FRAME_SYMBOLS                192
#define M17_FRAME_DURATION_MS            40
#define M17_SYNC_BITS                    16
#define M17_SYNC_SYMBOLS                 8
#define M17_PAYLOAD_BITS                 368
#define M17_PAYLOAD_SYMBOLS              184
#define M17_LSF_LSD_BYTES                28
#define M17_LSF_LSD_BITS                 224
#define M17_LSF_CRC_BYTES                2
#define M17_LSF_CRC_BITS                 16
#define M17_LSF_BYTES                    30
#define M17_LSF_TYPE1_BITS               240
#define M17_LSF_FLUSH_BITS               4
#define M17_LSF_TYPE1_FLUSH_BITS         244
#define M17_LSF_TYPE2_BITS               488
#define M17_LSF_TYPE3_BITS               368
#define M17_LSF_TYPE4_BITS               368
#define M17_LICH_CONTENT_BITS            48
#define M17_LICH_CHUNK_BITS              40
#define M17_LICH_COUNTER_BITS            3
#define M17_LICH_RESERVED_BITS           5
#define M17_LICH_CHUNKS                  6
#define M17_LICH_GOLAY_BLOCKS            4
#define M17_LICH_GOLAY_DATA_BITS         12
#define M17_LICH_GOLAY_CODE_BITS         24
#define M17_LICH_BITS                    96
#define M17_STREAM_FN_BITS               16
#define M17_STREAM_PAYLOAD_BITS          128
#define M17_STREAM_PAYLOAD_HALF_BITS     64
#define M17_STREAM_CONTENT_BITS          144
#define M17_STREAM_FLUSH_BITS            4
#define M17_STREAM_TYPE1_FLUSH_BITS      148
#define M17_STREAM_TYPE2_BITS            296
#define M17_STREAM_PUNCTURED_BITS        272
#define M17_BERT_PAYLOAD_BITS            197
#define M17_BERT_FLUSH_BITS              4
#define M17_BERT_TYPE1_FLUSH_BITS        201
#define M17_BERT_TYPE2_BITS              402
#define M17_PACKET_CHUNK_BYTES           25
#define M17_PACKET_CHUNK_BITS            200
#define M17_PACKET_METADATA_BITS         6
#define M17_PACKET_CONTENT_BITS          206
#define M17_PACKET_FLUSH_BITS            4
#define M17_PACKET_TYPE1_FLUSH_BITS      210
#define M17_PACKET_TYPE2_BITS            420
#define M17_PACKET_MIN_FRAMES            1
#define M17_PACKET_MAX_FRAMES            33
#define M17_PACKET_CRC_BYTES             2
#define M17_PACKET_MAX_APPLICATION_BYTES 823
#define M17_PACKET_MAX_TOTAL_BYTES       (M17_PACKET_MAX_APPLICATION_BYTES + M17_PACKET_CRC_BYTES)
#define M17_PREAMBLE_BITS                384
#define M17_PREAMBLE_SYMBOLS             192
#define M17_EOT_BITS                     384
#define M17_EOT_SYMBOLS                  192
#define M17_RECOMMENDED_SAMPLE_RATE_HZ   48000
#define M17_RECOMMENDED_UPSAMPLE_FACTOR  10
#define M17_RRC_ALPHA_NUM                1
#define M17_RRC_ALPHA_DEN                2
#define M17_RRC_MIN_SPAN_SYMBOLS         8
#define M17_RRC_RECOMMENDED_TAPS         81
#define M17_CSMA_DEFAULT_PROBABILITY_NUM 1
#define M17_CSMA_DEFAULT_PROBABILITY_DEN 4
#define M17_CSMA_DEFAULT_SLOT_TIME_MS    40
#define M17_PRBS9_MASK                   0x1FFU
#define M17_PRBS9_LOCK_BITS              18
#define M17_PRBS9_RESYNC_WINDOW_BITS     128
#define M17_PRBS9_RESYNC_ERROR_THRESHOLD 18
#define M17_SCRAMBLER_8BIT_MASK          0xFFU
#define M17_SCRAMBLER_16BIT_MASK         0xFFFFU
#define M17_SCRAMBLER_24BIT_MASK         0xFFFFFFU
#define M17_AES_NONCE_BYTES              14
#define M17_AES_COUNTER_BYTES            16
#define M17_AES_128_KEY_BYTES            16
#define M17_AES_192_KEY_BYTES            24
#define M17_AES_256_KEY_BYTES            32
#define M17_PREAMBLE_LSF_WORD            0x7777U
#define M17_PREAMBLE_BERT_WORD           0xDDDDU
#define M17_EOT_MARKER_WORD              0x555DU
#define M17_SYNC_LSF_WORD                0x55F7U
#define M17_SYNC_BERT_WORD               0xDF55U
#define M17_SYNC_STREAM_WORD             0xFF5DU
#define M17_SYNC_PACKET_WORD             0x75FFU

typedef struct {
    uint16_t lfsr;
    uint16_t lock_count;
    uint16_t window_bits;
    uint16_t window_errors;
    uint32_t total_bits;
    uint32_t total_errors;
    uint32_t resync_count;
    uint8_t locked;
} m17_prbs9_rx_state;

uint16_t m17_crc16(const uint8_t* in, uint16_t len);
unsigned long long int m17_encode_b40_callsign(unsigned long long int value, const char* csd);
uint16_t m17_prbs9_next_bit(uint16_t* lfsr);
void m17_prbs9_fill_bits(uint16_t* lfsr, uint8_t* out_bits, uint16_t bit_count);
void m17_prbs9_rx_init(m17_prbs9_rx_state* rx, uint16_t initial_state);
int m17_prbs9_rx_push_bit(m17_prbs9_rx_state* rx, uint8_t bit);
uint8_t m17_scrambler_key_bits_for_subtype(uint8_t subtype);
uint32_t m17_scrambler_mask_for_subtype(uint8_t subtype);
uint8_t m17_scrambler_next_bit(uint8_t subtype, uint32_t* lfsr);
int m17_scrambler_advance_state(uint8_t subtype, uint32_t seed, uint32_t bit_count, uint32_t* out_state);
int m17_scrambler_apply_bits(uint8_t subtype, uint32_t seed, uint16_t frame_number, const uint8_t* input_bits,
                             uint8_t* output_bits, uint16_t bit_count);
uint8_t m17_aes_key_bytes_for_subtype(uint8_t subtype);
void m17_aes_build_counter(const uint8_t nonce[M17_AES_NONCE_BYTES], uint16_t frame_number,
                           uint8_t counter[M17_AES_COUNTER_BYTES]);
int m17_packet_frame_count_for_app_bytes(uint16_t app_bytes, uint8_t* last_frame_bytes);
int m17_packet_app_bytes_from_eof(uint8_t full_frames, uint8_t last_frame_bytes, uint16_t* app_bytes);
int m17_packet_metadata_byte(uint8_t eof, uint8_t value, uint8_t* metadata_byte);
int m17_packet_parse_metadata_byte(uint8_t metadata_byte, uint8_t* eof, uint8_t* value);
void m17_convolution_encode_bits(const uint8_t* input_bits, uint8_t* output_bits, uint16_t bit_count);
uint16_t m17_puncture_bits(const uint8_t* input_bits, uint16_t input_bits_len, const uint8_t* puncture_pattern,
                           uint8_t pattern_len, uint8_t* output_bits, uint16_t output_capacity,
                           uint16_t* consumed_bits);
void m17_interleave_368_bits(const uint8_t* input_bits, uint8_t* output_bits);
void m17_deinterleave_368_bits(const uint8_t* input_bits, uint8_t* output_bits);
void m17_randomize_368_bits(const uint8_t* input_bits, uint8_t* output_bits);
void m17_payload_encode_bits(const uint8_t decoded_bits[M17_PAYLOAD_BITS], uint8_t randomized_bits[M17_PAYLOAD_BITS]);
void m17_payload_decode_bits(const uint8_t randomized_bits[M17_PAYLOAD_BITS], uint8_t decoded_bits[M17_PAYLOAD_BITS]);
void m17_golay24_encode12_bits(const uint8_t* data_bits, uint8_t* encoded_bits);
int m17_golay24_decode24_bits(const uint8_t* encoded_bits, uint8_t* decoded_bits);
int m17_lich_build_content(const uint8_t* lsf_bits, uint8_t lich_cnt, uint8_t* lich_content);
int m17_lich_parse_content(const uint8_t* lich_content, uint8_t* lich_cnt, uint8_t* reserved);
void m17_lich_encode_bits(const uint8_t* lich_content, uint8_t* encoded_bits);
int m17_lich_decode_bits(const uint8_t* encoded_bits, uint8_t* lich_content);
uint16_t m17_lsf_encode_type1_bits(const uint8_t type1_flush_bits[M17_LSF_TYPE1_FLUSH_BITS],
                                   uint8_t randomized_bits[M17_PAYLOAD_BITS], uint16_t* consumed_bits);
void m17_stream_build_type1_bits(uint16_t frame_number, const uint8_t* payload_bits, uint8_t* type1_flush_bits);
int m17_stream_parse_type1_bits(const uint8_t* type1_bits, uint16_t* frame_number, uint8_t* payload_bits);
uint16_t m17_stream_next_frame_counter(uint16_t frame_counter);
void m17_stream_pack_payload_halves(const uint8_t* first_half_bits, const uint8_t* second_half_bits,
                                    uint8_t* payload_bits);
void m17_stream_encode_type1_bits(const uint8_t* type1_flush_bits, uint8_t* punctured_bits);
void m17_stream_combine_frame_bits(const uint8_t* lich_bits, const uint8_t* stream_punctured_bits,
                                   uint8_t* combined_bits);
void m17_bert_build_type1_bits(const uint8_t* bert_payload_bits, uint8_t* type1_flush_bits);
uint16_t m17_bert_encode_type1_bits(const uint8_t* type1_flush_bits, uint8_t* randomized_bits, uint16_t* consumed_bits);
void m17_packet_build_type1_bits(const uint8_t* chunk_bits, uint8_t metadata_byte, uint8_t* type1_flush_bits);
uint16_t m17_packet_encode_type1_bits(const uint8_t* type1_flush_bits, uint8_t* randomized_bits,
                                      uint16_t* consumed_bits);
int m17_symbol_from_dibit(uint8_t dibit);
int m17_deviation_hz_from_dibit(uint8_t dibit);
void m17_fill_repeating_16bit_dibits(uint16_t pattern, uint8_t output_dibits[M17_FRAME_SYMBOLS]);
void m17_fill_sync_dibits_from_word(uint16_t sync_word, uint8_t output_dibits[M17_SYNC_SYMBOLS]);
void m17_frame_build_dibits(uint16_t sync_word, const uint8_t randomized_bits[M17_PAYLOAD_BITS],
                            uint8_t output_dibits[M17_FRAME_SYMBOLS]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_M17_M17_ALGORITHMS_H_ */
