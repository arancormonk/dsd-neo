// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The NXDN control-channel hunt (src/protocol/nxdn/nxdn_cc_hunt.c): a single-system NXDN session (an IDAS or NEXEDGE
 * import: -fi or -fn, -T, a channel map) whose control channel yields no validated content within the grace window
 * walks its map until a CAC that passed its CRC names the control channel, at session start and after a control
 * channel a CAC confirmed goes quiet. A Type-D site never hunts once proven.
 *
 * The tuner is a fake behind the trunk tuning hook table and the rigctl query hook; the decode clock is the test source.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/nxdn/nxdn_cc_hunt.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_confirm.h"

#define ROW_A 451000000L
#define ROW_B 452000000L
#define ROW_C 453000000L
#define T0_NS 1000000000000ULL

enum fake_mode { FAKE_OK = 0, FAKE_PENDING, FAKE_FAIL };

static dsd_opts* g_opts;
static dsd_state* g_state;
static int g_failures;
static enum fake_mode g_cc_mode;
static enum fake_mode g_freq_mode;
static long g_tunes[64];
static int g_tune_count;
static long g_radio_hz;
static int g_queries;
static uint64_t g_now_ns;

static void
expect(const char* label, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", label, got, want);
        g_failures++;
    }
}

static dsd_trunk_tune_result
fake_tune(enum fake_mode mode, dsd_state* state, long freq) {
    if (g_tune_count < 64) {
        g_tunes[g_tune_count] = freq;
    }
    g_tune_count++;
    if (mode == FAKE_FAIL) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    g_radio_hz = freq;
    state->trunk_cc_freq = freq; // as the engine's tune stages it
    return mode == FAKE_PENDING ? DSD_TRUNK_TUNE_RESULT_PENDING : DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
fake_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)ted_sps;
    (void)request_id;
    return fake_tune(g_cc_mode, state, freq);
}

static dsd_trunk_tune_result
fake_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)ted_sps;
    (void)request_id;
    return fake_tune(g_freq_mode, state, freq);
}

static long int
fake_query(const dsd_opts* opts) {
    (void)opts;
    g_queries++;
    return g_radio_hz;
}

static void
clock_at(double seconds) {
    g_now_ns = T0_NS + (uint64_t)(seconds * 1e9);
    dsd_decode_clock_test_set_ns(g_now_ns);
}

static void
tick_at(double seconds) {
    clock_at(seconds);
    nxdn_cc_hunt_tick_locked(g_opts, g_state);
}

/* Tick every 100 ms through (from, to]. */
static void
run_until(double from, double to) {
    for (double t = from + 0.1; t <= to + 1e-9; t += 0.1) {
        tick_at(t);
    }
}

/* An -fi session on rigctl, the radio on row A, a three-row map, no owner. */
static void
session(void) {
    dsd_state_ext_free_all(g_state);
    dsd_state_trunk_lcn_free(g_state);
    DSD_MEMSET(g_opts, 0, sizeof(*g_opts));
    DSD_MEMSET(g_state, 0, sizeof(*g_state));
    g_opts->trunk_enable = 1;
    g_opts->use_rigctl = 1;
    g_opts->audio_in_type = AUDIO_IN_TCP;
    DSD_SNPRINTF(g_opts->audio_in_dev, sizeof(g_opts->audio_in_dev), "%s", "tcp:127.0.0.1:7355");
    g_opts->frame_nxdn48 = 1;
    DSD_SNPRINTF(g_opts->chan_in_file, sizeof(g_opts->chan_in_file), "%s", "idas.csv");
    g_state->trunk_lcn_freq[0] = ROW_A;
    g_state->trunk_lcn_freq[1] = ROW_B;
    g_state->trunk_lcn_freq[2] = ROW_C;
    g_state->lcn_freq_count = 3;
    g_radio_hz = ROW_A;
    g_cc_mode = FAKE_OK;
    g_freq_mode = FAKE_OK;
    g_tune_count = 0;
    g_queries = 0;
    dsd_trunk_tuning_requests_reset();
    clock_at(0.0);
}

/* A dead start frequency: the window arms, expires after the grace, and the map is walked in file order, two seconds a
   row, without any probe becoming the control channel. */
static void
test_startup_walk(void) {
    session();
    tick_at(0.0);
    expect("startup: armed", nxdn_cc_hunt_active(g_state), 1);
    run_until(0.0, 5.9);
    expect("startup: no probe inside the grace", g_tune_count, 0);
    run_until(5.9, 12.5);
    expect("startup: probes", g_tune_count, 4);
    expect("startup: row A first", g_tunes[0], ROW_A);
    expect("startup: then B", g_tunes[1], ROW_B);
    expect("startup: then C", g_tunes[2], ROW_C);
    expect("startup: then A again", g_tunes[3], ROW_A);
    expect("startup: no control channel staged", g_state->trunk_cc_freq, 0);
    expect("startup: no P25 alias staged", g_state->p25_cc_freq, 0);
    expect("startup: no owner", g_state->trunk_recovery_protocol, DSD_TRUNK_RECOVERY_UNKNOWN);
}

/* A CAC on a probe ends the hunt there: the completed probe is the control channel, without a tuner query. */
static void
test_cac_ends_hunt(void) {
    session();
    tick_at(0.0);
    run_until(0.0, 8.5); // probes A (6.1) and B (8.1)
    expect("cac: two probes", g_tune_count, 2);
    const int queries = g_queries;
    nxdn_cc_hunt_note_control(g_opts, g_state);
    expect("cac: owner named", g_state->trunk_recovery_protocol, DSD_TRUNK_RECOVERY_NXDN);
    expect("cac: control channel", g_state->p25_cc_freq, ROW_B);
    expect("cac: shared alias", g_state->trunk_cc_freq, ROW_B);
    expect("cac: attributed from the probe", g_queries, queries);
    run_until(8.5, 14.4);
    expect("cac: no probe while the control channel is fresh", g_tune_count, 2);
}

/* Starting on a live control channel, repeated CACs ask rigctl once per tuning generation; an operator move forgets
   the attribution. */
static void
test_attribution_cached(void) {
    session();
    tick_at(0.0);
    for (int i = 0; i < 10; i++) {
        clock_at(0.1 * (double)(i + 1));
        nxdn_cc_hunt_note_control(g_opts, g_state);
    }
    expect("cache: one query for ten CACs", g_queries, 1);
    expect("cache: control channel", g_state->p25_cc_freq, ROW_A);
    nxdn_cc_hunt_note_retune(g_opts, g_state);
    g_radio_hz = ROW_C;
    nxdn_cc_hunt_note_control(g_opts, g_state);
    expect("cache: requeried after a move", g_queries, 2);
    expect("cache: moved control channel", g_state->p25_cc_freq, ROW_C);
}

/* A control channel a CAC confirmed goes quiet: after the grace the map is hunted, the old control channel revisited
   between alternate rows, and the return destination is kept until a CAC proves another. */
static void
test_post_loss(void) {
    session();
    tick_at(0.0);
    run_until(0.0, 8.5);                        // probes A, B
    nxdn_cc_hunt_note_control(g_opts, g_state); // B confirmed at 8.5
    run_until(8.5, 20.6);
    expect("loss: probes after the grace", g_tune_count, 6);
    expect("loss: next row first", g_tunes[2], ROW_C);
    expect("loss: control channel revisited", g_tunes[3], ROW_B);
    expect("loss: then the next row", g_tunes[4], ROW_A);
    expect("loss: revisited again", g_tunes[5], ROW_B);
    expect("loss: return destination kept", g_state->p25_cc_freq, ROW_B);
    expect("loss: owner kept", g_state->trunk_recovery_protocol, DSD_TRUNK_RECOVERY_NXDN);
}

/* Validated NXDN content holds the window (a busy traffic channel is not dead), and on a probe parks the hunt there. */
static void
test_content_holds(void) {
    session();
    tick_at(0.0);
    for (double t = 0.1; t <= 15.0 + 1e-9; t += 0.1) {
        g_state->nxdn_confirm_lock_m = dsd_decode_now_mono_s();
        tick_at(t);
    }
    expect("content: no probe while content keeps coming", g_tune_count, 0);
    run_until(15.0, 21.5);
    expect("content: hunt once the content stops", g_tune_count, 1);
    clock_at(22.0);
    g_state->nxdn_confirm_lock_m = dsd_decode_now_mono_s(); // a call on the probed row
    run_until(22.0, 27.9);
    expect("content: parked on the probe", g_tune_count, 1);
    run_until(27.9, 28.5);
    expect("content: resumes after the grace", g_tune_count, 2);
}

/* A Type-D site: a confirmed SCCH names the owner and ends a startup hunt; a Type-C control channel confirmed before
   is forgotten, so silence afterwards never hunts. */
static void
test_type_d(void) {
    session();
    tick_at(0.0);
    run_until(0.0, 6.5);
    expect("type-d: hunting", g_tune_count, 1);
    nxdn_cc_hunt_note_type_d(g_opts, g_state);
    expect("type-d: owner", g_state->trunk_recovery_protocol, DSD_TRUNK_RECOVERY_NXDN);
    expect("type-d: inactive", nxdn_cc_hunt_active(g_state), 0);
    run_until(6.5, 40.0);
    expect("type-d: no further probe", g_tune_count, 1);
    expect("type-d: not due", nxdn_cc_hunt_due(g_opts, g_state), 0);

    session();
    tick_at(0.0);
    nxdn_cc_hunt_note_control(g_opts, g_state); // a Type-C control channel
    nxdn_cc_hunt_note_retune(g_opts, g_state);  // the operator moves to a Type-D system
    nxdn_cc_hunt_note_type_d(g_opts, g_state);
    run_until(0.0, 40.0);
    expect("type-d after type-c: never hunts", g_tune_count, 0);
}

/* -fa: no startup hunt; validated NXDN control arms recovery at once. A mode change away from NXDN stops a hunt. */
static void
test_frame_sets(void) {
    session();
    g_opts->frame_p25p1 = g_opts->frame_p25p2 = g_opts->frame_dmr = g_opts->frame_provoice = 1;
    tick_at(0.0);
    expect("-fa: not due", nxdn_cc_hunt_due(g_opts, g_state), 0);
    run_until(0.0, 20.0);
    expect("-fa: no startup hunt", g_tune_count, 0);
    nxdn_cc_hunt_note_control(g_opts, g_state);
    expect("-fa: armed by the CAC", nxdn_cc_hunt_active(g_state), 1);
    run_until(20.0, 26.5);
    expect("-fa: recovery hunts", g_tune_count, 1);
    g_opts->frame_nxdn48 = 0; // the operator switches to P25 only
    g_opts->frame_dmr = g_opts->frame_provoice = 0;
    run_until(26.5, 40.0);
    expect("mode change: no further probe", g_tune_count, 1);
    expect("mode change: parked", nxdn_cc_hunt_active(g_state), 0);
}

/* A call's return to the control channel that is still in flight restarts the window at its completion and holds
   every probe; DISC clears trunk_is_tuned before the tune lands. */
static void
test_pending_return(void) {
    session();
    tick_at(0.0);
    nxdn_cc_hunt_note_control(g_opts, g_state); // A
    g_freq_mode = FAKE_PENDING;
    uint64_t foreign = 0U;
    clock_at(1.0);
    (void)dsd_trunk_tuning_hook_tune_to_freq(g_opts, g_state, ROW_A, 0, &foreign);
    expect("return: request in flight", foreign != 0U, 1);
    const int tunes = g_tune_count;
    run_until(1.0, 20.0);
    expect("return: no probe while it is in flight", g_tune_count, tunes);
    dsd_trunk_tuning_request_complete(foreign, DSD_TRUNK_TUNE_RESULT_OK);
    run_until(20.0, 25.9);
    expect("return: grace from the completion", g_tune_count, tunes);
    run_until(25.9, 26.5);
    expect("return: then the hunt", g_tune_count, tunes + 1);
}

/* Each hunt tune lands on another carrier: a weak streak or sticky confirmation from the last one does not carry. */
static void
test_carrier_evidence_reset(void) {
    session();
    tick_at(0.0);
    run_until(0.0, 5.9);
    g_state->nxdn_confirmed = 1;
    g_state->nxdn_confirm_weak_streak = 1;
    g_state->nxdn_sacch_frame_segcrc[0] = 0;
    run_until(5.9, 6.5);
    expect("evidence: probe", g_tune_count, 1);
    expect("evidence: confirmation reset", g_state->nxdn_confirmed, 0);
    expect("evidence: weak streak reset", g_state->nxdn_confirm_weak_streak, 0);
    expect("evidence: SACCH assembly reset", g_state->nxdn_sacch_frame_segcrc[0], 1);
}

/* A probe whose tune is unresolved is never credited; the deadline fails it and the hunt moves on. */
static void
test_pending_probe(void) {
    session();
    g_cc_mode = FAKE_PENDING;
    tick_at(0.0);
    run_until(0.0, 6.5);
    expect("pending: probe issued", g_tune_count, 1);
    nxdn_cc_hunt_note_control(g_opts, g_state);
    expect("pending: not credited", g_state->trunk_recovery_protocol, DSD_TRUNK_RECOVERY_UNKNOWN);
    run_until(6.5, 10.9);
    expect("pending: waits for the deadline", g_tune_count, 1);
    g_cc_mode = FAKE_OK;
    run_until(10.9, 11.5);
    expect("pending: next probe after the deadline", g_tune_count, 2);
    expect("pending: next row", g_tunes[1], ROW_B);
}

/* An operator move parks a hunt keeping a window from the move; trunking turned off parks it unarmed. */
static void
test_operator_and_trunking_off(void) {
    session();
    tick_at(0.0);
    run_until(0.0, 6.5);
    clock_at(7.0);
    nxdn_cc_hunt_note_retune(g_opts, g_state);
    run_until(7.0, 12.9);
    expect("move: parked", g_tune_count, 1);
    run_until(12.9, 13.5);
    expect("move: hunt resumes after the grace from the move", g_tune_count, 2);
    g_opts->trunk_enable = 0;
    run_until(13.5, 30.0);
    expect("trunking off: no probe", g_tune_count, 2);
    expect("trunking off: inactive", nxdn_cc_hunt_active(g_state), 0);
}

/* What never hunts: -Y, trunk scan, an I/Q replay, no map, an input nothing can retune. */
static void
test_never(void) {
    for (int c = 0; c < 5; c++) {
        session();
        switch (c) {
            case 0: g_opts->scanner_mode = 1; break;
            case 1: g_opts->trunk_scan_enabled = 1; break;
            case 2:
                g_opts->use_rigctl = 0;
                g_opts->audio_in_type = AUDIO_IN_RTL;
                DSD_SNPRINTF(g_opts->audio_in_dev, sizeof(g_opts->audio_in_dev), "%s", "iqreplay:x.cu8");
                break;
            case 3: g_opts->chan_in_file[0] = '\0'; break;
            default: g_opts->use_rigctl = 0; break;
        }
        tick_at(0.0);
        run_until(0.0, 20.0);
        expect("never: no probe", g_tune_count, 0);
        expect("never: not due", nxdn_cc_hunt_due(g_opts, g_state), 0);
    }
}

int
main(void) {
    g_opts = (dsd_opts*)calloc(1, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1, sizeof(*g_state));
    if (!g_opts || !g_state) {
        return 1;
    }
    dsd_decode_clock_use_test(T0_NS);
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = fake_tune_to_cc;
    hooks.tune_to_freq_request = fake_tune_to_freq;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_rigctl_query_hooks query = {0};
    query.get_current_freq_hz = fake_query;
    dsd_rigctl_query_hooks_set(query);

    test_startup_walk();
    test_cac_ends_hunt();
    test_attribution_cached();
    test_post_loss();
    test_content_holds();
    test_type_d();
    test_frame_sets();
    test_pending_return();
    test_carrier_evidence_reset();
    test_pending_probe();
    test_operator_and_trunking_off();
    test_never();

    dsd_state_ext_free_all(g_state);
    dsd_state_trunk_lcn_free(g_state);
    free(g_state);
    free(g_opts);
    if (g_failures == 0) {
        DSD_FPRINTF(stdout, "NXDN_CC_HUNT: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
