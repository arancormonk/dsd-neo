// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR privacy/keystream helper entrypoints.
 *
 * Declares optional key/keystream generation helpers used by CLI/UI controls.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_DMR_KEYSTREAM_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_DMR_KEYSTREAM_H_H

#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void tyt_ep_aes_keystream_creation(dsd_state* state, const char* input, int show_keys);
void tyt_ap_pc4_keystream_creation(dsd_state* state, const char* input, int show_keys);
void retevis_rc2_keystream_creation(dsd_state* state, const char* input, int show_keys);
int retevis_rc2_apply_frame49(dsd_state* state, char ambe_d[49]);
int baofeng_ap_pc5_keystream_creation(dsd_state* state, const char* input, int show_keys);
int baofeng_pc5_apply_frame49(const dsd_state* state, char ambe_d[49]);
int connect_systems_ee72_key_creation(dsd_state* state, const char* input, int show_keys);
void ken_dmr_scrambler_keystream_creation(dsd_state* state, char* input, int show_keys);
int ken_dmr_scrambler_apply_frame49(dsd_state* state, int slot, char ambe_d[49]);
void anytone_bp_keystream_creation(dsd_state* state, char* input, int show_keys);
int anytone_bp_apply_frame49(dsd_state* state, int slot, char ambe_d[49]);
int dmr_parse_static_keystream_spec(const char* input, uint8_t out_bits[882], int* out_mod, int* out_frame_mode,
                                    int* out_frame_off, int* out_frame_step, char* err, size_t err_cap);
void straight_mod_xor_keystream_creation(dsd_state* state, const char* input, int show_keys);
void straight_mod_xor_apply_frame49(dsd_state* state, int slot, char ambe_d[49]);
int dmr_ambe49_is_default_silence(const char ambe_d[49]);
int dmr_ambe49_has_zero_tail(const char ambe_d[49]);
int dmr_ambe49_should_skip_voice_stream(const char ambe_d[49]);
int dmr_voice_stream_apply_frame49(const uint8_t* ks_bits, long int* bit_counter, int algid, char ambe_d[49]);
int dmr_basic_privacy_apply_frame49(unsigned long long key_id, char ambe_d[49]);
int tyt_ap_pc4_apply_frame49(const dsd_state* state, char ambe_d[49]);
int tyt_ep_aes_apply_frame49(const dsd_state* state, char ambe_d[49]);
int hytera_bp_apply_frame49(unsigned long long k1, unsigned long long k2, unsigned long long k3, unsigned long long k4,
                            int* frame_counter, char ambe_d[49]);
int vertex_key_map_apply_frame49(dsd_state* state, int slot, unsigned long long key, char ambe_d[49]);
void tyt16_ambe2_codeword_keystream(const dsd_state* state, char ambe_fr[4][24], int fnum);
void csi72_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24]);
void kirisun_adv_keystream_creation(dsd_state* state);
void kirisun_uni_keystream_creation(dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CRYPTO_DMR_KEYSTREAM_H_H */
