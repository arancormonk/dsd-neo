// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dsd-neo/core/enc_lockout.h"
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
    /* Issue #575: no replay centre until a replay sample reaches the decoder. */
    if (opts.iq_replay_center_freq != 0U) {
        DSD_FPRINTF(stderr, "initOpts did not clear iq_replay_center_freq\n");
        return 22;
    }
    /* Issue #637: no receive filter and an empty naming table, whatever the storage held before. */
    for (int kind = 0; kind < DSD_RECEIVE_FILTER_COUNT; kind++) {
        if (opts.receive.filters[kind].enabled != 0) {
            DSD_FPRINTF(stderr, "initOpts left receive filter %d enabled\n", kind);
            return 23;
        }
    }
    if (opts.receive.names.count != 0U) {
        DSD_FPRINTF(stderr, "initOpts left a receive naming table\n");
        return 24;
    }
    return 0;
}

/* Issue #572: initOpts() and initState() seed their decode-domain "now" stamps through the one rebase list the engine
   reruns when it moves the decode clock onto a replay's capture clock and back, so each restamps exactly what the other
   seeds. Driven through the TEST source. */
static int
test_init_stamps_and_rebase_share_one_list(void) {
    static dsd_opts opts;
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 30;
    }
    const time_t seeded = 1000;
    const time_t rebased = 5000;
    dsd_decode_clock_use_test((uint64_t)seeded * 1000000000ULL);
    initOpts(&opts);
    initState(state);
    int rc = 0;
    if (opts.symbol_out_file_creation_time != seeded || state->last_cc_sync_time != seeded
        || state->last_vc_sync_time != seeded || state->last_t3_tune_time != seeded) {
        DSD_FPRINTF(stderr, "init did not seed its decode stamps from the decode clock\n");
        rc = 31;
    }
    state->p25_last_cc_msg_time = 77;
    state->p25_last_vc_tune_time = 78;
    dsd_decode_clock_test_set_ns((uint64_t)rebased * 1000000000ULL);
    dsd_state_rebase_decode_timestamps(&opts, state);
    if (rc == 0
        && (opts.symbol_out_file_creation_time != rebased || state->last_cc_sync_time != rebased
            || state->last_vc_sync_time != rebased || state->last_t3_tune_time != rebased)) {
        DSD_FPRINTF(stderr, "the rebase missed a stamp init seeds\n");
        rc = 32;
    }
    /* Stamps init leaves unset (0, "never") are decode state, not init's, and stay. */
    if (rc == 0 && (state->p25_last_cc_msg_time != 77 || state->p25_last_vc_tune_time != 78)) {
        DSD_FPRINTF(stderr, "the rebase touched a stamp init does not seed\n");
        rc = 33;
    }
    /* Either side alone. */
    dsd_decode_clock_test_set_ns(9000ULL * 1000000000ULL);
    dsd_state_rebase_decode_timestamps(NULL, state);
    dsd_state_rebase_decode_timestamps(NULL, NULL);
    if (rc == 0 && (state->last_vc_sync_time != 9000 || opts.symbol_out_file_creation_time != rebased)) {
        DSD_FPRINTF(stderr, "a state-only rebase did not restamp the state alone\n");
        rc = 34;
    }
    dsd_state_rebase_decode_timestamps(&opts, NULL);
    if (rc == 0 && opts.symbol_out_file_creation_time != 9000) {
        DSD_FPRINTF(stderr, "an opts-only rebase did not restamp the options\n");
        rc = 35;
    }
    dsd_decode_clock_use_system();
    freeState(state);
    free(state);
    return rc;
}

/* Issue #673: the per-call WAV open stamps start at 0 (none opened yet). A WAV opened on the system clock before the
   engine moved the decode clock back onto a replay's capture (-P opens while the arguments are parsed) has recorded
   nothing yet, so the rebase pulls its stamp back to now; a stamp behind now is a real open time and stays. */
static int
test_rebase_pulls_wav_open_stamps_back_to_now(void) {
    static dsd_opts opts;
    dsd_decode_clock_use_test(5000ULL * 1000000000ULL);
    opts.wav_out_open_time = 1;
    opts.wav_out_open_timeR = 2;
    initOpts(&opts);
    int rc = 0;
    if (opts.wav_out_open_time != 0 || opts.wav_out_open_timeR != 0) {
        DSD_FPRINTF(stderr, "initOpts left a per-call WAV open stamp\n");
        rc = 40;
    }
    opts.wav_out_open_time = 9000;
    opts.wav_out_open_timeR = 4000;
    dsd_state_rebase_decode_timestamps(&opts, NULL);
    if (rc == 0 && (opts.wav_out_open_time != 5000 || opts.wav_out_open_timeR != 4000)) {
        DSD_FPRINTF(stderr, "the rebase did not pull an open stamp ahead of now back, or moved one behind it\n");
        rc = 41;
    }
    dsd_decode_clock_use_system();
    return rc;
}

/* Each initState() ring has an identity of its own, nonzero and shared by its two slots, and keeps it through a reset
   of its rows (its push stamps go on counting there). The Qt call history tells by it a capture's rows decoded again
   in a fresh state from the rows it already read. */
static int
test_init_state_draws_a_ring_identity(void) {
    dsd_state* a = (dsd_state*)calloc(1, sizeof(*a));
    dsd_state* b = (dsd_state*)calloc(1, sizeof(*b));
    int rc = 0;
    if (!a || !b) {
        free(a);
        free(b);
        return 40;
    }
    initState(a);
    initState(b);
    if (!a->event_history_s || !b->event_history_s) {
        rc = 41;
    } else if (a->engine_fresh != 1U) {
        /* The engine moves the decode clock onto a replay's capture clock only on a state no run has used. */
        DSD_FPRINTF(stderr, "initState did not mark the state fresh for a run\n");
        rc = 45;
    } else if (a->event_history_s[0].instance == 0U
               || a->event_history_s[1].instance != a->event_history_s[0].instance) {
        DSD_FPRINTF(stderr, "a ring has no identity, or its slots disagree on it\n");
        rc = 42;
    } else if (b->event_history_s[0].instance == a->event_history_s[0].instance) {
        DSD_FPRINTF(stderr, "two states drew the same ring identity\n");
        rc = 43;
    } else {
        const uint64_t instance = a->event_history_s[0].instance;
        dsd_event_history_reset(a);
        if (a->event_history_s[0].instance != instance || a->event_history_s[1].instance != instance) {
            DSD_FPRINTF(stderr, "a reset of the rows changed the ring's identity\n");
            rc = 44;
        }
    }
    freeState(a);
    freeState(b);
    free(a);
    free(b);
    return rc;
}

/* Issue #672: every initState() draws a new state instance and a new LCN list revision from the process-wide counter,
   so a frontend copy keyed on them never takes a re-initialised or replacement state's data for its own; it starts the
   trunk-scan avoid list empty and publishable, and frees one a previous run left. */
static int
test_init_state_stamps_a_new_instance(void) {
    dsd_state* a = (dsd_state*)calloc(1, sizeof(*a));
    dsd_state* b = (dsd_state*)calloc(1, sizeof(*b));
    if (!a || !b) {
        free(a);
        free(b);
        return 50;
    }
    int rc = 0;
    initState(a);
    initState(b);
    const uint64_t first = a->state_instance;
    const uint64_t first_list = a->lcn_list_rev;
    if (first == 0U || a->lcn_list_rev == 0U || a->lcn_avoid_rev == 0U || b->state_instance == first
        || b->lcn_list_rev == first_list) {
        DSD_FPRINTF(stderr, "initState did not draw a distinct instance and list revision per state\n");
        rc = 51;
    } else if (a->trunk_scan_avoids_ok != 1U || a->lcn_avoid_total != 0U || a->trunk_scan_avoid_count != 0U
               || a->init_failed != 0U) {
        DSD_FPRINTF(stderr, "initState did not start the avoid stores empty and publishable\n");
        rc = 52;
    }
    /* What a previous run left on the state: its buffers freed, but a published list and spent flags still there. */
    freeState(a);
    const dsd_trunk_scan_avoid_entry entry = {.ordinal = 2U, .frequency_hz = 851012500U, .id = "B"};
    if (rc == 0 && dsd_state_trunk_scan_avoids_set(a, &entry, 1U) != 0) {
        rc = 53;
    }
    a->trunk_scan_avoids_ok = 0U;
    a->lcn_avoid_total = 3U;
    initState(a);
    if (rc == 0
        && (a->state_instance == first || a->state_instance == b->state_instance || a->lcn_list_rev == first_list)) {
        DSD_FPRINTF(stderr, "a re-initialised state kept or reused an earlier instance\n");
        rc = 54;
    } else if (rc == 0
               && (a->trunk_scan_avoids != NULL || a->trunk_scan_avoid_count != 0U || a->trunk_scan_avoids_ok != 1U
                   || a->lcn_avoid_total != 0U)) {
        DSD_FPRINTF(stderr, "a re-initialised state kept the previous run's avoid list\n");
        rc = 55;
    }
    freeState(a);
    freeState(b);
    free(a);
    free(b);
    return rc;
}

/* initState()'s checked allocations in the order it makes them, with what a failure of each must leave behind. */
typedef struct {
    const char* name;
    int required;
} init_alloc_site;

static const init_alloc_site k_init_alloc_sites[] = {
    {"dibit_buf", 1},
    {"dmr_payload_buf", 1},
    {"dmr_soft_buf", 0},
    {"audio_out_buf", 1},
    {"audio_out_bufR", 1},
    {"audio_out_float_buf", 1},
    {"audio_out_float_bufR", 1},
    {"cur_mp", 1},
    {"prev_mp", 1},
    {"prev_mp_enhanced", 1},
    {"cur_mp2", 1},
    {"prev_mp2", 1},
    {"prev_mp_enhanced2", 1},
    {"event_history_s", 0},
};
#define INIT_ALLOC_SITES ((long)(sizeof(k_init_alloc_sites) / sizeof(k_init_alloc_sites[0])))

/* The first checked allocation of @p st that is NULL, in initState()'s order, or -1 when none. A buffer and its cursor
   that disagree (one NULL, the other not) report that buffer's index plus INIT_ALLOC_SITES. */
static long
init_alloc_failed_site(const dsd_state* st) {
    const void* bufs[] = {
        st->dibit_buf,      st->dmr_payload_buf,     st->dmr_soft_buf,         st->audio_out_buf,
        st->audio_out_bufR, st->audio_out_float_buf, st->audio_out_float_bufR,
    };
    const void* cursors[] = {
        st->dibit_buf_p,
        st->dmr_payload_p,
        st->dmr_soft_p,
        st->audio_out_buf_p,
        st->audio_out_buf_pR,
        st->audio_out_float_buf_p,
        st->audio_out_float_buf_pR,
    };
    long failed = -1;
    for (long i = 0; i < 7; i++) {
        if ((bufs[i] == NULL) != (cursors[i] == NULL)) {
            return i + INIT_ALLOC_SITES;
        }
        if (bufs[i] == NULL && failed < 0) {
            failed = i;
        }
    }
    const void* mbe[] = {
        st->cur_mp, st->prev_mp, st->prev_mp_enhanced, st->cur_mp2, st->prev_mp2, st->prev_mp_enhanced2,
    };
    for (long i = 0; i < 6; i++) {
        if (mbe[i] == NULL && failed < 0) {
            failed = 7 + i;
        }
    }
    if (st->event_history_s == NULL && failed < 0) {
        failed = 13;
    }
    return failed;
}

/* Issue #672 B16: each of initState()'s checked allocations fails on its own. A required buffer marks the state
   init_failed; the optional DMR soft buffer and event history leave it usable, the history NULL rather than a bogus
   slot pointer; no cursor is built over a failed buffer; and freeState() releases what was allocated (ASan). */
static int
test_init_state_survives_each_allocation_failure(void) {
    for (long site = 0; site < INIT_ALLOC_SITES; site++) {
        dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
        if (!st) {
            return 60;
        }
        dsd_init_test_alloc_fail_after(site);
        initState(st);
        dsd_init_test_alloc_fail_after(-1);
        dsd_exitflag_store(0);
        const long failed = init_alloc_failed_site(st);
        const init_alloc_site* expect = &k_init_alloc_sites[site];
        int rc = 0;
        /* A vocoder parameter failure frees all six, so the first of them reads as the one that failed. */
        const long want = (site >= 7 && site <= 12) ? 7 : site;
        if (failed != want) {
            DSD_FPRINTF(stderr, "%s failure left site %ld failed\n", expect->name, failed);
            rc = 61;
        } else if ((st->init_failed != 0U) != (expect->required != 0)) {
            DSD_FPRINTF(stderr, "%s failure set init_failed=%u\n", expect->name, (unsigned)st->init_failed);
            rc = 62;
        }
        freeState(st);
        free(st);
        if (rc != 0) {
            return rc;
        }
    }
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!st) {
        return 60;
    }
    initState(st);
    const int clean = init_alloc_failed_site(st) == -1 && st->init_failed == 0U;
    freeState(st);
    free(st);
    return clean ? 0 : 63;
}

int
main(void) {
    int instance_rc = test_init_state_stamps_a_new_instance();
    if (instance_rc != 0) {
        return instance_rc;
    }
    int alloc_rc = test_init_state_survives_each_allocation_failure();
    if (alloc_rc != 0) {
        return alloc_rc;
    }
    int ring_rc = test_init_state_draws_a_ring_identity();
    if (ring_rc != 0) {
        return ring_rc;
    }
    int init_opts_rc = test_init_opts_clears_trunk_scan_targets_csv();
    if (init_opts_rc != 0) {
        return init_opts_rc;
    }
    int rebase_rc = test_init_stamps_and_rebase_share_one_list();
    if (rebase_rc != 0) {
        return rebase_rc;
    }
    int wav_open_rc = test_rebase_pulls_wav_open_stamps_back_to_now();
    if (wav_open_rc != 0) {
        return wav_open_rc;
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
    state->p25_p1_accepted_frames = 71U;
    state->p25_p1_clean_frames = 72U;
    state->p25_p1_corrected_frames = 73U;
    state->p25_p1_concealed_frames = 74U;
    state->p25_p1_accepted_corrections = 75U;
    state->p25_p1_suppressed_tail_frames = 76U;
    state->p25_p1_excluded_tail_corrections = 77U;
    state->trunk_chan_map[0x0123] = 851000000L;
    state->trunk_chan_map_used[0] = 0x0123U;
    state->trunk_chan_map_used_count = 1U;
    state->trunk_chan_map_seq = 99U;
    state->enc_lockout_key_epoch = 42U;
    for (int i = 0; i < DSD_ENC_LOCKOUT_MAX; i++) {
        state->enc_lockout_entries[i].in_use = 1U;
        state->enc_lockout_entries[i].target = (uint32_t)(2400 + i);
        state->enc_lockout_entries[i].is_group = (uint8_t)(i % 2);
        state->enc_lockout_entries[i].key_epoch = 42U;
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
    state->rtl_symbol_cache_media_start_ns = 123456789ULL;
    state->rtl_symbol_cache_media_duration_ns = 21333333ULL;
    state->rtl_symbol_cache_media_count = 512U;
    state->rtl_symbol_cache_media_first_index = 7U;
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
    state->dmr_lrrp_target[0] = 1234U;
    state->dmr_lrrp_target[1] = 5678U;
    state->dmr_data_target_is_group[0] = 1U;
    state->dmr_data_target_is_group[1] = 1U;
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
    state->p25_p1_crypto_conflict.active = 1U;
    state->p25_p1_crypto_conflict.algid = 0xA0U;
    state->p25_p1_crypto_conflict.keyid = 0x0064U;
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
        || state->p25_p1_hdu_crypto_fresh != 0 || state->p25_p1_crypto_conflict.active != 0U
        || state->p25_p1_crypto_conflict.algid != 0U || state->p25_p1_crypto_conflict.keyid != 0U) {
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
    if (state->p25_p1_accepted_frames != 0U || state->p25_p1_clean_frames != 0U || state->p25_p1_corrected_frames != 0U
        || state->p25_p1_concealed_frames != 0U || state->p25_p1_accepted_corrections != 0U
        || state->p25_p1_suppressed_tail_frames != 0U || state->p25_p1_excluded_tail_corrections != 0U) {
        DSD_FPRINTF(stderr, "expected P25P1 session voice counters to be reset after initState\n");
        freeState(state);
        free(state);
        return 29;
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
    /*
     * dmr_data_target_is_group[] qualifies dmr_lrrp_target[] for the --dmr-tg-key-csv lookup, so a
     * stale group flag surviving a target reset would qualify whatever target is written next.
     * Both slots are seeded above so a slot-0-only reset still fails here.
     */
    if (state->dmr_lrrp_target[0] != 0U || state->dmr_lrrp_target[1] != 0U || state->dmr_data_target_is_group[0] != 0U
        || state->dmr_data_target_is_group[1] != 0U) {
        DSD_FPRINTF(stderr, "initState did not clear the DMR data target and its group flag\n");
        freeState(state);
        free(state);
        return 29;
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

    if (state->enc_lockout_key_epoch != 1U) {
        DSD_FPRINTF(stderr, "initState did not reset the enc lockout key epoch\n");
        freeState(state);
        free(state);
        return 24;
    }
    for (int i = 0; i < DSD_ENC_LOCKOUT_MAX; i++) {
        if (state->enc_lockout_entries[i].in_use != 0U || state->enc_lockout_entries[i].target != 0U
            || state->enc_lockout_entries[i].is_group != 0U) {
            DSD_FPRINTF(stderr, "initState did not reset the enc lockout ledger\n");
            freeState(state);
            free(state);
            return 25;
        }
    }

    if (state->rtl_symbol_cache_pos != 0 || state->rtl_symbol_cache_len != 0 || state->rtl_symbol_cache_output_kind != 0
        || state->rtl_symbol_cache_channel_profile != 0 || state->rtl_symbol_cache_symbol_rate_hz != 0
        || state->rtl_symbol_cache_levels != 0 || state->rtl_symbol_cache_generation != 0U
        || state->rtl_symbol_cache_published_pending != 0 || state->rtl_symbol_cache_media_start_ns != 0U
        || state->rtl_symbol_cache_media_duration_ns != 0U || state->rtl_symbol_cache_media_count != 0U
        || state->rtl_symbol_cache_media_first_index != 0U || state->rtl_fsk_sps_num != 0 || state->rtl_fsk_sps_den != 0
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
