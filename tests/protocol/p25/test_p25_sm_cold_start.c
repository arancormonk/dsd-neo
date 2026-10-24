// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 startup control-channel hunt (issue #703): a single-system trunked session whose start frequency yields no
 * validated control within the grace window walks its channel map with the ordinary hunt, on the decoder thread only,
 * keeps unproven frequencies out of the control-channel alias, ends in place when validated control names an owner,
 * and parks only once the tuning layer is settled.
 *
 * The tuner stub models the radio separately from the request: g_hw_freq moves only when a tune succeeds, while the
 * decoder's options (rtlsdr_center_freq) are staged on issue, as the engine does for an asynchronous RTL tune.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define T0_NS    1000000000000000000ULL
#define START_HZ 851000000L
#define ROW0_HZ  851012500L
#define ROW1_HZ  851537500L
#define ROW2_HZ  852062500L
#define OPER_HZ  853000000L
#define GRACE_MS 5000U
#define T0_S     ((double)T0_NS / 1e9)

static dsd_opts* g_o;
static dsd_state* g_s;

static int g_tunes;
static long g_last_tune;
static int g_last_sps;
static uint64_t g_last_request;
static long g_hw_freq;
static long g_query_freq; // rigctl answer; 0 = the peer did not answer
static dsd_trunk_tune_result g_script;
static dsd_trunk_tune_result g_vc_script; // voice-channel tunes (a followed grant)

static dsd_trunk_tune_result
stub_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    g_tunes++;
    g_last_tune = freq;
    g_last_sps = ted_sps;
    g_last_request = request_id;
    if (g_script == DSD_TRUNK_TUNE_RESULT_OK) {
        g_hw_freq = freq;
    }
    if (g_script == DSD_TRUNK_TUNE_RESULT_OK || g_script == DSD_TRUNK_TUNE_RESULT_PENDING) {
        // The real CC tune stages the decoder's tuner frequency and the shared alias on issue.
        opts->rtlsdr_center_freq = (uint32_t)freq;
        state->trunk_cc_freq = freq;
        state->last_cc_sync_time = dsd_decode_time();
        state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    }
    return g_script;
}

static dsd_trunk_tune_result
stub_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    if (g_vc_script == DSD_TRUNK_TUNE_RESULT_OK) {
        g_hw_freq = freq;
        opts->trunk_is_tuned = 1;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    }
    return g_vc_script;
}

static dsd_trunk_tune_result
stub_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static long int
stub_query(const dsd_opts* opts) {
    (void)opts;
    return g_query_freq;
}

static void
at_ms(uint64_t ms) {
    dsd_decode_clock_test_set_ns(T0_NS + ms * 1000000ULL);
}

static int
near_s(double got, double want) {
    return fabs(got - want) < 1e-6;
}

/* The engine's call: decoder thread, under the P25 SM tick guard. */
static void
startup_tick(uint64_t ms) {
    at_ms(ms);
    p25_sm_tick_guard_enter();
    p25_sm_cold_start_tick_locked(p25_sm_get_ctx(), g_o, g_s);
    p25_sm_tick_guard_leave();
}

/* The watchdog's call. */
static void
watchdog_tick(uint64_t ms) {
    at_ms(ms);
    p25_sm_try_tick(g_o, g_s);
}

static p25_sm_ctx_t*
sm(void) {
    return p25_sm_get_ctx();
}

/* A -ft session on an RTL tuner parked on a dead start frequency, with a three-row channel map. */
static void
setup_ft(void) {
    dsd_state_trunk_lcn_free(g_s);
    dsd_state_ext_free_all(g_s); // a followed grant attaches the talkgroup policy's state
    DSD_MEMSET(g_o, 0, sizeof(*g_o));
    DSD_MEMSET(g_s, 0, sizeof(*g_s));
    g_o->trunk_enable = 1;
    g_o->frame_p25p1 = g_o->frame_p25p2 = g_o->frame_dmr = 1;
    g_o->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(g_o->audio_in_dev, sizeof(g_o->audio_in_dev), "%s", "rtl:0:851M");
    g_o->rtlsdr_center_freq = (uint32_t)START_HZ;
    DSD_SNPRINTF(g_o->chan_in_file, sizeof(g_o->chan_in_file), "%s", "site.csv");
    g_s->trunk_lcn_freq[0] = ROW0_HZ;
    g_s->trunk_lcn_freq[1] = ROW1_HZ;
    g_s->trunk_lcn_freq[2] = ROW2_HZ;
    g_s->lcn_freq_count = 3;
    g_tunes = 0;
    g_last_tune = 0;
    g_last_sps = -1;
    g_last_request = 0U;
    g_hw_freq = START_HZ;
    g_query_freq = START_HZ;
    g_script = DSD_TRUNK_TUNE_RESULT_OK;
    g_vc_script = DSD_TRUNK_TUNE_RESULT_FAILED;
    dsd_trunk_tuning_requests_reset();
    at_ms(0);
    p25_sm_init_ctx(sm(), g_o, g_s);
}

/* Arm at 0 and run the window out: the first probe goes to row 0 at 5100 ms. */
static void
hunt_first_probe(void) {
    startup_tick(0);
    startup_tick(GRACE_MS + 100U);
    assert(g_tunes == 1 && g_last_tune == ROW0_HZ);
}

static void
test_hunts_the_map_after_the_grace(void) {
    setup_ft();
    startup_tick(0);
    assert(g_tunes == 0 && sm()->t_cold_start_armed_m > 0.0);
    startup_tick(4900);
    assert(g_tunes == 0);
    startup_tick(5100);
    assert(g_tunes == 1 && g_last_tune == ROW0_HZ);
    assert(sm()->state == P25_SM_ON_CC && sm()->cold_start_hunt == 1);
    assert(g_s->trunk_cc_freq == 0 && g_s->p25_cc_freq == 0); // a probe is no control channel
    startup_tick(7300);
    assert(g_tunes == 2 && g_last_tune == ROW1_HZ);
    startup_tick(9500);
    assert(g_tunes == 3 && g_last_tune == ROW2_HZ);
    startup_tick(11700);
    assert(g_tunes == 4 && g_last_tune == ROW0_HZ);
    assert(g_s->trunk_cc_freq == 0 && g_s->p25_cc_freq == 0);
}

static void
test_watchdog_never_runs_the_startup_hunt(void) {
    setup_ft();
    g_o->frame_dmr = 0; // -1/-2: the watchdog itself is admitted
    g_o->frame_p25p2 = 0;
    watchdog_tick(0);
    watchdog_tick(5100);
    watchdog_tick(10000);
    assert(g_tunes == 0 && sm()->state == P25_SM_IDLE && sm()->t_cold_start_armed_m <= 0.0);
    startup_tick(10000);
    startup_tick(15100);
    assert(g_tunes == 1);
    const p25_sm_state_e before = sm()->state;
    const double tune_m = sm()->t_cc_tune_m;
    watchdog_tick(17300); // the probe has timed out, but only the decoder thread may move on
    assert(g_tunes == 1 && sm()->state == before && near_s(sm()->t_cc_tune_m, tune_m));
}

static void
test_validated_control_ends_the_hunt_in_place(void) {
    setup_ft();
    hunt_first_probe();
    at_ms(5600);
    p25_sm_note_validated_cc(g_o, g_s); // a CRC-good block on the probed frequency
    assert(g_s->p25_cc_freq == ROW0_HZ && g_s->trunk_cc_freq == ROW0_HZ);
    startup_tick(5700);
    assert(sm()->cold_start_hunt == 0 && sm()->state == P25_SM_IDLE);
    watchdog_tick(5800); // normal recovery takes the machine on
    assert(sm()->state == P25_SM_ON_CC);
    watchdog_tick(7500);
    startup_tick(7500);
    assert(g_tunes == 1);
}

/* Even once validated control has named the owner, the watchdog leaves a startup hunt to the decoder thread, whose next
   pass ends it: the watchdog never touches a machine in its startup hunt. */
static void
test_watchdog_leaves_a_named_hunt_to_the_decoder(void) {
    setup_ft();
    hunt_first_probe();
    at_ms(5600);
    p25_sm_note_validated_cc(g_o, g_s);
    const p25_sm_state_e probing = sm()->state;
    watchdog_tick(5700);
    assert(sm()->cold_start_hunt == 1 && sm()->state == probing);
    startup_tick(5800);
    assert(sm()->cold_start_hunt == 0 && sm()->state == P25_SM_IDLE);
    watchdog_tick(5900); // ordinary recovery takes the machine on
    assert(sm()->state == P25_SM_ON_CC && g_s->p25_cc_freq == ROW0_HZ);
}

/* A grant followed while a startup hunt waits between probes with no acquisition open (a zero first map row): the owner
   it names ends the startup bookkeeping, never the followed call. */
static void
test_followed_grant_stands(void) {
    setup_ft();
    g_s->trunk_lcn_freq[0] = 0;
    startup_tick(0);
    startup_tick(5100); // the hunt starts on a zero row: nothing to probe yet
    assert(sm()->cold_start_hunt == 1 && sm()->cc_sync_pending == 0 && g_tunes == 0);
    at_ms(5200);
    p25_sm_note_validated_cc(g_o, g_s); // a CRC-good grant on the start frequency names P25
    g_vc_script = DSD_TRUNK_TUNE_RESULT_OK;
    g_o->trunk_tune_group_calls = 1;
    g_s->trunk_chan_map[0x1234] = 852600000L;
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, 852600000L, 1234, 42, 0);
    p25_sm_event(sm(), g_o, g_s, &grant);
    assert(sm()->state == P25_SM_TUNED && g_o->trunk_is_tuned == 1);
    startup_tick(5300);
    assert(sm()->state == P25_SM_TUNED && g_o->trunk_is_tuned == 1 && sm()->cold_start_hunt == 0);
}

/* A rigctl peer that does not answer the frequency query: the decode boundary establishes the control channel from the
   probe the tuner completed, before any handler of the same block (a grant) can end the acquisition. */
static void
test_failed_tuner_query_uses_the_probe(void) {
    setup_ft();
    g_o->audio_in_type = AUDIO_IN_NULL;
    g_o->audio_in_dev[0] = '\0';
    g_o->use_rigctl = 1;
    g_query_freq = 0; // the rigctl peer does not answer
    hunt_first_probe();
    at_ms(5600);
    g_s->p2_is_lcch = 1; // the validated block is an LCCH MAC: a TDMA control channel
    p25_sm_note_validated_cc(g_o, g_s);
    assert(g_s->p25_cc_freq == ROW0_HZ && g_s->trunk_cc_freq == ROW0_HZ);
    assert(g_s->p25_cc_is_tdma == 1); // its type, as the tuner seed records it
    startup_tick(5700);
    assert(g_s->p25_cc_freq == ROW0_HZ && sm()->cold_start_hunt == 0);
}

static void
test_live_cc_at_start_never_hunts(void) {
    setup_ft();
    startup_tick(0);
    at_ms(1000);
    p25_sm_note_validated_cc(g_o, g_s);
    assert(g_s->p25_cc_freq == START_HZ);
    for (uint64_t ms = 1100; ms <= 4500; ms += 400) {
        startup_tick(ms);
        watchdog_tick(ms);
    }
    assert(g_tunes == 0 && sm()->state == P25_SM_ON_CC && sm()->t_cold_start_armed_m <= 0.0);
}

static void
test_raw_sync_never_holds_the_hunt(void) {
    setup_ft();
    for (uint64_t ms = 0; ms <= 5000; ms += 500) {
        at_ms(ms);
        g_s->synctype = g_s->lastsynctype = DSD_SYNC_P25P1_POS; // noise passes sync words
        g_s->last_cc_sync_time_m = dsd_decode_now_mono_s();
        startup_tick(ms);
    }
    startup_tick(5100);
    assert(g_tunes == 1);
}

static void
test_validated_dmr_content_restarts_the_window(void) {
    setup_ft();
    g_s->dmr_confidence_lock_m = T0_S + 3.0; // content came and went while nothing ticked
    startup_tick(0);
    startup_tick(5100);
    startup_tick(7900);
    assert(g_tunes == 0);
    startup_tick(8100);
    assert(g_tunes == 1 && g_last_tune == ROW0_HZ);
    g_s->dmr_confidence_lock_m = T0_S + 9.0; // validated content on the probed frequency
    startup_tick(10300);                     // the probe acquisition times out
    assert(g_tunes == 1 && sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0);
    startup_tick(13900);
    assert(g_tunes == 1);
    startup_tick(14100);
    assert(g_tunes == 2 && g_last_tune == ROW1_HZ); // the walk resumes where it stood
}

static void
test_owner_dmr_mid_hunt_parks_p25(void) {
    setup_ft();
    hunt_first_probe();
    dsd_trunk_recovery_note_protocol(g_s, DSD_TRUNK_RECOVERY_DMR);
    startup_tick(5200);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0 && sm()->cc_sync_pending == 0);
    for (uint64_t ms = 6000; ms <= 30000; ms += 2000) {
        startup_tick(ms);
        watchdog_tick(ms);
    }
    assert(g_tunes == 1 && g_s->p25_cc_freq == 0);
}

static void
test_map_cleared_mid_hunt_stops(void) {
    setup_ft();
    hunt_first_probe();
    g_o->chan_in_file[0] = '\0';
    startup_tick(6100);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0 && sm()->t_cold_start_armed_m <= 0.0);
    startup_tick(7300);
    assert(g_tunes == 1);
    DSD_SNPRINTF(g_o->chan_in_file, sizeof(g_o->chan_in_file), "%s", "site.csv");
    startup_tick(8000); // a map applied live arms a fresh window
    startup_tick(12900);
    assert(g_tunes == 1);
    startup_tick(13100);
    assert(g_tunes == 2);
}

static void
test_input_switch_and_trunking_off_park_at_once(void) {
    setup_ft();
    hunt_first_probe();
    g_s->input_boundary = 1;
    startup_tick(5200);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0 && sm()->t_cold_start_armed_m <= 0.0);
    g_s->input_boundary = 0;
    startup_tick(6000);
    assert(near_s(sm()->t_cold_start_armed_m, T0_S + 6.0));

    setup_ft();
    hunt_first_probe();
    g_o->trunk_enable = 0;
    startup_tick(5200);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0);
}

static void
test_failed_probe_then_next_row(void) {
    setup_ft();
    startup_tick(0);
    g_script = DSD_TRUNK_TUNE_RESULT_PENDING;
    startup_tick(5100);
    const uint64_t first = g_last_request;
    assert(first != 0U && sm()->cc_tune_pending == 1);
    dsd_trunk_tuning_request_publish(first, DSD_TRUNK_TUNE_RESULT_FAILED);
    g_script = DSD_TRUNK_TUNE_RESULT_OK;
    startup_tick(5200);
    assert(g_tunes == 2 && g_last_tune == ROW1_HZ); // the next row replaces the failure
    assert(dsd_trunk_tuning_pending_request() == 0U);
}

static void
test_stop_with_probe_in_flight(void) {
    // Success: the hunt waits for the outcome, then parks.
    setup_ft();
    startup_tick(0);
    g_script = DSD_TRUNK_TUNE_RESULT_PENDING;
    startup_tick(5100);
    const uint64_t a = g_last_request;
    g_o->chan_in_file[0] = '\0'; // eligibility lost with the probe in flight
    startup_tick(5500);
    assert(g_tunes == 1 && sm()->cold_start_hunt == 1 && dsd_trunk_tuning_pending_request() == a);
    g_hw_freq = ROW0_HZ;
    dsd_trunk_tuning_request_publish(a, DSD_TRUNK_TUNE_RESULT_OK);
    startup_tick(5700);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0 && dsd_trunk_tuning_pending_request() == 0U);
    assert(g_tunes == 1);

    // Failure: a settle tune to the frequency the options name replaces it before the park.
    setup_ft();
    startup_tick(0);
    g_script = DSD_TRUNK_TUNE_RESULT_PENDING;
    startup_tick(5100);
    const uint64_t b = g_last_request;
    g_o->chan_in_file[0] = '\0';
    dsd_trunk_tuning_request_publish(b, DSD_TRUNK_TUNE_RESULT_FAILED); // the radio stayed on START_HZ
    assert((long)g_o->rtlsdr_center_freq == ROW0_HZ && g_hw_freq == START_HZ);
    g_script = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    startup_tick(5300);
    assert(g_tunes == 2 && sm()->cold_start_hunt == 1); // a deferred settle tune is retried
    g_script = DSD_TRUNK_TUNE_RESULT_OK;
    startup_tick(5400);
    assert(g_tunes == 3 && g_last_tune == ROW0_HZ && g_hw_freq == (long)g_o->rtlsdr_center_freq);
    assert(g_last_sps == 0); // a settle tune keeps the receive profile in force
    startup_tick(5500);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0 && dsd_trunk_tuning_pending_request() == 0U);
    assert(g_s->p25_cc_freq == 0 && g_s->trunk_cc_freq == 0); // settling establishes nothing
}

/* A tune in flight that the hunt does not track (an ordinary channel-cycle handoff took the acquisition over) is waited
   for, never replaced: a settle tune replaces only a failure. */
static void
test_settle_waits_for_a_tune_in_flight(void) {
    setup_ft();
    startup_tick(0);
    g_script = DSD_TRUNK_TUNE_RESULT_PENDING;
    startup_tick(5100);
    const uint64_t probe = g_last_request;
    at_ms(5200);
    p25_sm_cold_start_note_retune(sm(), g_o, g_s); // an operator move...
    (void)p25_sm_on_external_cc_tune(sm(), g_o, g_s, DSD_TRUNK_TUNE_RESULT_OK, 0U, "channel-cycle"); // ...handed over
    startup_tick(5300);
    startup_tick(5400);
    assert(g_tunes == 1 && sm()->cold_start_hunt == 1);
    g_hw_freq = ROW0_HZ;
    dsd_trunk_tuning_request_publish(probe, DSD_TRUNK_TUNE_RESULT_OK);
    startup_tick(5500);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0 && g_tunes == 1);
}

static void
test_stop_after_two_failures(void) {
    setup_ft();
    startup_tick(0);
    g_script = DSD_TRUNK_TUNE_RESULT_PENDING;
    startup_tick(5100);
    dsd_trunk_tuning_request_publish(g_last_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    startup_tick(5200); // the failure is followed by the next probe, which fails too
    assert(g_tunes == 2 && g_last_tune == ROW1_HZ);
    dsd_trunk_tuning_request_publish(g_last_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    g_o->chan_in_file[0] = '\0';
    g_script = DSD_TRUNK_TUNE_RESULT_OK;
    startup_tick(5300);
    assert(g_tunes == 3 && g_last_tune == ROW1_HZ); // one settle tune to what the options name
    startup_tick(5400);
    assert(sm()->state == P25_SM_IDLE && dsd_trunk_tuning_pending_request() == 0U);
}

static void
test_operator_moves(void) {
    // Settled: the hunt parks keeping a window that runs from the move.
    setup_ft();
    hunt_first_probe();
    at_ms(6000);
    p25_sm_cold_start_note_retune(sm(), g_o, g_s);
    startup_tick(6100);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0);
    assert(near_s(sm()->t_cold_start_armed_m, T0_S + 6.0));
    startup_tick(10900);
    assert(g_tunes == 1);
    startup_tick(11100);
    assert(g_tunes == 2);

    // A probe in flight: the operator's tune lands after it; the park waits for its outcome and keeps the operator's
    // frequency as the receiver's.
    setup_ft();
    startup_tick(0);
    g_script = DSD_TRUNK_TUNE_RESULT_PENDING;
    startup_tick(5100);
    const uint64_t a = g_last_request;
    at_ms(5500);
    g_o->rtlsdr_center_freq = (uint32_t)OPER_HZ; // the operator's generic tune
    g_hw_freq = OPER_HZ;
    p25_sm_cold_start_note_retune(sm(), g_o, g_s);
    startup_tick(5600);
    assert(sm()->cold_start_hunt == 1 && g_tunes == 1);
    dsd_trunk_tuning_request_publish(a, DSD_TRUNK_TUNE_RESULT_OK);
    startup_tick(5800);
    assert(sm()->state == P25_SM_IDLE && sm()->cold_start_hunt == 0 && dsd_trunk_tuning_pending_request() == 0U);
    at_ms(5900);
    p25_sm_note_validated_cc(g_o, g_s);
    assert(g_s->p25_cc_freq == OPER_HZ); // never the probe the operator moved off

    // Two failed probes, then a move whose own tune was refused: the options still name the last probe; a settle tune
    // makes the radio agree before the park.
    setup_ft();
    startup_tick(0);
    g_script = DSD_TRUNK_TUNE_RESULT_PENDING;
    startup_tick(5100);
    dsd_trunk_tuning_request_publish(g_last_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    startup_tick(5200);
    dsd_trunk_tuning_request_publish(g_last_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    at_ms(5250);
    p25_sm_cold_start_note_retune(sm(), g_o, g_s); // the boundary of a RadioReference apply whose tune is refused
    g_script = DSD_TRUNK_TUNE_RESULT_OK;
    startup_tick(5300);
    assert(g_last_tune == ROW1_HZ && g_hw_freq == ROW1_HZ && (long)g_o->rtlsdr_center_freq == ROW1_HZ);
    startup_tick(5400);
    assert(sm()->state == P25_SM_IDLE && dsd_trunk_tuning_pending_request() == 0U && g_s->p25_cc_freq == 0);

    // Armed but not hunting yet: the window restarts at the move.
    setup_ft();
    startup_tick(0);
    at_ms(3000);
    p25_sm_cold_start_note_retune(sm(), g_o, g_s);
    startup_tick(5100);
    startup_tick(7900);
    assert(g_tunes == 0);
    startup_tick(8100);
    assert(g_tunes == 1);
}

/* Auto (-fa) presumes P25 for the startup hunt as -ft does; a selected EDACS session (-fh -fa), whose control names
   no owner, and a session without a channel map never hunt. */
static void
test_edacs_session_and_no_map_never_hunt(void) {
    setup_ft();
    g_o->frame_nxdn48 = g_o->frame_nxdn96 = g_o->frame_provoice = 1; // -fa
    g_s->ea_mode = -1;                                               // selects no EDACS addressing
    for (uint64_t ms = 0; ms <= 20000; ms += 1000) {
        startup_tick(ms);
    }
    assert(g_tunes >= 1 && g_last_sps > 0); // a P25 probe
    setup_ft();
    g_o->frame_nxdn48 = g_o->frame_nxdn96 = g_o->frame_provoice = 1;
    g_s->ea_mode = 0; // -fh -fa
    for (uint64_t ms = 0; ms <= 20000; ms += 1000) {
        startup_tick(ms);
    }
    assert(g_tunes == 0);
    setup_ft();
    g_o->chan_in_file[0] = '\0';
    for (uint64_t ms = 0; ms <= 20000; ms += 1000) {
        startup_tick(ms);
    }
    assert(g_tunes == 0);
}

static void
test_external_acquisition_ends_cold_start(void) {
    setup_ft();
    g_o->frame_dmr = 0; // -1/-2, where the watchdog owns ordinary recovery
    g_o->frame_p25p2 = 0;
    hunt_first_probe();
    at_ms(5500);
    (void)p25_sm_restart_pending_cc_acquisition(sm(), g_o, g_s, dsd_decode_now_mono_s(), "test");
    assert(sm()->cold_start_hunt == 0 && sm()->t_cold_start_armed_m <= 0.0);
    watchdog_tick(11000); // the watchdog ticks it again: its timeout leads to an ordinary hunt
    assert(g_tunes == 2);
}

static void
test_note_validated_cc_helper(void) {
    setup_ft();
    g_o->trunk_enable = 0;
    p25_sm_note_validated_cc(g_o, g_s);
    assert(g_s->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_P25 && g_s->p25_cc_freq == 0);
    setup_ft();
    g_s->p25_cc_freq = ROW2_HZ;
    p25_sm_note_validated_cc(g_o, g_s);
    assert(g_s->p25_cc_freq == ROW2_HZ);
    setup_ft();
    g_o->trunk_is_tuned = 1; // on a voice channel the shared alias names the control channel
    g_s->trunk_cc_freq = ROW1_HZ;
    p25_sm_note_validated_cc(g_o, g_s);
    assert(g_s->p25_cc_freq == ROW1_HZ);
}

int
main(void) {
    g_o = (dsd_opts*)calloc(1, sizeof(*g_o));
    g_s = (dsd_state*)calloc(1, sizeof(*g_s));
    assert(g_o && g_s);
    dsd_decode_clock_use_test(T0_NS);
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = stub_tune_to_cc;
    hooks.tune_to_freq_request = stub_tune_to_freq;
    hooks.return_to_cc_request = stub_return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = stub_query});

    test_hunts_the_map_after_the_grace();
    test_watchdog_never_runs_the_startup_hunt();
    test_validated_control_ends_the_hunt_in_place();
    test_watchdog_leaves_a_named_hunt_to_the_decoder();
    test_failed_tuner_query_uses_the_probe();
    test_followed_grant_stands();
    test_live_cc_at_start_never_hunts();
    test_raw_sync_never_holds_the_hunt();
    test_validated_dmr_content_restarts_the_window();
    test_owner_dmr_mid_hunt_parks_p25();
    test_map_cleared_mid_hunt_stops();
    test_input_switch_and_trunking_off_park_at_once();
    test_failed_probe_then_next_row();
    test_stop_with_probe_in_flight();
    test_settle_waits_for_a_tune_in_flight();
    test_stop_after_two_failures();
    test_operator_moves();
    test_edacs_session_and_no_map_never_hunt();
    test_external_acquisition_ends_cold_start();
    test_note_validated_cc_helper();

    p25_sm_init_ctx(sm(), NULL, NULL);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_trunk_tuning_requests_reset();
    dsd_decode_clock_use_system();
    dsd_state_trunk_lcn_free(g_s);
    dsd_state_ext_free_all(g_s);
    free(g_s);
    free(g_o);
    printf("P25 SM cold start: OK\n");
    return 0;
}
