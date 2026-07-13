// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief Private M17 decode, encode, and packet helpers.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_M17_M17_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_M17_M17_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct m17_lsf_result;

enum m17_stream_result {
    M17_STREAM_INVALID = -1,
    M17_STREAM_CAN_FILTERED = 0,
    M17_STREAM_CLEAR_DISPATCHED = 1,
    M17_STREAM_ENCRYPTED_LOCKED = 2,
    M17_STREAM_ENCRYPTED_DISPATCHED = 3,
    M17_STREAM_SIGNATURE_CONSUMED = 4,
};

short m17_clip_float_to_short(float value);
uint16_t m17_compose_frame_info(uint16_t ps, uint16_t dt, uint16_t et, uint16_t es, uint16_t cn, uint16_t signature,
                                uint16_t reserved);
unsigned long long m17_read_ip_source(const uint8_t* ip_frame);
size_t m17_encode_packet_protocol_id(uint32_t identifier, uint8_t* out);

int m17_process_lich(dsd_state* state, const dsd_opts* opts, const uint8_t* lich_bits);
int m17_apply_lsf_result(dsd_state* state, const struct m17_lsf_result* res);
int m17_dispatch_stream_payload(const dsd_opts* opts, dsd_state* state, const uint8_t* payload, uint16_t frame_number,
                                uint8_t* processed_payload);

void m17_process_bert_payload(const dsd_opts* opts, dsd_state* state, const uint8_t* bert_bits);
void m17_depuncture_p2_hard(const uint8_t* punctured, uint8_t* depunc, int depunc_bits);
void m17_decode_bert_payload_bits(const uint8_t* m17_bits, uint8_t* bert_bits);

void m17_dibits_to_symbols(const uint8_t* output_dibits, int* output_symbols);
void m17_upsample_symbols_10x(const int* output_symbols, int* output_up);
void m17_baseband_no_filter(const int* output_up, short* baseband);
void m17_maybe_apply_dead_air(int type, uint8_t* output_dibits, short* baseband);
void m17_reverse_brt_bits(const uint8_t* input, uint8_t* output);

void m17_setup_conn_disc_eotx(unsigned long long src, uint8_t reflector_module, uint8_t* conn, uint8_t* disc,
                              uint8_t* eotx);
void m17_load_lsf_callsigns(uint8_t* m17_lsf, unsigned long long dst, unsigned long long src);
uint16_t m17_attach_lsf_crc(uint8_t* m17_lsf, uint8_t* lsf_packed);

int m17_decode_pkt_should_report_encrypted(const dsd_state* state, uint32_t protocol);
int m17_pkt_ptr_clamped(int pbc_count);
void m17_pkt_finalize_eot(const dsd_opts* opts, dsd_state* state, uint16_t app_len, int end);
void m17_ip_dispatch_frame(const dsd_opts* opts, dsd_state* state, const uint8_t* ip_frame, int len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_M17_M17_INTERNAL_H_ */
