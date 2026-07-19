// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Basic tests for the unified P25 state machine.
 * 4-state model: IDLE, ON_CC, TUNED, HUNTING
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_opts g_opts;
static dsd_state g_state;
static int g_return_requests;

static dsd_trunk_tune_result
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    return freq > 0 ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    g_return_requests++;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static void
reset_test_state(void) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_return_requests = 0;
    g_opts.trunk_enable = 1;
    g_opts.trunk_hangtime = 2.0f; // op25 TGID_HOLD_TIME
    g_opts.trunk_tune_group_calls = 1;
    g_opts.verbose = 0;
    g_state.p25_cc_freq = 851000000; // Fake CC freq
}

static void
expire_traffic_hang(p25_sm_ctx_t* ctx) {
    ctx->t_hangtime_m = dsd_time_now_monotonic_s() - ctx->config.hangtime_s - 0.1;
    p25_sm_tick_ctx(ctx, &g_opts, &g_state);
}

static int
seed_exact(uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;

    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    return dsd_tg_policy_upsert_exact(&g_state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

// Test: Init sets correct initial state
static int
test_init_with_cc(void) {
    reset_test_state();
    p25_sm_ctx_t ctx;

    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    if (ctx.state != P25_SM_ON_CC) {
        DSD_FPRINTF(stderr, "FAIL: Expected ON_CC, got %s\n", p25_sm_state_name(ctx.state));
        return 1;
    }
    if (!ctx.initialized) {
        DSD_FPRINTF(stderr, "FAIL: Expected initialized=1\n");
        return 1;
    }
    return 0;
}

// Test: Init without CC sets IDLE
static int
test_init_without_cc(void) {
    reset_test_state();
    g_state.p25_cc_freq = 0; // No CC known
    p25_sm_ctx_t ctx;

    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    if (ctx.state != P25_SM_IDLE) {
        DSD_FPRINTF(stderr, "FAIL: Expected IDLE, got %s\n", p25_sm_state_name(ctx.state));
        return 1;
    }
    return 0;
}

// Test: Grant transitions to TUNED
static int
test_grant_to_tuned(void) {
    reset_test_state();
    // Set up a channel->freq mapping so grant can compute frequency
    g_state.trunk_chan_map[0x1234] = 851500000;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // In 4-state model, grant goes to TUNED (which includes armed/following/hangtime)
    if (ctx.state != P25_SM_TUNED) {
        DSD_FPRINTF(stderr, "FAIL: Expected TUNED after grant, got %s\n", p25_sm_state_name(ctx.state));
        return 1;
    }
    if (ctx.vc_freq_hz != 851500000) {
        DSD_FPRINTF(stderr, "FAIL: Expected vc_freq_hz=851500000, got %ld\n", ctx.vc_freq_hz);
        return 1;
    }
    if (ctx.vc_tg != 1000) {
        DSD_FPRINTF(stderr, "FAIL: Expected vc_tg=1000, got %d\n", ctx.vc_tg);
        return 1;
    }
    return 0;
}

// Test: PTT sets voice_active in TUNED state
static int
test_ptt_voice_active(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    // Grant
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // PTT
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // Still in TUNED state (now unified)
    if (ctx.state != P25_SM_TUNED) {
        DSD_FPRINTF(stderr, "FAIL: Expected TUNED after PTT, got %s\n", p25_sm_state_name(ctx.state));
        return 1;
    }
    if (ctx.slots[0].voice_active != 1) {
        DSD_FPRINTF(stderr, "FAIL: Expected slot[0].voice_active=1\n");
        return 1;
    }
    return 0;
}

static int
test_private_ptt_preserves_grant_identity(void) {
    reset_test_state();
    g_opts.trunk_tune_private_calls = 1;
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    p25_sm_event_t ev = p25_sm_ev_indiv_grant(0x1234, 851500000, 0xABCDEF, 0x010203, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    ev = p25_sm_ev_ptt_call(0, 0, 0, 0x010203, 1, P25_SM_SVC_UNKNOWN);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (!ctx.slots[0].voice_active || ctx.slots[0].is_group || ctx.slots[0].dst != 0xABCDEF
        || ctx.slots[0].target_id != 0xABCDEF || g_state.gi[0] != 1 || g_state.lasttg != 0xABCDEF) {
        DSD_FPRINTF(stderr, "FAIL: Zero-address MAC_PTT did not retain the private grant identity\n");
        return 1;
    }

    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 0x4567, 0, 0x010203, 1, P25_SM_SVC_UNKNOWN);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (!ctx.slots[0].voice_active || ctx.slots[0].is_group || ctx.slots[0].dst != 0xABCDEF
        || ctx.slots[0].target_id != 0xABCDEF || g_state.gi[0] != 1 || g_state.lasttg != 0xABCDEF) {
        DSD_FPRINTF(stderr, "FAIL: Nonzero MAC_PTT group field replaced the private grant identity\n");
        return 1;
    }

    ev = p25_sm_ev_end_call_at(0, 0x4567, 0x010203, ctx.slots[0].last_active_m + 0.001);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.slots[0].voice_active || !ctx.slots[0].grant_active || ctx.slots[0].last_end_m <= 0.0
        || ctx.t_hangtime_m <= 0.0) {
        DSD_FPRINTF(stderr, "FAIL: Private END treated the MAC group field as the destination identity\n");
        return 1;
    }
    return 0;
}

static int
test_p1_hdu_crypto_survives_first_active(void) {
    const struct {
        int algid;
        int keyid;
        uint64_t mi;
        dsd_p25_crypto_state expected;
    } cases[] = {
        {0x80, 0x1234, UINT64_C(0x0102030405060708), DSD_P25_CRYPTO_CLEAR},
        {0x81, 0x5678, UINT64_C(0x1112131415161718), DSD_P25_CRYPTO_DECRYPTABLE},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_test_state();
        g_state.trunk_chan_map[0x1234] = 851500000;
        g_state.R = UINT64_C(0x0123456789ABCDEF);

        p25_sm_ctx_t ctx;
        p25_sm_init_ctx(&ctx, &g_opts, &g_state);
        p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0x40);
        p25_sm_event(&ctx, &g_opts, &g_state, &ev);
        if (ctx.vc_is_tdma || g_state.p25_crypto_state[0] != DSD_P25_CRYPTO_ENCRYPTED_PENDING) {
            DSD_FPRINTF(stderr, "FAIL: Phase 1 HDU preservation setup did not start pending\n");
            return 1;
        }

        if (p25_crypto_resolve(NULL, &g_state, DSD_P25_CRYPTO_PHASE1, 0, cases[i].algid, cases[i].keyid, cases[i].mi,
                               1000)
            != cases[i].expected) {
            DSD_FPRINTF(stderr, "FAIL: Synthetic HDU metadata did not resolve authoritatively\n");
            return 1;
        }

        ev = p25_sm_ev_active(0);
        p25_sm_event(&ctx, &g_opts, &g_state, &ev);
        if (!ctx.slots[0].voice_active || g_state.p25_crypto_state[0] != cases[i].expected
            || g_state.payload_algid != cases[i].algid || g_state.payload_keyid != cases[i].keyid
            || g_state.payload_miP != cases[i].mi || ctx.slots[0].crypto_attempt_m > 0.0) {
            DSD_FPRINTF(stderr, "FAIL: First Phase 1 ACTIVE discarded authoritative HDU crypto metadata\n");
            return 1;
        }
    }
    return 0;
}

static int
test_conventional_end_is_follower_noop(void) {
    reset_test_state();
    g_opts.trunk_enable = 0;
    g_state.p25_cc_freq = 0;
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    const double now_m = dsd_time_now_monotonic_s();

    if (!p25_sm_emit_end_call_at(&g_opts, &g_state, 0, 1000, 123, now_m)) {
        DSD_FPRINTF(stderr, "FAIL: Conventional SACCH END was rejected by the trunk follower\n");
        return 1;
    }
    if (p25_sm_emit_facch_end_call_at(&g_opts, &g_state, 1, 2000, 456, now_m) != P25_SM_END_APPLIED) {
        DSD_FPRINTF(stderr, "FAIL: Conventional FACCH END was rejected by the trunk follower\n");
        return 1;
    }
    if (ctx->state != P25_SM_IDLE || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Conventional END follower no-op changed trunk state\n");
        return 1;
    }
    return 0;
}

// Test: END closes one transmission but retains the traffic allocation until
// its inactivity hang expires.
static int
test_end_clears_voice(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    // Grant -> PTT -> END
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    if (ctx.state != P25_SM_TUNED || ctx.t_hangtime_m <= 0.0 || !ctx.slots[0].grant_active) {
        DSD_FPRINTF(stderr, "FAIL: Expected retained TUNED allocation after END, got %s\n",
                    p25_sm_state_name(ctx.state));
        return 1;
    }
    if (ctx.slots[0].voice_active != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected slot[0].voice_active=0 after END\n");
        return 1;
    }
    if (g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: END requested a control-channel return\n");
        return 1;
    }

    expire_traffic_hang(&ctx);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Hang expiry did not release exactly once\n");
        return 1;
    }
    return 0;
}

static int
test_tdma_boundaries_only_hang_after_last_assigned_voice(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    ev = p25_sm_ev_idle(1);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.vc_activity_seen || ctx.t_hangtime_m > 0.0 || !ctx.slots[0].grant_active) {
        DSD_FPRINTF(stderr, "FAIL: Unassigned companion IDLE manufactured traffic activity or hang\n");
        return 1;
    }

    const double stale_m = dsd_time_now_monotonic_s() - ctx.config.grant_timeout_s - 0.1;
    ctx.t_tune_m = stale_m;
    ctx.slots[0].last_grant_m = stale_m;
    p25_sm_tick_ctx(&ctx, &g_opts, &g_state);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Unassigned companion IDLE suppressed the grant timeout\n");
        return 1;
    }

    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 2000, 456, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(1, 2000, 0, 456, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    ev = p25_sm_ev_hangtime(1);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (!ctx.slots[0].voice_active || ctx.slots[1].voice_active || ctx.t_hangtime_m > 0.0) {
        DSD_FPRINTF(stderr, "FAIL: Ending one assigned slot armed hang while its companion remained active\n");
        return 1;
    }

    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.t_hangtime_m <= 0.0 || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Last assigned voice transition did not arm traffic hang\n");
        return 1;
    }
    return 0;
}

static int
test_anonymous_followup_restarts_crypto_pending(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (g_state.p25_crypto_state[0] != DSD_P25_CRYPTO_CLEAR || !ctx.slots[0].voice_active) {
        DSD_FPRINTF(stderr, "FAIL: Initial clear assignment did not enter clear voice\n");
        return 1;
    }

    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_active(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (g_state.p25_crypto_state[0] != DSD_P25_CRYPTO_ENCRYPTED_PENDING || ctx.slots[0].svc_bits != P25_SM_SVC_UNKNOWN
        || ctx.slots[0].voice_active || g_state.p25_p2_audio_allowed[0] != 0 || ctx.slots[0].crypto_attempt_m <= 0.0) {
        DSD_FPRINTF(stderr, "FAIL: Anonymous follow-up inherited stale clear crypto classification\n");
        return 1;
    }
    return 0;
}

static int
test_unassigned_companion_start_is_rejected(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    const double hang_started_m = ctx.t_hangtime_m;

    ev = p25_sm_ev_active_call(1, 2000, 0, 456, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.slots[1].voice_active || fabs(ctx.t_hangtime_m - hang_started_m) > 1.0e-9 || ctx.state != P25_SM_TUNED
        || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Unassigned companion ACTIVE bypassed routing policy or canceled hang\n");
        return 1;
    }
    return 0;
}

// Test: State name function for 4-state model
static int
test_state_names(void) {
    if (strcmp(p25_sm_state_name(P25_SM_IDLE), "IDLE") != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected 'IDLE'\n");
        return 1;
    }
    if (strcmp(p25_sm_state_name(P25_SM_ON_CC), "ON_CC") != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected 'ON_CC'\n");
        return 1;
    }
    if (strcmp(p25_sm_state_name(P25_SM_TUNED), "TUNED") != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected 'TUNED'\n");
        return 1;
    }
    if (strcmp(p25_sm_state_name(P25_SM_HUNTING), "HUNT") != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected 'HUNT'\n");
        return 1;
    }
    return 0;
}

// Test: Config defaults
static int
test_config_defaults(void) {
    reset_test_state();
    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    // Check defaults (aligned with op25 timing parameters)
    if (ctx.config.hangtime_s != 2.0) {
        DSD_FPRINTF(stderr, "FAIL: Expected hangtime_s=2.0 (op25 TGID_HOLD_TIME), got %.2f\n", ctx.config.hangtime_s);
        return 1;
    }
    if (ctx.config.grant_timeout_s != 3.0) {
        DSD_FPRINTF(stderr, "FAIL: Expected grant_timeout_s=3.0 (op25 TSYS_HOLD_TIME), got %.2f\n",
                    ctx.config.grant_timeout_s);
        return 1;
    }
    if (ctx.config.cc_grace_s != 5.0) {
        DSD_FPRINTF(stderr, "FAIL: Expected cc_grace_s=5.0 (op25 CC_HUNT_TIME), got %.2f\n", ctx.config.cc_grace_s);
        return 1;
    }
    return 0;
}

// Test: Singleton access
static int
test_singleton(void) {
    p25_sm_ctx_t* sm1 = p25_sm_get_ctx();
    p25_sm_ctx_t* sm2 = p25_sm_get_ctx();

    if (sm1 != sm2) {
        DSD_FPRINTF(stderr, "FAIL: Singleton should return same pointer\n");
        return 1;
    }
    if (!sm1->initialized) {
        DSD_FPRINTF(stderr, "FAIL: Singleton should be initialized\n");
        return 1;
    }
    return 0;
}

// Test: SACCH slot mapping helper
static int
test_sacch_slot_mapping(void) {
    // SACCH uses inverted slot mapping
    if (p25_sacch_to_voice_slot(0) != 1) {
        DSD_FPRINTF(stderr, "FAIL: p25_sacch_to_voice_slot(0) should be 1\n");
        return 1;
    }
    if (p25_sacch_to_voice_slot(1) != 0) {
        DSD_FPRINTF(stderr, "FAIL: p25_sacch_to_voice_slot(1) should be 0\n");
        return 1;
    }
    return 0;
}

// Test: P25P2 TDMA - END on one slot keeps TUNED if other slot still active
static int
test_tdma_partial_end_stays_tuned(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    // Mark this channel as TDMA (P25P2)
    g_state.p25_chan_tdma_explicit[1] = 2; // iden=1, explicit TDMA hint

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    // Grant on TDMA channel
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 2000, 456, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // Should be detected as TDMA
    if (!ctx.vc_is_tdma) {
        DSD_FPRINTF(stderr, "FAIL: Expected vc_is_tdma=1 for TDMA channel\n");
        return 1;
    }

    // PTT on both slots
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(1);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // Simulate audio allowed on slot 1 (slot 0 will end, slot 1 still active)
    g_state.p25_p2_audio_allowed[1] = 1;

    // END on slot 0 only
    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // Should stay TUNED because slot 1 is still active
    if (ctx.state != P25_SM_TUNED) {
        DSD_FPRINTF(stderr, "FAIL: Expected TUNED after END on slot 0 (slot 1 still active), got %s\n",
                    p25_sm_state_name(ctx.state));
        return 1;
    }

    // Now end slot 1 as well
    g_state.p25_p2_audio_allowed[1] = 0;
    ev = p25_sm_ev_end(1);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // Both transmissions have ended, but the physical allocation remains.
    if (ctx.state != P25_SM_TUNED || ctx.t_hangtime_m <= 0.0 || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected retained TDMA carrier after both ENDs, got %s\n",
                    p25_sm_state_name(ctx.state));
        return 1;
    }

    expire_traffic_hang(&ctx);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: TDMA hang expiry did not release exactly once\n");
        return 1;
    }

    return 0;
}

// Test: P25P2 TDMA - an armed grant on the other slot keeps the carrier until
// that slot ends or times out, even if it has not produced PTT/ACTIVE yet.
static int
test_tdma_pending_other_slot_blocks_release(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 1001, 124, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    if (!ctx.slots[1].grant_active || ctx.slots[1].target_id != 1001) {
        DSD_FPRINTF(stderr, "FAIL: Expected slot 1 pending grant context\n");
        return 1;
    }

    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    g_state.p25_p2_audio_allowed[0] = 1;

    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    if (ctx.state != P25_SM_TUNED) {
        DSD_FPRINTF(stderr, "FAIL: Expected TUNED while slot 1 has a pending grant, got %s\n",
                    p25_sm_state_name(ctx.state));
        return 1;
    }
    if (ctx.slots[0].grant_active != 1 || ctx.slots[1].grant_active != 1) {
        DSD_FPRINTF(stderr, "FAIL: END discarded a retained slot assignment\n");
        return 1;
    }
    if (ctx.t_hangtime_m > 0.0) {
        DSD_FPRINTF(stderr, "FAIL: Pending companion grant was placed on the shorter traffic hang timer\n");
        return 1;
    }

    double acquisition_m = dsd_time_now_monotonic_s() - ctx.config.hangtime_s - 0.1;
    ctx.t_tune_m = acquisition_m;
    ctx.slots[1].last_grant_m = acquisition_m;
    p25_sm_tick_ctx(&ctx, &g_opts, &g_state);
    if (ctx.state != P25_SM_TUNED || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Pending companion did not survive until its grant deadline\n");
        return 1;
    }

    acquisition_m = dsd_time_now_monotonic_s() - ctx.config.grant_timeout_s - 0.1;
    ctx.t_tune_m = acquisition_m;
    ctx.slots[1].last_grant_m = acquisition_m;
    p25_sm_tick_ctx(&ctx, &g_opts, &g_state);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Pending companion did not release on grant timeout\n");
        return 1;
    }

    reset_test_state();
    g_opts.trunk_tune_data_calls = 1;
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_data_grant(0x1235, 851500000, 1001, 124, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.t_hangtime_m > 0.0 || !ctx.slots[1].grant_active || !ctx.slots[1].data_call) {
        DSD_FPRINTF(stderr, "FAIL: Pending data companion was placed on the traffic hang timer\n");
        return 1;
    }
    acquisition_m = dsd_time_now_monotonic_s() - ctx.config.hangtime_s - 0.1;
    ctx.t_tune_m = acquisition_m;
    ctx.slots[1].last_grant_m = acquisition_m;
    p25_sm_tick_ctx(&ctx, &g_opts, &g_state);
    if (ctx.state != P25_SM_TUNED || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Pending data companion did not survive until its grant deadline\n");
        return 1;
    }
    acquisition_m = dsd_time_now_monotonic_s() - ctx.config.grant_timeout_s - 0.1;
    ctx.t_tune_m = acquisition_m;
    ctx.slots[1].last_grant_m = acquisition_m;
    p25_sm_tick_ctx(&ctx, &g_opts, &g_state);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Pending data companion did not release on grant timeout\n");
        return 1;
    }

    return 0;
}

// Test: P25P2 TDMA - an encrypted locked-out slot does not keep media active,
// while the retained carrier still observes normal inactivity hangtime.
static int
test_tdma_enc_lockout_slot_does_not_block_release(void) {
    reset_test_state();
    g_opts.trunk_tune_enc_calls = 0;
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 1001, 124, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(1);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    g_state.p25_p2_audio_allowed[0] = 1;
    g_state.p25_p2_audio_allowed[1] = 1;

    g_state.p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;
    ev = p25_sm_ev_enc(1, 0x84, 0x1234, 1001);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    if (ctx.state != P25_SM_TUNED || ctx.slots[1].grant_active != 0 || ctx.slots[1].voice_active != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected encrypted slot 1 to mute without keeping an active grant\n");
        return 1;
    }

    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || ctx.t_hangtime_m <= 0.0 || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected hang retention after clear slot END with encrypted slot muted, got %s\n",
                    p25_sm_state_name(ctx.state));
        return 1;
    }

    expire_traffic_hang(&ctx);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Encrypted companion case did not release on hang expiry\n");
        return 1;
    }

    return 0;
}

// Test: P25P2 TDMA - END on a single-slot call closes media and retains the
// carrier for a follow-up transmission.
static int
test_tdma_single_slot_end_retains_carrier(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    // Mark this channel as TDMA (P25P2)
    g_state.p25_chan_tdma_explicit[1] = 2; // iden=1, explicit TDMA hint

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    // Grant on TDMA channel
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // Should be detected as TDMA
    if (!ctx.vc_is_tdma) {
        DSD_FPRINTF(stderr, "FAIL: Expected vc_is_tdma=1 for TDMA channel\n");
        return 1;
    }

    // PTT on slot 0 ONLY - slot 1 never has any activity
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    // Simulate what xcch.c does: enable audio on PTT
    g_state.p25_p2_audio_allowed[0] = 1;

    // Simulate audio in the ring buffer (jitter buffer has samples)
    g_state.p25_p2_audio_ring_count[0] = 5;

    // Verify slot 1 never had activity
    if (ctx.slots[1].last_active_m != 0.0) {
        DSD_FPRINTF(stderr, "FAIL: Expected slot 1 last_active_m=0 (never active)\n");
        return 1;
    }

    // END arrives while the decoder gate and jitter ring still contain media.
    ev = p25_sm_ev_end(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    if (ctx.state != P25_SM_TUNED || ctx.t_hangtime_m <= 0.0 || !ctx.slots[0].grant_active || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected retained carrier after END on single-slot TDMA call, got %s\n",
                    p25_sm_state_name(ctx.state));
        return 1;
    }

    // Verify the SM cleared audio_allowed for slot 0
    if (g_state.p25_p2_audio_allowed[0] != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected audio_allowed[0]=0 after END, got %d\n", g_state.p25_p2_audio_allowed[0]);
        return 1;
    }

    if (g_state.p25_p2_audio_ring_count[0] != 0) {
        DSD_FPRINTF(stderr, "FAIL: Expected slot 0 jitter ring cleanup at END\n");
        return 1;
    }

    expire_traffic_hang(&ctx);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Single-slot hang expiry did not release exactly once\n");
        return 1;
    }

    return 0;
}

// Test: identified MAC_END_PTT events cannot tear down a newer same-slot call,
// and ending one slot preserves an active companion slot.
static int
test_tdma_end_identity_and_order_guards(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);

    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 101, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 2000, 202, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(1);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    g_state.lasttg = 1000;
    g_state.lastsrc = 101;
    g_state.lasttgR = 2000;
    g_state.lastsrcR = 202;
    g_state.p25_p2_audio_allowed[0] = 1;
    g_state.p25_p2_audio_allowed[1] = 1;

    ev = p25_sm_ev_end_call_at(1, 1900, 191, ctx.slots[1].last_active_m + 0.001);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (!ctx.slots[1].voice_active || !ctx.slots[1].grant_active || !g_state.p25_p2_audio_allowed[1]
        || ctx.slots[1].last_end_m != 0.0) {
        DSD_FPRINTF(stderr, "FAIL: Stale identity END cleared the current slot 1 call\n");
        return 1;
    }
    if (!ctx.slots[0].voice_active || !ctx.slots[0].grant_active || !g_state.p25_p2_audio_allowed[0]) {
        DSD_FPRINTF(stderr, "FAIL: Stale slot 1 END changed active companion slot 0\n");
        return 1;
    }

    ev = p25_sm_ev_end_call_at(1, 2000, 202, ctx.slots[1].last_active_m + 0.001);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || ctx.slots[1].voice_active || !ctx.slots[1].grant_active
        || g_state.p25_p2_audio_allowed[1] || ctx.slots[1].last_end_m <= 0.0) {
        DSD_FPRINTF(stderr, "FAIL: Current slot 1 END was not applied cleanly\n");
        return 1;
    }
    if (!ctx.slots[0].voice_active || !ctx.slots[0].grant_active || !g_state.p25_p2_audio_allowed[0]) {
        DSD_FPRINTF(stderr, "FAIL: Slot 1 END changed active companion slot 0\n");
        return 1;
    }

    // A repeated END is a no-op. The synthetic reopened gate makes an
    // accidental second application observable without involving XCCH.
    const double accepted_end_m = ctx.slots[1].last_end_m;
    g_state.p25_p2_audio_allowed[1] = 1;
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (!g_state.p25_p2_audio_allowed[1] || fabs(ctx.slots[1].last_end_m - accepted_end_m) > 1.0e-9) {
        DSD_FPRINTF(stderr, "FAIL: Repeated END was applied more than once\n");
        return 1;
    }
    g_state.p25_p2_audio_allowed[1] = 0;

    // A newly observed PTT/ACTIVE boundary opens a fresh epoch even if media
    // policy suppresses voice_active and the TG/source identity is reused.
    ctx.slots[1].last_start_m = accepted_end_m + 1.0e-9;
    g_state.p25_p2_audio_allowed[1] = 1;
    ev = p25_sm_ev_end_call_at(1, 2000, 202, ctx.slots[1].last_start_m + 1.0e-9);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (g_state.p25_p2_audio_allowed[1] || ctx.slots[1].last_end_m <= accepted_end_m) {
        DSD_FPRINTF(stderr, "FAIL: New same-identity epoch END was mistaken for a repeat\n");
        return 1;
    }

    // Even a matching END identity is stale when its captured observation
    // predates a newly accepted slot assignment.
    ctx.slots[1].grant_active = 1;
    ctx.slots[1].target_id = 3000;
    ctx.slots[1].ota_tg = 3000;
    ctx.slots[1].src = 303;
    ctx.slots[1].is_group = 1;
    ctx.slots[1].last_grant_m = ctx.slots[1].last_end_m + 1.0;
    g_state.lasttgR = 3000;
    g_state.lastsrcR = 303;
    const double before_new_grant = ctx.slots[1].last_grant_m - 0.001;
    ev = p25_sm_ev_end_call_at(1, 3000, 303, before_new_grant);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (!ctx.slots[1].grant_active) {
        DSD_FPRINTF(stderr, "FAIL: END observed before a newer grant cleared that grant\n");
        return 1;
    }

    return 0;
}

// Test: only two matching FACCH END_PTT indications can authoritatively
// release a fully inactive TDMA carrier. SACCH/general END and intervening
// activity do not satisfy the heuristic.
static int
test_tdma_facch_double_end_release(void) {
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    double first_m = dsd_time_now_monotonic_s() + 0.01;
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: First FACCH END released the carrier\n");
        return 1;
    }

    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m + 0.5);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Matching FACCH END pair did not release exactly once\n");
        return 1;
    }

    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    first_m = dsd_time_now_monotonic_s() + 0.01;
    ev = p25_sm_ev_end_call_at(0, 1000, 123, first_m);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Repeated non-FACCH END released the carrier\n");
        return 1;
    }

    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    first_m += 0.2;
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m + 0.5);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Intervening activity did not reset FACCH END qualification\n");
        return 1;
    }

    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 2000, 456, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(1, 2000, 0, 456, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    first_m = dsd_time_now_monotonic_s() + 0.01;
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m + 0.5);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || !ctx.slots[1].voice_active || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Active companion slot did not block FACCH release\n");
        return 1;
    }

    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    first_m = dsd_time_now_monotonic_s();
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    ev = p25_sm_ev_group_grant(0x1235, 851500000, 2000, 456, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (!ctx.slots[1].grant_active || ctx.slots[1].last_grant_m <= first_m) {
        DSD_FPRINTF(stderr, "FAIL: Companion grant was not recorded after the first FACCH END\n");
        return 1;
    }
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m + 0.5);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || !ctx.slots[1].grant_active || ctx.slots[1].target_id != 2000
        || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Repeated FACCH END discarded a newer companion assignment\n");
        return 1;
    }

    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 2000, 456, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    first_m = dsd_time_now_monotonic_s() + 0.01;
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_facch_end_call_at(0, 1000, 123, first_m + 0.5);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || !ctx.slots[1].grant_active || ctx.slots[1].target_id != 2000
        || g_return_requests != 0) {
        DSD_FPRINTF(stderr, "FAIL: Repeated FACCH END discarded a preexisting pending companion assignment\n");
        return 1;
    }

    return 0;
}

static int
test_inband_target_change_rechecks_policy(void) {
    reset_test_state();
    g_opts.trunk_use_allow_list = 1;
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    if (seed_exact(1000, "A", "ALLOW") != 0) {
        DSD_FPRINTF(stderr, "FAIL: Could not seed in-band policy case\n");
        return 1;
    }

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 123, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || ctx.tune_count != 1 || !ctx.slots[0].voice_active) {
        DSD_FPRINTF(stderr, "FAIL: Allowed in-band policy case did not begin\n");
        return 1;
    }

    ev = p25_sm_ev_ptt_call(0, 1001, 0, 124, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_ON_CC || ctx.tune_count != 1 || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Rejected in-band target did not return to CC without retuning\n");
        return 1;
    }
    return 0;
}

static int
test_inband_policy_reject_preserves_tdma_companion(void) {
    reset_test_state();
    g_opts.trunk_use_allow_list = 1;
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    if (seed_exact(1000, "A", "ALLOW-LEFT") != 0 || seed_exact(2000, "A", "ALLOW-RIGHT") != 0) {
        DSD_FPRINTF(stderr, "FAIL: Could not seed companion policy case\n");
        return 1;
    }

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 101, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 2000, 202, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 101, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(1, 2000, 0, 202, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    g_state.p25_p2_audio_allowed[0] = 1;
    g_state.p25_p2_audio_allowed[1] = 1;

    ev = p25_sm_ev_active_call(0, 1001, 0, 303, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || g_return_requests != 0 || ctx.slots[0].grant_active || ctx.slots[0].voice_active
        || g_state.p25_p2_audio_allowed[0] || g_state.p25_policy_tg[0] != 0) {
        DSD_FPRINTF(stderr, "FAIL: Rejected slot was not cleared without releasing its TDMA carrier\n");
        return 1;
    }
    if (!ctx.slots[1].grant_active || !ctx.slots[1].voice_active || ctx.slots[1].target_id != 2000
        || ctx.slots[1].src != 202 || !g_state.p25_p2_audio_allowed[1] || g_state.lasttgR != 2000
        || g_state.lastsrcR != 202) {
        DSD_FPRINTF(stderr, "FAIL: Policy rejection changed the allowed TDMA companion\n");
        return 1;
    }
    return 0;
}

static int
test_inband_policy_reject_releases_after_companion_ended(void) {
    reset_test_state();
    g_opts.trunk_use_allow_list = 1;
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.trunk_chan_map[0x1235] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    if (seed_exact(1000, "A", "ALLOW-LEFT") != 0 || seed_exact(2000, "A", "ALLOW-RIGHT") != 0) {
        DSD_FPRINTF(stderr, "FAIL: Could not seed ended companion policy case\n");
        return 1;
    }

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    p25_sm_event_t ev = p25_sm_ev_group_grant(0x1234, 851500000, 1000, 101, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_group_grant(0x1235, 851500000, 2000, 202, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(0, 1000, 0, 101, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt_call(1, 2000, 0, 202, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_end_call_at(1, 2000, 202, dsd_time_now_monotonic_s());
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_TUNED || ctx.slots[1].voice_active || !ctx.slots[1].grant_active
        || ctx.t_hangtime_m > 0.0) {
        DSD_FPRINTF(stderr, "FAIL: Companion END did not leave the active slot in control of the carrier\n");
        return 1;
    }

    ev = p25_sm_ev_active_call(0, 1001, 0, 303, 1, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    if (ctx.state != P25_SM_ON_CC || g_return_requests != 1) {
        DSD_FPRINTF(stderr, "FAIL: Ended companion grant left rejected carrier without a release deadline\n");
        return 1;
    }
    return 0;
}

// Test: ENC event on P25P2 keeps allow-list and TG-hold blocks closed even
// when the encrypted call is locally decryptable.
static int
test_tdma_enc_respects_media_policy(void) {
    p25_sm_ctx_t ctx;
    p25_sm_event_t ev;

    // Case 1: Allow-list blocked decryptable call stays muted.
    reset_test_state();
    g_opts.trunk_use_allow_list = 1;
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    if (seed_exact(1000, "A", "KNOWN") != 0) {
        DSD_FPRINTF(stderr, "FAIL: seed_exact allow-list setup failed\n");
        return 1;
    }

    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 1001, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    g_state.lasttg = 1001;
    g_state.lastsrc = 123;
    g_state.gi[0] = 0;
    g_state.aes_key_loaded[0] = 1;
    g_state.aes_key_segments[0] = 4U;
    g_state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    g_state.p25_p2_audio_allowed[0] = 0;

    ev = p25_sm_ev_enc(0, 0x84, 0x1234, 1001);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    if (g_state.p25_p2_audio_allowed[0] != 0) {
        DSD_FPRINTF(stderr, "FAIL: ENC reopened allow-list blocked decryptable audio\n");
        return 1;
    }

    // Case 2: Non-matching TG hold keeps decryptable encrypted audio muted.
    reset_test_state();
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    g_state.tg_hold = 2000;

    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 2001, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    g_state.lasttg = 2001;
    g_state.lastsrc = 123;
    g_state.gi[0] = 0;
    g_state.aes_key_loaded[0] = 1;
    g_state.aes_key_segments[0] = 4U;
    g_state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    g_state.p25_p2_audio_allowed[0] = 0;

    ev = p25_sm_ev_enc(0, 0x84, 0x1234, 2001);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    if (g_state.p25_p2_audio_allowed[0] != 0) {
        DSD_FPRINTF(stderr, "FAIL: ENC reopened TG-hold blocked decryptable audio\n");
        return 1;
    }

    // Case 3: Allowed decryptable call still opens audio.
    reset_test_state();
    g_opts.trunk_use_allow_list = 1;
    g_state.trunk_chan_map[0x1234] = 851500000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    if (seed_exact(3000, "A", "ALLOW") != 0) {
        DSD_FPRINTF(stderr, "FAIL: seed_exact allowed setup failed\n");
        return 1;
    }

    p25_sm_init_ctx(&ctx, &g_opts, &g_state);
    ev = p25_sm_ev_group_grant(0x1234, 851500000, 3000, 123, 0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);
    ev = p25_sm_ev_ptt(0);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    g_state.lasttg = 3000;
    g_state.lastsrc = 123;
    g_state.gi[0] = 0;
    g_state.aes_key_loaded[0] = 1;
    g_state.aes_key_segments[0] = 4U;
    g_state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    g_state.p25_p2_audio_allowed[0] = 0;

    ev = p25_sm_ev_enc(0, 0x84, 0x1234, 3000);
    p25_sm_event(&ctx, &g_opts, &g_state, &ev);

    if (g_state.p25_p2_audio_allowed[0] != 1) {
        DSD_FPRINTF(stderr, "FAIL: ENC failed to reopen allowed decryptable audio\n");
        return 1;
    }

    return 0;
}

int
main(void) {
    int fail = 0;
    install_trunk_tuning_hooks();

    printf("Testing P25 SM (4-state model)...\n");

    fail += test_init_with_cc();
    fail += test_init_without_cc();
    fail += test_grant_to_tuned();
    fail += test_ptt_voice_active();
    fail += test_private_ptt_preserves_grant_identity();
    fail += test_p1_hdu_crypto_survives_first_active();
    fail += test_conventional_end_is_follower_noop();
    fail += test_end_clears_voice();
    fail += test_tdma_boundaries_only_hang_after_last_assigned_voice();
    fail += test_anonymous_followup_restarts_crypto_pending();
    fail += test_unassigned_companion_start_is_rejected();
    fail += test_state_names();
    fail += test_config_defaults();
    fail += test_singleton();
    fail += test_sacch_slot_mapping();
    fail += test_tdma_partial_end_stays_tuned();
    fail += test_tdma_pending_other_slot_blocks_release();
    fail += test_tdma_enc_lockout_slot_does_not_block_release();
    fail += test_tdma_single_slot_end_retains_carrier();
    fail += test_tdma_end_identity_and_order_guards();
    fail += test_tdma_facch_double_end_release();
    fail += test_inband_target_change_rechecks_policy();
    fail += test_inband_policy_reject_preserves_tdma_companion();
    fail += test_inband_policy_reject_releases_after_companion_ended();
    fail += test_tdma_enc_respects_media_policy();

    if (fail) {
        printf("FAILED: %d test(s)\n", fail);
    } else {
        printf("PASSED: All P25 SM tests\n");
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return fail ? 1 : 0;
}
