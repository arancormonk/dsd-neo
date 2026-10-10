// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Internal audio2 helper declarations for production-local helpers that have
 * focused regression tests. Production builds keep these helpers file-local;
 * the helper test target overrides DSD_AUDIO2_INTERNAL to link them directly.
 */

#ifndef DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO2_INTERNAL_H
#define DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO2_INTERNAL_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>

#ifndef DSD_AUDIO2_INTERNAL
#define DSD_AUDIO2_INTERNAL static
#endif

DSD_AUDIO2_INTERNAL int dsd_p25_algid_is_encrypted(const dsd_state* state);
DSD_AUDIO2_INTERNAL int dsd_p25_algid_can_decrypt(const dsd_state* state);
DSD_AUDIO2_INTERNAL int dsd_nxdn_can_decrypt(const dsd_state* state);
DSD_AUDIO2_INTERNAL int dsd_is_all_zero_s16(const short* buf, size_t n);

DSD_AUDIO2_INTERNAL void dsd_output_float_blocks(dsd_opts* opts, dsd_state* state, const float* const* blocks,
                                                 size_t block_count, size_t frames, int channels, int skip_silent);
DSD_AUDIO2_INTERNAL void dsd_output_s16_blocks(dsd_opts* opts, dsd_state* state, const short* const* blocks,
                                               size_t block_count, size_t frames, int channels, int skip_silent);

DSD_AUDIO2_INTERNAL void dsd_dmr_init_slot_mute_flags(const dsd_opts* opts, const dsd_state* state, int* encL,
                                                      int* encR);
DSD_AUDIO2_INTERNAL void dsd_dmr_apply_mono_slot_gate(const dsd_opts* opts, const dsd_state* state, int* encL,
                                                      int* encR);
DSD_AUDIO2_INTERNAL void dsd_duplicate_active_float_slot_to_stereo(float* a, float* b, float* c, int encL, int encR,
                                                                   int* outL, int* outR);
DSD_AUDIO2_INTERNAL void dsd_dmr_ss3_init_enc_flags(const dsd_opts* opts, const dsd_state* state, int* encL, int* encR);
DSD_AUDIO2_INTERNAL void dsd_dmr_apply_tg_hold_and_slot_preference_ss3(dsd_opts* opts, const dsd_state* state,
                                                                       unsigned long TGL, unsigned long TGR, int* encL,
                                                                       int* encR);
DSD_AUDIO2_INTERNAL void dsd_dmr_apply_stereo_output_policy_ss3(const dsd_opts* opts, dsd_state* state, int encL,
                                                                int encR);

#endif // DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO2_INTERNAL_H
