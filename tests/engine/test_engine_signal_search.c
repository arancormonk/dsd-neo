// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
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
static uint64_t tuner_owner;
/* How long ago the radio input delivered its latest samples. */
static uint64_t stalled_ms;
/* State snapshots published to frontends, and the carrier boundaries run when the latest was. */
static int snapshots, snapshot_boundary;

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

static void
observer(const dsd_signal_search_status* s, const dsd_signal_search_hit* h, void* ctx) {
    (void)h;
    (void)ctx;
    status = *s;
}

static void
advance(unsigned ms) {
    now_ms += ms;
    dsd_decode_clock_test_set_ns(now_ms * 1000000);
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
    assert(owner >= 42 && owner <= 52);
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
    return dsd_realtime_mono_ms() - stalled_ms;
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
dsd_audio_ensure_digital_output(dsd_opts* opts) {
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
    ready = 2; // measured, below the detection threshold: strength is still reported
    advance(100);
    tick(o, s);
    assert(status.live_power_valid && status.live_power_dbfs > -40.5f && status.live_power_dbfs < -39.5f);
    assert(status.hit.power_dbfs > -30.5f && status.hit.power_dbfs < -29.5f); // detection-time power is kept apart

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
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    visit_strongest(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && o->rtlsdr_center_freq == 100025000);
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
    s->rtl_ctx = NULL;
    dsd_state_ext_free_all(s);
    free(s);
    free(o);
    dsd_decode_clock_use_system();
    return 0;
}
