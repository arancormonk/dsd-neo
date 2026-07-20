// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/state.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
test_init_opts_clears_trunk_scan_targets_csv(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0xA5, sizeof(opts));
    initOpts(&opts);
    if (opts.trunk_scan_targets_csv[0] != '\0') {
        DSD_FPRINTF(stderr, "initOpts did not clear trunk_scan_targets_csv\n");
        return 20;
    }
    if (opts.show_keys != 0U) {
        DSD_FPRINTF(stderr, "initOpts did not default show_keys to redacted\n");
        return 21;
    }
    return 0;
}

int
main(void) {
    int init_opts_rc = test_init_opts_clears_trunk_scan_targets_csv();
    if (init_opts_rc != 0) {
        return init_opts_rc;
    }

    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    /*
     * Pre-seed fields that have regressed before so initState must prove it owns
     * both pointer members and sparse runtime caches. The assertions are grouped
     * by subsystem to keep each failure diagnostic narrow.
     */
    state->rc2_context = state;
    state->nid_corrections_total = 12;
    state->nid_failures_total = 34;
    state->nid_parity_overrides = 56;
    state->trunk_chan_map[0x0123] = 851000000L;
    state->trunk_chan_map_used[0] = 0x0123U;
    state->trunk_chan_map_used_count = 1U;
    state->trunk_chan_map_seq = 99U;
    state->p25_enc_tg_cache_next = 3U;
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        state->p25_enc_tg_cache_until[i] = 1234567890 + i;
        state->p25_enc_tg_cache_tg[i] = (uint32_t)(2400 + i);
        state->p25_enc_tg_cache_is_group[i] = (uint8_t)(i % 2);
    }
    state->rtl_symbol_cache[0] = 1234.0f;
    state->rtl_symbol_cache[DSD_RTL_SYMBOL_CACHE_CAP - 1] = 5678.0f;
    state->rtl_symbol_cache_pos = 2;
    state->rtl_symbol_cache_len = 4;
    state->rtl_symbol_cache_output_kind = 3;
    state->rtl_symbol_cache_channel_profile = 5;
    state->rtl_symbol_cache_symbol_rate_hz = 6000;
    state->rtl_symbol_cache_levels = 4;
    state->rtl_symbol_cache_generation = 42U;
    state->rtl_symbol_cache_published_pending = 2;
    state->rtl_fsk_sps_num = 24000;
    state->rtl_fsk_sps_den = 9600;
    state->rtl_fsk_sps_accum = 4800;
    state->p25_cqpsk_dibit_map_idx = DSD_P25_CQPSK_DIBIT_MAP_N1200;
    state->ess_b[0][95] = 1;
    state->ess_b_llr[1][95] = 123;
    state->fourv_counter[0] = 2;
    state->data_header_dd_format[0] = 0x16U;
    state->data_header_dd_format[1] = 0x18U;
    state->data_header_bit_padding[0] = 16U;
    state->data_header_bit_padding[1] = 7U;
    state->p25_p1_soft_hamming_ok = 77U;
    state->p25_last_cc_msg_time = 1234567890;
    state->p25_last_cc_msg_time_m = 12345.5;
    state->K = 42;
    state->R = 0x1234567891ULL;
    state->RR = 0x1234567892ULL;
    state->H = 0x0000000000001234ULL;
    state->K1 = 0x1111111111111111ULL;
    state->K2 = 0x2222222222222222ULL;
    state->K3 = 0x3333333333333333ULL;
    state->K4 = 0x4444444444444444ULL;
    state->hytera_key_segments = 4U;
    state->M = 0x21;
    state->payload_mi = 0x1111222233334444ULL;
    state->payload_miR = 0x5555666677778888ULL;
    state->payload_miN = 0x9999AAAABBBBCCCCULL;
    state->payload_miP = 0xDDDDEEEEFFFF0001ULL;
    state->payload_algid = 0x84;
    state->payload_algidR = 0x89;
    state->payload_keyid = 0x1234;
    state->payload_keyidR = 0x5678;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    state->p25_p1_hdu_crypto_fresh = 1;
    state->p25_policy_tg[0] = 0x1234U;
    state->p25_policy_tg[1] = 0x5678U;
    state->p25_mac_frag[0].active = 1U;
    state->p25_mac_frag[0].opcode = 0x89U;
    state->p25_mac_frag[0].data_len = 4U;
    state->p25_mac_frag[0].collected = 2U;
    state->p25_mac_frag[0].data[0] = 0xAAU;
    state->p25_mac_frag[1].active = 1U;
    state->p25_mac_frag[1].opcode = 0x8AU;
    state->p25_mac_frag[1].data_len = 8U;
    state->p25_mac_frag[1].collected = 6U;
    state->p25_mac_frag[1].data[5] = 0xBBU;
    state->dropL = 1;
    state->dropR = 2;
    state->nxdn_cipher_type = 3U;
    state->nxdn_key = 9U;
    state->nxdn_pn95_seed = 0x1FFU;
    state->nxdn_new_iv = 1U;
    state->aes_key[0] = 0xAAU;
    state->aes_iv[0] = 0xBBU;
    state->aes_ivR[0] = 0xCCU;
    state->A1[0] = 0x1111111111111111ULL;
    state->A2[1] = 0x2222222222222222ULL;
    state->A3[0] = 0x3333333333333333ULL;
    state->A4[1] = 0x4444444444444444ULL;
    state->aes_key_loaded[0] = 1;
    state->aes_key_segments[1] = 4U;
    state->rkey_array[0x0123] = 0x1234567890ULL;
    state->rkey_array_loaded[0x0123] = 1U;
    state->keyloader = 1;
    state->late_entry_mi_fragment[1][7][2] = 0x1111222233334444ULL;
    state->csi_ee = 1;
    state->csi_ee_key[0] = 0xDDU;
    state->static_ks_bits[0][0] = 1U;
    state->static_ks_counter[0] = 7;
    state->vertex_ks_key[0] = 0x1234567891ULL;
    state->vertex_ks_bits[0][0] = 1U;
    state->vertex_ks_mod[0] = 8;
    state->vertex_ks_frame_mode[0] = 1;
    state->vertex_ks_frame_off[0] = 2;
    state->vertex_ks_frame_step[0] = 3;
    state->vertex_ks_count = 1;
    state->vertex_ks_active_idx[0] = 0;
    state->vertex_ks_counter[0] = 4;
    state->vertex_ks_warned[0] = 1U;
    initState(state);

    if (state->rc2_context != NULL) {
        DSD_FPRINTF(stderr, "expected rc2_context to be NULL after initState\n");
        // Avoid invalid free if initialization regresses.
        state->rc2_context = NULL;
        freeState(state);
        free(state);
        return 2;
    }
    if (state->K != 0 || state->R != 0ULL || state->RR != 0ULL || state->H != 0ULL || state->K1 != 0ULL
        || state->K2 != 0ULL || state->K3 != 0ULL || state->K4 != 0ULL || state->hytera_key_segments != 0U
        || state->M != 0) {
        DSD_FPRINTF(stderr, "initState did not clear manual crypto key fields\n");
        freeState(state);
        free(state);
        return 14;
    }
    if (state->payload_mi != 0ULL || state->payload_miR != 0ULL || state->payload_miN != 0ULL
        || state->payload_miP != 0ULL || state->payload_algid != 0 || state->payload_algidR != 0
        || state->payload_keyid != 0 || state->payload_keyidR != 0
        || state->p25_crypto_state[0] != DSD_P25_CRYPTO_UNKNOWN || state->p25_crypto_state[1] != DSD_P25_CRYPTO_UNKNOWN
        || state->p25_p1_hdu_crypto_fresh != 0 || state->p25_policy_tg[0] != 0U || state->p25_policy_tg[1] != 0U) {
        DSD_FPRINTF(stderr, "initState did not clear payload crypto metadata\n");
        freeState(state);
        free(state);
        return 15;
    }
    if (state->dropL != 256 || state->dropR != 256 || state->nxdn_cipher_type != 0U || state->nxdn_key != 0U
        || state->nxdn_pn95_seed != 228U || state->nxdn_new_iv != 0U) {
        DSD_FPRINTF(stderr, "initState did not restore crypto counter/cipher defaults\n");
        freeState(state);
        free(state);
        return 16;
    }
    if (state->aes_key[0] != 0U || state->aes_iv[0] != 0U || state->aes_ivR[0] != 0U || state->A1[0] != 0ULL
        || state->A2[1] != 0ULL || state->A3[0] != 0ULL || state->A4[1] != 0ULL || state->aes_key_loaded[0] != 0
        || state->aes_key_segments[1] != 0U) {
        DSD_FPRINTF(stderr, "initState did not clear AES key state\n");
        freeState(state);
        free(state);
        return 17;
    }
    if (state->rkey_array[0x0123] != 0ULL || state->rkey_array_loaded[0x0123] != 0U || state->keyloader != 0
        || state->late_entry_mi_fragment[1][7][2] != 0ULL) {
        DSD_FPRINTF(stderr, "initState did not clear keyloader/keyring/late-entry state\n");
        freeState(state);
        free(state);
        return 18;
    }
    if (state->csi_ee != 0 || state->csi_ee_key[0] != 0U || state->static_ks_bits[0][0] != 0U
        || state->static_ks_counter[0] != 0 || state->vertex_ks_key[0] != 0ULL || state->vertex_ks_bits[0][0] != 0U
        || state->vertex_ks_mod[0] != 0 || state->vertex_ks_frame_mode[0] != 0 || state->vertex_ks_frame_off[0] != 0
        || state->vertex_ks_frame_step[0] != 0 || state->vertex_ks_count != 0 || state->vertex_ks_active_idx[0] != -1
        || state->vertex_ks_counter[0] != 0 || state->vertex_ks_warned[0] != 0U) {
        DSD_FPRINTF(stderr, "initState did not clear vendor static-keystream state\n");
        freeState(state);
        free(state);
        return 19;
    }
    if (!state->dmr_soft_buf || state->dmr_soft_p != state->dmr_soft_buf + 200) {
        DSD_FPRINTF(stderr, "initState did not allocate/reset dibit soft-decision buffers\n");
        freeState(state);
        free(state);
        return 9;
    }
    for (int i = 0; i < 200; i++) {
        if (state->dmr_soft_buf[i].reliability != 0 || state->dmr_soft_buf[i].llr[0] != 0
            || state->dmr_soft_buf[i].llr[1] != 0) {
            DSD_FPRINTF(stderr, "initState did not clear dibit soft-decision buffer prefix\n");
            freeState(state);
            free(state);
            return 10;
        }
    }
    if (state->nid_corrections_total != 0 || state->nid_failures_total != 0 || state->nid_parity_overrides != 0) {
        DSD_FPRINTF(stderr, "expected NID counters to be reset after initState\n");
        freeState(state);
        free(state);
        return 3;
    }
    if (state->ess_b[0][95] != 0 || state->ess_b_llr[1][95] != 0 || state->fourv_counter[0] != 0
        || state->fourv_counter[1] != 0) {
        DSD_FPRINTF(stderr, "initState did not clear P25P2 ESS fragment state\n");
        freeState(state);
        free(state);
        return 11;
    }
    if (state->p25_p1_soft_hamming_ok != 0U) {
        DSD_FPRINTF(stderr, "initState did not clear P25P1 soft Hamming telemetry\n");
        freeState(state);
        free(state);
        return 12;
    }
    if (state->data_header_dd_format[0] != 0U || state->data_header_dd_format[1] != 0U
        || state->data_header_bit_padding[0] != 0U || state->data_header_bit_padding[1] != 0U) {
        DSD_FPRINTF(stderr, "initState did not clear transient DMR short-data metadata\n");
        freeState(state);
        free(state);
        return 28;
    }
    if (state->p25_last_cc_msg_time != 0 || !isfinite(state->p25_last_cc_msg_time_m)
        || fabs(state->p25_last_cc_msg_time_m) > 1e-12) {
        DSD_FPRINTF(stderr, "initState did not clear P25 decoded control-channel timestamps\n");
        freeState(state);
        free(state);
        return 27;
    }
    for (int i = 0; i < 2; i++) {
        if (state->p25_mac_frag[i].active != 0U || state->p25_mac_frag[i].opcode != 0U
            || state->p25_mac_frag[i].data_len != 0U || state->p25_mac_frag[i].collected != 0U
            || state->p25_mac_frag[i].data[0] != 0U || state->p25_mac_frag[i].data[5] != 0U) {
            DSD_FPRINTF(stderr, "initState did not clear P25P2 MAC fragment state\n");
            freeState(state);
            free(state);
            return 26;
        }
    }

    // Sparse trunk map bookkeeping must reset before any first live grant arrives.
    if (state->trunk_chan_map_used_count != 0U || state->trunk_chan_map[0x0123] != 0
        || state->trunk_chan_map_seq != 0U) {
        DSD_FPRINTF(stderr, "initState did not clear trunk channel-map sparse state\n");
        freeState(state);
        free(state);
        return 4;
    }

    if (state->p25_enc_tg_cache_next != 0U) {
        DSD_FPRINTF(stderr, "initState did not reset P25 encrypted TG cache cursor\n");
        freeState(state);
        free(state);
        return 24;
    }
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        if (state->p25_enc_tg_cache_until[i] != 0 || state->p25_enc_tg_cache_tg[i] != 0U
            || state->p25_enc_tg_cache_is_group[i] != 0U) {
            DSD_FPRINTF(stderr, "initState did not reset P25 encrypted TG cache\n");
            freeState(state);
            free(state);
            return 25;
        }
    }

    if (state->rtl_symbol_cache_pos != 0 || state->rtl_symbol_cache_len != 0 || state->rtl_symbol_cache_output_kind != 0
        || state->rtl_symbol_cache_channel_profile != 0 || state->rtl_symbol_cache_symbol_rate_hz != 0
        || state->rtl_symbol_cache_levels != 0 || state->rtl_symbol_cache_generation != 0U
        || state->rtl_symbol_cache_published_pending != 0 || state->rtl_fsk_sps_num != 0 || state->rtl_fsk_sps_den != 0
        || state->rtl_fsk_sps_accum != 0 || state->p25_cqpsk_dibit_map_idx != DSD_P25_CQPSK_DIBIT_MAP_IDENTITY
        || state->rtl_symbol_cache[0] != 0.0f || state->rtl_symbol_cache[DSD_RTL_SYMBOL_CACHE_CAP - 1] != 0.0f) {
        DSD_FPRINTF(stderr, "initState did not clear RTL symbol cache state\n");
        freeState(state);
        free(state);
        return 5;
    }

    // Rolling min/max helpers are initialized state consumers, not raw fields.
    for (int i = 0; i < 4; i++) {
        state->minbuf[i] = (float)(-10 - i);
        state->maxbuf[i] = (float)(10 + i);
    }
    state->midx = 1;
    dsd_state_invalidate_minmax_sums(state);
    dsd_state_push_minmax_window(state, 4, -20.0f, 20.0f);
    if (state->midx != 2 || state->minmax_sum_window != 4 || state->min != -13.75f || state->max != 13.75f) {
        DSD_FPRINTF(stderr, "min/max rolling window mismatch: midx=%d window=%d min=%.2f max=%.2f\n", state->midx,
                    state->minmax_sum_window, (double)state->min, (double)state->max);
        freeState(state);
        free(state);
        return 6;
    }

    // Sparse channel-map insertion and removal keep the used-index set compact.
    dsd_state_set_trunk_chan_freq(state, 0x1234U, 851500000L);
    dsd_state_set_trunk_chan_freq(state, 0x0123U, 851000000L);
    dsd_state_set_trunk_chan_freq(state, 0U, 769006250L);
    if (state->trunk_chan_map_used_count != 2U || state->trunk_chan_map_used[0] != 0x0123U
        || state->trunk_chan_map_used[1] != 0x1234U || state->trunk_chan_map[0x1234] != 851500000L
        || state->trunk_chan_map[0] != 769006250L) {
        DSD_FPRINTF(stderr, "trunk channel-map sparse index mismatch\n");
        freeState(state);
        free(state);
        return 7;
    }

    dsd_state_set_trunk_chan_freq(state, 0x1234U, 0);
    if (state->trunk_chan_map_used_count != 1U || state->trunk_chan_map_used[0] != 0x0123U
        || state->trunk_chan_map[0x1234] != 0) {
        DSD_FPRINTF(stderr, "trunk channel-map sparse removal mismatch\n");
        freeState(state);
        free(state);
        return 8;
    }

    state->samplesPerSymbol = 10;
    state->symbolCenter = 5;
    dsd_state_rescale_symbol_timing(state, 48000, 43200);
    if (state->samplesPerSymbol != 9 || state->symbolCenter != 5) {
        DSD_FPRINTF(stderr, "symbol timing half-up rounding mismatch: sps=%d center=%d\n", state->samplesPerSymbol,
                    state->symbolCenter);
        freeState(state);
        free(state);
        return 13;
    }

    freeState(state);
    free(state);
    return 0;
}
