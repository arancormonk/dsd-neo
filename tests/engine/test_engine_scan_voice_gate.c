// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Voice-gated scan unit tests (issue #381).
 *
 * Pins the -Y gate: off never steps, IDLE-only sync steps at qualify, voice
 * holds through the tail from the last media time even after a SYNC_LOSS end,
 * 0.10 s span debounce, policy gating (blocked encrypted, private evaluator,
 * unknown identity), DATA ignored, operator hold, and visit resets.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/scan_voice_gate.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_failures = 0;

#define CHECK(tag, cond)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL: %s:%d: %s\n", __func__, __LINE__, (tag));                                       \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static int
test_tune_allowed(const dsd_opts* opts, int call_type_enabled, int encrypted, int data_call) {
    if (!opts || !call_type_enabled || opts->trunk_use_allow_list) {
        return 0;
    }
    if (encrypted && opts->trunk_tune_enc_calls == 0) {
        return 0;
    }
    if (data_call && opts->trunk_tune_data_calls == 0) {
        return 0;
    }
    return 1;
}

int
dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                  int encrypted, int data_call, dsd_tg_policy_decision* out) {
    (void)state;
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->target_id = tg;
    out->source_id = src;
    out->encrypted = encrypted;
    out->data_call = data_call;
    out->audio_allowed = 1;
    out->record_allowed = 1;
    out->stream_allowed = 1;
    out->tune_allowed = test_tune_allowed(opts, opts ? opts->trunk_tune_group_calls : 0, encrypted, data_call);
    return 0;
}

int
dsd_tg_policy_evaluate_private_call(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                    int encrypted, int data_call, dsd_tg_policy_decision* out) {
    (void)state;
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->target_id = dst;
    out->source_id = src;
    out->encrypted = encrypted;
    out->data_call = data_call;
    out->audio_allowed = 1;
    out->record_allowed = 1;
    out->stream_allowed = 1;
    out->tune_allowed = test_tune_allowed(opts, opts ? opts->trunk_tune_private_calls : 0, encrypted, data_call);
    return 0;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} gate_fixture;

static dsd_opts g_opts;
static dsd_state g_state;

static int
fixture_init(gate_fixture* fix) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    if (dsd_call_state_ensure(&g_state) <= 0) {
        return -1;
    }
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_private_calls = 1;
    g_opts.trunk_tune_data_calls = 0;
    g_opts.trunk_tune_enc_calls = 1;
    g_opts.scanner_mode = 1;
    g_opts.scan_voice_only = 1;
    g_opts.scan_voice_qualify_ms = 1000;
    g_opts.scan_voice_hold_ms = 2000;
    g_state.lcn_freq_roll = 3;
    fix->opts = &g_opts;
    fix->state = &g_state;
    return 0;
}

static void
fixture_free(gate_fixture* fix) {
    if (!fix || !fix->state) {
        return;
    }
    dsd_state_ext_free_all(fix->state);
}

/* Open a voice epoch on slot 0 at time t with the given identity/crypto, then
 * run media at t and t + span so the caller controls the media span. */
static void
open_voice_epoch(gate_fixture* fix, double t, double span, dsd_call_kind kind, uint64_t src, uint64_t dst,
                 dsd_call_crypto_state crypto) {
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof(obs));
    obs.protocol = 1;
    obs.slot = 0;
    obs.kind = kind;
    obs.ota_source_id = src;
    obs.ota_target_id = dst;
    obs.policy_target_id = dst;
    obs.observed_m = t;
    (void)dsd_call_state_observe(fix->state, &obs, DSD_CALL_BOUNDARY_BEGIN);
    if (crypto != DSD_CALL_CRYPTO_CLEAR && crypto != DSD_CALL_CRYPTO_UNKNOWN) {
        dsd_call_crypto_update update;
        DSD_MEMSET(&update, 0, sizeof(update));
        update.classification = crypto;
        update.observed_m = t;
        (void)dsd_call_state_update_crypto(fix->state, 0, &update);
    }
    (void)dsd_call_state_update_media(fix->state, 0, 1, t);
    (void)dsd_call_state_update_media(fix->state, 0, 1, t + span);
}

static void
test_gate_off_never_steps(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    fix.opts->scan_voice_only = 0;
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    CHECK("phase off", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_OFF);
    CHECK("no step", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 200.0) == 0);
    fixture_free(&fix);
}

static void
test_idle_only_steps_at_qualify(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    CHECK("qualify phase", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY);
    CHECK("no early step", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 100.9) == 0);
    CHECK("steps at qualify", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 101.0) != 0);
    CHECK("probe idle", dsd_scan_voice_probe(fix.opts, fix.state) < 0.0);
    fixture_free(&fix);
}

static void
test_never_synced_falls_back(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 0, 100.0);
    CHECK("no step without sync", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 500.0) == 0);
    fixture_free(&fix);
}

static void
test_voice_holds_through_tail(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    open_voice_epoch(&fix, 100.2, 0.15, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.4);
    CHECK("voice phase", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_VOICE);
    CHECK("probe at media", fabs(dsd_scan_voice_probe(fix.opts, fix.state) - 100.35) < 1e-6);
    /* Sync loss ends the epoch; the tail still runs from the last media. */
    (void)dsd_call_state_end_ex(fix.state, 0, 100.5, DSD_CALL_END_SYNC_LOSS);
    CHECK("probe after end", dsd_scan_voice_probe(fix.opts, fix.state) < 0.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 0, 100.6);
    CHECK("tail phase", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_TAIL);
    CHECK("holds in tail", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 101.0) == 0);
    CHECK("steps after hold", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 102.36) != 0);
    fixture_free(&fix);
}

static void
test_span_debounce(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    open_voice_epoch(&fix, 100.0, 0.05, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    CHECK("short span ignored", dsd_scan_voice_probe(fix.opts, fix.state) < 0.0);
    (void)dsd_call_state_update_media(fix.state, 0, 1, 100.2);
    CHECK("long span counts", dsd_scan_voice_probe(fix.opts, fix.state) > 0.0);
    fixture_free(&fix);
}

static void
test_policy_gating(void) {
    gate_fixture enc;
    if (fixture_init(&enc) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    enc.opts->trunk_tune_enc_calls = 0;
    open_voice_epoch(&enc, 100.0, 0.15, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_ENCRYPTED);
    CHECK("blocked encrypted ignored", dsd_scan_voice_probe(enc.opts, enc.state) < 0.0);
    fixture_free(&enc);

    gate_fixture clear;
    if (fixture_init(&clear) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    open_voice_epoch(&clear, 100.0, 0.15, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_ENCRYPTED);
    CHECK("policy-allowed encrypted holds", dsd_scan_voice_probe(clear.opts, clear.state) > 0.0);
    fixture_free(&clear);

    gate_fixture unknown;
    if (fixture_init(&unknown) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    open_voice_epoch(&unknown, 100.0, 0.15, DSD_CALL_KIND_VOICE, 0, 0, DSD_CALL_CRYPTO_UNKNOWN);
    CHECK("unknown identity holds", dsd_scan_voice_probe(unknown.opts, unknown.state) > 0.0);
    fixture_free(&unknown);

    gate_fixture priv;
    if (fixture_init(&priv) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    priv.opts->trunk_tune_private_calls = 0;
    open_voice_epoch(&priv, 100.0, 0.15, DSD_CALL_KIND_PRIVATE_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    CHECK("private blocked", dsd_scan_voice_probe(priv.opts, priv.state) < 0.0);
    fixture_free(&priv);

    gate_fixture data;
    if (fixture_init(&data) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_call_observation obs = dsd_call_observation_data(1, 0, 11, 22);
    obs.observed_m = 100.0;
    (void)dsd_call_state_observe(data.state, &obs, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_update_media(data.state, 0, 1, 100.0);
    (void)dsd_call_state_update_media(data.state, 0, 1, 100.2);
    CHECK("data ignored", dsd_scan_voice_probe(data.opts, data.state) < 0.0);
    fixture_free(&data);
}

static void
test_operator_hold_and_visit_reset(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    fix.state->lcn_scan_hold = 1;
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 150.0);
    CHECK("hold wins", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 200.0) == 0);
    /* A plain hold release (no roll change: the release only calls mark_cc_sync())
     * restarts the qualify window instead of hopping on the next tick. */
    fix.state->lcn_scan_hold = 0;
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 200.0);
    CHECK("release requalifies", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 200.9) == 0);
    CHECK("release sync anchor", fabs(fix.state->scan_voice_gate_sync_m - 200.0) < 1e-9);
    CHECK("release steps at qualify", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 201.0) != 0);
    /* Voice held before the hold does not survive the release either. */
    open_voice_epoch(&fix, 200.2, 0.15, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 200.4);
    CHECK("voice armed", fix.state->scan_voice_gate_voice_m > 0.0);
    (void)dsd_call_state_end_ex(fix.state, 0, 200.5, DSD_CALL_END_SYNC_LOSS);
    fix.state->lcn_scan_hold = 1;
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 200.6);
    fix.state->lcn_scan_hold = 0;
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 200.7);
    CHECK("release drops voice anchor", fix.state->scan_voice_gate_voice_m < 0.0);
    CHECK("release restarts qualify", fabs(fix.state->scan_voice_gate_sync_m - 200.7) < 1e-9);
    /* An external roll change (avoid, `L` cycle) restarts the visit too. */
    fix.state->lcn_freq_roll = 4;
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 250.0);
    CHECK("requalified", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 250.5) == 0);
    CHECK("sync anchor reset", fabs(fix.state->scan_voice_gate_sync_m - 250.0) < 1e-9);
    CHECK("voice anchor reset", fix.state->scan_voice_gate_voice_m < 0.0);
    /* An explicit retune also resets the visit. */
    dsd_scan_voice_gate_note_retune(fix.state, 300.0);
    CHECK("retune resets sync", fix.state->scan_voice_gate_sync_m < 0.0);
    CHECK("retune resets voice", fix.state->scan_voice_gate_voice_m < 0.0);
    CHECK("retune stamps arrive", fabs(fix.state->scan_voice_gate_arrive_m - 300.0) < 1e-9);
    fixture_free(&fix);
}

static void
test_stale_epoch_cannot_rearm(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    /* Voice before the visit arrived must not arm the new visit. */
    open_voice_epoch(&fix, 50.0, 0.15, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.1);
    CHECK("stale voice ignored", fix.state->scan_voice_gate_voice_m < 0.0);
    CHECK("still qualify", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY);
    fixture_free(&fix);
}

static void
test_zero_ms_falls_back_to_defaults(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    fix.opts->scan_voice_qualify_ms = 0;
    fix.opts->scan_voice_hold_ms = 0;
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    CHECK("default qualify", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 101.0) != 0);
    fixture_free(&fix);
}

static void
test_tick_leaves_phase_alone_without_scanner_mode(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    /* Under --trunk-scan the coordinator owns the phase; the -Y tick must not clobber it. */
    fix.opts->scanner_mode = 0;
    fix.state->scan_voice_gate_phase = (uint8_t)DSD_SCAN_VOICE_GATE_TAIL;
    fix.state->scan_voice_gate_sync_m = -1.0;
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    CHECK("phase untouched", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_TAIL);
    CHECK("sync anchor untouched", fix.state->scan_voice_gate_sync_m < 0.0);
    fixture_free(&fix);
}

int
main(void) {
    test_tick_leaves_phase_alone_without_scanner_mode();
    test_gate_off_never_steps();
    test_idle_only_steps_at_qualify();
    test_never_synced_falls_back();
    test_voice_holds_through_tail();
    test_span_debounce();
    test_policy_gating();
    test_operator_hold_and_visit_reset();
    test_stale_epoch_cannot_rearm();
    test_zero_ms_falls_back_to_defaults();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d voice-gate check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
