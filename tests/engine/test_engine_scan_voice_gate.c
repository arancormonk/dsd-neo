// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Voice-gated scan unit tests (issue #381).
 *
 * Pins the -Y gate: off never steps, IDLE-only sync steps at qualify, voice
 * holds through the tail from the last media time even when a terminator ends
 * the call before the first gate tick, 0.10 s span debounce, policy gating
 * (blocked encrypted, private evaluator, unknown identity), DATA ignored,
 * operator hold, and visit resets. Also pins the scan timing publication the
 * Scan Timing row renders (issue #508): reason, effective windows, and a deadline
 * that agrees with dsd_scan_voice_gate_should_step() to the millisecond.
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
#include <time.h>

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

static dsd_scan_voice_probe_result
probe_voice(const gate_fixture* fix) {
    dsd_scan_voice_probe_result result;
    const int rc = dsd_scan_voice_probe(fix ? fix->opts : NULL, fix ? fix->state : NULL, &result);
    CHECK("probe succeeds", rc >= 0);
    CHECK("probe status matches retained media", rc == (result.retained_media_m >= 0.0 ? 1 : 0));
    return result;
}

/* Open a voice epoch on one slot at time t with the given identity/crypto, then
 * run media at t and t + span so the caller controls the media span. */
static void
open_voice_epoch_slot(gate_fixture* fix, uint8_t slot, double t, double span, dsd_call_kind kind, uint64_t src,
                      uint64_t dst, dsd_call_crypto_state crypto) {
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof(obs));
    obs.protocol = 1;
    obs.slot = slot;
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
        (void)dsd_call_state_update_crypto(fix->state, slot, &update);
    }
    (void)dsd_call_state_update_media(fix->state, slot, 1, t);
    (void)dsd_call_state_update_media(fix->state, slot, 1, t + span);
}

static void
open_voice_epoch(gate_fixture* fix, double t, double span, dsd_call_kind kind, uint64_t src, uint64_t dst,
                 dsd_call_crypto_state crypto) {
    open_voice_epoch_slot(fix, 0U, t, span, kind, src, dst, crypto);
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
    CHECK("gate off abstains", dsd_scan_voice_gate_owns_step(fix.opts, fix.state) == 0);
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
    CHECK("synced gate owns step", dsd_scan_voice_gate_owns_step(fix.opts, fix.state) != 0);
    CHECK("no early step", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 100.9) == 0);
    CHECK("steps at qualify", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 101.0) != 0);
    const dsd_scan_voice_probe_result media = probe_voice(&fix);
    CHECK("probe idle active", media.active_media_m < 0.0);
    CHECK("probe idle retained", media.retained_media_m < 0.0);
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
    CHECK("unsynced gate abstains", dsd_scan_voice_gate_owns_step(fix.opts, fix.state) == 0);
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
    dsd_scan_voice_probe_result media = probe_voice(&fix);
    CHECK("active probe at media", fabs(media.active_media_m - 100.35) < 1e-6);
    CHECK("retained probe at media", fabs(media.retained_media_m - 100.35) < 1e-6);
    /* Sync loss ends the epoch; the tail still runs from the last media. */
    (void)dsd_call_state_end_ex(fix.state, 0, 100.5, DSD_CALL_END_SYNC_LOSS);
    media = probe_voice(&fix);
    CHECK("active probe after end", media.active_media_m < 0.0);
    CHECK("retained probe after end", fabs(media.retained_media_m - 100.35) < 1e-6);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 0, 100.6);
    CHECK("tail phase", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_TAIL);
    CHECK("holds in tail", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 101.0) == 0);
    CHECK("steps after hold", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 102.36) != 0);
    fixture_free(&fix);
}

static void
test_terminator_before_first_tick_holds(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    fix.opts->scan_voice_hold_ms = 5000;
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    open_voice_epoch(&fix, 100.25, 0.125, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    /* DMR BS dispatch consumes the terminator before returning to the engine's first gate tick. */
    (void)dsd_call_state_end_ex(fix.state, 0, 100.5, DSD_CALL_END_TERMINATOR);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.625);
    CHECK("ended media publishes tail", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_TAIL);
    CHECK("ended media arms last frame", fabs(fix.state->scan_voice_gate_voice_m - 100.375) < 1e-6);
    CHECK("retained media owns step", dsd_scan_voice_gate_owns_step(fix.opts, fix.state) != 0);
    CHECK("custom hold remains before boundary", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 105.374) == 0);
    CHECK("custom hold expires at boundary", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 105.375) != 0);
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
    dsd_scan_voice_probe_result media = probe_voice(&fix);
    CHECK("short active span ignored", media.active_media_m < 0.0);
    CHECK("short retained span ignored", media.retained_media_m < 0.0);
    (void)dsd_call_state_update_media(fix.state, 0, 1, 100.2);
    media = probe_voice(&fix);
    CHECK("long active span counts", media.active_media_m > 0.0);
    CHECK("long retained span counts", media.retained_media_m > 0.0);
    fixture_free(&fix);
}

static void
test_reacquired_segment_reearns_span(void) {
    static const dsd_call_end_reason reasons[] = {
        DSD_CALL_END_SYNC_LOSS,
        DSD_CALL_END_UNVERIFIED_TERMINATOR,
    };
    for (size_t i = 0U; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        gate_fixture fix;
        if (fixture_init(&fix) != 0) {
            DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
            g_failures++;
            return;
        }
        dsd_scan_voice_gate_note_retune(fix.state, 100.0);
        open_voice_epoch(&fix, 100.1, 0.2, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
        (void)dsd_call_state_end_ex(fix.state, 0U, 100.35, reasons[i]);

        /* This is the vocoder's production reopen shape: identity-less BEGIN followed by one
         * media mark. It must not borrow the prior segment's span and refresh a dead channel. */
        dsd_call_observation observation;
        DSD_MEMSET(&observation, 0, sizeof(observation));
        observation.protocol = 1;
        observation.slot = 0U;
        observation.kind = DSD_CALL_KIND_VOICE;
        observation.observed_m = 100.45;
        CHECK("recoverable end reopens", dsd_call_state_observe(fix.state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        CHECK("first reacquired media marks", dsd_call_state_update_media(fix.state, 0U, 1, 100.45) == 1);
        dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.45);
        dsd_scan_voice_probe_result media = probe_voice(&fix);
        CHECK("single reacquired frame is not voice", media.active_media_m < 0.0 && media.retained_media_m < 0.0);
        CHECK("single reacquired frame does not arm hold", fix.state->scan_voice_gate_voice_m < 0.0);
        CHECK("single reacquired frame stays qualify",
              fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY);

        CHECK("reacquired media continues", dsd_call_state_update_media(fix.state, 0U, 1, 100.56) == 1);
        dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.56);
        media = probe_voice(&fix);
        CHECK("reacquired span counts", fabs(media.active_media_m - 100.56) < 1e-6);
        CHECK("reacquired span arms hold", fabs(fix.state->scan_voice_gate_voice_m - 100.56) < 1e-6);
        fixture_free(&fix);
    }
}

static void
test_non_media_updates_do_not_extend_hold(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    open_voice_epoch(&fix, 100.0, 0.2, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof(obs));
    obs.protocol = 1;
    obs.slot = 0;
    obs.kind = DSD_CALL_KIND_GROUP_VOICE;
    obs.ota_source_id = 11;
    obs.ota_target_id = 22;
    obs.policy_target_id = 22;
    obs.observed_m = 101.0;
    (void)dsd_call_state_observe(fix.state, &obs, DSD_CALL_BOUNDARY_CONTINUE);
    dsd_call_crypto_update crypto;
    DSD_MEMSET(&crypto, 0, sizeof(crypto));
    crypto.classification = DSD_CALL_CRYPTO_CLEAR;
    crypto.observed_m = 102.0;
    (void)dsd_call_state_update_crypto(fix.state, 0, &crypto);
    (void)dsd_call_state_end_ex(fix.state, 0, 103.0, DSD_CALL_END_TERMINATOR);
    const dsd_scan_voice_probe_result media = probe_voice(&fix);
    CHECK("ended call is not active", media.active_media_m < 0.0);
    CHECK("metadata and end preserve media anchor", fabs(media.retained_media_m - 100.2) < 1e-6);
    fixture_free(&fix);
}

static void
test_probe_tracks_active_and_retained_slots_independently(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    open_voice_epoch_slot(&fix, 0U, 100.25, 0.25, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    (void)dsd_call_state_end_ex(fix.state, 0U, 100.625, DSD_CALL_END_TERMINATOR);
    open_voice_epoch_slot(&fix, 1U, 100.125, 0.25, DSD_CALL_KIND_GROUP_VOICE, 33, 44, DSD_CALL_CRYPTO_CLEAR);

    const dsd_scan_voice_probe_result media = probe_voice(&fix);
    CHECK("active slot reported", fabs(media.active_media_m - 100.375) < 1e-6);
    CHECK("newer ended slot retained", fabs(media.retained_media_m - 100.5) < 1e-6);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.75);
    CHECK("active slot keeps voice phase", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_VOICE);
    CHECK("newer retained slot anchors hold", fabs(fix.state->scan_voice_gate_voice_m - 100.5) < 1e-6);
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
    CHECK("blocked encrypted ignored", probe_voice(&enc).retained_media_m < 0.0);
    fixture_free(&enc);

    gate_fixture clear;
    if (fixture_init(&clear) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    open_voice_epoch(&clear, 100.0, 0.15, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_ENCRYPTED);
    CHECK("policy-allowed encrypted holds", probe_voice(&clear).retained_media_m > 0.0);
    fixture_free(&clear);

    gate_fixture unknown;
    if (fixture_init(&unknown) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    open_voice_epoch(&unknown, 100.0, 0.15, DSD_CALL_KIND_VOICE, 0, 0, DSD_CALL_CRYPTO_UNKNOWN);
    CHECK("unknown identity holds", probe_voice(&unknown).retained_media_m > 0.0);
    fixture_free(&unknown);

    gate_fixture priv;
    if (fixture_init(&priv) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    priv.opts->trunk_tune_private_calls = 0;
    open_voice_epoch(&priv, 100.0, 0.15, DSD_CALL_KIND_PRIVATE_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    CHECK("private blocked", probe_voice(&priv).retained_media_m < 0.0);
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
    CHECK("data ignored", probe_voice(&data).retained_media_m < 0.0);
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
    (void)dsd_call_state_end_ex(fix.state, 0, 50.2, DSD_CALL_END_TERMINATOR);
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

/* ---- Shared talkgroup-hold probe (issue #507) ------------------------------ */

/* Open one call epoch with explicit identities. The hold probe reads phase, kind and the
 * identity fields only, so these epochs deliberately carry no media. */
static void
open_call_identity(gate_fixture* fix, uint8_t slot, dsd_call_kind kind, uint64_t src, uint64_t ota_target,
                   uint64_t policy_target, double t) {
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof(obs));
    obs.protocol = 1;
    obs.slot = slot;
    obs.kind = kind;
    obs.ota_source_id = src;
    obs.ota_target_id = ota_target;
    obs.policy_target_id = policy_target;
    obs.observed_m = t;
    (void)dsd_call_state_observe(fix->state, &obs, DSD_CALL_BOUNDARY_BEGIN);
}

/* Both scanners suspend the per-visit limit only while the held talkgroup's call is the one
 * being followed, so the probe has to be exact about which call counts. */
static void
test_tg_hold_call_active(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    CHECK("null state is not a held call", dsd_scan_tg_hold_call_active(NULL) == 0);
    fix.state->tg_hold = 22;
    CHECK("hold without any call", dsd_scan_tg_hold_call_active(fix.state) == 0);

    /* Slot 0: a group call to 22. */
    open_call_identity(&fix, 0U, DSD_CALL_KIND_GROUP_VOICE, 11, 22, 22, 100.0);
    fix.state->tg_hold = 0;
    CHECK("no hold", dsd_scan_tg_hold_call_active(fix.state) == 0);
    fix.state->tg_hold = 22;
    CHECK("held group call", dsd_scan_tg_hold_call_active(fix.state) == 1);
    fix.state->tg_hold = 99;
    CHECK("hold on another talkgroup", dsd_scan_tg_hold_call_active(fix.state) == 0);
    fix.state->tg_hold = 22;
    (void)dsd_call_state_end_ex(fix.state, 0U, 100.5, DSD_CALL_END_EXPLICIT);
    CHECK("ended call", dsd_scan_tg_hold_call_active(fix.state) == 0);

    /* A data call to the held talkgroup is not a call being followed. */
    open_call_identity(&fix, 0U, DSD_CALL_KIND_DATA, 11, 22, 22, 101.0);
    CHECK("data call", dsd_scan_tg_hold_call_active(fix.state) == 0);
    (void)dsd_call_state_end_ex(fix.state, 0U, 101.5, DSD_CALL_END_EXPLICIT);

    /* A private call is held by either end, so the held id may be the source. */
    open_call_identity(&fix, 1U, DSD_CALL_KIND_PRIVATE_VOICE, 22, 77, 77, 102.0);
    CHECK("held private call source", dsd_scan_tg_hold_call_active(fix.state) == 1);
    (void)dsd_call_state_end_ex(fix.state, 1U, 102.5, DSD_CALL_END_EXPLICIT);
    /* A group call's source is not an identity the hold follows. */
    open_call_identity(&fix, 1U, DSD_CALL_KIND_GROUP_VOICE, 22, 77, 77, 103.0);
    CHECK("group call source", dsd_scan_tg_hold_call_active(fix.state) == 0);
    (void)dsd_call_state_end_ex(fix.state, 1U, 103.5, DSD_CALL_END_EXPLICIT);

    /* The remapped policy target is the identity the hold is compared against. */
    open_call_identity(&fix, 1U, DSD_CALL_KIND_GROUP_VOICE, 11, 500, 22, 104.0);
    CHECK("held remapped target", dsd_scan_tg_hold_call_active(fix.state) == 1);
    fix.state->tg_hold = 500;
    CHECK("hold on the pre-remap target", dsd_scan_tg_hold_call_active(fix.state) == 0);
    fixture_free(&fix);
}

/* ---- Scan timing publication for the -Y row (issue #508) ------------------- */

static void
check_timing_window(const char* started_tag, const char* deadline_tag, const char* span_tag,
                    const dsd_scan_timing_publication* timing, double started_m, double deadline_m, uint32_t span_ms) {
    CHECK(started_tag, fabs(timing->started_m - started_m) < 1e-6);
    CHECK(deadline_tag, fabs(timing->deadline_m - deadline_m) < 1e-6);
    CHECK(span_tag, timing->span_ms == span_ms);
}

/* Anti-drift: the published deadline is only useful if it is the instant
 * dsd_scan_voice_gate_should_step() first says yes. A countdown that reaches 0.0 a
 * frame before or after the hop is a bug report waiting to happen. */
static void
check_deadline_matches_should_step(const gate_fixture* fix, const char* early_tag, const char* late_tag) {
    const double deadline_m = fix->state->scan_timing.deadline_m;
    CHECK(early_tag, dsd_scan_voice_gate_should_step(fix->opts, fix->state, deadline_m - 1e-3) == 0);
    CHECK(late_tag, dsd_scan_voice_gate_should_step(fix->opts, fix->state, deadline_m + 1e-3) == 1);
}

static void
test_y_timing_silent_under_trunk_scan(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    /* Under --trunk-scan the coordinator owns the publication for its parked target,
     * exactly as it owns scan_voice_gate_phase. */
    fix.opts->trunk_scan_enabled = 1;
    dsd_scan_timing_clear(fix.state);
    fix.state->scan_timing.reason = (uint8_t)DSD_SCAN_STAY_CALL_FOLLOW;
    fix.state->scan_timing.deadline_m = 142.0;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("trunk scan keeps reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_CALL_FOLLOW);
    CHECK("trunk scan keeps deadline", fabs(fix.state->scan_timing.deadline_m - 142.0) < 1e-6);
    /* No scanner running at all publishes nothing either. */
    fix.opts->trunk_scan_enabled = 0;
    fix.opts->scanner_mode = 0;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("no scanner keeps reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_CALL_FOLLOW);
    CHECK("no scanner keeps deadline", fabs(fix.state->scan_timing.deadline_m - 142.0) < 1e-6);
    fixture_free(&fix);
}

static void
test_y_timing_legacy_hangtime_window(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    fix.opts->scan_voice_only = 0;
    fix.opts->trunk_hangtime = 2.0f;
    /* The step rule compares the wall-clock anchor, so that is what the countdown is
     * built from: wall 1000 with the clocks read at wall 1000.25 / monotonic 100.25. */
    fix.state->last_cc_sync_time = (time_t)1000;
    fix.state->last_cc_sync_time_m = 0.0;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.25, 1000.25);
    CHECK("hangtime reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_HANGTIME);
    CHECK("hangtime is conventional", fix.state->scan_timing.conventional == 1U);
    check_timing_window("hangtime start", "hangtime deadline", "hangtime span", &fix.state->scan_timing, 100.0, 102.0,
                        2000U);
    CHECK("hangtime has no dwell", fix.state->scan_timing.dwell_ms == 0U);
    CHECK("hangtime has no hold", fix.state->scan_timing.hold_ms == 0U);
    /* A later tick re-expresses the same wall anchor and lands on the same deadline. */
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 101.5, 1001.5);
    check_timing_window("hangtime start again", "hangtime deadline again", "hangtime span again",
                        &fix.state->scan_timing, 100.0, 102.0, 2000U);
    /* NXDN stamps the wall anchor two seconds ahead after a confirmed frame (and leaves
     * the monotonic twin alone), so the window starts in the future and the countdown
     * begins above -t rather than reaching zero two seconds before the hop. */
    fix.state->last_cc_sync_time = (time_t)1002;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.25, 1000.25);
    check_timing_window("nxdn future start", "nxdn future deadline", "nxdn future span", &fix.state->scan_timing, 102.0,
                        104.0, 2000U);
    fix.state->last_cc_sync_time = (time_t)1000;
    /* A gate that is enabled but has never synced this visit abstains, so the legacy
     * rule still owns the step -- and it has neither window to report. */
    fix.opts->scan_voice_only = 1;
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 0, 100.5);
    CHECK("unsynced gate abstains", dsd_scan_voice_gate_owns_step(fix.opts, fix.state) == 0);
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("unsynced gate reports hangtime", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_HANGTIME);
    CHECK("unsynced gate hides dwell", fix.state->scan_timing.dwell_ms == 0U);
    CHECK("unsynced gate hides hold", fix.state->scan_timing.hold_ms == 0U);
    /* -t takes any non-negative double, so the millisecond span saturates instead of
     * wrapping into the publication's uint32_t. */
    fix.opts->scan_voice_only = 0;
    fix.opts->trunk_hangtime = 1.0e9f;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("absurd hangtime saturates the span", fix.state->scan_timing.span_ms == UINT32_MAX);
    fixture_free(&fix);
}

static void
test_y_timing_hangtime_without_anchor(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    fix.opts->scan_voice_only = 0;
    fix.opts->trunk_hangtime = 2.0f;
    fix.state->last_cc_sync_time = (time_t)0;
    fix.state->last_cc_sync_time_m = 100.0;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("unanchored reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_HANGTIME);
    CHECK("unanchored has no start", fix.state->scan_timing.started_m < 0.0);
    CHECK("unanchored has no deadline", fix.state->scan_timing.deadline_m < 0.0);
    CHECK("unanchored has no span", fix.state->scan_timing.span_ms == 0U);
    /* -t 0 has an anchor but no window to count down. */
    fix.state->last_cc_sync_time = (time_t)100;
    fix.opts->trunk_hangtime = 0.0f;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("zero hangtime reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_HANGTIME);
    CHECK("zero hangtime has no deadline", fix.state->scan_timing.deadline_m < 0.0);
    CHECK("zero hangtime has no span", fix.state->scan_timing.span_ms == 0U);
    fixture_free(&fix);
}

static void
test_y_timing_manual_hold_pauses_the_timer(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    fix.state->lcn_scan_hold = 1;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("manual hold reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_MANUAL_HOLD);
    CHECK("manual hold has no start", fix.state->scan_timing.started_m < 0.0);
    CHECK("manual hold has no deadline", fix.state->scan_timing.deadline_m < 0.0);
    CHECK("manual hold has no span", fix.state->scan_timing.span_ms == 0U);
    /* The row's effective windows stay visible: they are paused, not gone. */
    CHECK("manual hold keeps dwell", fix.state->scan_timing.dwell_ms == 1000U);
    CHECK("manual hold keeps hold", fix.state->scan_timing.hold_ms == 2000U);
    CHECK("manual hold never steps", dsd_scan_voice_gate_should_step(fix.opts, fix.state, 400.0) == 0);
    fixture_free(&fix);
}

static void
test_y_timing_qualify_window(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    fix.opts->scan_voice_qualify_ms = 3000;
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("qualify reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_IDLE_DWELL);
    CHECK("qualify is conventional", fix.state->scan_timing.conventional == 1U);
    CHECK("qualify publishes the effective dwell", fix.state->scan_timing.dwell_ms == 3000U);
    CHECK("qualify publishes the effective hold", fix.state->scan_timing.hold_ms == 2000U);
    check_timing_window("qualify start", "qualify deadline", "qualify span", &fix.state->scan_timing, 100.0, 103.0,
                        3000U);
    check_deadline_matches_should_step(&fix, "qualify holds before the deadline", "qualify steps after the deadline");
    fixture_free(&fix);
}

static void
test_y_timing_voice_and_tail_share_the_hold_window(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    open_voice_epoch(&fix, 100.2, 0.15, DSD_CALL_KIND_GROUP_VOICE, 11, 22, DSD_CALL_CRYPTO_CLEAR);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.4);
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("voice reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_VOICE);
    check_timing_window("voice start", "voice deadline", "voice span", &fix.state->scan_timing, 100.35, 102.35, 2000U);
    check_deadline_matches_should_step(&fix, "voice holds before the deadline", "voice steps after the deadline");
    /* The tail is the same window from the same last-media anchor; only the reason moves. */
    (void)dsd_call_state_end_ex(fix.state, 0, 100.5, DSD_CALL_END_SYNC_LOSS);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 0, 100.6);
    CHECK("tail phase", fix.state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_TAIL);
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("tail reason", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_ACTIVITY_HOLD);
    check_timing_window("tail start", "tail deadline", "tail span", &fix.state->scan_timing, 100.35, 102.35, 2000U);
    check_deadline_matches_should_step(&fix, "tail holds before the deadline", "tail steps after the deadline");
    fixture_free(&fix);
}

static void
test_y_timing_hop_restarts_the_window(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    fix.opts->trunk_hangtime = 2.0f;
    fix.state->last_cc_sync_time = (time_t)100;
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    check_timing_window("first visit start", "first visit deadline", "first visit span", &fix.state->scan_timing, 100.0,
                        101.0, 1000U);
    /* A hop stamps a fresh legacy anchor and re-opens the gate's visit, so the
     * countdown restarts on the new row instead of carrying the old one over. */
    fix.state->last_cc_sync_time = (time_t)200;
    dsd_scan_voice_gate_note_retune(fix.state, 200.0);
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 200.0, 200.0);
    CHECK("hop falls back to hangtime", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_HANGTIME);
    check_timing_window("hop start", "hop deadline", "hop span", &fix.state->scan_timing, 200.0, 202.0, 2000U);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 200.0);
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 200.0, 200.0);
    CHECK("new visit qualifies", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_IDLE_DWELL);
    check_timing_window("new visit start", "new visit deadline", "new visit span", &fix.state->scan_timing, 200.0,
                        201.0, 1000U);
    check_deadline_matches_should_step(&fix, "new visit holds", "new visit steps");
    fixture_free(&fix);
}

/* The per-visit cap rides the same publication (issue #507 extends #508). Nothing arms it
 * yet, and "off" is an explicit negative deadline: a zeroed double would render as a
 * deadline at monotonic 0, which is long past. */
static void
test_y_timing_seeds_visit_cap_off(void) {
    gate_fixture fix;
    if (fixture_init(&fix) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: fixture\n", __func__);
        g_failures++;
        return;
    }
    fix.state->scan_timing.visit_deadline_m = 42.0;
    fix.state->scan_timing.visit_limit_ms = 7000U;
    dsd_scan_timing_clear(fix.state);
    CHECK("clear disarms the visit cap", fix.state->scan_timing.visit_deadline_m < 0.0);
    CHECK("clear zeroes the visit limit", fix.state->scan_timing.visit_limit_ms == 0U);
    /* The gate-owned path seeds it off ... */
    fix.state->scan_timing.visit_deadline_m = 42.0;
    fix.state->scan_timing.visit_limit_ms = 7000U;
    dsd_scan_voice_gate_note_retune(fix.state, 100.0);
    dsd_scan_voice_gate_tick(fix.opts, fix.state, 1, 100.0);
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.0, 100.0);
    CHECK("qualify seeds no visit deadline", fix.state->scan_timing.visit_deadline_m < 0.0);
    CHECK("qualify seeds no visit limit", fix.state->scan_timing.visit_limit_ms == 0U);
    /* ... and so does the legacy hangtime path, which leaves the seed early. */
    fix.state->scan_timing.visit_deadline_m = 42.0;
    fix.state->scan_timing.visit_limit_ms = 7000U;
    fix.opts->scan_voice_only = 0;
    fix.opts->trunk_hangtime = 2.0f;
    fix.state->last_cc_sync_time = (time_t)1000;
    dsd_engine_scan_y_timing_tick(fix.opts, fix.state, 100.25, 1000.25);
    CHECK("hangtime reason still published", fix.state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_HANGTIME);
    CHECK("hangtime seeds no visit deadline", fix.state->scan_timing.visit_deadline_m < 0.0);
    CHECK("hangtime seeds no visit limit", fix.state->scan_timing.visit_limit_ms == 0U);
    fixture_free(&fix);
}

int
main(void) {
    test_tick_leaves_phase_alone_without_scanner_mode();
    test_gate_off_never_steps();
    test_idle_only_steps_at_qualify();
    test_never_synced_falls_back();
    test_voice_holds_through_tail();
    test_terminator_before_first_tick_holds();
    test_span_debounce();
    test_reacquired_segment_reearns_span();
    test_non_media_updates_do_not_extend_hold();
    test_probe_tracks_active_and_retained_slots_independently();
    test_policy_gating();
    test_operator_hold_and_visit_reset();
    test_stale_epoch_cannot_rearm();
    test_zero_ms_falls_back_to_defaults();
    test_tg_hold_call_active();
    test_y_timing_silent_under_trunk_scan();
    test_y_timing_legacy_hangtime_window();
    test_y_timing_hangtime_without_anchor();
    test_y_timing_manual_hold_pauses_the_timer();
    test_y_timing_qualify_window();
    test_y_timing_voice_and_tail_share_the_hold_window();
    test_y_timing_hop_restarts_the_window();
    test_y_timing_seeds_visit_cap_off();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d voice-gate check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
