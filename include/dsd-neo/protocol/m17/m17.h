// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief M17 protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#ifdef DSD_NEO_TEST_HOOKS
#include <stddef.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void processM17STR(dsd_opts* opts, dsd_state* state);
void processM17PKT(dsd_opts* opts, dsd_state* state);
void processM17LSF(dsd_opts* opts, dsd_state* state);
void processM17BRT(dsd_opts* opts, dsd_state* state);
void processM17IPF(dsd_opts* opts, dsd_state* state);
void encodeM17STR(dsd_opts* opts, dsd_state* state);
void encodeM17BRT(dsd_opts* opts, dsd_state* state);
void encodeM17PKT(dsd_opts* opts, dsd_state* state);

#ifdef DSD_NEO_TEST_HOOKS
struct m17_lsf_result;

enum dsd_neo_m17_test_stream_result {
    DSD_NEO_M17_TEST_STREAM_INVALID = -1,
    DSD_NEO_M17_TEST_STREAM_CAN_FILTERED = 0,
    DSD_NEO_M17_TEST_STREAM_CLEAR_DISPATCHED = 1,
    DSD_NEO_M17_TEST_STREAM_ENCRYPTED_LOCKED = 2,
    DSD_NEO_M17_TEST_STREAM_ENCRYPTED_DISPATCHED = 3,
    DSD_NEO_M17_TEST_STREAM_SIGNATURE_CONSUMED = 4,
};

int dsd_neo_m17_test_apply_lsf_result(dsd_state* state, const struct m17_lsf_result* res);
int dsd_neo_m17_test_dispatch_stream_payload(const dsd_opts* opts, dsd_state* state, const uint8_t payload[128],
                                             uint16_t frame_number, uint8_t processed_payload[128]);
void dsd_neo_m17_test_process_bert_payload(const dsd_opts* opts, dsd_state* state, const uint8_t bert_bits[197]);
void dsd_neo_m17_test_unpack_bytes_to_bits(const uint8_t* bytes, int byte_count, uint8_t* out_bits);
void dsd_neo_m17_test_depuncture_p2_hard(const uint8_t punctured[368], uint8_t* depunc, int depunc_bits);
void dsd_neo_m17_test_decode_bert_payload_bits(const uint8_t m17_bits[368], uint8_t bert_bits[197]);
uint16_t dsd_neo_m17_test_compose_frame_info(uint16_t ps, uint16_t dt, uint16_t et, uint16_t es, uint16_t cn,
                                             uint16_t signature, uint16_t reserved);
unsigned long long dsd_neo_m17_test_read_ip_source(const uint8_t ip_frame[10]);
void dsd_neo_m17_test_setup_conn_disc_eotx(unsigned long long src, uint8_t reflector_module, uint8_t conn[11],
                                           uint8_t disc[10], uint8_t eotx[10]);
void dsd_neo_m17_test_load_lsf_callsigns(uint8_t m17_lsf[240], unsigned long long dst, unsigned long long src);
uint16_t dsd_neo_m17_test_attach_lsf_crc(uint8_t m17_lsf[240], uint8_t lsf_packed[30]);
void dsd_neo_m17_test_apply_frame_prefix_dibits(int type, uint8_t output_dibits[192]);
void dsd_neo_m17_test_load_payload_dibits(const uint8_t input[368], uint8_t output_dibits[192]);
short dsd_neo_m17_test_clip_float_to_short(float value);
void dsd_neo_m17_test_dibits_to_symbols(const uint8_t output_dibits[192], int output_symbols[192]);
void dsd_neo_m17_test_upsample_symbols_10x(const int output_symbols[192], int output_up[1920]);
void dsd_neo_m17_test_baseband_no_filter(const int output_up[1920], short baseband[1920]);
void dsd_neo_m17_test_maybe_apply_dead_air(int type, uint8_t output_dibits[192], short baseband[1920]);
void dsd_neo_m17_test_reverse_brt_bits(const uint8_t input[197], uint8_t output[208]);
size_t dsd_neo_m17_test_strlen_limit(const char* text, size_t limit);
int dsd_neo_m17_test_prepare_pkt_payload(const char* text, uint8_t* packed, uint8_t* full_bits, uint16_t* app_len,
                                         int* block, uint8_t* lst, uint16_t* crc);
int dsd_neo_m17_test_prepare_pkt_from_state(const dsd_state* input_state, uint8_t* lsf_bits, uint8_t* packed,
                                            uint16_t* app_len, uint16_t* lsf_crc, uint8_t* can, unsigned long long* dst,
                                            unsigned long long* src);
int dsd_neo_m17_test_decode_pkt_should_report_encrypted(const dsd_state* state, uint32_t protocol);
int dsd_neo_m17_test_pkt_ptr_clamped(int pbc_count);
void dsd_neo_m17_test_dispatch_ip_frame(const dsd_opts* opts, dsd_state* state, const uint8_t* ip_frame, int len);
int dsd_neo_m17_test_process_lich(dsd_state* state, const dsd_opts* opts, const uint8_t* lich_bits);
void dsd_neo_m17_test_finalize_packet_eot(const dsd_opts* opts, dsd_state* state, uint16_t app_len, int end);
size_t dsd_neo_m17_test_encode_packet_protocol_id(uint32_t identifier, uint8_t out[4]);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H */
