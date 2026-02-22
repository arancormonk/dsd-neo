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

#pragma once

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void tyt_ep_aes_keystream_creation(dsd_state* state, char* input);
void tyt_ap_pc4_keystream_creation(dsd_state* state, char* input);
void retevis_rc2_keystream_creation(dsd_state* state, char* input);
int baofeng_ap_pc5_keystream_creation(dsd_state* state, const char* input);
int connect_systems_ee72_key_creation(dsd_state* state, const char* input);
void ken_dmr_scrambler_keystream_creation(dsd_state* state, char* input);
void anytone_bp_keystream_creation(dsd_state* state, char* input);
void straight_mod_xor_keystream_creation(dsd_state* state, char* input);
void straight_mod_xor_apply_frame49(dsd_state* state, int slot, char ambe_d[49]);
void tyt16_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24], int fnum);
void csi72_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24]);
void kirisun_adv_keystream_creation(dsd_state* state);
void kirisun_uni_keystream_creation(dsd_state* state);

#ifdef __cplusplus
}
#endif
