// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
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
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
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
    assert(owner >= 42 && owner <= 62);
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

void
dsd_engine_carrier_boundary(dsd_opts* opts, dsd_state* state, dsd_carrier_boundary_kind reason, int guard) {
    (void)opts;
    (void)reason;
    (void)guard;
    ++boundary;
    state->dmr_color_code = 16;
    state->nac = 0;
}

void
dsd_frame_sync_reset_acquisition(const dsd_opts* opts, dsd_state* state, int clear) {
    (void)opts;
    (void)state;
    (void)clear;
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
    config->avoid_count = 0;
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
    two.avoid_count = 0;
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
    one.avoid_count = 0;
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
    // A failed search keeps its error: a full list refuses without replacing it.
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    failed = 1;
    land(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && status.message[0] != '\0');
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
    one.avoid_count = 0;
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
    one.avoid_count = 0;
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
    advance(1999);
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
    s->rtl_ctx = NULL;
    dsd_state_ext_free_all(s);
    free(s);
    free(o);
    dsd_decode_clock_use_system();
    return 0;
}
