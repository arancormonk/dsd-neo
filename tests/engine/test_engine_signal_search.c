// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/manual_tune.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_avoids.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static dsd_signal_search_status status;
static dsd_signal_capture capture = {1536000, 100384000, 1, 384000, 384000, 0, 0};
static dsd_signal_measure_request request;
static uint32_t ticket, target, generation = 1;
static int tune_pending, buffered, defer, failed, live = 1, ready, cancelled, bypassed, carrier, boundary;
/* The monitor has judged the landed channel (dsd_analog_rx_carrier_observed_now()). */
static int observed = 1;
static int family, gain_value = 123, gain_auto, autogain = 1;
static uint64_t now_ms = 1000;
/* Independent of the decode clock: the capture can arrive while its timestamp is being read. */
static uint64_t realtime_ms = 1000000;
static uint64_t capture_read_advance_ms;
static uint64_t capture_ahead_ms;
static uint64_t tuner_owner;
/* How long ago the radio input delivered its latest samples. */
static uint64_t stalled_ms;
/* State snapshots published to frontends, and the carrier boundaries run when the latest was. */
static int snapshots, snapshot_boundary;

uint64_t
test_dsd_realtime_mono_ms(void) {
    return realtime_ms;
}

static void
count_snapshot(const dsd_state* state) {
    (void)state;
    snapshots++;
    snapshot_boundary = boundary;
}

/* The receive family the latest retune carries, and its target; -1 when none is attached. */
static rtl_stream_retune_analog_profile retune_family = {-1, 0, 0, 0};
static uint32_t retune_family_hz;

void
rtl_signal_search_set_owner(uint64_t session) {
    tuner_owner = session;
}

/* Publications that carried a changed hit, and the last such hit. */
static int hits_published;
static dsd_signal_search_hit published_hit;

static void
observer(const dsd_signal_search_status* s, const dsd_signal_search_hit* h, void* ctx) {
    (void)ctx;
    status = *s;
    if (h) {
        ++hits_published;
        published_hit = *h;
    }
}

static void
advance(unsigned ms) {
    now_ms += ms;
    dsd_decode_clock_test_set_ns(now_ms * 1000000);
}

/* The canonical call records the coordinator reads, one per slot (the core's dsd_call_state_get(), stubbed). */
static dsd_call_snapshot calls[DSD_CALL_STATE_SLOT_COUNT];

int
dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    (void)state;
    if (slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    *out = calls[slot];
    return out->epoch != 0U ? 1 : 0;
}

int
rtl_stream_is_active(void) {
    return live;
}

uint32_t
rtl_stream_output_generation(void) {
    return generation;
}

int
rtl_stream_tune_async(uint32_t hz, uint64_t owner, uint32_t* out) {
    assert(owner >= 42 && owner <= 99);
    if (defer) {
        --defer;
        return 1;
    }
    target = hz;
    *out = ++ticket;
    tune_pending = 1;
    return 0;
}

int
rtl_stream_tune_poll(uint32_t id, int* result) {
    assert(id == ticket);
    if (tune_pending) {
        return 0;
    }
    *result = failed ? -1 : 0;
    return 1;
}

static void
applied(void) {
    tune_pending = 0;
    ++generation;
    if (!buffered) {
        capture.generation = generation;
        capture.capture_hz = target + 384000;
    }
}

int
rtl_signal_capture_get(dsd_signal_capture* out) {
    *out = capture;
    return 1;
}

uint64_t
rtl_signal_capture_last_ms(void) {
    realtime_ms += capture_read_advance_ms;
    return dsd_realtime_mono_ms() + capture_ahead_ms - stalled_ms;
}

int
rtl_signal_measure_request(const dsd_signal_measure_request* r) {
    request = *r;
    return 0;
}

int
rtl_signal_measure_get(dsd_signal_measure_report* out) {
    if (!ready) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->session_id = request.session_id;
    out->request_id = request.request_id;
    out->generation = capture.generation;
    out->count = request.count;
    for (uint32_t i = 0; i < out->count; ++i) {
        out->candidates[i].frequency_hz = request.frequencies[i];
        out->candidates[i].valid = 1;
        out->candidates[i].detected = ready == 1;
        // A single-channel request is the live strength of a landed visit; give it its own level.
        out->candidates[i].power_dbfs = out->count == 1 ? -40.0f : -30 + (float)i;
        out->candidates[i].noise_dbfs = -70;
    }
    if (ready == 3) {
        --out->request_id;
    } // stale, never a detection
    ready = 0;
    return 1;
}

void
rtl_signal_measure_cancel(void) {
    ++cancelled;
    ready = 0;
}

void
rtl_signal_search_bypass(int value) {
    bypassed = value;
}

int
rtl_stream_get_gain(int* gain, int* automatic) {
    *gain = gain_value;
    *automatic = gain_auto;
    return 0;
}

int
rtl_stream_get_tuner_autogain(void) {
    return autogain;
}

void
rtl_stream_enforce_tuner_autogain(int value) {
    autogain = value;
}

void
rtl_stream_clear_pending_retune_profile(void) {}

int
rtl_stream_airspy_controls(const dsd_airspy_config* cfg) {
    (void)cfg;
    return 0;
}

int
rtl_stream_request_analog_profile(int f, int kind, int width) {
    (void)kind;
    (void)width;
    family = f;
    return 0;
}

int
rtl_stream_prepare_retune_analog_profile_for_target(uint32_t hz, const rtl_stream_retune_analog_profile* p) {
    family = p->family;
    retune_family = *p;
    retune_family_hz = hz;
    return 0;
}

void
rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t hz, int cq, int rate, int levels, int profile, int sps,
                                                       int persist, const rtl_stream_retune_gain_profile* g) {
    (void)hz;
    (void)cq;
    (void)rate;
    (void)levels;
    (void)profile;
    (void)sps;
    (void)persist;
    gain_value = g->tuner_gain_tenth_db;
    gain_auto = g->tuner_gain_is_auto;
    autogain = g->tuner_autogain_on;
}

int
dsd_audio_ensure_analog_output(dsd_opts* opts) {
    (void)opts;
    return 0;
}

int
dsd_audio_ensure_digital_output(dsd_opts* opts, const dsd_state* state) {
    (void)state;
    (void)opts;
    return 0;
}

int
dsd_scan_analog_carrier_open(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return carrier;
}

int
dsd_analog_rx_carrier_observed_now(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return observed;
}

static int manual_supersedes;

void
dsd_engine_manual_tune_supersede(dsd_state* state) {
    (void)state;
    ++manual_supersedes;
}

/* The visit tag in force when the latest carrier boundary ran: what the recordings it closed are published under. */
static int boundary_tagged;
static uint64_t boundary_session, boundary_hit;
/* The P25 Phase 2 code in force when the latest carrier boundary ran, and whether it was proven: what the calls it
   ended were labelled with. */
static uint64_t boundary_code;
static uint8_t boundary_proven;

/* The P25 Phase 2 code (WACN(20) << 24 | SYSID(12) << 12 | NAC(12)) @p state holds. */
static uint64_t
code_held(const dsd_state* state) {
    return ((uint64_t)state->p2_wacn << 24) | ((uint64_t)state->p2_sysid << 12) | (uint64_t)state->p2_cc;
}

/* As the engine's carrier boundary: the carrier left's codes and their proof go (dsd_engine_forget_carrier_codes()),
   and the carrier count moves, which drops a P25 Phase 2 code recovery's candidate waiting on that carrier (the
   boundary's p25p2_frame_forget_carrier() drops it at once). */
void
dsd_engine_carrier_boundary(dsd_opts* opts, dsd_state* state, dsd_carrier_boundary_kind reason, int guard) {
    (void)opts;
    (void)reason;
    (void)guard;
    ++boundary;
    boundary_tagged = dsd_signal_search_visit_get(&boundary_session, &boundary_hit);
    boundary_code = code_held(state);
    boundary_proven = state->p2_cc_verified;
    state->dmr_color_code = 16;
    state->nac = 0;
    dsd_state_forget_p2_proof(state);
    state->carrier_seq++;
}

/* What the engine's signal_search_probe answer was at the latest acquisition reset: what the frame sync keeps for its
   hunt order (issue #804). */
static int reset_probe_answer = -1;

void
dsd_frame_sync_reset_acquisition(const dsd_opts* opts, dsd_state* state, int clear) {
    (void)clear;
    reset_probe_answer = dsd_engine_signal_search_probe(opts, state);
}

static void
tick(dsd_opts* o, dsd_state* s) {
    (void)dsd_engine_signal_search_tick(o, s);
}

static void
act(dsd_opts* o, dsd_state* s, int action) {
    dsd_signal_search_control a = {status.session_id, status.hit.hit_id, action, 0};
    assert(dsd_engine_signal_search_control(o, s, &a) == 0);
    tick(o, s);
}

static void
land(dsd_opts* o, dsd_state* s) {
    applied();
    tick(o, s);
}

/* Land on a detection from a fresh span measurement: the strongest candidate is the last one. */
static void
visit_strongest(dsd_opts* o, dsd_state* s) {
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && !status.hit.ended);
    land(o, s);
}

static void
test_listening_verdict_and_live_strength(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    failed = 0;
    live = 1;
    carrier = 0;
    config->session_id = 44;
    config->ranges[0].digital = 0;
    config->ranges[0].max_visit_ms = 0;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);

    // An analog monitor that has not judged the landed channel yet is no quiet channel: a front end still designing
    // its noise-squelch plan delivers nothing for it to read. Quiet timing starts at the first verdict.
    observed = 0;
    visit_strongest(o, s);
    const uint32_t first = status.hit.frequency_hz;
    assert(status.phase == DSD_SEARCH_LISTENING && first == 100025000);
    advance(2000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    assert(!dsd_engine_signal_search_should_yield(o, s));
    observed = 1; // the first read finds no carrier: quiet timing starts now
    tick(o, s);
    advance(499);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && !dsd_engine_signal_search_should_yield(o, s));
    advance(1);
    assert(dsd_engine_signal_search_should_yield(o, s));
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);

    // A verdict that never comes is bounded: quiet timing starts at the deadline.
    observed = 0;
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.frequency_hz == 100012500);
    advance(2999);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    advance(1);
    tick(o, s);
    advance(499);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    advance(1);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);

    // Live strength: the landed channel alone, on the detector's scale, and never carried past the visit.
    observed = 1;
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING);
    ready = 1;
    tick(o, s); // the one detection not yet visited this pass
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.frequency_hz == 100000000);
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && !status.live_power_valid);
    carrier = 1;
    tick(o, s);
    assert(request.count == 1 && request.frequencies[0] == status.hit.frequency_hz);
    assert(request.capture.capture_hz - request.capture.capture_offset_hz == status.hit.frequency_hz);
    const uint64_t seq_before = status.live_seq;
    ready = 2; // measured, below the detection threshold: strength is still reported
    advance(100);
    tick(o, s);
    assert(status.live_power_valid && status.live_power_dbfs > -40.5f && status.live_power_dbfs < -39.5f);
    assert(status.hit.power_dbfs > -30.5f && status.hit.power_dbfs < -29.5f); // detection-time power is kept apart
    assert(status.live_seq == seq_before + 1);
    // The next measurement reads the same level: it is still a fresh reading, and the sequence says so.
    const float first_level = status.live_power_dbfs;
    realtime_ms += 250;
    tick(o, s);
    ready = 2;
    tick(o, s);
    assert(status.live_power_valid && status.live_seq == seq_before + 2);
    assert(status.live_power_dbfs > first_level - 0.001f && status.live_power_dbfs < first_level + 0.001f);

    // A level no accepted measurement refreshes is withdrawn three intervals and a retry after it was measured: here
    // the capture stops being the landed tune, so nothing is measured. A fresh measurement shows a level again.
    const dsd_signal_capture landed = capture;
    capture.capture_offset_hz += 5000;
    const dsd_signal_measure_request measured = request;
    realtime_ms += 1749;
    tick(o, s);
    assert(status.live_power_valid && request.request_id == measured.request_id);
    realtime_ms += 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && !status.live_power_valid);
    assert(status.live_seq == seq_before + 2); // a withdrawal is no measurement
    capture = landed;
    realtime_ms += 250;
    tick(o, s);
    assert(request.request_id != measured.request_id && !status.live_power_valid);
    ready = 2;
    tick(o, s);
    assert(status.live_power_valid && status.live_power_dbfs > -40.5f && status.live_power_dbfs < -39.5f);

    // A session avoid can be lifted by frequency, without a hit; lifting a missing one is harmless.
    const uint32_t avoided = status.hit.frequency_hz;
    act(o, s, DSD_SEARCH_ACTION_AVOID);
    assert(status.phase == DSD_SEARCH_TUNING && !status.live_power_valid);
    dsd_signal_search_control lift = {44, 0, DSD_SEARCH_ACTION_UNAVOID, avoided};
    assert(dsd_engine_signal_search_control(o, s, &lift) == 0);
    assert(dsd_engine_signal_search_control(o, s, &lift) == 0);
    lift.frequency_hz = 0;
    assert(dsd_engine_signal_search_control(o, s, &lift) < 0);
    // The lifted frequency is eligible again from the next pass, while frequencies still avoided stay excluded.
    carrier = 0;
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING);
    ready = 1;
    tick(o, s); // every channel was visited this pass
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.pass == 2);
    visit_strongest(o, s);
    assert(status.hit.frequency_hz == 100025000);
    act(o, s, DSD_SEARCH_ACTION_AVOID);
    visit_strongest(o, s);
    assert(status.hit.frequency_hz == 100012500);
    act(o, s, DSD_SEARCH_ACTION_AVOID);
    visit_strongest(o, s); // the strongest two are avoided; only the lifted one remains
    assert(status.hit.frequency_hz == avoided && avoided == 100000000);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    land(o, s);
    ready = 1;
    tick(o, s);
    tick(o, s); // nothing eligible in pass 2 now: on to pass 3
    assert(status.phase == DSD_SEARCH_TUNING && status.pass == 3);
    visit_strongest(o, s); // the still-avoided 100.025 and 100.0125 MHz never come back
    assert(status.hit.frequency_hz == avoided);
    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED && !status.owned);
}

/* A session that ends under a search (the decoder stops, so no Stop runs its restore tune) leaves the frequency the
   search started from: the teardown autosaves the configuration next, which must not keep a visit's frequency. */
static void
test_teardown_restores_frequency(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    observed = 1;
    o->rtlsdr_center_freq = 100000000;
    config->session_id = 45;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.frequency_hz == 100025000);
    assert(o->rtlsdr_center_freq == 100025000);
    dsd_engine_signal_search_shutdown(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && !status.owned && !dsd_engine_signal_search_active(s));
    assert(o->rtlsdr_center_freq == 100000000);
}

/* A Stop's restore tune moves the receiver off the channel the search left it on, a carrier change like any visit's:
   the call heard there ends at the boundary, so nothing received on the restored frequency joins it. */
static void
test_stop_restore_is_a_carrier_boundary(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    o->rtlsdr_center_freq = 100000000;
    config->session_id = 46;
    const int supersedes = manual_supersedes;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && o->rtlsdr_center_freq == 100025000);
    /* Its tunes decide where the receiver goes: a manual tune still pending can no longer put it back (issue #671). */
    assert(manual_supersedes > supersedes);
    const int before = boundary;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(boundary == before + 1 && target == 100000000);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && boundary == before + 1 && o->rtlsdr_center_freq == 100000000);
}

/* A Pause stops the decoder reading the channel a visit holds, and the main loop runs no noCarrier() while paused: the
   call heard there ends at the Pause, so its duration and voice end do not wait for a Resume or Stop. */
static void
test_pause_is_a_carrier_boundary(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    o->rtlsdr_center_freq = 100000000;
    config->session_id = 47;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    const int before = boundary;
    snapshots = 0;
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && status.hit.ended && boundary == before + 1);
    /* The engine loop publishes nothing while paused: the call the boundary ended reaches frontends from here. */
    assert(snapshots > 0 && snapshot_boundary == before + 1);
    advance(30000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_PAUSED && boundary == before + 1);
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && o->rtlsdr_center_freq == 100000000);
}

/* A search that fails stops the decoder reading the channel as a Pause does, and runs no noCarrier() until a Stop:
   whether its input stalls during a held visit or goes away, the call heard there ends when the search fails. */
static void
test_error_is_a_carrier_boundary(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    o->rtlsdr_center_freq = 100000000;
    config->session_id = 48;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    visit_strongest(o, s);
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_HELD);
    int before = boundary;
    stalled_ms = 20000;
    snapshots = 0;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && status.hit.ended && boundary == before + 1);
    assert(snapshots > 0 && snapshot_boundary == before + 1);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && boundary == before + 1);
    stalled_ms = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED);

    config->session_id = 49;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    before = boundary;
    live = 0;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && boundary == before + 1);
    tick(o, s);
    assert(boundary == before + 1);
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED);
    live = 1;
}

/* The Auto baseline decodes DMR whatever preset the session started from: a mono preset such as -f1 turns both DMR
   dispatch flags off, and with both off no direct-mode voice frame is read, so a voice-only DMR detection could never
   be confirmed. A session that chose mono DMR keeps it; Stop gives the session its own flags back. */
static void
test_auto_baseline_dispatches_dmr(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    o->rtlsdr_center_freq = 100000000;
    for (int mono = 0; mono <= 1; ++mono) {
        o->dmr_stereo = 0;
        o->dmr_mono = mono;
        config->session_id = 50 + (uint64_t)mono;
        assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
        assert(o->frame_dmr && o->dmr_mono == mono && o->dmr_stereo == !mono);
        tick(o, s);
        act(o, s, DSD_SEARCH_ACTION_STOP);
        land(o, s); // the span tune the start queued
        land(o, s); // the restore tune
        assert(status.phase == DSD_SEARCH_STOPPED && o->dmr_stereo == 0 && o->dmr_mono == mono);
    }
    o->dmr_mono = 0;
    o->dmr_stereo = 1;
}

/* A Stop's restore retune carries the session's receive family, as each visit's does: the live family request the
   restore queues is older than the retune, and a retune landing first retires it, which would leave an analog session
   on the digital family the search last used. */
static void
test_restore_retune_carries_family(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    o->rtlsdr_center_freq = 100000000;
    o->analog_only = 1;
    o->analog_demod = DSD_ANALOG_DEMOD_FM;
    o->analog_nfm_bandwidth_hz = 11000;
    config->session_id = 52;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s); // the span tune the start queued
    assert(target == 100000000 && retune_family_hz == 100000000);
    assert(retune_family.family == DSD_RX_FAMILY_ANALOG && retune_family.kind == DSD_ANALOG_DEMOD_FM);
    assert(retune_family.width_hz == 11000);
    land(o, s); // the restore tune
    assert(status.phase == DSD_SEARCH_STOPPED && o->analog_only == 1);
    o->analog_only = 0;
    o->analog_nfm_bandwidth_hz = 0;
}

/* Fresh input never trips the stall timeout when its timestamp advances during the read. The timeout uses real
   time, independently of the decode clock, and still rejects samples strictly older than ten seconds. */
static void
test_capture_freshness(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    config->session_id = 53;
    live = 1;
    capture_read_advance_ms = 1;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_HELD && !status.hit.ended);

    capture_read_advance_ms = 0;
    capture_ahead_ms = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_HELD && !status.hit.ended);
    capture_ahead_ms = 0;
    stalled_ms = 10000;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_HELD && !status.hit.ended);
    stalled_ms = 10001;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && status.hit.ended);
    assert(strcmp(status.message, "The radio input stopped delivering samples") == 0);
    stalled_ms = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED);
}

/* The status names the range being searched, from the coordinator's own configuration: a detection in range A keeps
   its own range name when the search moves on, and the status shows the quiet range B the search is in now. Edited
   ranges reach the status only at the Resume that applies them. */
static void
test_status_names_running_range(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    carrier = 0;
    observed = 1;
    o->rtlsdr_center_freq = 100000000;
    dsd_signal_search_config two = *config;
    two.session_id = 54;
    two.range_count = 2;
    DSD_SNPRINTF(two.ranges[0].name, sizeof(two.ranges[0].name), "%s", "Air band");
    two.ranges[0].digital = 0;
    two.ranges[0].max_visit_ms = 0;
    two.ranges[1] = two.ranges[0];
    DSD_SNPRINTF(two.ranges[1].name, sizeof(two.ranges[1].name), "%s", "Marine");
    two.ranges[1].start_hz = 100100000;
    two.ranges[1].end_hz = 100125000;
    assert(dsd_engine_signal_search_start(o, s, &two, observer, NULL, error, sizeof(error)) == 0);
    // Named from the first publication, before any span is planned.
    assert(strcmp(status.range_name, "Air band") == 0 && status.range_start_hz == 100000000);
    assert(status.range_end_hz == 100025000 && status.channel_count == 3);
    tick(o, s);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && strcmp(status.hit.range_name, "Air band") == 0);
    tick(o, s); // the monitor's first verdict starts quiet timing
    advance(500);
    tick(o, s); // quiet: back to the span
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    land(o, s);
    ready = 2; // nothing detected in the rest of range A
    tick(o, s);
    tick(o, s); // range B's span
    assert(status.phase == DSD_SEARCH_TUNING && status.range_index == 1);
    assert(strcmp(status.range_name, "Marine") == 0 && status.range_start_hz == 100100000);
    assert(status.range_end_hz == 100125000);
    assert(strcmp(status.hit.range_name, "Air band") == 0); // the retained detection keeps its own range
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING && strcmp(status.range_name, "Marine") == 0);

    // Paused, the ranges are edited; the running range is still B until Resume applies the edit.
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && strcmp(status.range_name, "Marine") == 0);
    DSD_SNPRINTF(two.ranges[0].name, sizeof(two.ranges[0].name), "%s", "Air band low");
    two.ranges[0].end_hz = 100012500;
    tick(o, s);
    assert(strcmp(status.range_name, "Marine") == 0 && status.range_start_hz == 100100000);
    assert(dsd_engine_signal_search_start(o, s, &two, observer, NULL, error, sizeof(error)) == 0);
    assert(status.phase == DSD_SEARCH_SEARCHING && status.range_index == 0);
    assert(strcmp(status.range_name, "Air band low") == 0 && status.range_start_hz == 100000000);
    assert(status.range_end_hz == 100012500 && status.channel_count == 2);
    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED);
}

/* Avoiding a frequency named by a history row: no hit is needed, the visit it names is left, and the session's list is
   bounded with a visible refusal. */
static void
test_avoid_by_frequency(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    carrier = 0;
    observed = 1;
    o->rtlsdr_center_freq = 100000000;
    dsd_signal_search_config one = *config;
    one.session_id = 55;
    one.range_count = 1;
    one.ranges[0].start_hz = 100000000;
    one.ranges[0].end_hz = 100025000;
    one.ranges[0].digital = 0;
    one.ranges[0].max_visit_ms = 0;
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.frequency_hz == 100025000);
    const uint64_t hit = status.hit.hit_id;

    // What an action would act on, resolved without acting: the live hit's frequency, or the one named.
    dsd_signal_search_control avoid_hit = {55, hit, DSD_SEARCH_ACTION_AVOID, 0};
    uint32_t resolved = 0;
    assert(dsd_engine_signal_search_target(s, &avoid_hit, &resolved) == 0 && resolved == 100025000);
    avoid_hit.hit_id = hit + 1;
    assert(dsd_engine_signal_search_target(s, &avoid_hit, &resolved) < 0);
    avoid_hit.hit_id = hit;
    avoid_hit.session_id = 56;
    assert(dsd_engine_signal_search_target(s, &avoid_hit, &resolved) < 0);

    // Another frequency: avoided, and the visit goes on. Again: still fine.
    dsd_signal_search_control by_frequency = {55, 0, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 100012500};
    resolved = 0;
    assert(dsd_engine_signal_search_target(s, &by_frequency, &resolved) == 0 && resolved == 100012500);
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) == 0);
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) == 0);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && !status.hit.ended);
    // A stale session or no frequency is refused.
    by_frequency.session_id = 56;
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) < 0);
    by_frequency.session_id = 55;
    by_frequency.frequency_hz = 0;
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) < 0);
    // The frequency being visited: left, as AVOID leaves it.
    by_frequency.frequency_hz = 100025000;
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) == 0);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);

    // Later spans exclude both: only 100.000 MHz is visited, this pass and the next.
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.frequency_hz == 100000000);
    land(o, s);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    land(o, s);
    ready = 1;
    tick(o, s);
    tick(o, s);
    assert(status.pass == 2);
    visit_strongest(o, s);
    assert(status.hit.frequency_hz == 100000000);

    // A full list refuses a new frequency and says so; one already on it still succeeds and clears the refusal.
    by_frequency.frequency_hz = 200000000;
    for (uint32_t i = 2; i < DSD_SEARCH_MAX_AVOIDS; ++i, by_frequency.frequency_hz += 12500) {
        assert(dsd_engine_signal_search_control(o, s, &by_frequency) == 0);
    }
    assert(status.message[0] == '\0');
    const uint64_t revision = status.revision;
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) < 0);
    assert(strcmp(status.message, "The avoid list is full") == 0 && status.revision > revision);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && !status.hit.ended); // nothing was skipped
    avoid_hit = (dsd_signal_search_control){55, status.hit.hit_id, DSD_SEARCH_ACTION_AVOID, 0};
    assert(dsd_engine_signal_search_control(o, s, &avoid_hit) < 0); // the current hit has no room either
    assert(strcmp(status.message, "The avoid list is full") == 0);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && !status.hit.ended);
    by_frequency.frequency_hz = 100012500;
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) == 0);
    assert(status.message[0] == '\0');
    // A refusal belongs to the detection it was about: the next visit's tune clears it, without any other action.
    by_frequency.frequency_hz = 300000000;
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) < 0);
    assert(strcmp(status.message, "The avoid list is full") == 0);
    tick(o, s); // the monitor's first verdict starts quiet timing
    advance(500);
    tick(o, s); // quiet: the visit ends and the search goes back to its span
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    assert(strcmp(status.message, "The avoid list is full") == 0); // a span tune is not a new detection
    for (int i = 0; i < 8 && !(status.phase == DSD_SEARCH_TUNING && !status.hit.ended); ++i) {
        land(o, s);
        ready = 1;
        tick(o, s);
    }
    assert(status.phase == DSD_SEARCH_TUNING && !status.hit.ended && status.message[0] == '\0');
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);

    // A refusal decided outside the coordinator is shown in its status, for its own session only.
    dsd_engine_signal_search_notice(s, 56, "elsewhere");
    assert(status.message[0] == '\0');
    dsd_engine_signal_search_notice(s, 55, "The permanent avoid list is full");
    assert(strcmp(status.message, "The permanent avoid list is full") == 0);
    // A failed search keeps its error: its full session list went with it, and a session avoid is refused without
    // replacing the error.
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    failed = 1;
    land(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && status.message[0] != '\0');
    uint64_t avoid_session = 1;
    assert(dsd_signal_search_avoids_session_get(&avoid_session, NULL, 0) == 0 && avoid_session == 0);
    char search_error_text[sizeof(status.message)];
    DSD_SNPRINTF(search_error_text, sizeof(search_error_text), "%s", status.message);
    by_frequency.frequency_hz = 300000000;
    assert(dsd_engine_signal_search_control(o, s, &by_frequency) < 0);
    assert(strcmp(status.message, search_error_text) == 0);
    failed = 0;
    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED);
    // No search: nothing to resolve, nothing to show.
    by_frequency.frequency_hz = 100012500;
    assert(dsd_engine_signal_search_target(s, &by_frequency, &resolved) < 0);
    assert(dsd_engine_signal_search_target(NULL, &by_frequency, &resolved) < 0);
    dsd_engine_signal_search_notice(s, 55, "after the stop");
    assert(strcmp(status.message, "after the stop") != 0);
}

static dsd_signal_search_site_note
p25_network_note(uint32_t wacn, uint16_t sysid) {
    dsd_signal_search_site_note note = {0};
    note.kind = DSD_SEARCH_SITE_NOTE_P25_NETWORK;
    note.wacn = wacn;
    note.sysid = sysid;
    return note;
}

static dsd_signal_search_site_note
p25_site_note(uint16_t sysid, uint8_t rfss, uint8_t site) {
    dsd_signal_search_site_note note = {0};
    note.kind = DSD_SEARCH_SITE_NOTE_P25_SITE;
    note.sysid = sysid;
    note.rfss = rfss;
    note.site = site;
    return note;
}

static int
site_text_is(const dsd_signal_search_site* record, const char* want) {
    char text[96];
    (void)dsd_signal_search_site_format(record, text, sizeof(text));
    return strcmp(text, want) == 0;
}

/* The checked site record joins notes by the SYSID each one advertises and replaces itself on a new system. */
static void
test_site_record_binding(void) {
    dsd_signal_search_site record;
    dsd_signal_search_site_clear(&record);
    assert(site_text_is(&record, ""));
    dsd_signal_search_site_note note = p25_site_note(0x1A3, 1, 2);
    // An RFSS status before any network status sets the SYSID; the network status that follows joins it.
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(site_text_is(&record, "SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2"));
    note = p25_network_note(0xBEE00, 0x1A3);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(site_text_is(&record, "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2"));
    // The same broadcast again changes nothing.
    assert(!dsd_signal_search_site_apply(&record, &note));
    note = p25_site_note(0x1A3, 1, 2);
    assert(!dsd_signal_search_site_apply(&record, &note));
    // A site advertised under another SYSID is not this record's.
    note = p25_site_note(0x2B4, 7, 9);
    assert(!dsd_signal_search_site_apply(&record, &note));
    assert(site_text_is(&record, "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2"));
    // The same system's next site replaces the site part only.
    note = p25_site_note(0x1A3, 1, 3);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(site_text_is(&record, "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 3"));
    // A network status naming another WACN or another SYSID replaces the record whole.
    note = p25_network_note(0xBEE01, 0x1A3);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(site_text_is(&record, "WACN BEE01 \xC2\xB7 SYS 1A3"));
    note = p25_site_note(0x1A3, 4, 5);
    assert(dsd_signal_search_site_apply(&record, &note));
    note = p25_network_note(0xBEE01, 0x2B4);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(site_text_is(&record, "WACN BEE01 \xC2\xB7 SYS 2B4"));
    // A site note whose SYSID differs from a site-only record replaces nothing either.
    dsd_signal_search_site_clear(&record);
    note = p25_site_note(0x1A3, 1, 2);
    assert(dsd_signal_search_site_apply(&record, &note));
    note = p25_network_note(0xBEE00, 0x2B4);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(site_text_is(&record, "WACN BEE00 \xC2\xB7 SYS 2B4"));
    // Another protocol's note replaces the record; DMR and NXDN notes are whole identities.
    dsd_signal_search_site_note dmr = {0};
    dmr.kind = DSD_SEARCH_SITE_NOTE_DMR_TIII;
    assert(!dsd_signal_search_site_apply(&record, &dmr)); // no text, no identity
    DSD_SNPRINTF(dmr.text, sizeof(dmr.text), "%s", "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 3.1 \xC2\xB7 SYS 0A3F");
    assert(dsd_signal_search_site_apply(&record, &dmr));
    assert(strcmp(record.protocol, "DMR") == 0);
    assert(site_text_is(&record, "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 3.1 \xC2\xB7 SYS 0A3F"));
    dsd_signal_search_site_note nxdn = {0};
    nxdn.kind = DSD_SEARCH_SITE_NOTE_NXDN;
    nxdn.nxdn_site = 12;
    nxdn.nxdn_sys = 3;
    assert(dsd_signal_search_site_apply(&record, &nxdn));
    assert(strcmp(record.protocol, "NXDN") == 0 && site_text_is(&record, "Site 12 \xC2\xB7 Sys 3"));
    note = p25_network_note(0xBEE00, 0x1A3);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(strcmp(record.protocol, "P25") == 0 && site_text_is(&record, "WACN BEE00 \xC2\xB7 SYS 1A3"));
    // Nothing else is a note.
    dsd_signal_search_site_note unknown = {0};
    assert(!dsd_signal_search_site_apply(&record, &unknown) && !dsd_signal_search_site_apply(&record, NULL));
    char small[8];
    assert(dsd_signal_search_site_format(&record, small, sizeof(small)) == sizeof(small) - 1);
}

/* A detection shows the site its visit heard in checked broadcasts, from the visit alone. */
static void
test_site_notes_on_detection(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    carrier = 0;
    observed = 1;
    o->rtlsdr_center_freq = 100000000;
    dsd_signal_search_config one = *config;
    one.session_id = 61;
    one.range_count = 1;
    one.ranges[0].start_hz = 100000000;
    one.ranges[0].end_hz = 100025000;
    one.ranges[0].digital = 1;
    one.ranges[0].max_visit_ms = 0;
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    // A broadcast heard while the search measures its span belongs to no detection.
    dsd_signal_search_site_note early = p25_network_note(0xABCDE, 0x123);
    dsd_signal_search_note_site(s, &early);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && status.hit.site[0] == '\0');

    // A site noted before the protocol is identified shows once it is.
    int published = hits_published;
    dsd_signal_search_site_note site = p25_site_note(0x1A3, 1, 2);
    dsd_signal_search_note_site(s, &site);
    assert(status.hit.site[0] == '\0' && hits_published == published);
    dsd_signal_search_note_evidence(s, "P25", 0, 0, NULL);
    assert(strcmp(status.hit.site, "SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2") == 0);
    // The network status after the RFSS status joins it, published with the hit.
    published = hits_published;
    dsd_signal_search_site_note network = p25_network_note(0xBEE00, 0x1A3);
    dsd_signal_search_note_site(s, &network);
    assert(hits_published == published + 1);
    assert(strcmp(published_hit.site, "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2") == 0);
    assert(strcmp(status.hit.site, published_hit.site) == 0);
    // A repeated broadcast publishes nothing.
    const uint64_t revision = status.revision;
    dsd_signal_search_note_site(s, &network);
    dsd_signal_search_note_site(s, &site);
    assert(status.revision == revision && hits_published == published + 1);
    // A note from a frame an input interruption cut is dropped.
    s->input_interrupted = 1;
    dsd_signal_search_site_note cut = p25_network_note(0xBEE00, 0x2B4);
    dsd_signal_search_note_site(s, &cut);
    s->input_interrupted = 0;
    assert(strcmp(status.hit.site, published_hit.site) == 0 && status.revision == revision);
    // A protocol change clears the site: going back to P25 shows none until a new broadcast names it.
    dsd_signal_search_note_evidence(s, "DMR", 0, 0, NULL);
    assert(status.hit.site[0] == '\0');
    dsd_signal_search_note_evidence(s, "P25", 0, 0, NULL);
    assert(status.hit.site[0] == '\0');
    dsd_signal_search_note_site(s, &site);
    dsd_signal_search_note_site(s, &network);
    assert(strcmp(status.hit.site, "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2") == 0);
    // NXDN48 and NXDN96 are one protocol's site.
    dsd_signal_search_site_note nxdn = {0};
    nxdn.kind = DSD_SEARCH_SITE_NOTE_NXDN;
    nxdn.nxdn_site = 12;
    nxdn.nxdn_sys = 3;
    s->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    dsd_signal_search_note_evidence(s, "NXDN", 0, 0, NULL);
    dsd_signal_search_note_site(s, &nxdn);
    assert(strcmp(status.hit.protocol, "NXDN48") == 0 && strcmp(status.hit.site, "Site 12 \xC2\xB7 Sys 3") == 0);
    s->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    dsd_signal_search_note_evidence(s, "NXDN", 0, 0, NULL);
    assert(strcmp(status.hit.protocol, "NXDN96") == 0 && strcmp(status.hit.site, "Site 12 \xC2\xB7 Sys 3") == 0);
    dsd_signal_search_note_evidence(s, "P25", 0, 0, NULL);
    dsd_signal_search_note_site(s, &site);
    dsd_signal_search_note_site(s, &network);
    assert(strcmp(status.hit.site, "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2") == 0);

    // The last broadcast before the channel falls silent is the detection's: it is in the live status and in the
    // ended hit the visit publishes.
    tick(o, s);
    advance(500);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && published_hit.ended);
    assert(strcmp(status.hit.site, "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2") == 0);
    assert(strcmp(published_hit.site, status.hit.site) == 0);
    // Between visits nothing is noted, and the next landing starts with no site.
    dsd_signal_search_note_site(s, &early);
    for (int i = 0; i < 8 && !(status.phase == DSD_SEARCH_TUNING && !status.hit.ended); ++i) {
        land(o, s);
        ready = 1;
        tick(o, s);
    }
    assert(status.phase == DSD_SEARCH_TUNING && !status.hit.ended && status.hit.site[0] == '\0');
    land(o, s);
    dsd_signal_search_note_evidence(s, "P25", 0, 0, NULL);
    assert(status.hit.confirmed && status.hit.site[0] == '\0');
    dsd_signal_search_note_site(s, &early);
    assert(strcmp(status.hit.site, "WACN ABCDE \xC2\xB7 SYS 123") == 0);

    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED);
    // No search: a note goes nowhere.
    dsd_signal_search_note_site(s, &network);
    assert(strcmp(status.hit.site, "WACN ABCDE \xC2\xB7 SYS 123") == 0);
}

/* Issue #802 B7: a checked proof that gives the access code a new value after the visit's last evidence (a P25 Phase 2
   NAC) publishes a confirmed hit of that sync type's family as changed, so the observer reads the code again. It
   confirms nothing and keeps the visit's quiet timing, and an unconfirmed hit, another family's sync type, a cut frame
   and an ended hit take nothing from it. */
static void
test_access_note_on_detection(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    carrier = 0;
    observed = 1;
    o->rtlsdr_center_freq = 100000000;
    dsd_signal_search_config one = *config;
    one.session_id = 63;
    const int saved_sync = s->synctype;
    const int saved_sps = s->sps_hunt_idx;
    one.range_count = 1;
    one.ranges[0].start_hz = 100000000;
    one.ranges[0].end_hz = 100025000;
    one.ranges[0].digital = 1;
    one.ranges[0].max_visit_ms = 0;
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    // A proof while the search measures its span belongs to no detection.
    s->synctype = DSD_SYNC_P25P2_POS;
    int published = hits_published;
    dsd_signal_search_note_access(s);
    assert(hits_published == published);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && !status.hit.confirmed);

    // An unconfirmed hit: the note confirms nothing and publishes nothing.
    published = hits_published;
    dsd_signal_search_note_access(s);
    assert(hits_published == published && !status.hit.confirmed && status.phase == DSD_SEARCH_IDENTIFYING);

    // NXDN48 and NXDN96 are one family: an NXDN sync type's proof reaches an NXDN48 hit.
    s->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    dsd_signal_search_note_evidence(s, "NXDN", 0, 0, NULL);
    assert(status.hit.confirmed && strcmp(status.hit.protocol, "NXDN48") == 0);
    s->synctype = DSD_SYNC_NXDN_POS;
    published = hits_published;
    dsd_signal_search_note_access(s);
    assert(hits_published == published + 1 && strcmp(published_hit.protocol, "NXDN48") == 0);

    // A confirmed P25 hit, then the transmission's last evidence. A proof 400 ms later publishes the hit as changed,
    // and nothing else moves.
    s->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    s->synctype = DSD_SYNC_P25P2_POS;
    dsd_signal_search_note_evidence(s, "P25", 0, 0, NULL);
    assert(status.hit.confirmed && strcmp(status.hit.protocol, "P25") == 0 && status.phase == DSD_SEARCH_LISTENING);
    tick(o, s);
    advance(400);
    published = hits_published;
    const uint64_t revision = status.revision;
    dsd_signal_search_note_access(s);
    assert(hits_published == published + 1 && status.revision == revision + 1);
    assert(published_hit.hit_id == status.hit.hit_id && !published_hit.ended);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.confirmed);
    // Another family's sync type: the decoder holds that protocol's code, not the hit's.
    s->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dsd_signal_search_note_access(s);
    s->synctype = DSD_SYNC_ANALOG;
    dsd_signal_search_note_access(s);
    assert(hits_published == published + 1);
    s->synctype = DSD_SYNC_P25P2_POS;
    // A proof from a frame an input interruption cut is dropped.
    s->input_interrupted = 1;
    dsd_signal_search_note_access(s);
    s->input_interrupted = 0;
    assert(hits_published == published + 1);
    // The proof kept the visit's quiet timing: the visit ends 500 ms after the last evidence, not after the proof.
    advance(100);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && published_hit.ended);
    // An ended hit takes nothing.
    published = hits_published;
    dsd_signal_search_note_access(s);
    assert(hits_published == published);

    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED);
    // No search: a note goes nowhere.
    published = hits_published;
    dsd_signal_search_note_access(s);
    assert(hits_published == published);
    s->synctype = saved_sync;
    s->sps_hunt_idx = saved_sps;
}

/* Put an active call on @p slot: one epoch of one transmission, as the canonical layer would hold it. */
static dsd_call_snapshot*
put_call(uint8_t slot, uint64_t epoch, double started_m, int protocol, uint64_t target_id, uint64_t source_id) {
    dsd_call_snapshot* call = &calls[slot];
    DSD_MEMSET(call, 0, sizeof(*call));
    call->epoch = epoch;
    call->slot = slot;
    call->started_m = started_m;
    call->phase = DSD_CALL_PHASE_ACTIVE;
    call->protocol = protocol;
    call->kind = DSD_CALL_KIND_GROUP_VOICE;
    call->ota_target_id = target_id;
    call->ota_source_id = source_id;
    call->crypto = DSD_CALL_CRYPTO_CLEAR;
    return call;
}

static void
put_crypto(dsd_call_snapshot* call, dsd_call_crypto_state crypto, uint8_t algid, uint16_t kid, uint8_t received) {
    call->crypto = crypto;
    call->algid = algid;
    call->kid = kid;
    call->crypto_received = received;
    call->received_algid = (received & DSD_CALL_CRYPTO_RX_ALG) ? algid : 0U;
    call->received_kid = (received & DSD_CALL_CRYPTO_RX_KID) ? kid : 0U;
}

static int
tuple_is(const dsd_signal_search_hit* hit, uint32_t target_id, uint32_t source_id, int slot, const char* crypt) {
    return hit->target == target_id && hit->source == source_id && hit->slot == slot && strcmp(hit->crypt, crypt) == 0;
}

/* A detection's IDs, texts, slot and crypt are one checked call record's: the most recent call heard on the hit,
   never a mix of two slots' calls, and the crypt only what that call received over the air. */
static void
test_call_tuple_on_detection(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    carrier = 0;
    observed = 1;
    o->rtlsdr_center_freq = 100000000;
    DSD_MEMSET(calls, 0, sizeof(calls));
    dsd_signal_search_config one = *config;
    one.session_id = 62;
    one.range_count = 1;
    one.ranges[0].start_hz = 100000000;
    one.ranges[0].end_hz = 100025000;
    one.ranges[0].digital = 1;
    one.ranges[0].max_visit_ms = 0;
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    // A call heard while the search measures its span belongs to no detection.
    dsd_call_snapshot* a = put_call(0, 1, 1.0, DSD_SYNC_DMR_BS_VOICE_POS, 100, 200);
    dsd_signal_search_note_call(s, 0);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && tuple_is(&status.hit, 0, 0, 0, ""));

    // Issue #637: traffic the receive policy rejects names no hit and keeps the search from listening on it.
    o->receive.filters[DSD_RECEIVE_DCC].enabled = 1;
    o->receive.filters[DSD_RECEIVE_DCC].block.bits[0] = UINT64_C(1) << 2;
    s->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    s->dmr_color_code = 2;
    dsd_signal_search_note_call(s, 0);
    dsd_signal_search_note_evidence(s, "DMR", 100, 200, NULL);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && tuple_is(&status.hit, 0, 0, 0, ""));
    DSD_MEMSET(&o->receive, 0, sizeof(o->receive));
    s->synctype = DSD_SYNC_NONE;
    s->dmr_color_code = 16;

    // A call noted before the protocol is identified shows once it is, whole: IDs, texts, slot and received crypt.
    put_crypto(a, DSD_CALL_CRYPTO_ENCRYPTED, 0x21, 0x05, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID);
    DSD_SNPRINTF(a->source_text, sizeof(a->source_text), "%s", "Unit 200");
    int published = hits_published;
    dsd_signal_search_note_call(s, 0);
    assert(hits_published == published && status.hit.target == 0);
    dsd_signal_search_note_evidence(s, "DMR", 0, 0, NULL);
    assert(tuple_is(&status.hit, 100, 200, 1, "ALG 21 \xC2\xB7 KID 0005"));
    assert(strcmp(status.hit.source_text, "Unit 200") == 0 && status.hit.target_text[0] == '\0');
    assert(tuple_is(&published_hit, 100, 200, 1, "ALG 21 \xC2\xB7 KID 0005"));

    // Two simultaneous DMR slots: the call that began later is the tuple, whole, and an older call still running on
    // the other slot re-announcing itself does not mix its IDs or crypto into it.
    dsd_call_snapshot* b = put_call(1, 1, 2.0, DSD_SYNC_DMR_BS_VOICE_NEG, 300, 400);
    DSD_SNPRINTF(b->target_text, sizeof(b->target_text), "%s", "Fire");
    published = hits_published;
    dsd_signal_search_note_call(s, 1);
    assert(hits_published == published + 1);
    assert(tuple_is(&status.hit, 300, 400, 2, "") && tuple_is(&published_hit, 300, 400, 2, ""));
    assert(strcmp(status.hit.target_text, "Fire") == 0 && status.hit.source_text[0] == '\0');
    const uint64_t revision = status.revision;
    dsd_signal_search_note_call(s, 0);
    assert(tuple_is(&status.hit, 300, 400, 2, "") && status.revision == revision);
    // The tuple's own call updates it in place; a repeat that changes nothing publishes nothing.
    put_crypto(b, DSD_CALL_CRYPTO_ENCRYPTED_PENDING, 0x24, 0x0007, DSD_CALL_CRYPTO_RX_KID);
    dsd_signal_search_note_call(s, 1);
    assert(tuple_is(&status.hit, 300, 400, 2, "KID 0007") && strcmp(status.hit.target_text, "Fire") == 0);
    published = hits_published;
    dsd_signal_search_note_call(s, 1);
    assert(hits_published == published);

    // Evidence IDs are only the fallback for a hit no call record names: they never overwrite the tuple.
    dsd_signal_search_note_evidence(s, "DMR", 777, 888, NULL);
    assert(tuple_is(&status.hit, 300, 400, 2, "KID 0007"));

    // A later call on the same hit replaces the tuple whole, on either slot. It does not inherit the crypto the call
    // before it received: a checked privacy indication alone shows as "Encrypted".
    a = put_call(0, 2, 3.0, DSD_SYNC_DMR_BS_VOICE_POS, 500, 600);
    put_crypto(a, DSD_CALL_CRYPTO_ENCRYPTED_PENDING, 0x21, 0x05, 0);
    a->enc_received = 1U;
    dsd_signal_search_note_call(s, 0);
    assert(tuple_is(&status.hit, 500, 600, 1, "Encrypted"));
    assert(status.hit.target_text[0] == '\0' && status.hit.source_text[0] == '\0');
    // A forced or inferred ALG ID the record carries but never received shows nothing beyond "Encrypted".
    a->algid = 0x22;
    a->kid = 0xFF;
    dsd_signal_search_note_call(s, 0);
    assert(tuple_is(&status.hit, 500, 600, 1, "Encrypted"));
    // An encrypted or decryptable classification no checked signalling of the call supports (a forced scrambler over a
    // VCALL advertising clear) shows nothing at all.
    a->enc_received = 0U;
    put_crypto(a, DSD_CALL_CRYPTO_DECRYPTABLE, 0x01, 0x00, 0);
    dsd_signal_search_note_call(s, 0);
    assert(tuple_is(&status.hit, 500, 600, 1, ""));
    put_crypto(a, DSD_CALL_CRYPTO_ENCRYPTED, 0x22, 0xFF, 0);
    dsd_signal_search_note_call(s, 0);
    assert(tuple_is(&status.hit, 500, 600, 1, ""));
    // A clear call shows no crypt, whatever was received for it (a P25 ALG 80 is "clear").
    put_crypto(a, DSD_CALL_CRYPTO_CLEAR, 0x80, 0, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID);
    dsd_signal_search_note_call(s, 0);
    assert(tuple_is(&status.hit, 500, 600, 1, ""));

    // Only an active, CRC-clean record of a protocol the search identifies is a tuple.
    published = hits_published;
    b = put_call(1, 2, 4.0, DSD_SYNC_DMR_BS_VOICE_NEG, 901, 902);
    b->crc_invalid = 1;
    dsd_signal_search_note_call(s, 1);
    b->crc_invalid = 0;
    b->phase = DSD_CALL_PHASE_ENDED;
    dsd_signal_search_note_call(s, 1);
    b->phase = DSD_CALL_PHASE_ACTIVE;
    b->protocol = DSD_SYNC_X2TDMA_DATA_POS;
    dsd_signal_search_note_call(s, 1);
    b->protocol = DSD_SYNC_DMR_BS_VOICE_NEG;
    s->input_interrupted = 1;
    dsd_signal_search_note_call(s, 1);
    s->input_interrupted = 0;
    dsd_signal_search_note_call(s, 7);
    assert(tuple_is(&status.hit, 500, 600, 1, "") && hits_published == published);

    // A protocol change clears the tuple; the new protocol's call shows its own, with its Phase 2 slot.
    dsd_signal_search_note_evidence(s, "P25", 0, 0, NULL);
    assert(tuple_is(&status.hit, 0, 0, 0, "") && status.hit.target_text[0] == '\0');
    dsd_signal_search_note_call(s, 0); // the DMR call is not this protocol's
    assert(tuple_is(&status.hit, 0, 0, 0, ""));
    b = put_call(1, 3, 5.0, DSD_SYNC_P25P2_POS, 1001, 1002);
    put_crypto(b, DSD_CALL_CRYPTO_DECRYPTABLE, 0x84, 0x0001, DSD_CALL_CRYPTO_RX_ALG);
    dsd_signal_search_note_call(s, 1);
    assert(tuple_is(&status.hit, 1001, 1002, 2, "ALG 84"));
    // A P25 Phase 1 call has no timeslot.
    a = put_call(0, 3, 6.0, DSD_SYNC_P25P1_POS, 2001, 2002);
    put_crypto(a, DSD_CALL_CRYPTO_ENCRYPTED, 0x84, 0x0001, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID);
    dsd_signal_search_note_call(s, 0);
    assert(tuple_is(&status.hit, 2001, 2002, 0, "ALG 84 \xC2\xB7 KID 0001"));

    // The last call before the channel falls silent is the detection's: the ended hit keeps it, and the call ending
    // changes nothing.
    tick(o, s);
    advance(500);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && published_hit.ended);
    assert(tuple_is(&status.hit, 2001, 2002, 0, "ALG 84 \xC2\xB7 KID 0001"));
    assert(tuple_is(&published_hit, 2001, 2002, 0, "ALG 84 \xC2\xB7 KID 0001"));
    // Between visits nothing is noted, and the next landing starts with no tuple; its evidence IDs are the fallback
    // until a call record names it.
    dsd_signal_search_note_call(s, 0);
    for (int i = 0; i < 8 && !(status.phase == DSD_SEARCH_TUNING && !status.hit.ended); ++i) {
        land(o, s);
        ready = 1;
        tick(o, s);
    }
    assert(status.phase == DSD_SEARCH_TUNING && !status.hit.ended && tuple_is(&status.hit, 0, 0, 0, ""));
    land(o, s);
    /* Issue #637: a Phase 2 SACCH decoded on slot 1 announces a slot 2 call; with slot 2 blocked that report neither
       confirms the hit nor names it, whatever slot the decoder is on. */
    o->receive.filters[DSD_RECEIVE_SLOT].enabled = 1;
    o->receive.filters[DSD_RECEIVE_SLOT].block.bits[0] = UINT64_C(1) << 2;
    s->currentslot = 0;
    dsd_trunk_scan_hook_p25_conventional_activity(o, s, DSD_SYNC_P25P2_POS, 1, 5005U, 5006U, 0, 0, 0);
    assert(!status.hit.confirmed && tuple_is(&status.hit, 0, 0, 0, ""));
    DSD_MEMSET(&o->receive, 0, sizeof(o->receive));
    dsd_signal_search_note_evidence(s, "P25", 2001, 2002, NULL);
    assert(status.hit.confirmed && tuple_is(&status.hit, 2001, 2002, 0, ""));
    // A record of the previous visit's call is not this visit's until the protocol re-publishes it.
    a = put_call(0, 4, 7.0, DSD_SYNC_P25P1_POS, 3001, 0);
    dsd_signal_search_note_call(s, 0);
    assert(tuple_is(&status.hit, 3001, 0, 0, ""));
    // Issue #637: a newer call the receive policy rejects (its slot blocked) never becomes the tuple, while traffic it
    // admits on the carrier still does.
    o->receive.filters[DSD_RECEIVE_SLOT].enabled = 1;
    o->receive.filters[DSD_RECEIVE_SLOT].block.bits[0] = UINT64_C(1) << 2;
    (void)put_call(1, 5, 8.0, DSD_SYNC_P25P2_POS, 3003, 0);
    dsd_signal_search_note_call(s, 1);
    assert(tuple_is(&status.hit, 3001, 0, 0, ""));
    DSD_MEMSET(&o->receive, 0, sizeof(o->receive));

    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED);
    // No search: a note goes nowhere.
    a->ota_target_id = 4001;
    dsd_signal_search_note_call(s, 0);
    assert(status.hit.target == 3001);
    DSD_MEMSET(calls, 0, sizeof(calls));
}

/* Issue #809: the visit tag names the detection whose audio the decoder is receiving, and every recording closed while
   it is set. It is set when a visit lands, and cleared right after the boundary that closes what the visit received. */
static int
tag_is(uint64_t session, uint64_t hit) {
    uint64_t tag_session = 0, tag_hit = 0;
    return dsd_signal_search_visit_get(&tag_session, &tag_hit) == 1 && tag_session == session && tag_hit == hit;
}

static int
tag_clear(void) {
    uint64_t tag_session = 1, tag_hit = 1;
    return dsd_signal_search_visit_get(&tag_session, &tag_hit) == 0 && tag_session == 0 && tag_hit == 0;
}

/* The latest carrier boundary closed its recordings under the visit (@p session, @p hit). */
static int
boundary_closed_visit(uint64_t session, uint64_t hit) {
    return boundary_tagged && boundary_session == session && boundary_hit == hit;
}

/* One range of three channels, 100.0 to 100.025 MHz, for session @p session. */
static dsd_signal_search_config
tag_config(const dsd_signal_search_config* base, uint64_t session, int digital, uint32_t max_visit_ms) {
    dsd_signal_search_config one = *base;
    one.session_id = session;
    one.range_count = 1;
    one.ranges[0].enabled = 1;
    one.ranges[0].start_hz = 100000000;
    one.ranges[0].end_hz = 100025000;
    one.ranges[0].digital = digital;
    one.ranges[0].max_visit_ms = max_visit_ms;
    return one;
}

/* Start @p one on a live input with no carrier, and queue its first span tune. */
static void
start_tag_session(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* one) {
    char error[192];
    live = 1;
    carrier = 0;
    observed = 1;
    failed = 0;
    stalled_ms = 0;
    o->rtlsdr_center_freq = 100000000;
    assert(dsd_engine_signal_search_start(o, s, one, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
}

/* Stop with no tune pending: the restore tune lands, and the decoder session goes on untagged. */
static void
stop_tag_session(dsd_opts* o, dsd_state* s) {
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && tag_clear());
}

static void
test_visit_tag_set_on_landing(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 63, 0, 0);
    start_tag_session(o, s, &one);
    assert(status.phase == DSD_SEARCH_TUNING && tag_clear()); // a span tune names no visit
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING && tag_clear());
    ready = 1;
    tick(o, s);
    // The detection is chosen and its tune queued: nothing of it is received before the landing.
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id != 0 && tag_clear());
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && !bypassed && tag_is(63, status.hit.hit_id));
    stop_tag_session(o, s);
}

/* Every way a visit is left (quiet, its time limit, Skip, Avoid) closes what it received under its tag, then clears the
   tag before the next tune passes anything. */
static void
test_visit_tag_cleared_after_departure_boundary(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 64, 0, 5000);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    uint64_t hit = status.hit.hit_id;
    assert(status.phase == DSD_SEARCH_LISTENING && tag_is(64, hit));
    tick(o, s); // the monitor's first verdict starts quiet timing
    advance(500);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    assert(boundary_closed_visit(64, hit) && tag_clear());

    visit_strongest(o, s);
    hit = status.hit.hit_id;
    assert(status.phase == DSD_SEARCH_LISTENING && tag_is(64, hit));
    carrier = 1; // an active channel is left at the visit limit
    advance(5000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    assert(boundary_closed_visit(64, hit) && tag_clear());
    carrier = 0;

    visit_strongest(o, s);
    hit = status.hit.hit_id;
    assert(status.phase == DSD_SEARCH_LISTENING && tag_is(64, hit));
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    assert(boundary_closed_visit(64, hit) && tag_clear());

    land(o, s);
    ready = 1;
    tick(o, s); // every channel was visited this pass
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.pass == 2);
    visit_strongest(o, s);
    hit = status.hit.hit_id;
    assert(status.phase == DSD_SEARCH_LISTENING && tag_is(64, hit));
    act(o, s, DSD_SEARCH_ACTION_AVOID);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    assert(boundary_closed_visit(64, hit) && tag_clear());
    land(o, s);
    stop_tag_session(o, s);
}

/* A digital probe that finds no identity retunes the same detection for analog listening: the probe's audio closes
   under the hit, and the analog landing names the same hit again. */
static void
test_visit_tag_probe_fallback_same_hit(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 65, 1, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    const uint64_t hit = status.hit.hit_id;
    assert(status.identifying && tag_is(65, hit));
    advance(3000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id == hit && !status.hit.ended);
    assert(boundary_closed_visit(65, hit) && tag_clear());
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.hit_id == hit && tag_is(65, hit));
    stop_tag_session(o, s);
}

/* A Pause, or a search failure, ends the visit's reception at the next tick: its boundary closes under the tag, which
   then clears. */
static void
test_visit_tag_pause_and_error(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    dsd_signal_search_config one = tag_config(config, 66, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    uint64_t hit = status.hit.hit_id;
    assert(tag_is(66, hit));
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && boundary_closed_visit(66, hit) && tag_clear());
    stop_tag_session(o, s);

    one = tag_config(config, 67, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    hit = status.hit.hit_id;
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_HELD && tag_is(67, hit));
    stalled_ms = 20000;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && boundary_closed_visit(67, hit) && tag_clear());
    stalled_ms = 0;
    stop_tag_session(o, s);
}

/* A Resume restores no ended hit: the tag stays clear through the span tune and its measurement, until the next visit's
   landing names that visit. Edited ranges resume the same way. */
static void
test_visit_tag_resume_waits_for_landing(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    char error[192];
    dsd_signal_search_config one = tag_config(config, 68, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    const uint64_t first = status.hit.hit_id;
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && tag_clear());
    act(o, s, DSD_SEARCH_ACTION_RESUME);
    assert(status.phase == DSD_SEARCH_TUNING && !boundary_tagged && tag_clear());
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING && tag_clear());
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && tag_clear());
    land(o, s);
    const uint64_t second = status.hit.hit_id;
    assert(status.phase == DSD_SEARCH_LISTENING && second != first && tag_is(68, second));

    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && tag_clear());
    one.ranges[0].end_hz = 100012500;
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    assert(status.phase == DSD_SEARCH_SEARCHING && tag_clear());
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && !boundary_tagged && tag_clear());
    land(o, s);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && tag_clear());
    land(o, s);
    const uint64_t third = status.hit.hit_id;
    assert(status.phase == DSD_SEARCH_LISTENING && third != second && tag_is(68, third));

    // A Pause and a Resume taken before the tick that would end the paused reception (one command drain): the span
    // tune's boundary is the one that closes the visit, still under its tag.
    dsd_signal_search_control pause = {68, third, DSD_SEARCH_ACTION_PAUSE, 0};
    assert(dsd_engine_signal_search_control(o, s, &pause) == 0);
    dsd_signal_search_control resume = {68, third, DSD_SEARCH_ACTION_RESUME, 0};
    assert(dsd_engine_signal_search_control(o, s, &resume) == 0);
    assert(tag_is(68, third));
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && boundary_closed_visit(68, third) && tag_clear());
    land(o, s);
    stop_tag_session(o, s);
}

/* A Stop's restore tune closes the visit under its tag; the decoder session goes on, so the tag then clears. */
static void
test_visit_tag_stop_restore(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 69, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    const uint64_t hit = status.hit.hit_id;
    assert(tag_is(69, hit));
    const int before = boundary;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(boundary == before + 1 && boundary_closed_visit(69, hit) && tag_clear());
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && boundary == before + 1 && tag_clear());
}

/* A Stop the input can no longer restore still ends the visit's reception, under its tag, before the search stops: the
   decoder session goes on, and nothing it receives after the Stop is the search's. */
static void
test_visit_tag_stop_with_input_lost(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 70, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    const uint64_t hit = status.hit.hit_id;
    assert(tag_is(70, hit));
    const int before = boundary;
    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED && !status.owned && status.hit.ended);
    assert(boundary == before + 1 && boundary_closed_visit(70, hit) && tag_clear());
    tick(o, s);
    assert(boundary == before + 1 && tag_clear());
    live = 1;
}

/* A Stop's restore tune ends the visit's reception and tells the frontends the call ended. An input lost before that tune
   lands, or a restore that fails, ends nothing again: one boundary. In ERROR the engine loop publishes nothing, so the
   snapshot the Stop published is the one that shows the call ended. */
static void
test_stop_then_input_lost_one_boundary(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    dsd_signal_search_config one = tag_config(config, 74, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    const uint64_t hit = status.hit.hit_id;
    int before = boundary;
    snapshots = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(boundary == before + 1 && boundary_closed_visit(74, hit) && tag_clear());
    assert(snapshots > 0 && snapshot_boundary == before + 1);
    const int published = snapshots;
    live = 0; // the input goes away before the restore tune lands
    tick(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && boundary == before + 1 && snapshots == published && tag_clear());
    live = 1;

    one = tag_config(config, 75, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    before = boundary;
    snapshots = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    failed = 1;
    land(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && boundary == before + 1 && tag_clear());
    assert(snapshots > 0 && snapshot_boundary == before + 1);
    failed = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && tag_clear());
}

/* Hold and its release stay on the same visit: the tag does not change. */
static void
test_visit_tag_hold_keeps(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 71, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    const uint64_t hit = status.hit.hit_id;
    const int before = boundary;
    carrier = 1;
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_HELD && tag_is(71, hit));
    advance(40000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_HELD && tag_is(71, hit));
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_LISTENING && boundary == before && tag_is(71, hit));
    carrier = 0;
    stop_tag_session(o, s);
}

/* The decoder session ends under the search (the Qt Stop, the exit flag, a failed device): the shutdown ends the hit and
   keeps its tag, so the teardown's final closes still name the visit. The engine clears it after its last close
   (ENGINE_LIFECYCLE_FINALIZE). */
static void
test_session_end_keeps_tag_until_final_close(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 72, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    const uint64_t hit = status.hit.hit_id;
    const int before = boundary;
    dsd_engine_signal_search_shutdown(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && status.hit.ended && !dsd_engine_signal_search_active(s));
    assert(boundary == before && tag_is(72, hit));
    dsd_signal_search_visit_clear(); // the engine's teardown, after its last close
}

/* A search start clears a tag an earlier session left: its first tune's boundary closes audio received before the
   search, untagged. */
static void
test_visit_tag_cleared_at_search_start(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    char error[192];
    const dsd_signal_search_config one = tag_config(config, 73, 0, 0);
    live = 1;
    o->rtlsdr_center_freq = 100000000;
    dsd_signal_search_visit_set(72, 9);
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    assert(tag_clear());
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && !boundary_tagged && tag_clear());
    land(o, s);
    stop_tag_session(o, s);
}

/* Issue #802 B9: a digital probe that heard only traffic the receive policy rejects leaves the detection when it ends
   unconfirmed and unheld, instead of falling back to NFM monitoring, where the rejected carrier would count as activity
   until the maximum visit. */

/* A one-range digital session whose probe runs two seconds, the window the refusals below are timed against (the
   default digital probe is longer, issue #804). */
static dsd_signal_search_config
refusal_config(const dsd_signal_search_config* base, uint64_t session, uint32_t max_visit_ms) {
    dsd_signal_search_config one = tag_config(base, session, 1, max_visit_ms);
    one.ranges[0].probe_ms = 2000;
    return one;
}

/* Land on the next detection of a digital range, its probe running. From a span tune, or the tune leaving a visit. */
static void
probe_next(dsd_opts* o, dsd_state* s) {
    const uint64_t before = status.hit.hit_id;
    for (int i = 0; i < 8 && !(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id != before); ++i) {
        land(o, s);
        ready = 1;
        tick(o, s);
    }
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id != before && !status.hit.ended);
    land(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && status.identifying && !status.hit.confirmed);
}

/* Run the visit's 2 s probe out; nothing happens a millisecond before it ends. */
static void
run_probe_out(dsd_opts* o, dsd_state* s) {
    advance(1999);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && !status.hit.confirmed);
    advance(1);
    assert(dsd_engine_signal_search_should_yield(o, s));
    tick(o, s);
}

/* The probe of detection @p hit of session @p session left it for the span: the hit ended as an unconfirmed record ("RF
   only"), what the visit received closed under its tag, and no NFM tune of the detection followed. No message. */
static void
assert_left(uint64_t session, uint64_t hit, uint32_t frequency) {
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id == hit && status.hit.ended && !status.identifying);
    assert(!status.hit.confirmed && status.hit.unresolved && status.hit.protocol[0] == '\0');
    assert(status.hit.access[0] == '\0' && status.hit.tone[0] == '\0' && status.hit.target == 0);
    assert(published_hit.hit_id == hit && published_hit.ended && !published_hit.confirmed);
    assert(target != frequency && family == DSD_RX_FAMILY_DIGITAL);
    assert(boundary_closed_visit(session, hit) && tag_clear());
    assert(status.message[0] == '\0');
}

/* The probe of detection @p hit fell back to NFM monitoring of the same detection, as before issue #802 B9. */
static void
assert_fell_back(uint64_t hit, uint32_t frequency) {
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id == hit && !status.hit.ended);
    assert(!status.hit.confirmed && target == frequency && family == DSD_RX_FAMILY_ANALOG);
}

/* Leave the visit on the air with Skip. */
static void
skip_visit(dsd_opts* o, dsd_state* s) {
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
}

/* Block DMR colour code 3, the only filter in force. A search judges by the policy configured when it started: each tune
   applies it again. */
static void
block_cc3(dsd_opts* o) {
    DSD_MEMSET(&o->receive, 0, sizeof(o->receive));
    o->receive.filters[DSD_RECEIVE_DCC].enabled = 1;
    o->receive.filters[DSD_RECEIVE_DCC].block.bits[0] = UINT64_C(1) << 3;
}

/* Land the tune that left the last visit, then stop the session, and the filters it ran under with it. */
static void
stop_refusal_session(dsd_opts* o, dsd_state* s) {
    land(o, s);
    stop_tag_session(o, s);
    DSD_MEMSET(&o->receive, 0, sizeof(o->receive));
    s->synctype = DSD_SYNC_NONE;
}

/* DMR traffic on colour code @p cc, reported with the decoder's current sync type and slot. */
static void
dmr_evidence(dsd_state* s, unsigned cc) {
    s->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    s->dmr_color_code = cc;
    dsd_signal_search_note_evidence(s, "DMR", 100, 200, NULL);
}

static void
test_refused_probe_leaves(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 50, 0);
    block_cc3(o);
    start_tag_session(o, s, &one);

    // Rejected traffic, then the probe's end: the detection is left, never monitored as NFM.
    probe_next(o, s);
    uint64_t hit = status.hit.hit_id;
    uint32_t frequency = status.hit.frequency_hz;
    dmr_evidence(s, 3);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && !status.hit.confirmed && status.hit.target == 0);
    run_probe_out(o, s);
    assert_left(50, hit, frequency);
    // Admitted traffic reported after the visit ended joins nothing.
    dmr_evidence(s, 1);
    assert(status.hit.ended && !status.hit.confirmed);

    // The refusal belonged to that visit: the next one, with no evidence, falls back as before.
    probe_next(o, s);
    hit = status.hit.hit_id;
    frequency = status.hit.frequency_hz;
    run_probe_out(o, s);
    assert_fell_back(hit, frequency);
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.unresolved && !status.hit.ended);
    skip_visit(o, s);

    // Traffic still being checked (no colour code proven yet) falls back as before.
    probe_next(o, s);
    hit = status.hit.hit_id;
    frequency = status.hit.frequency_hz;
    dmr_evidence(s, 16);
    run_probe_out(o, s);
    assert_fell_back(hit, frequency);
    land(o, s);
    skip_visit(o, s);

    // Rejected traffic reported while an input interruption stands comes from a cut frame: it is no refusal.
    probe_next(o, s);
    hit = status.hit.hit_id;
    frequency = status.hit.frequency_hz;
    s->input_interrupted = 1;
    dmr_evidence(s, 3);
    s->input_interrupted = 0;
    run_probe_out(o, s);
    assert_fell_back(hit, frequency);
    // Rejected traffic reported while the fallback tune is pending, or on the NFM visit after it, leaves nothing.
    dmr_evidence(s, 3);
    land(o, s);
    dmr_evidence(s, 3);
    carrier = 1;
    advance(3000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.hit_id == hit && !status.hit.ended);
    carrier = 0;
    skip_visit(o, s);
    stop_refusal_session(o, s);
}

/* Any admitted traffic confirms the visit, in either order with rejected traffic; a two-slot carrier's slots are each
   their own traffic, and a silent slot's permission is no evidence. */
static void
test_refused_probe_verdict_order(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 51, 0);
    // Colour code 3 and slot 2 blocked.
    block_cc3(o);
    o->receive.filters[DSD_RECEIVE_SLOT].enabled = 1;
    o->receive.filters[DSD_RECEIVE_SLOT].block.bits[0] = UINT64_C(1) << 2;
    start_tag_session(o, s, &one);
    for (int admitted_first = 0; admitted_first < 2; ++admitted_first) {
        probe_next(o, s);
        dmr_evidence(s, admitted_first ? 1 : 3);
        dmr_evidence(s, admitted_first ? 3 : 1);
        assert(status.phase == DSD_SEARCH_LISTENING && status.hit.confirmed && strcmp(status.hit.protocol, "DMR") == 0);
        // The probe's deadline passes with the admitted traffic going on: the visit stays digital and listening.
        const uint32_t frequency = status.hit.frequency_hz;
        for (int i = 0; i < 6; ++i) {
            advance(400);
            dmr_evidence(s, 1);
            tick(o, s);
        }
        assert(status.phase == DSD_SEARCH_LISTENING && status.hit.confirmed && !status.hit.ended);
        assert(target == frequency && family == DSD_RX_FAMILY_DIGITAL);
        skip_visit(o, s);
        assert(status.hit.confirmed);
    }

    // On colour code 1, slot 2's traffic alone, slot 1 silent, leaves the detection.
    probe_next(o, s);
    const uint64_t hit = status.hit.hit_id;
    const uint32_t frequency = status.hit.frequency_hz;
    s->dmr_color_code = 1;
    dsd_signal_search_note_evidence_at(s, "DMR", DSD_SYNC_DMR_BS_VOICE_POS, 1, 100, 200, NULL);
    run_probe_out(o, s);
    assert_left(51, hit, frequency);
    // Slot 1's traffic confirms, before or after slot 2's.
    for (int admitted_first = 0; admitted_first < 2; ++admitted_first) {
        probe_next(o, s);
        s->dmr_color_code = 1;
        dsd_signal_search_note_evidence_at(s, "DMR", DSD_SYNC_DMR_BS_VOICE_POS, admitted_first ? 0 : 1, 100, 200, NULL);
        dsd_signal_search_note_evidence_at(s, "DMR", DSD_SYNC_DMR_BS_VOICE_POS, admitted_first ? 1 : 0, 100, 200, NULL);
        assert(status.phase == DSD_SEARCH_LISTENING && status.hit.confirmed);
        skip_visit(o, s);
    }
    stop_refusal_session(o, s);
}

/* Rejected traffic beside traffic still being checked, nothing admitted: the probe leaves the detection when it ends
   unheld (a check still running vetoes nothing, since NFM monitoring could not confirm the carrier either), and held it
   keeps the NFM fallback. */
static void
test_refused_probe_mixed(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 52, 0);
    // Colour code 3 and slot 2 blocked.
    block_cc3(o);
    o->receive.filters[DSD_RECEIVE_SLOT].enabled = 1;
    o->receive.filters[DSD_RECEIVE_SLOT].block.bits[0] = UINT64_C(1) << 2;
    start_tag_session(o, s, &one);
    for (int held = 0; held < 2; ++held) {
        for (int checking_first = 0; checking_first < 2; ++checking_first) {
            probe_next(o, s);
            const uint64_t hit = status.hit.hit_id;
            const uint32_t frequency = status.hit.frequency_hz;
            if (held) {
                act(o, s, DSD_SEARCH_ACTION_HOLD);
            }
            // Slot 1 on a colour code not proven yet (checking) and slot 2 (rejected by the slot filter).
            s->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
            s->dmr_color_code = 16;
            dsd_signal_search_note_evidence_at(s, "DMR", DSD_SYNC_DMR_BS_VOICE_POS, checking_first ? 0 : 1, 0, 0, NULL);
            dsd_signal_search_note_evidence_at(s, "DMR", DSD_SYNC_DMR_BS_VOICE_POS, checking_first ? 1 : 0, 0, 0, NULL);
            // The colour code alone, still checking, reported with the decoder's current slot.
            dsd_signal_search_note_evidence(s, "DMR", 0, 0, NULL);
            assert(!status.hit.confirmed && status.identifying);
            if (!held) {
                run_probe_out(o, s);
                assert_left(52, hit, frequency);
                continue;
            }
            assert(status.phase == DSD_SEARCH_HELD);
            advance(2000);
            tick(o, s);
            assert_fell_back(hit, frequency);
            land(o, s);
            assert(status.phase == DSD_SEARCH_HELD && !status.hit.ended);
            act(o, s, DSD_SEARCH_ACTION_HOLD);
            skip_visit(o, s);
        }
    }
    stop_refusal_session(o, s);
}

/* NXDN by its RAN verdict (an unknown RAN, or a stand-in, is still being checked); a protocol with no code the receive
   policy reads (dPMR, M17, YSF, D-STAR) confirms under a filter of another protocol, as before. */
static void
test_refused_probe_nxdn_and_codeless(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 57, 0);
    // RAN 5 and colour code 3 blocked.
    block_cc3(o);
    o->receive.filters[DSD_RECEIVE_RAN].enabled = 1;
    o->receive.filters[DSD_RECEIVE_RAN].block.bits[0] = UINT64_C(1) << 5;
    start_tag_session(o, s, &one);
    s->synctype = DSD_SYNC_NXDN_POS;
    probe_next(o, s);
    const uint64_t hit = status.hit.hit_id;
    const uint32_t frequency = status.hit.frequency_hz;
    s->nxdn_last_ran = 5;
    s->nxdn_last_ran_stand_in = 0;
    dsd_signal_search_note_evidence(s, "NXDN", 0, 0, NULL);
    run_probe_out(o, s);
    assert_left(57, hit, frequency);
    for (int stand_in = 0; stand_in < 2; ++stand_in) {
        probe_next(o, s);
        const uint64_t checking = status.hit.hit_id;
        s->nxdn_last_ran = stand_in ? 5U : 64U;
        s->nxdn_last_ran_stand_in = (uint8_t)stand_in;
        dsd_signal_search_note_evidence(s, "NXDN", 0, 0, NULL);
        run_probe_out(o, s);
        assert_fell_back(checking, status.hit.frequency_hz);
        land(o, s);
        skip_visit(o, s);
    }
    s->nxdn_last_ran_stand_in = 0;

    static const struct {
        const char* name;
        int sync;
    } codeless[] = {
        {"dPMR", DSD_SYNC_DPMR_FS1_POS},
        {"M17", DSD_SYNC_M17_STR_POS},
        {"YSF", DSD_SYNC_YSF_POS},
        {"D-STAR", DSD_SYNC_DSTAR_VOICE_POS},
    };

    for (size_t i = 0; i < sizeof(codeless) / sizeof(codeless[0]); ++i) {
        probe_next(o, s);
        s->dmr_color_code = 3;
        s->synctype = codeless[i].sync;
        dsd_signal_search_note_evidence(s, codeless[i].name, 0, 0, NULL);
        assert(status.phase == DSD_SEARCH_LISTENING && status.hit.confirmed);
        assert(strcmp(status.hit.protocol, codeless[i].name) == 0);
        skip_visit(o, s);
    }
    stop_refusal_session(o, s);
}

/* Hold keeps its documented fallback: a probe held when it ends falls back to NFM, whether the hold came before or after
   the rejected traffic, and its release then follows the usual quiet rule. A hold released before the end holds
   nothing. */
static void
test_refused_probe_held(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 58, 0);
    block_cc3(o);
    start_tag_session(o, s, &one);
    for (int hold_first = 0; hold_first < 2; ++hold_first) {
        probe_next(o, s);
        const uint64_t hit = status.hit.hit_id;
        const uint32_t frequency = status.hit.frequency_hz;
        if (hold_first) {
            act(o, s, DSD_SEARCH_ACTION_HOLD);
        }
        dmr_evidence(s, 3);
        if (!hold_first) {
            act(o, s, DSD_SEARCH_ACTION_HOLD);
        }
        assert(status.phase == DSD_SEARCH_HELD && status.identifying);
        advance(1999);
        tick(o, s);
        assert(status.phase == DSD_SEARCH_HELD);
        advance(1);
        tick(o, s);
        assert_fell_back(hit, frequency);
        land(o, s);
        assert(status.phase == DSD_SEARCH_HELD && !status.identifying && tag_is(58, hit));
        carrier = 1;
        advance(40000);
        tick(o, s);
        assert(status.phase == DSD_SEARCH_HELD && !status.hit.ended);
        carrier = 0;
        act(o, s, DSD_SEARCH_ACTION_HOLD);
        assert(status.phase == DSD_SEARCH_LISTENING && !status.hit.ended);
        advance(499);
        tick(o, s);
        assert(status.phase == DSD_SEARCH_LISTENING);
        advance(1);
        tick(o, s);
        assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && !status.hit.confirmed);
    }

    probe_next(o, s);
    const uint64_t hit = status.hit.hit_id;
    const uint32_t frequency = status.hit.frequency_hz;
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    dmr_evidence(s, 3);
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_IDENTIFYING);
    run_probe_out(o, s);
    assert_left(58, hit, frequency);
    stop_refusal_session(o, s);
}

/* A maximum visit shorter than the probe leaves first, as before. */
static void
test_refused_probe_max_visit(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 59, 1500);
    block_cc3(o);
    start_tag_session(o, s, &one);
    probe_next(o, s);
    const uint64_t hit = status.hit.hit_id;
    dmr_evidence(s, 3);
    advance(1499);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING);
    advance(1);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && boundary_closed_visit(59, hit) && tag_clear());
    stop_refusal_session(o, s);
}

/* Skip, Avoid, Pause, Resume and Stop (Explore here's handoff stops the search) during a probe that heard rejected
   traffic work as they always do, and the refusal never reaches the next visit. */
static void
test_refused_probe_actions(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 60, 0);
    uint64_t hit = 0;
    block_cc3(o);
    start_tag_session(o, s, &one);
    for (int action = 0; action < 2; ++action) {
        probe_next(o, s);
        hit = status.hit.hit_id;
        dmr_evidence(s, 3);
        act(o, s, action ? DSD_SEARCH_ACTION_AVOID : DSD_SEARCH_ACTION_SKIP);
        assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && boundary_closed_visit(60, hit) && tag_clear());
        probe_next(o, s);
        hit = status.hit.hit_id;
        run_probe_out(o, s);
        assert_fell_back(hit, status.hit.frequency_hz);
        land(o, s);
        skip_visit(o, s);
    }
    probe_next(o, s);
    hit = status.hit.hit_id;
    dmr_evidence(s, 3);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && status.hit.ended && boundary_closed_visit(60, hit) && tag_clear());
    act(o, s, DSD_SEARCH_ACTION_RESUME);
    probe_next(o, s);
    hit = status.hit.hit_id;
    run_probe_out(o, s);
    assert_fell_back(hit, status.hit.frequency_hz);
    land(o, s);
    skip_visit(o, s);
    probe_next(o, s);
    hit = status.hit.hit_id;
    dmr_evidence(s, 3);
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.hit.ended && !status.hit.confirmed && boundary_closed_visit(60, hit) && tag_clear());
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && tag_clear());
    DSD_MEMSET(&o->receive, 0, sizeof(o->receive));
    s->synctype = DSD_SYNC_NONE;
}

/* Issue #802 B11: a site broadcast and an access code belong to the carrier, so one carrying an access code the receive
   policy rejects reaches no detection: judged by the code alone, before the hit is confirmed or after it. */

/* A checked DMR Tier III identity. */
static dsd_signal_search_site_note
dmr_site_note(const char* text) {
    dsd_signal_search_site_note note = {0};
    note.kind = DSD_SEARCH_SITE_NOTE_DMR_TIII;
    DSD_SNPRINTF(note.text, sizeof(note.text), "%s", text);
    return note;
}

static void
test_rejected_code_site(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 61, 0);
    // Colour code 3 and NXDN RAN 5 blocked.
    block_cc3(o);
    o->receive.filters[DSD_RECEIVE_RAN].enabled = 1;
    o->receive.filters[DSD_RECEIVE_RAN].block.bits[0] = UINT64_C(1) << 5;
    start_tag_session(o, s, &one);

    // Admitted colour code 1 confirms the detection with its site.
    probe_next(o, s);
    s->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    s->dmr_color_code = 1;
    dsd_signal_search_note_evidence(s, "DMR", 100, 200, "CC 1");
    const dsd_signal_search_site_note admitted = dmr_site_note("TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4");
    dsd_signal_search_note_site(s, &admitted);
    assert(status.hit.confirmed && strcmp(status.hit.site, admitted.text) == 0);
    // Co-channel colour code 3 locks and broadcasts its own identity: neither its site nor its access code proof reaches
    // the detection, and nothing is published for it.
    s->dmr_color_code = 3;
    const int published = hits_published;
    const dsd_signal_search_site_note rejected = dmr_site_note("TIII Small \xC2\xB7 Net 9 \xC2\xB7 Site 1");
    dsd_signal_search_note_site(s, &rejected);
    dsd_signal_search_note_evidence(s, "DMR", 300, 400, "CC 3");
    dsd_signal_search_note_access(s);
    assert(hits_published == published && strcmp(status.hit.site, admitted.text) == 0);
    assert(strcmp(status.hit.access, "CC 1") == 0 && status.hit.target == 100);
    // Back on colour code 1, a new broadcast shows.
    s->dmr_color_code = 1;
    const dsd_signal_search_site_note next = dmr_site_note("TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 5");
    dsd_signal_search_note_site(s, &next);
    assert(hits_published == published + 1 && strcmp(status.hit.site, next.text) == 0);
    skip_visit(o, s);

    // Before confirmation: the rejected code's broadcast never joins the identity admitted after it. A code not proven
    // yet is no rejection: that broadcast waits for the identification, as before.
    probe_next(o, s);
    s->dmr_color_code = 3;
    dsd_signal_search_note_site(s, &rejected);
    s->dmr_color_code = 1;
    dsd_signal_search_note_evidence(s, "DMR", 100, 200, "CC 1");
    assert(status.hit.confirmed && status.hit.site[0] == '\0');
    skip_visit(o, s);
    probe_next(o, s);
    s->dmr_color_code = 16;
    dsd_signal_search_note_site(s, &admitted);
    s->dmr_color_code = 1;
    dsd_signal_search_note_evidence(s, "DMR", 100, 200, "CC 1");
    assert(status.hit.confirmed && strcmp(status.hit.site, admitted.text) == 0);
    skip_visit(o, s);

    // NXDN by its RAN, whatever the decoder's sync type says when the broadcast is noted.
    probe_next(o, s);
    dsd_signal_search_site_note nxdn = {0};
    nxdn.kind = DSD_SEARCH_SITE_NOTE_NXDN;
    nxdn.nxdn_site = 12;
    nxdn.nxdn_sys = 3;
    s->synctype = DSD_SYNC_NONE;
    s->nxdn_last_ran = 5;
    dsd_signal_search_note_site(s, &nxdn);
    s->synctype = DSD_SYNC_NXDN_POS;
    s->nxdn_last_ran = 7;
    dsd_signal_search_note_evidence(s, "NXDN", 0, 0, NULL);
    assert(status.hit.confirmed && status.hit.site[0] == '\0');
    dsd_signal_search_note_site(s, &nxdn);
    assert(strcmp(status.hit.site, "Site 12 \xC2\xB7 Sys 3") == 0);
    s->nxdn_last_ran = 64;
    stop_refusal_session(o, s);
}

/* The code alone: a slot filter blocking the slot the decoder is on never keeps a P25 Phase 2 carrier's network status
   or its proven NAC from a detection its admitted slot confirmed. The NAC filter does. */
static void
test_rejected_code_p25p2_slot(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = refusal_config(config, 63, 0);
    DSD_MEMSET(&o->receive, 0, sizeof(o->receive));
    o->receive.filters[DSD_RECEIVE_SLOT].enabled = 1;
    o->receive.filters[DSD_RECEIVE_SLOT].block.bits[0] = UINT64_C(1) << 1;
    o->receive.filters[DSD_RECEIVE_NAC].enabled = 1;
    o->receive.filters[DSD_RECEIVE_NAC].allow.bits[0x293 / 64] = UINT64_C(1) << (0x293 % 64);
    start_tag_session(o, s, &one);
    probe_next(o, s);
    s->synctype = DSD_SYNC_P25P2_POS;
    s->currentslot = 0;
    s->p2_proven_nac = 0x293;
    dsd_signal_search_note_evidence_at(s, "P25", DSD_SYNC_P25P2_POS, 1, 0, 0, NULL);
    assert(status.hit.confirmed && strcmp(status.hit.protocol, "P25") == 0);
    int published = hits_published;
    const dsd_signal_search_site_note network = p25_network_note(0xBEE00, 0x1A3);
    dsd_signal_search_note_site(s, &network);
    assert(hits_published == published + 1 && strcmp(status.hit.site, "WACN BEE00 \xC2\xB7 SYS 1A3") == 0);
    dsd_signal_search_note_access(s);
    assert(hits_published == published + 2);
    // Another NAC, which the filter does not allow, on the same carrier: its broadcast and its proof stay off.
    s->p2_proven_nac = 0x2A4;
    published = hits_published;
    const dsd_signal_search_site_note other = p25_network_note(0xBEE00, 0x2B4);
    dsd_signal_search_note_site(s, &other);
    dsd_signal_search_note_access(s);
    assert(hits_published == published && strcmp(status.hit.site, "WACN BEE00 \xC2\xB7 SYS 1A3") == 0);
    s->p2_proven_nac = -1;
    s->synctype = DSD_SYNC_NONE;
    skip_visit(o, s);
    stop_refusal_session(o, s);
}

/* Issue #804 F5: an Auto visit starts on the 4800/4 C4FM profile, whatever the hunt held when the search started or
   reached on an earlier visit; a -m lock keeps its own timing, modulation and profile pin. Stop restores the
   session's own. */
static void
put_timing(dsd_state* s, int profile, int sps, int center, int rf_mod) {
    s->sps_hunt_idx = profile;
    s->samplesPerSymbol = sps;
    s->symbolCenter = center;
    s->rf_mod = rf_mod;
}

static int
timing_is(const dsd_state* s, int profile, int sps, int center, int rf_mod) {
    return s->sps_hunt_idx == profile && s->samplesPerSymbol == sps && s->symbolCenter == center && s->rf_mod == rf_mod;
}

/* Leave the visited channel for the next detection of the pass, and land there. */
static void
next_visit(dsd_opts* o, dsd_state* s) {
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    visit_strongest(o, s);
}

static void
test_auto_visit_starts_on_4800_c4fm(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    /* The session's hunt sat on 2400/4 at Start; the session learned CQPSK on 4800/4. */
    static const int held[2][4] = {{DSD_FRAME_SYNC_SPS_PROFILE_2400_4, 20, 9, 0},
                                   {DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 1}};
    for (int i = 0; i < 2; ++i) {
        const dsd_signal_search_config one = tag_config(config, 76 + (uint64_t)i, 1, 0);
        o->mod_cli_lock = 0;
        put_timing(s, held[i][0], held[i][1], held[i][2], held[i][3]);
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        assert(status.phase == DSD_SEARCH_IDENTIFYING);
        assert(timing_is(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0));
        /* The hunt moves on during the visit: the next visit starts over. */
        put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 8, 3, 1);
        next_visit(o, s);
        assert(status.phase == DSD_SEARCH_IDENTIFYING);
        assert(timing_is(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0));
        stop_tag_session(o, s);
        assert(timing_is(s, held[i][0], held[i][1], held[i][2], held[i][3]));
    }
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0);
}

/* The front end a session starts on: its output rate now, whether it runs the analog family (or has a family switch
   outstanding), and where a retune that carries the digital family lands, on the FSK discriminator and on the CQPSK
   chain. DSD_NEO_CQPSK set lands CQPSK for a tune whose CQPSK state is no target's own choice. */
static struct {
    unsigned int live_hz, fsk_hz, cqpsk_hz;
    int analog, env_cqpsk;
} front_end;

static unsigned int
front_end_live_hz(void) {
    return front_end.live_hz;
}

static int
front_end_family_landing_after_pending(void) {
    return front_end.analog;
}

static unsigned int
front_end_rate_for_family(int f, int cqpsk_enable, int symbol_rate_hz, int cqpsk_explicit) {
    if (f != DSD_RX_FAMILY_DIGITAL || symbol_rate_hz <= 0) {
        return 0U;
    }
    const int cqpsk = cqpsk_explicit ? cqpsk_enable : (cqpsk_enable || front_end.env_cqpsk);
    return cqpsk ? front_end.cqpsk_hz : front_end.fsk_hz;
}

/* Start sessions on a front end that answers with these rates through the stream metrics hooks. */
static void
use_front_end(unsigned int live_hz, unsigned int fsk_hz, unsigned int cqpsk_hz, int analog, int env_cqpsk) {
    front_end.live_hz = live_hz;
    front_end.fsk_hz = fsk_hz;
    front_end.cqpsk_hz = cqpsk_hz;
    front_end.analog = analog;
    front_end.env_cqpsk = env_cqpsk;
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    hooks.output_rate_hz = front_end_live_hz;
    hooks.family_landing_after_pending = front_end_family_landing_after_pending;
    hooks.output_rate_for_family = front_end_rate_for_family;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

/* Every Auto visit lands the digital family with C4FM, the search's own choice, so it is timed at the rate that landing
   runs, never the rate the session's front end runs at Start (issue #804): a CQPSK session at 24 kHz whose FSK stream
   resamples to 48 kHz (digital_resample=on) lands every visit on 4800/4 at 10 samples per symbol, and the frame-sync
   hunt then never reads its 5 as 9600/2. */
static void
test_auto_visit_timed_for_its_landing(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    /* live, FSK and CQPSK landing rates, analog at Start, DSD_NEO_CQPSK; the session's timing; the visit's sps/center. */
    static const struct {
        unsigned int live_hz, fsk_hz, cqpsk_hz;
        int analog, env_cqpsk;
        int held[4];
        int sps, center;
    } starts[] = {
        /* CQPSK at 24 kHz, the FSK stream resampled to 48 kHz. */
        {24000, 48000, 24000, 0, 0, {DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 5, 2, 1}, 10, 4},
        /* The same with DSD_NEO_CQPSK set: the visit's C4FM is the search's own choice and stands over it. */
        {24000, 48000, 24000, 0, 1, {DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 5, 2, 1}, 10, 4},
        /* CQPSK at 24 kHz with no resampling: FSK lands at 24 kHz too. */
        {24000, 24000, 24000, 0, 0, {DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 5, 2, 1}, 5, 2},
        /* The analog monitor at 48 kHz at Start; the digital family lands at the 24 kHz complex rate. */
        {48000, 24000, 24000, 1, 0, {DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0}, 5, 2},
    };

    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        use_front_end(starts[i].live_hz, starts[i].fsk_hz, starts[i].cqpsk_hz, starts[i].analog, starts[i].env_cqpsk);
        const int* held = starts[i].held;
        const dsd_signal_search_config one = tag_config(config, 81 + (uint64_t)i, 1, 0);
        o->mod_cli_lock = 0;
        put_timing(s, held[0], held[1], held[2], held[3]);
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        assert(status.phase == DSD_SEARCH_IDENTIFYING);
        assert(retune_family.family == DSD_RX_FAMILY_DIGITAL && retune_family.cqpsk_explicit == 1);
        assert(timing_is(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, starts[i].sps, starts[i].center, 0));
        put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_9600_2, 5, 2, 0);
        next_visit(o, s);
        assert(status.phase == DSD_SEARCH_IDENTIFYING);
        assert(timing_is(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, starts[i].sps, starts[i].center, 0));
        stop_tag_session(o, s);
        assert(timing_is(s, held[0], held[1], held[2], held[3]));
    }
    dsd_rtl_stream_metrics_hooks_set(NULL);
    DSD_MEMSET(&front_end, 0, sizeof(front_end));
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0);
}

/* A typed range's visit lands the digital family the same way, with the row's own modulation: the row is timed for
   that landing too, not for the rate the session's CQPSK chain runs at when the row is entered. */
static void
test_typed_visit_timed_for_its_landing(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    /* The range's digital value (P25, DMR, NXDN48); the visit's profile, sps, center and modulation at 48 kHz. */
    static const int typed[3][5] = {
        {2, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0},
        {3, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 2},
        {5, DSD_FRAME_SYNC_SPS_PROFILE_2400_4, 20, 9, 0},
    };
    use_front_end(24000, 48000, 24000, 0, 0);
    for (int i = 0; i < 3; ++i) {
        const dsd_signal_search_config one = tag_config(config, 85 + (uint64_t)i, typed[i][0], 0);
        o->mod_cli_lock = 0;
        put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 5, 2, 1);
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        assert(status.phase == DSD_SEARCH_IDENTIFYING);
        assert(retune_family.family == DSD_RX_FAMILY_DIGITAL && retune_family.cqpsk_explicit == 1);
        assert(timing_is(s, typed[i][1], typed[i][2], typed[i][3], typed[i][4]));
        next_visit(o, s);
        assert(timing_is(s, typed[i][1], typed[i][2], typed[i][3], typed[i][4]));
        stop_tag_session(o, s);
        assert(timing_is(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 5, 2, 1));
    }
    dsd_rtl_stream_metrics_hooks_set(NULL);
    DSD_MEMSET(&front_end, 0, sizeof(front_end));
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0);
}

static void
test_locked_tuple_kept(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    /* -mq: CQPSK on 4800/4. -m2: CQPSK on 6000/4, the P25 Phase 2 profile pinned. */
    static const int locked[2][4] = {{DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0},
                                     {DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 8, 3, 1}};
    for (int i = 0; i < 2; ++i) {
        const dsd_signal_search_config one = tag_config(config, 78 + (uint64_t)i, 1, 0);
        o->mod_cli_lock = 1;
        o->mod_c4fm = 0;
        o->mod_qpsk = 1;
        o->mod_gfsk = 0;
        o->mod_p25p2_profile_lock = locked[i][3];
        put_timing(s, locked[i][0], locked[i][1], locked[i][2], 1);
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        assert(timing_is(s, locked[i][0], locked[i][1], locked[i][2], 1));
        assert(o->mod_cli_lock == 1 && o->mod_qpsk == 1 && o->mod_c4fm == 0);
        assert(o->mod_p25p2_profile_lock == locked[i][3]);
        put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_2400_4, 20, 9, 0);
        next_visit(o, s);
        assert(timing_is(s, locked[i][0], locked[i][1], locked[i][2], 1));
        stop_tag_session(o, s);
        assert(timing_is(s, locked[i][0], locked[i][1], locked[i][2], 1) && o->mod_cli_lock == 1);
        assert(o->mod_p25p2_profile_lock == locked[i][3]);
    }
    o->mod_cli_lock = o->mod_qpsk = o->mod_p25p2_profile_lock = 0;
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0);
}

/* A -m lock pins the profile, modulation and P25 Phase 2 pin, not the rate its timing was taken at: a locked session
   on the 48 kHz analog monitor at Start, whose CQPSK chain lands at the 24 kHz complex rate, visits with its kept
   profile timed for that landing, and Stop restores the session's own. */
static void
test_locked_tuple_timed_for_its_landing(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    /* -mq: CQPSK on 4800/4. -m2: CQPSK on 6000/4, pinned. The session's timing at 48 kHz; the visit's at 24 kHz. */
    static const int locked[2][6] = {
        {DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0, 5, 2},
        {DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 8, 3, 1, 4, 1},
    };
    use_front_end(48000, 24000, 24000, 1, 0);
    for (int i = 0; i < 2; ++i) {
        const dsd_signal_search_config one = tag_config(config, 88 + (uint64_t)i, 1, 0);
        o->mod_cli_lock = 1;
        o->mod_c4fm = 0;
        o->mod_qpsk = 1;
        o->mod_gfsk = 0;
        o->mod_p25p2_profile_lock = locked[i][3];
        put_timing(s, locked[i][0], locked[i][1], locked[i][2], 1);
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        assert(retune_family.family == DSD_RX_FAMILY_DIGITAL && retune_family.cqpsk_explicit == 1);
        assert(timing_is(s, locked[i][0], locked[i][4], locked[i][5], 1));
        assert(o->mod_cli_lock == 1 && o->mod_qpsk == 1 && o->mod_c4fm == 0);
        assert(o->mod_p25p2_profile_lock == locked[i][3]);
        put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_2400_4, 20, 9, 0);
        next_visit(o, s);
        assert(timing_is(s, locked[i][0], locked[i][4], locked[i][5], 1));
        stop_tag_session(o, s);
        assert(timing_is(s, locked[i][0], locked[i][1], locked[i][2], 1) && o->mod_cli_lock == 1);
        assert(o->mod_p25p2_profile_lock == locked[i][3]);
    }
    dsd_rtl_stream_metrics_hooks_set(NULL);
    DSD_MEMSET(&front_end, 0, sizeof(front_end));
    o->mod_cli_lock = o->mod_qpsk = o->mod_p25p2_profile_lock = 0;
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0);
}

/* Issue #804 F6: every departure puts the session's own P25 site back whole, never the proof. */
#define SITE_CODE(wacn, sysid, nac) (((uint64_t)(wacn) << 24) | ((uint64_t)(sysid) << 12) | (uint64_t)(nac))
static const uint64_t CODE_A = SITE_CODE(0xBEE00, 0x1A3, 0x293);
static const uint64_t CODE_B = SITE_CODE(0x45564, 0x006, 0x006);

/* The channel an imported channel map names, and the one a visit maps. */
enum { IMPORTED_CHANNEL = 5, VISIT_CHANNEL = 7, VISIT_IDEN = 3 };

/* The raw channels of a pending neighbour announcement the session's site and a visited site each heard. */
enum { SESSION_PENDING_CHANNEL = 0x1011, VISIT_PENDING_CHANNEL = 0x300A };

static void
hold_code(dsd_state* s, uint64_t code) {
    s->p2_wacn = code >> 24;
    s->p2_sysid = (code >> 12) & 0xFFFU;
    s->p2_cc = code & 0xFFFU;
}

static void
clear_chan_map(dsd_state* s) {
    while (s->trunk_chan_map_used_count) {
        dsd_state_set_trunk_chan_freq(s, s->trunk_chan_map_used[0], 0);
    }
}

/* A neighbour announcement on raw @p channel, waiting for the IDEN that resolves it, appended to the site's. */
static void
add_pending_neighbor(dsd_state* s, uint16_t channel) {
    p25_pending_announcement_t* e = &s->p25_pending_announcements[s->p25_pending_announcement_count++];
    DSD_MEMSET(e, 0, sizeof(*e));
    e->populated = 1;
    e->kind = P25_PENDING_ANNOUNCEMENT_NEIGHBOR;
    e->rfss = 1;
    e->site = 4;
    e->sysid = 0x006;
    e->channel = channel;
}

/* The session's P25 site at Start: @p code (0 for none), unproven, hand-set when @p hardset, nothing learned, and
   channel IMPORTED_CHANNEL mapped to @p imported (0 for no channel map). */
static void
session_site(dsd_state* s, uint64_t code, int hardset, long imported) {
    hold_code(s, code);
    s->p2_hardset = hardset;
    dsd_state_forget_p2_proof(s);
    s->p2_rfssid = s->p2_siteid = 0;
    s->p25_cc_freq = s->trunk_cc_freq = 0;
    s->p25_cc_is_tdma = 2;
    s->p25_sys_is_tdma = 0;
    s->p25_chan_iden = 0;
    DSD_MEMSET(s->p25_chan_tdma_explicit, 0, sizeof(s->p25_chan_tdma_explicit));
    DSD_MEMSET(s->p25_iden_fdma, 0, sizeof(s->p25_iden_fdma));
    DSD_MEMSET(s->p25_iden_tdma, 0, sizeof(s->p25_iden_tdma));
    s->p25_pending_announcement_count = s->p25_secondary_cc_count = 0;
    DSD_MEMSET(s->p25_pending_announcements, 0, sizeof(s->p25_pending_announcements));
    DSD_MEMSET(s->p25_secondary_cc_entries, 0, sizeof(s->p25_secondary_cc_entries));
    s->p25_cc_prot_valid = s->p25_sys_services_valid = s->p25_site_lra_valid = s->p25_site_network_active_valid = 0;
    clear_chan_map(s);
    if (imported) {
        dsd_state_set_trunk_chan_freq(s, IMPORTED_CHANNEL, imported);
    }
}

/* Whether none of a visited site's broadcasts stays on @p s: no pending announcement or secondary control channel,
   and none of its protection, services, LRA or network-active broadcasts. */
static int
no_site_broadcasts(const dsd_state* s) {
    return s->p25_pending_announcement_count == 0 && !s->p25_pending_announcements[0].populated
           && s->p25_secondary_cc_count == 0 && s->p25_secondary_cc_entries[0].freq == 0 && !s->p25_cc_prot_valid
           && !s->p25_sys_services_valid && !s->p25_site_lra_valid && !s->p25_site_network_active_valid;
}

/* Whether nothing a visit learned stays on @p s: no RFSS or site, control channel, TDMA flags, IDEN update or site
   broadcast. */
static int
nothing_learned(const dsd_state* s) {
    return s->p2_rfssid == 0 && s->p2_siteid == 0 && s->p25_cc_freq == 0 && s->trunk_cc_freq == 0
           && s->p25_cc_is_tdma == 2 && s->p25_sys_is_tdma == 0 && s->p25_chan_iden == 0
           && s->p25_chan_tdma_explicit[VISIT_IDEN] == 0 && !s->p25_iden_tdma[VISIT_IDEN].populated
           && no_site_broadcasts(s);
}

/* Whether @p s holds the session's site as session_site() set it, the proof cleared. */
static int
session_back(const dsd_state* s, uint64_t code, int hardset, long imported) {
    return code_held(s) == code && s->p2_hardset == hardset && s->p2_cc_verified == 0U && s->p2_proven_nac == -1
           && nothing_learned(s) && s->trunk_chan_map[VISIT_CHANNEL] == 0
           && s->trunk_chan_map[IMPORTED_CHANNEL] == imported && s->trunk_chan_map_used_count == (imported ? 1U : 0U);
}

/* A TDMA IDEN update for VISIT_IDEN, stamped with the system of @p code (the WACN and SYSID in force when it decoded). */
static void
learn_iden(dsd_state* s, uint64_t code, long base) {
    p25_iden_entry_t* e = &s->p25_iden_tdma[VISIT_IDEN];
    DSD_MEMSET(e, 0, sizeof(*e));
    e->base_freq = base;
    e->chan_type = 3;
    e->chan_spac = 100;
    e->populated = 1;
    e->trust = 2;
    e->wacn = code >> 24;
    e->sysid = (code >> 12) & 0xFFFU;
    s->p25_chan_tdma_explicit[VISIT_IDEN] = 1;
    s->p25_chan_iden = VISIT_IDEN;
}

/* What a visited Phase 2 site teaches the decoder: its code, proven there, its RFSS and site, its TDMA control channel,
   an IDEN update, a channel mapped, a neighbour announcement still waiting for its IDEN, a secondary control channel
   and its site broadcasts. */
static void
learn_site(dsd_state* s, uint64_t code) {
    hold_code(s, code);
    s->p2_cc_verified = 1U;
    s->p2_proven_nac = (int)(code & 0xFFFU); // the proof names the code's NAC (issue #802)
    s->p2_rfssid = 2;
    s->p2_siteid = 9;
    s->p25_cc_freq = s->trunk_cc_freq = 769768750;
    s->p25_cc_is_tdma = 1;
    s->p25_sys_is_tdma = 1;
    learn_iden(s, code, 153950000);
    dsd_state_set_trunk_chan_freq(s, VISIT_CHANNEL, 770152750);
    add_pending_neighbor(s, VISIT_PENDING_CHANNEL);
    p25_secondary_cc_entry_t* scc = &s->p25_secondary_cc_entries[s->p25_secondary_cc_count++];
    scc->freq = 769918750;
    scc->channel = 0x3010;
    scc->rfss = 2;
    scc->site = 9;
    s->p25_cc_prot_valid = s->p25_sys_services_valid = s->p25_site_lra_valid = s->p25_site_network_active_valid = 1;
    s->p25_cc_prot_algid = 0x84;
    s->p25_site_lra = 0x22;
}

static void
test_code_does_not_leak_between_visits(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 80, 1, 0);
    session_site(s, CODE_A, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(session_back(s, CODE_A, 0, 0));
    learn_site(s, CODE_B);
    const int before = boundary;
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    /* The departure's boundary ended the visit's calls under the visit's own proven code; then the session's came
       back, unproven, and the next landing has it. */
    assert(boundary == before + 1 && boundary_code == CODE_B && boundary_proven);
    assert(session_back(s, CODE_A, 0, 0));
    visit_strongest(o, s);
    assert(session_back(s, CODE_A, 0, 0));
    /* A Phase 1 NID's NAC alone, as a conventional Phase 1 visit writes it: no mixed code reaches the next visit. */
    s->p2_cc = 0x111;
    next_visit(o, s);
    assert(session_back(s, CODE_A, 0, 0));
    stop_tag_session(o, s);
    assert(session_back(s, CODE_A, 0, 0));
}

/* An IDEN update a visit received before the network status naming its system is stamped with the code in force, which
   is the session's: the departure drops it all the same. */
static void
test_iden_before_nsb_restored(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    for (int hardset = 0; hardset <= 1; ++hardset) {
        const dsd_signal_search_config one = tag_config(config, 81 + (uint64_t)hardset, 1, 0);
        session_site(s, CODE_A, hardset, 0);
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        learn_iden(s, CODE_A, 153950000);
        next_visit(o, s);
        assert(session_back(s, CODE_A, hardset, 0));
        stop_tag_session(o, s);
    }
    session_site(s, 0, 0, 0);
}

/* The operator's configuration comes back whole: a hand-set code (-X) and an imported channel map. */
static void
test_hardset_restored(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 83, 1, 0);
    session_site(s, CODE_A, 1, 851012500);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(session_back(s, CODE_A, 1, 851012500));
    learn_site(s, CODE_B);
    s->p2_hardset = 0;
    dsd_state_set_trunk_chan_freq(s, IMPORTED_CHANNEL, 852000000);
    const uint64_t seq = s->trunk_chan_map_seq;
    next_visit(o, s);
    /* The map moved back: its sequence says so to the frontends that copy it on a change. */
    assert(session_back(s, CODE_A, 1, 851012500) && s->trunk_chan_map_seq > seq);
    stop_tag_session(o, s);
    assert(session_back(s, CODE_A, 1, 851012500) && s->trunk_chan_map_seq > seq);
    session_site(s, 0, 0, 0);
}

static void
test_unknown_identity_restored(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 84, 1, 0);
    session_site(s, 0, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    learn_site(s, CODE_B);
    next_visit(o, s);
    assert(session_back(s, 0, 0, 0));
    learn_site(s, CODE_B);
    stop_tag_session(o, s);
    assert(session_back(s, 0, 0, 0));
}

/* A code proven on the carrier the session was on at Start proves nothing on another: the restore never brings the
   proof back, so recovery stays eligible and no NAC is reported from it. */
static void
test_verified_seed_at_start_not_restored_as_proof(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 85, 1, 0);
    session_site(s, CODE_A, 0, 0);
    s->p2_cc_verified = 1U;
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(session_back(s, CODE_A, 0, 0));
    next_visit(o, s);
    assert(session_back(s, CODE_A, 0, 0));
    stop_tag_session(o, s);
    assert(session_back(s, CODE_A, 0, 0));
}

/* A neighbour or secondary control channel announcement waiting for its IDEN belongs to the site that sent it: the
   session's comes back at a departure, and a visited site's never does. Left behind, a visited site's would be resolved
   with the session's IDEN tables once the session's network status came back with its system unchanged, publishing a
   neighbour at a frequency of the wrong system. */
static void
test_pending_announcements_stay_with_their_site(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 96, 1, 0);
    session_site(s, CODE_A, 0, 0);
    add_pending_neighbor(s, SESSION_PENDING_CHANNEL);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    learn_site(s, CODE_B);
    assert(s->p25_pending_announcement_count == 2);
    next_visit(o, s);
    assert(s->p25_pending_announcement_count == 1 && s->p25_pending_announcements[0].populated);
    assert(s->p25_pending_announcements[0].channel == SESSION_PENDING_CHANNEL);
    assert(!s->p25_pending_announcements[1].populated && s->p25_secondary_cc_count == 0);
    learn_site(s, CODE_B);
    stop_tag_session(o, s);
    assert(s->p25_pending_announcement_count == 1
           && s->p25_pending_announcements[0].channel == SESSION_PENDING_CHANNEL);
    assert(!s->p25_pending_announcements[1].populated);
    session_site(s, 0, 0, 0);
}

/* The decoder session ends under the search during a call: the final call snapshots come after the search's shutdown,
   and no boundary has closed the visit, so its code and proof stay the call's own until the engine's teardown releases
   the session's site. Stopped with the input lost, the reception closes first and the session's site comes back
   unproven at once. */
static void
test_shutdown_during_active_call(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    dsd_signal_search_config one = tag_config(config, 86, 1, 0);
    session_site(s, CODE_A, 0, 0);
    s->p2_cc_verified = 1U;
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    learn_site(s, CODE_B);
    const int before = boundary;
    dsd_engine_signal_search_shutdown(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && boundary == before);
    uint8_t kind = 0;
    uint16_t nac = 0;
    assert(dsd_access_code_current(s, DSD_SYNC_P25P2_POS, 0, 0, &kind, &nac) == 1 && nac == (CODE_B & 0xFFFU));
    dsd_signal_search_visit_clear(); // the engine's teardown, after its last close
    /* Then, before the band plan export, the session's site comes back unproven: no visited system's code or IDEN
       tables outlive the session. Once. */
    dsd_engine_signal_search_release_site(s);
    assert(session_back(s, CODE_A, 0, 0));
    s->p2_cc = 0x111;
    dsd_engine_signal_search_release_site(s);
    assert(s->p2_cc == 0x111);

    one = tag_config(config, 87, 1, 0);
    session_site(s, CODE_A, 0, 0);
    s->p2_cc_verified = 1U;
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    learn_site(s, CODE_B);
    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED && boundary_code == CODE_B && boundary_proven);
    assert(session_back(s, CODE_A, 0, 0));
    assert(dsd_access_code_current(s, DSD_SYNC_P25P2_POS, 0, 0, &kind, &nac) == 0);
    /* The Stop put it back already: the teardown owes nothing. */
    s->p2_cc = 0x111;
    dsd_engine_signal_search_release_site(s);
    assert(s->p2_cc == 0x111);
    live = 1;
    session_site(s, 0, 0, 0);
}

/* Review focus 2: a departure in the middle of a call and of a pending code recovery. The recovery's candidate waits
   on the carrier it was found on (p25p2_seed_recover.c drops it once the carrier count moves); every departure route
   moves it at its boundary, under the visit's own code, and then puts the session's site back, no proof surviving. */
static uint32_t pending_carrier;

static void
visit_mid_recovery(dsd_state* s) {
    hold_code(s, CODE_B);
    s->synctype = DSD_SYNC_P25P2_POS;
    pending_carrier = s->carrier_seq;
}

static int
departed_mid_recovery(const dsd_state* s) {
    return s->carrier_seq != pending_carrier && boundary_code == CODE_B && session_back(s, CODE_A, 0, 0);
}

static void
test_departure_mid_recovery_drops_pending(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    dsd_signal_search_config one = tag_config(config, 88, 1, 2500);
    one.ranges[0].probe_ms = 2000; // the departures below are timed against a two-second probe
    session_site(s, CODE_A, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    /* The probe's analog fallback on the same detection. */
    visit_mid_recovery(s);
    advance(1000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && s->carrier_seq == pending_carrier);
    advance(1000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && !status.hit.ended && departed_mid_recovery(s));
    land(o, s);
    /* The visit limit, with the carrier still open. */
    visit_mid_recovery(s);
    carrier = 1;
    advance(500);
    tick(o, s);
    carrier = 0;
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && departed_mid_recovery(s));
    visit_strongest(o, s);
    /* Skip, then Avoid. */
    visit_mid_recovery(s);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(departed_mid_recovery(s));
    visit_strongest(o, s);
    visit_mid_recovery(s);
    act(o, s, DSD_SEARCH_ACTION_AVOID);
    assert(status.phase == DSD_SEARCH_TUNING && departed_mid_recovery(s));
    land(o, s);
    stop_tag_session(o, s);
    /* Pause. */
    one = tag_config(config, 89, 1, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    visit_mid_recovery(s);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && departed_mid_recovery(s));
    stop_tag_session(o, s);
    /* Stop. */
    one = tag_config(config, 90, 1, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    visit_mid_recovery(s);
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(departed_mid_recovery(s));
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && session_back(s, CODE_A, 0, 0));
    s->synctype = 0;
    session_site(s, 0, 0, 0);
}

/* Issue #804: the session's learned P25 Phase 2 codes, which code recovery tries on a carrier. */
static uint64_t
test_code(unsigned i) {
    return SITE_CODE(0x10000U + i, 0x100U + i, 0x200U + i);
}

static size_t
codes_now(const dsd_state* s, uint64_t* out, size_t cap) {
    DSD_MEMSET(out, 0, cap * sizeof(*out));
    return dsd_signal_search_p25p2_codes(s, out, cap);
}

static void
test_code_list_bound_dedupe_order(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 91, 1, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    uint64_t out[20];
    assert(codes_now(s, out, 20) == 0);
    /* Every source joins; 16 are kept, most recently confirmed first. */
    for (unsigned i = 0; i < 18; ++i) {
        dsd_signal_search_note_p25p2_code(s, test_code(i), DSD_P25P2_CODE_RECOVERED + (int)(i % 3));
    }
    assert(codes_now(s, out, 20) == 16);
    for (unsigned i = 0; i < 16; ++i) {
        assert(out[i] == test_code(17 - i));
    }
    /* Confirmed again, a code moves to the front, once. */
    dsd_signal_search_note_p25p2_code(s, test_code(5), DSD_P25P2_CODE_P1_NETWORK);
    assert(codes_now(s, out, 20) == 16 && out[0] == test_code(5) && out[1] == test_code(17) && out[15] == test_code(2));
    for (unsigned i = 1; i < 16; ++i) {
        assert(out[i] != test_code(5));
    }
    /* A code with a reserved field, or an unknown source, joins nothing. */
    static const uint64_t reserved[] = {SITE_CODE(0, 1, 1),
                                        SITE_CODE(0xFFFFF, 1, 1),
                                        SITE_CODE(1, 0, 1),
                                        SITE_CODE(1, 0xFFF, 1),
                                        SITE_CODE(1, 1, 0),
                                        SITE_CODE(1, 1, 0xFFF),
                                        SITE_CODE(1, 1, 1) | (1ULL << 44)};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        dsd_signal_search_note_p25p2_code(s, reserved[i], DSD_P25P2_CODE_RECOVERED);
    }
    dsd_signal_search_note_p25p2_code(s, test_code(40), 0);
    dsd_signal_search_note_p25p2_code(s, test_code(41), 4);
    assert(codes_now(s, out, 20) == 16 && out[0] == test_code(5) && out[15] == test_code(2));
    /* A smaller answer is the most recent part of it. */
    assert(codes_now(s, out, 3) == 3 && out[0] == test_code(5) && out[1] == test_code(17) && out[2] == test_code(16));
    stop_tag_session(o, s);
}

static void
test_code_list_pause_resume_and_new_start(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    char error[192];
    const dsd_signal_search_config one = tag_config(config, 92, 1, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    dsd_signal_search_note_p25p2_code(s, CODE_A, DSD_P25P2_CODE_P2_NETWORK);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED);
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    visit_strongest(o, s);
    uint64_t out[4];
    assert(codes_now(s, out, 4) == 1 && out[0] == CODE_A);
    stop_tag_session(o, s);
    const dsd_signal_search_config next = tag_config(config, 93, 1, 0);
    start_tag_session(o, s, &next);
    visit_strongest(o, s);
    assert(codes_now(s, out, 4) == 0);
    stop_tag_session(o, s);
}

static void
test_code_list_only_while_receiving(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    uint64_t out[4];
    dsd_signal_search_config one = tag_config(config, 94, 1, 0);
    start_tag_session(o, s, &one);
    /* No visit yet: the span tune. */
    dsd_signal_search_note_p25p2_code(s, test_code(1), DSD_P25P2_CODE_P2_NETWORK);
    visit_strongest(o, s);
    assert(codes_now(s, out, 4) == 0);
    /* A frame an input interruption cut. */
    s->input_interrupted = 1;
    dsd_signal_search_note_p25p2_code(s, test_code(2), DSD_P25P2_CODE_P2_NETWORK);
    assert(codes_now(s, out, 4) == 0);
    s->input_interrupted = 0;
    dsd_signal_search_note_p25p2_code(s, test_code(3), DSD_P25P2_CODE_P2_NETWORK);
    assert(codes_now(s, out, 4) == 1 && out[0] == test_code(3));
    /* Between visits the list is kept, and given to no one. */
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(status.phase == DSD_SEARCH_TUNING && codes_now(s, out, 4) == 0);
    dsd_signal_search_note_p25p2_code(s, test_code(4), DSD_P25P2_CODE_P2_NETWORK);
    visit_strongest(o, s);
    assert(codes_now(s, out, 4) == 1 && out[0] == test_code(3));
    /* The probe's analog fallback listens to the channel as NFM: no code is tried there. */
    advance(3000);
    tick(o, s);
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && codes_now(s, out, 4) == 0);
    stop_tag_session(o, s);
    /* An analog-only range. */
    one = tag_config(config, 95, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    dsd_signal_search_note_p25p2_code(s, test_code(5), DSD_P25P2_CODE_P2_NETWORK);
    assert(codes_now(s, out, 4) == 0);
    stop_tag_session(o, s);
    /* No search. */
    dsd_signal_search_note_p25p2_code(s, test_code(6), DSD_P25P2_CODE_P2_NETWORK);
    assert(codes_now(s, out, 4) == 0);
}

/* Issue #804: the start profile (F5), the session's P25 site (F6) and its code list. */
/* The frame sync's signal_search_probe answer (issue #804): 1 only while a visit to an Auto range probes, from the
   acquisition reset its tune runs (which the hunt keeps) to the evidence that ends the probe (the hunt asks again when
   its dwell is spent); 0 for a span tune, a typed or analog range, the probe's analog fallback, a listening, held-off,
   ended, paused or stopped visit. */
static void
test_probe_hook_answer(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    assert(dsd_engine_signal_search_probe(o, s) == 0); // no search
    dsd_signal_search_config one = tag_config(config, 90, 1, 0);
    start_tag_session(o, s, &one);
    assert(status.phase == DSD_SEARCH_TUNING && dsd_engine_signal_search_probe(o, s) == 0); // a span tune
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING && dsd_engine_signal_search_probe(o, s) == 0);
    ready = 1;
    tick(o, s);
    /* The visit's tune: its acquisition reset already sees the probe, so the hunt starts the visit in the search order. */
    assert(status.phase == DSD_SEARCH_TUNING && reset_probe_answer == 1 && dsd_engine_signal_search_probe(o, s) == 1);
    land(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && dsd_engine_signal_search_probe(o, s) == 1);
    /* Evidence the search refuses ends nothing. */
    s->input_interrupted = 1;
    dsd_signal_search_note_evidence(s, "DMR", 1, 2, NULL);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && dsd_engine_signal_search_probe(o, s) == 1);
    s->input_interrupted = 0;
    /* Admitted evidence ends the probe: the hunt's next question gets 0. The engine writes nothing of the hunt's. */
    s->sps_hunt_search_order = 1;
    dsd_signal_search_note_evidence(s, "DMR", 1, 2, NULL);
    assert(status.phase == DSD_SEARCH_LISTENING && dsd_engine_signal_search_probe(o, s) == 0);
    assert(s->sps_hunt_search_order == 1);
    s->sps_hunt_search_order = 0;
    /* Left at Skip: the span tune after it answers 0. */
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(status.hit.ended && reset_probe_answer == 0 && dsd_engine_signal_search_probe(o, s) == 0);
    /* A probe that finds nothing: its analog fallback answers 0 from that tune's reset. */
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && dsd_engine_signal_search_probe(o, s) == 1);
    advance(3000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && !status.hit.ended);
    assert(reset_probe_answer == 0 && dsd_engine_signal_search_probe(o, s) == 0);
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && dsd_engine_signal_search_probe(o, s) == 0);
    /* Paused mid-probe: the pause's reset answers 0. */
    next_visit(o, s);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && dsd_engine_signal_search_probe(o, s) == 1);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && reset_probe_answer == 0 && dsd_engine_signal_search_probe(o, s) == 0);
    stop_tag_session(o, s);
    assert(reset_probe_answer == 0 && dsd_engine_signal_search_probe(o, s) == 0);

    /* Stopped mid-probe: no answer of 1 from the Stop on. */
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(dsd_engine_signal_search_probe(o, s) == 1);
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(dsd_engine_signal_search_probe(o, s) == 0);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && dsd_engine_signal_search_probe(o, s) == 0);

    /* A typed range (P25) and an analog range never probe in the search order. */
    for (int digital = 0; digital <= 2; digital += 2) {
        one = tag_config(config, 91 + (uint64_t)digital, digital, 0);
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        assert(status.hit.hit_id != 0 && !status.hit.ended);
        assert(reset_probe_answer == 0 && dsd_engine_signal_search_probe(o, s) == 0);
        stop_tag_session(o, s);
    }
}

static void
test_p25_site_and_codes(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    test_auto_visit_starts_on_4800_c4fm(o, s, config);
    test_auto_visit_timed_for_its_landing(o, s, config);
    test_typed_visit_timed_for_its_landing(o, s, config);
    test_locked_tuple_kept(o, s, config);
    test_locked_tuple_timed_for_its_landing(o, s, config);
    test_code_does_not_leak_between_visits(o, s, config);
    test_iden_before_nsb_restored(o, s, config);
    test_pending_announcements_stay_with_their_site(o, s, config);
    test_hardset_restored(o, s, config);
    test_unknown_identity_restored(o, s, config);
    test_verified_seed_at_start_not_restored_as_proof(o, s, config);
    test_shutdown_during_active_call(o, s, config);
    test_departure_mid_recovery_drops_pending(o, s, config);
    test_code_list_bound_dedupe_order(o, s, config);
    test_code_list_pause_resume_and_new_start(o, s, config);
    test_code_list_only_while_receiving(o, s, config);
    test_probe_hook_answer(o, s, config);
}

/* The runtime session list as the engine last published it: its length, with its session in @p session. */
static uint32_t avoided[DSD_SEARCH_MAX_AVOIDS];

static size_t
session_avoids(uint64_t* session) {
    return dsd_signal_search_avoids_session_get(session, avoided, DSD_SEARCH_MAX_AVOIDS);
}

/* The store's permanent avoids, written as the app-control store writes them; a 0 is no frequency. */
static void
set_permanent(uint32_t first, uint32_t second) {
    const uint32_t list[2] = {first, second};
    dsd_signal_search_avoids_permanent_set(list, 2);
}

/* A by-frequency action on the running session. */
static int
control_frequency(dsd_opts* o, dsd_state* s, int action, uint32_t frequency) {
    const dsd_signal_search_control a = {status.session_id, 0, action, frequency};
    return dsd_engine_signal_search_control(o, s, &a);
}

/* The store's permanent avoids and this session's are two lists of 1,024 each (issue #828): a full permanent list
   leaves the session all its room, and an avoid of a permanent frequency needs none. */
static void
test_avoid_lists_room(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    static uint32_t permanent[DSD_SEARCH_MAX_AVOIDS];
    for (uint32_t i = 0; i < DSD_SEARCH_MAX_AVOIDS; ++i) {
        permanent[i] = 200000000U + i * 12500U;
    }
    dsd_signal_search_avoids_permanent_set(permanent, DSD_SEARCH_MAX_AVOIDS);
    const dsd_signal_search_config one = tag_config(config, 97, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.frequency_hz == 100025000);
    uint32_t frequency = 300000000;
    for (uint32_t i = 0; i < DSD_SEARCH_MAX_AVOIDS; ++i, frequency += 12500) {
        assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, frequency) == 0);
    }
    assert(status.message[0] == '\0');
    uint64_t session = 0;
    assert(session_avoids(&session) == DSD_SEARCH_MAX_AVOIDS && session == 97 && avoided[0] == 300000000);
    // The 1,025th session avoid has no room, and says so.
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, frequency) < 0);
    assert(strcmp(status.message, "The avoid list is full") == 0);
    // A permanent frequency takes no room: accepted, and nothing is added.
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, permanent[DSD_SEARCH_MAX_AVOIDS - 1]) == 0);
    assert(session_avoids(&session) == DSD_SEARCH_MAX_AVOIDS && status.message[0] == '\0');
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && !status.hit.ended);
    stop_tag_session(o, s);
    dsd_signal_search_avoids_permanent_set(NULL, 0);
}

/* A frequency is in at most one list (issue #828): an avoid of a permanent frequency adds nothing to the session list
   and drops a session copy, Stop avoiding (UNAVOID) never lifts a permanent avoid, and the store's changes reach the
   running search at its next action or tick: an added one is excluded, and leaves the visit it names; a removed one is
   measured again. */
static void
test_session_avoid_of_permanent(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 98, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.frequency_hz == 100025000);
    uint64_t session = 0;
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 100012500) == 0);
    assert(session_avoids(&session) == 1 && session == 98 && avoided[0] == 100012500);
    // Made permanent: the next action finds it so, and the session copy goes.
    set_permanent(100012500, 0);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 100012500) == 0);
    assert(session_avoids(&session) == 0 && session == 98);
    // Stop avoiding lifts only a session avoid: the permanent one stays excluded (below).
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_UNAVOID, 100012500) == 0);
    // The hit being visited, already permanent: Avoid permanently leaves the visit and adds nothing.
    set_permanent(100012500, 100025000);
    act(o, s, DSD_SEARCH_ACTION_AVOID_PERMANENT);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && session_avoids(&session) == 0);
    // Both permanent frequencies are excluded: only 100.000 MHz is visited, this pass and the next.
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.frequency_hz == 100000000);
    land(o, s);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    land(o, s);
    ready = 1;
    tick(o, s);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.pass == 2);
    visit_strongest(o, s);
    assert(status.hit.frequency_hz == 100000000);
    // A permanent avoid removed mid-search is measured again.
    set_permanent(100025000, 0);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    land(o, s);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.frequency_hz == 100012500);
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    // One added for the visit, with no action: the next tick leaves it.
    set_permanent(100025000, 100012500);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && session_avoids(&session) == 0);
    land(o, s);
    stop_tag_session(o, s);
    dsd_signal_search_avoids_permanent_set(NULL, 0);
}

/* Avoid this session on a visited hit (AVOID) or by frequency (AVOID_FREQUENCY), when the store has just made that
   frequency permanent: the action succeeds, adds nothing, the session copy of another frequency made permanent with it
   goes, and the visit is left (issue #828). */
static void
test_hit_avoids_of_permanent(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 46, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.frequency_hz == 100025000);
    uint64_t session = 0;
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 100012500) == 0);
    assert(session_avoids(&session) == 1 && session == 46 && avoided[0] == 100012500);
    // AVOID on the hit, both frequencies now permanent.
    set_permanent(100025000, 100012500);
    const dsd_signal_search_control avoid_hit = {46, status.hit.hit_id, DSD_SEARCH_ACTION_AVOID, 0};
    assert(dsd_engine_signal_search_control(o, s, &avoid_hit) == 0);
    assert(session_avoids(&session) == 0 && session == 46 && status.message[0] == '\0');
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    // The next detection is the one frequency in neither list.
    land(o, s);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.frequency_hz == 100000000);
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    // AVOID_FREQUENCY naming the visited frequency, made permanent with a session avoid of another one.
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 200000000) == 0);
    assert(session_avoids(&session) == 1 && avoided[0] == 200000000);
    const uint32_t all[] = {100025000, 100012500, 100000000, 200000000};
    dsd_signal_search_avoids_permanent_set(all, 4);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 100000000) == 0);
    assert(session_avoids(&session) == 0 && session == 46 && status.message[0] == '\0');
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    land(o, s);
    stop_tag_session(o, s);
    dsd_signal_search_avoids_permanent_set(NULL, 0);
}

/* A permanent avoid made for the hit being visited leaves it at once (issue #828), held and decoded without a break
   too: the in-frame yield check finds the store's change with no command queued and no main-loop tick, and the tick it
   returns to leaves the visit. A change that does not name the visit cuts no frame. */
static void
test_permanent_leaves_held_visit(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 99, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_HELD);
    carrier = 1;
    assert(!dsd_runtime_controls_pending() && !dsd_engine_signal_search_should_yield(o, s));
    set_permanent(200000000, 0);
    assert(!dsd_engine_signal_search_should_yield(o, s));
    const uint64_t hit = status.hit.hit_id;
    set_permanent(200000000, status.hit.frequency_hz);
    assert(dsd_engine_signal_search_should_yield(o, s));
    // The yield check only asks for the tick: the hit is retired there.
    assert(status.phase == DSD_SEARCH_HELD && status.hit.hit_id == hit && !status.hit.ended);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id == hit && status.hit.ended);
    carrier = 0;
    land(o, s);
    stop_tag_session(o, s);
    dsd_signal_search_avoids_permanent_set(NULL, 0);
}

/* Paused, the search still takes the store's changes, and a Resume with edited ranges keeps both lists (issue #828). */
static void
test_paused_search_keeps_both_lists(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    dsd_signal_search_config one = tag_config(config, 77, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 100000000) == 0);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 200000000) == 0);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED);
    set_permanent(100012500, 200000000);
    tick(o, s);
    uint64_t session = 0;
    assert(session_avoids(&session) == 1 && session == 77 && avoided[0] == 100000000);
    DSD_SNPRINTF(one.ranges[0].name, sizeof(one.ranges[0].name), "%s", "Edited");
    char error[192];
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    assert(status.phase == DSD_SEARCH_SEARCHING && strcmp(status.range_name, "Edited") == 0);
    assert(session_avoids(&session) == 1 && session == 77 && avoided[0] == 100000000);
    // The session avoid and the permanent one stay excluded: only 100.025 MHz is visited, this pass and the next.
    tick(o, s);
    visit_strongest(o, s);
    assert(status.hit.frequency_hz == 100025000);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    land(o, s);
    ready = 1;
    tick(o, s);
    const uint64_t pass = status.pass;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.pass == pass + 1);
    visit_strongest(o, s);
    assert(status.hit.frequency_hz == 100025000);
    stop_tag_session(o, s);
    dsd_signal_search_avoids_permanent_set(NULL, 0);
}

/* The session list is the running session's alone (issue #828): a search that fails or stops clears the runtime list
   and publishes no session avoid after that; a failed search refuses session avoids and keeps its error. */
static void
test_session_list_ends_with_session(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 79, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 200000000) == 0);
    uint64_t session = 0;
    assert(session_avoids(&session) == 1 && session == 79);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    failed = 1;
    land(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && status.message[0] != '\0');
    assert(session_avoids(&session) == 0 && session == 0);
    char search_error_text[sizeof(status.message)];
    DSD_SNPRINTF(search_error_text, sizeof(search_error_text), "%s", status.message);
    const uint64_t revision = dsd_signal_search_avoids_session_revision();
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 300000000) < 0);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_UNAVOID, 200000000) < 0);
    set_permanent(200000000, 0);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && strcmp(status.message, search_error_text) == 0);
    assert(dsd_signal_search_avoids_session_revision() == revision && session_avoids(&session) == 0);
    failed = 0;
    stop_tag_session(o, s);
    assert(dsd_signal_search_avoids_session_revision() == revision);
    dsd_signal_search_avoids_permanent_set(NULL, 0);

    const dsd_signal_search_config two = tag_config(config, 82, 0, 0);
    start_tag_session(o, s, &two);
    visit_strongest(o, s);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 200000000) == 0);
    assert(session_avoids(&session) == 1 && session == 82);
    stop_tag_session(o, s);
    assert(session_avoids(&session) == 0 && session == 0);
}

/* Taps interleaved with the store's changes (issue #828): each action is judged against the lists as they stand when
   the decoder runs it. A session avoid run after a permanent avoid was made and removed again adds the frequency to the
   session list; one run while it was permanent adds nothing, and the removal then leaves it in neither list, so it is
   measured again. */
static void
test_avoid_interleaving(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 44, 0, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(status.hit.frequency_hz == 100025000);
    uint64_t session = 0;
    // Made permanent and removed again before the decoder runs the session avoid: it is a session avoid.
    set_permanent(100012500, 0);
    set_permanent(0, 0);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 100012500) == 0);
    assert(session_avoids(&session) == 1 && avoided[0] == 100012500);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_UNAVOID, 100012500) == 0);
    assert(session_avoids(&session) == 0);
    // Made permanent, the session avoid run, then removed: in neither list.
    set_permanent(100012500, 0);
    assert(control_frequency(o, s, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 100012500) == 0);
    assert(session_avoids(&session) == 0);
    set_permanent(0, 0);
    tick(o, s);
    assert(session_avoids(&session) == 0);
    // So the next measurement visits it (100.025 MHz was visited this pass).
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    land(o, s);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.frequency_hz == 100012500);
    land(o, s);
    stop_tag_session(o, s);
}

static void
test_avoid_lists(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    test_avoid_lists_room(o, s, config);
    test_session_avoid_of_permanent(o, s, config);
    test_hit_avoids_of_permanent(o, s, config);
    test_permanent_leaves_held_visit(o, s, config);
    test_paused_search_keeps_both_lists(o, s, config);
    test_session_list_ends_with_session(o, s, config);
    test_avoid_interleaving(o, s, config);
}

int
main(void) {
    dsd_opts* o = calloc(1, sizeof(*o));
    dsd_state* s = calloc(1, sizeof(*s));
    dsd_telemetry_hooks_set((dsd_telemetry_hooks){count_snapshot, NULL, NULL});
    assert(o && s);
    // Controls must reject absent sessions before validating their action payload.
    dsd_signal_search_control absent = {42, 0, DSD_SEARCH_ACTION_STOP, 0};
    assert(dsd_engine_signal_search_control(o, NULL, &absent) < 0);
    assert(dsd_engine_signal_search_control(o, s, &absent) < 0);
    assert(dsd_engine_signal_search_control(o, s, NULL) < 0);
    assert(!dsd_engine_signal_search_active(s) && !tuner_owner && !cancelled);
    o->audio_in_type = AUDIO_IN_RTL;
    o->rtlsdr_center_freq = 100000000;
    o->frame_dstar = 1;
    o->rtl_dsp_bw_khz = 48;
    s->samplesPerSymbol = 10;
    s->rtl_ctx = (void*)1;
    DSD_SNPRINTF(o->audio_in_dev, sizeof(o->audio_in_dev), "%s", "rtl:0:100M");
    dsd_decode_clock_use_test(now_ms * 1000000);
    dsd_signal_search_config config = {0};
    config.session_id = 42;
    config.range_count = 1;
    dsd_signal_search_range_default(&config.ranges[0]);
    // A new range probes for three seconds: Auto reaches P25 Phase 2 and confirms it inside that (issue #804).
    assert(config.ranges[0].probe_ms == 3000 && config.ranges[0].digital == 1);
    config.ranges[0].start_hz = 100000000;
    config.ranges[0].end_hz = 100025000;
    char error[192];
    assert(dsd_engine_signal_search_start(o, s, &config, observer, NULL, error, sizeof(error)) == 0);
    assert(bypassed && dsd_signal_search_is_running(s) && status.owned);
    assert(tuner_owner == 42);
    assert(dsd_engine_signal_search_control(o, s, NULL) < 0);
    defer = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && !tune_pending);
    tick(o, s);
    assert(tune_pending);
    buffered = 1;
    applied();
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING);
    buffered = 0;
    capture.generation = generation;
    capture.capture_hz = target + 384000;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING && !autogain);
    ready = 3;
    tick(o, s);
    assert(status.hit.hit_id == 0);
    tick(o, s);
    ready = 1;
    tick(o, s);
    assert(status.hit.frequency_hz == 100025000 && !status.hit.confirmed && bypassed);
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_TUNING && bypassed);
    land(o, s);
    assert(status.phase == DSD_SEARCH_HELD && !bypassed && status.identifying);
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    assert(status.phase == DSD_SEARCH_IDENTIFYING && !bypassed);
    // A sync without protocol evidence cannot claim any digital identity.
    s->synctype = 12;
    s->nac = 99;
    advance(2999);
    tick(o, s);
    assert(!status.hit.confirmed && status.hit.protocol[0] == '\0');
    advance(1);
    assert(dsd_engine_signal_search_should_yield(o, s));
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && family == 1);
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.hit.unresolved);
    carrier = 1;
    advance(100);
    tick(o, s);
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    advance(40000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_HELD);
    dsd_signal_search_control stale = {42, status.hit.hit_id + 1, DSD_SEARCH_ACTION_SKIP, 0};
    assert(dsd_engine_signal_search_control(o, s, &stale) < 0);
    act(o, s, DSD_SEARCH_ACTION_AVOID);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    land(o, s);
    ready = 1;
    tick(o, s);
    assert(status.hit.frequency_hz == 100012500 && !status.hit.confirmed && s->nac == 0);
    land(o, s);
    // Evidence a decoder reports while an input interruption stands comes from a frame the interruption cut, whose
    // rest is erasures getSymbol() stood in: it confirms nothing and names nothing.
    s->input_interrupted = 1;
    dsd_signal_search_note_evidence(s, "DMR", 123, 456, "CC 7");
    assert(!status.hit.confirmed && status.hit.protocol[0] == '\0' && status.hit.target == 0);
    s->input_interrupted = 0;
    dsd_signal_search_note_evidence(s, "DMR", 123, 456, "CC 7");
    assert(status.hit.confirmed && status.hit.target == 123 && status.phase == DSD_SEARCH_LISTENING);
    s->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    dsd_signal_search_note_evidence(s, "NXDN", 0, 0, NULL);
    assert(strcmp(status.hit.protocol, "NXDN48") == 0);
    assert(status.hit.target == 0 && status.hit.source == 0 && status.hit.access[0] == '\0');
    s->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    dsd_signal_search_note_evidence(s, "NXDN", 0, 0, NULL);
    assert(strcmp(status.hit.protocol, "NXDN96") == 0);
    carrier = 0;
    advance(499);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING);
    advance(1);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING);
    // Pause while the retune is pending; restart keeps its ticket and native avoids.
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED);
    config.ranges[0].end_hz = 100025000;
    config.ranges[0].digital = 0;
    assert(dsd_engine_signal_search_start(o, s, &config, observer, NULL, error, sizeof(error)) == 0);
    uint32_t pending_ticket = ticket;
    tick(o, s);
    assert(ticket == pending_ticket);
    land(o, s);
    assert(ticket != pending_ticket);
    land(o, s);
    ready = 1;
    tick(o, s);
    assert(status.hit.frequency_hz == 100012500); // 100025000 still avoided
    land(o, s);
    advance(30000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    act(o, s, DSD_SEARCH_ACTION_STOP);
    defer = 1;
    land(o, s);
    assert(dsd_engine_signal_search_active(s));
    tick(o, s);
    failed = 1;
    land(o, s);
    assert(status.phase == DSD_SEARCH_ERROR);
    failed = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && !bypassed && !dsd_signal_search_is_running(s) && !status.owned);
    assert(!tuner_owner);
    assert(dsd_engine_signal_search_control(o, s, &absent) < 0);
    assert(o->frame_dstar && o->rtlsdr_center_freq == 100000000 && gain_value == 123 && autogain);
    // A new session releases temporary avoids, and failed tunes never begin measurement.
    config.session_id = 43;
    assert(dsd_engine_signal_search_start(o, s, &config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    failed = 1;
    land(o, s);
    assert(status.phase == DSD_SEARCH_ERROR);
    live = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED);
    assert(boundary > 0 && cancelled > 0);
    test_listening_verdict_and_live_strength(o, s, &config);
    test_teardown_restores_frequency(o, s, &config);
    test_stop_restore_is_a_carrier_boundary(o, s, &config);
    test_pause_is_a_carrier_boundary(o, s, &config);
    test_error_is_a_carrier_boundary(o, s, &config);
    test_auto_baseline_dispatches_dmr(o, s, &config);
    test_restore_retune_carries_family(o, s, &config);
    test_capture_freshness(o, s, &config);
    test_status_names_running_range(o, s, &config);
    test_avoid_by_frequency(o, s, &config);
    test_site_record_binding();
    test_site_notes_on_detection(o, s, &config);
    test_call_tuple_on_detection(o, s, &config);
    test_access_note_on_detection(o, s, &config);
    test_visit_tag_set_on_landing(o, s, &config);
    test_visit_tag_cleared_after_departure_boundary(o, s, &config);
    test_visit_tag_probe_fallback_same_hit(o, s, &config);
    test_visit_tag_pause_and_error(o, s, &config);
    test_visit_tag_resume_waits_for_landing(o, s, &config);
    test_visit_tag_stop_restore(o, s, &config);
    test_visit_tag_stop_with_input_lost(o, s, &config);
    test_stop_then_input_lost_one_boundary(o, s, &config);
    test_visit_tag_hold_keeps(o, s, &config);
    test_session_end_keeps_tag_until_final_close(o, s, &config);
    test_visit_tag_cleared_at_search_start(o, s, &config);
    test_refused_probe_leaves(o, s, &config);
    test_refused_probe_verdict_order(o, s, &config);
    test_refused_probe_mixed(o, s, &config);
    test_refused_probe_nxdn_and_codeless(o, s, &config);
    test_refused_probe_held(o, s, &config);
    test_refused_probe_max_visit(o, s, &config);
    test_refused_probe_actions(o, s, &config);
    test_rejected_code_site(o, s, &config);
    test_rejected_code_p25p2_slot(o, s, &config);
    test_p25_site_and_codes(o, s, &config);
    test_avoid_lists(o, s, &config);
    s->rtl_ctx = NULL;
    dsd_state_ext_free_all(s);
    free(s);
    free(o);
    dsd_decode_clock_use_system();
    return 0;
}
