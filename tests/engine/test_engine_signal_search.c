// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/manual_tune.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/protocol/dmr/dmr_t3_grant.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_avoids.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/app_control/command_dispatch.h"
#include "../../src/app_control/commands_internal.h"
#include "../../src/engine/scan_owner.h"
#include "../../src/protocol/dmr/dmr_t3_channels_internal.h"
#include "test_env.h"

static dsd_signal_search_status status;
static dsd_signal_capture capture = {.rate_hz = 1536000,
                                     .capture_hz = 100384000,
                                     .generation = 1,
                                     .rotation_hz = 384000,
                                     .capture_offset_hz = 384000,
                                     .clip_level = DSD_SIGNAL_CLIP_LEVEL_DEFAULT};
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

/* The reason the next span or live request is refused for, or DSD_SIGNAL_REQUEST_OK to take it. */
static dsd_signal_request_failure request_failure = DSD_SIGNAL_REQUEST_OK;

int
rtl_signal_measure_request(const dsd_signal_measure_request* r, dsd_signal_request_failure* out_failure) {
    if (out_failure) {
        *out_failure = request_failure;
    }
    if (request_failure != DSD_SIGNAL_REQUEST_OK) {
        return -1;
    }
    request = *r;
    return 0;
}

/* The reason of the failure answer `ready` -1 (the outstanding request's) or -3 (an earlier request's) gives. */
static dsd_signal_measure_failure fail_reason = DSD_SIGNAL_MEASURE_CLIPPED;

/* The next answer: 1 a report detecting every candidate, 2 one detecting none, 3 a stale report, -1 or -3 a failure
   carrying its request's ids and generation, no candidates and fail_reason, as the tap's. */
int
rtl_signal_measure_get(dsd_signal_measure_report* out) {
    if (!ready) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (ready < 0) {
        out->session_id = request.session_id;
        out->request_id = request.request_id - (ready == -3 ? 1U : 0U);
        out->generation = request.capture.generation;
        out->failure = fail_reason;
        ready = 0;
        return -1;
    }
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

/* The retunes marked a new channel (issue #899): the receiver starts closed where one lands on the analog family. */
static int g_retune_marks;

void
rtl_stream_prepare_retune_new_channel_for_target(uint32_t target_freq_hz, int new_channel) {
    (void)target_freq_hz;
    if (new_channel) {
        g_retune_marks++;
    }
}

/* The engine's side of the receiver's gate (issue #899; channel_scan.c, not linked here): how deep the holds are, so a
   case sees that a family request or retune went out while the decoder wrote no gate. */
static int g_gate_hold_depth;

void
dsd_engine_receiver_gate_hold(int hold) {
    g_gate_hold_depth += hold ? 1 : -1;
    assert(g_gate_hold_depth >= 0);
}

void
dsd_engine_receiver_gate_due(void) {}

int
rtl_stream_airspy_controls(const dsd_airspy_config* cfg) {
    (void)cfg;
    return 0;
}

/* Issue #899: whether the last live family request went out under the decoder's gate hold. */
static int family_in_hold;

int
rtl_stream_request_analog_profile(int f, int kind, int width) {
    (void)kind;
    (void)width;
    family = f;
    family_in_hold = g_gate_hold_depth > 0;
    return 0;
}

int
rtl_stream_prepare_retune_analog_profile_for_target(uint32_t hz, const rtl_stream_retune_analog_profile* p) {
    family = p->family;
    retune_family = *p;
    retune_family_hz = hz;
    return 0;
}

/* The symbol profile the latest retune queued: its target, CQPSK, symbol rate, levels, channel filter, TED and whether
   the TED is kept as an override. */
static struct {
    uint32_t hz;
    int cq, rate, levels, profile, sps, persist;
} queued_profile;

/* The front end's TED samples-per-symbol override (0: none). */
static int ted_override;

int
rtl_stream_get_ted_sps_override(void) {
    return ted_override;
}

void
rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t hz, int cq, int rate, int levels, int profile, int sps,
                                                       int persist, const rtl_stream_retune_gain_profile* g) {
    queued_profile.hz = hz;
    queued_profile.cq = cq;
    queued_profile.rate = rate;
    queued_profile.levels = levels;
    queued_profile.sps = sps;
    queued_profile.persist = persist;
    queued_profile.profile = profile;
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
/* The kind of the latest carrier boundary: a visit's is a tune, which the production boundary runs without dropping the
   DMR Tier III channels the air taught (ENGINE_STREAM_GAP proves it; discussion #900 F2). */
static dsd_carrier_boundary_kind boundary_kind = DSD_CARRIER_BOUNDARY_STREAM_GAP;

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
    (void)guard;
    ++boundary;
    boundary_kind = reason;
    boundary_tagged = dsd_signal_search_visit_get(&boundary_session, &boundary_hit);
    boundary_code = code_held(state);
    boundary_proven = state->p2_cc_verified;
    state->dmr_color_code = 16;
    state->nac = 0;
    dsd_state_forget_p2_proof(state);
    /* The engine's boundary for every kind but a stream gap: the carrier left's P25 patches go with it. This stub
       does the clear; ENGINE_P25_PATCH_OWNER proves the production boundary's. */
    if (reason != DSD_CARRIER_BOUNDARY_STREAM_GAP) {
        dsd_p25_patch_clear_all(state);
    }
    state->carrier_seq++;
}

/* The TETRA state's cold starts (issue #748): how many, and the carrier boundaries run, the P25 code held and the visit
   tag in force at the latest, which say it ran after the departure's boundary closed the visit and before the session's
   site came back. */
static int tetra_resets, tetra_reset_boundary, tetra_reset_tagged;
static uint64_t tetra_reset_code;

void
tetra_scan_visit_reset(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    ++tetra_resets;
    tetra_reset_boundary = boundary;
    tetra_reset_code = code_held(state);
    tetra_reset_tagged = dsd_signal_search_visit_get(NULL, NULL);
}

/* The DSP rate the stream publishes (0: not yet), and the TED timing it applied for a TETRA profile it publishes. */
static int dsp_rate_hz;
static int tetra_ted_override, tetra_ted_applied;

int
dsd_engine_scan_dsp_rate_hz(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return dsp_rate_hz;
}

void
dsd_engine_scan_tetra_ted_sps(const dsd_opts* opts, const dsd_state* state, int* override_sps, int* applied_sps) {
    (void)opts;
    (void)state;
    if (override_sps) {
        *override_sps = tetra_ted_override;
    }
    if (applied_sps) {
        *applied_sps = tetra_ted_applied;
    }
}

/* Whether the stream settled on another receive profile than TETRA's (trunk_tuning.c, issue #890): what a case sets. */
static int tetra_chain_other;

int
dsd_engine_scan_tetra_chain_settled_other(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return tetra_chain_other;
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

/* Issue #743 (B13): a replay restarted mid-run decodes on the system clock but still cannot retune, and an I/Q stream
   never can. Both are refused at the start, before the search takes the tuner or runs a carrier boundary; the stream
   with its own reason, which names it. */
static void
test_fixed_tuning_refused_before_a_boundary(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    static const char k_live[] = "Signal search needs exclusive use of a live tunable SDR";
    static const char k_stream[] = "Signal search needs a tunable SDR: an I/Q stream cannot retune";

    static const struct {
        const char* dev;
        const char* why;
    } fixed[] = {
        {"iqreplay:capture.iq.json", k_live},
        {"iqtcp:127.0.0.1:5555:240k:cs16", k_stream},
        {"iqudp:0.0.0.0:7356:48k:cu8", k_stream},
    };

    char live_dev[sizeof o->audio_in_dev];
    DSD_SNPRINTF(live_dev, sizeof live_dev, "%s", o->audio_in_dev);
    dsd_decode_clock_use_system();
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        DSD_SNPRINTF(o->audio_in_dev, sizeof(o->audio_in_dev), "%s", fixed[i].dev);
        const int before = boundary;
        char error[192] = {0};
        assert(dsd_engine_signal_search_start_refused(o, s, config, error, sizeof(error)) < 0);
        assert(strcmp(error, fixed[i].why) == 0);
        error[0] = '\0';
        assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) < 0);
        assert(strcmp(error, fixed[i].why) == 0);
        tick(o, s);
        assert(boundary == before && !dsd_signal_search_is_running(s) && !tuner_owner && !tune_pending);
    }
    DSD_SNPRINTF(o->audio_in_dev, sizeof(o->audio_in_dev), "%s", live_dev);
    dsd_decode_clock_use_test(now_ms * 1000000);
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
    /* A scan quiet check made before the search is not the search's (issue #834): the start moves its counter. */
    const dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(s);
    assert(owner);
    const uint64_t quiet_inv = owner->quiet_inv_seq;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    assert(dsd_engine_scan_owner_get(s)->quiet_inv_seq != quiet_inv);
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

/* Issue #899: Stop gives an NFM session with the analog default its gate back before the live family request of the
   restore, so the restored analog channel never runs under the search's gate: the receiver holds Noise +10 when the
   family is asked for, and the options hold the default in force. */
static void
test_restore_hands_back_the_analog_default_first(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    char error[192];
    live = 1;
    o->rtlsdr_center_freq = 100000000;
    o->analog_only = 1;
    o->analog_demod = DSD_ANALOG_DEMOD_FM;
    o->squelch_default_mode = DSD_SQUELCH_MODE_NOISE;
    o->squelch_default_margin_db = 10;
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && o->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED);
    config->session_id = 53;
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
    tick(o, s);
    assert(o->squelch_default_state != DSD_SQUELCH_DEFAULT_APPLIED);
    family_in_hold = 0;
    const int marks_before = g_retune_marks;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s); // the span tune the start queued; the restore follows it
    const dsd_squelch_setting noise10 = dsd_squelch_setting_dynamic(DSD_SQUELCH_MODE_NOISE, 10);
    /* Round 7, "closed start, no IDs": the session comes back on a new channel the restore's retune is marked as, and
       its family request goes out under the decoder's gate hold, so nothing the session resolves to reaches the range's
       channel; the decoder writes the session's gate, the analog default, once the retune and request have landed. */
    assert(target == 100000000 && family == DSD_RX_FAMILY_ANALOG && family_in_hold);
    assert(g_retune_marks == marks_before + 1 && g_gate_hold_depth == 0);
    assert(o->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED);
    const dsd_squelch_setting session_gate = dsd_scan_mode_channel_squelch(o, s);
    assert(dsd_squelch_setting_equal(&session_gate, &noise10) == 1);
    land(o, s); // the restore tune
    assert(status.phase == DSD_SEARCH_STOPPED && o->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED);
    (void)dsd_squelch_analog_default_release(o);
    o->squelch_default_mode = DSD_SQUELCH_MODE_LEVEL;
    o->squelch_default_state = DSD_SQUELCH_DEFAULT_IDLE;
    o->analog_only = 0;
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

/* A span measurement the tap refuses fails the search, saying why: the detector could not be allocated, or the plan is
   one it does not support. */
static void
test_request_failure_messages(dsd_opts* o, dsd_state* s, dsd_signal_search_config* config) {
    static const struct {
        dsd_signal_request_failure failure;
        const char* message;
    } cases[] = {
        {DSD_SIGNAL_REQUEST_NO_MEMORY, "Not enough memory for a measurement"},
        {DSD_SIGNAL_REQUEST_UNSUPPORTED, "Unsupported FFT measurement plan"},
    };

    char error[192];
    live = 1;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        config->session_id = 90 + i;
        assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) == 0);
        tick(o, s);
        assert(status.phase == DSD_SEARCH_TUNING);
        request_failure = cases[i].failure;
        land(o, s);
        tick(o, s);
        assert(status.phase == DSD_SEARCH_ERROR);
        assert(strcmp(status.message, cases[i].message) == 0);
        request_failure = DSD_SIGNAL_REQUEST_OK;
        act(o, s, DSD_SEARCH_ACTION_STOP);
        land(o, s);
        assert(status.phase == DSD_SEARCH_STOPPED);
    }
}

/* Clipping (issue #828): sessions over one or two one-span analog ranges, "Air band" and "Marine", whose span
   measurements the stub answers by hand. The realtime clock drives the deadlines. */
static void
clip_start(dsd_opts* o, dsd_state* s, uint64_t session, uint32_t ranges) {
    dsd_signal_search_config config;
    DSD_MEMSET(&config, 0, sizeof(config));
    config.session_id = session;
    config.range_count = ranges;
    dsd_signal_search_range_default(&config.ranges[0]);
    DSD_SNPRINTF(config.ranges[0].name, sizeof(config.ranges[0].name), "%s", "Air band");
    config.ranges[0].start_hz = 100000000;
    config.ranges[0].end_hz = 100025000;
    config.ranges[0].digital = 0;
    config.ranges[0].max_visit_ms = 0;
    config.ranges[1] = config.ranges[0];
    DSD_SNPRINTF(config.ranges[1].name, sizeof(config.ranges[1].name), "%s", "Marine");
    config.ranges[1].start_hz = 100100000;
    config.ranges[1].end_hz = 100125000;
    live = 1;
    carrier = 0;
    observed = 1;
    char error[192];
    assert(dsd_engine_signal_search_start(o, s, &config, observer, NULL, error, sizeof(error)) == 0);
    assert(!status.overloaded && status.overload_range[0] == '\0');
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING);
    land(o, s); // the first span request goes out
    assert(status.phase == DSD_SEARCH_SEARCHING && status.range_index == 0 && status.channel_index == 0);
}

/* One tick @p ms after the last; the outstanding span request answered with a failure of @p reason. */
static void
fail_after(dsd_opts* o, dsd_state* s, uint64_t ms, dsd_signal_measure_failure reason) {
    realtime_ms += ms;
    fail_reason = reason;
    ready = -1;
    tick(o, s);
}

/* One tick @p ms after the last, the outstanding request unanswered. */
static void
wait_after(dsd_opts* o, dsd_state* s, uint64_t ms) {
    realtime_ms += ms;
    tick(o, s);
}

/* Still measuring range @p range's span. */
static int
measuring_span(uint32_t range) {
    return status.phase == DSD_SEARCH_SEARCHING && status.range_index == range && status.channel_index == 0;
}

/* The span just left with nothing detected: the search is past its channels and the next span is still to plan. */
static int
left_span(void) {
    return status.phase == DSD_SEARCH_SEARCHING && status.channel_index == status.channel_count;
}

/* The next span: its tune is queued, lands, and its first request goes out. */
static void
next_span(dsd_opts* o, dsd_state* s) {
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.channel_index == 0);
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING);
}

/* Skips the span being measured: a clip, and another a second later. */
static void
clip_span_out(dsd_opts* o, dsd_state* s) {
    fail_after(o, s, 0, DSD_SIGNAL_MEASURE_CLIPPED);
    fail_after(o, s, 1000, DSD_SIGNAL_MEASURE_CLIPPED);
    assert(left_span() && status.overloaded);
}

/* Measures the span clean (nothing detected) and leaves it. */
static void
clean_span_out(dsd_opts* o, dsd_state* s) {
    ready = 2;
    tick(o, s);
    assert(left_span());
}

static void
clip_stop(dsd_opts* o, dsd_state* s) {
    ready = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    for (int i = 0; i < 3 && status.phase != DSD_SEARCH_STOPPED; ++i) {
        land(o, s);
    }
    assert(status.phase == DSD_SEARCH_STOPPED);
}

/* A span that keeps clipping for a second is skipped with the overload warning instead of failing the search, and
   measured again next pass; nothing is detected from a clipped measurement. Shorter clipping skips nothing, a complete
   report is used before any deadline is judged, any other failure keeps the clip run going but cannot end it at a
   second, and the warning names the first skipped range of a pass until a pass skips nothing. */
static void
test_clipping_span_skipped(dsd_opts* o, dsd_state* s) {
    clip_start(o, s, 60, 2);
    const uint32_t air_hz = target;
    const int hits = hits_published;

    // Under a second of clipping skips nothing; a complete report a second after the first clip is still used.
    fail_after(o, s, 0, DSD_SIGNAL_MEASURE_CLIPPED);
    fail_after(o, s, 999, DSD_SIGNAL_MEASURE_CLIPPED);
    assert(measuring_span(0) && !status.overloaded);
    realtime_ms += 1;
    clean_span_out(o, s);
    assert(!status.overloaded && status.range_index == 0);

    // Range B: a drop keeps the run going without ending it; the next clip, a second and a half after the first, skips.
    next_span(o, s);
    assert(status.range_index == 1);
    fail_after(o, s, 0, DSD_SIGNAL_MEASURE_CLIPPED);
    fail_after(o, s, 1000, DSD_SIGNAL_MEASURE_DROPPED);
    wait_after(o, s, 400);
    assert(measuring_span(1) && !status.overloaded);
    fail_after(o, s, 100, DSD_SIGNAL_MEASURE_CLIPPED);
    assert(left_span() && status.range_index == 1 && status.pass == 1);
    assert(status.overloaded && strcmp(status.overload_range, "Marine") == 0);
    assert(status.message[0] == '\0' && hits_published == hits && status.hit.hit_id == 0);

    // Pass 2 measures range A again on the same tune. Its first skip renames the warning; a later one does not.
    next_span(o, s);
    assert(status.pass == 2 && status.range_index == 0 && target == air_hz);
    assert(status.overloaded && strcmp(status.overload_range, "Marine") == 0);
    clip_span_out(o, s);
    assert(strcmp(status.overload_range, "Air band") == 0);
    next_span(o, s);
    clip_span_out(o, s);
    assert(status.range_index == 1 && strcmp(status.overload_range, "Air band") == 0);

    // A pass after one that skipped keeps the warning while it runs, clean or not; the end of a clean pass clears it.
    next_span(o, s);
    assert(status.pass == 3 && status.overloaded && strcmp(status.overload_range, "Air band") == 0);
    clean_span_out(o, s);
    next_span(o, s);
    clean_span_out(o, s);
    assert(status.overloaded);
    next_span(o, s);
    assert(status.pass == 4 && !status.overloaded && status.overload_range[0] == '\0');
    assert(hits_published == hits && status.phase != DSD_SEARCH_ERROR);
    clip_stop(o, s);
}

/* The span deadlines that skip, each row on a fresh one-range session: a first clip late in the five seconds skips at
   five seconds, a clip and then no answer a second after the clip, and clipping and then only drops at five seconds. */
static void
test_clipping_deadline_skips(dsd_opts* o, dsd_state* s) {
    // A first clip at 4.6 s: neither deadline at 5 s, a skip just after.
    clip_start(o, s, 61, 1);
    wait_after(o, s, 4600);
    fail_after(o, s, 0, DSD_SIGNAL_MEASURE_CLIPPED);
    wait_after(o, s, 400);
    assert(measuring_span(0) && !status.overloaded);
    wait_after(o, s, 1);
    assert(left_span() && status.overloaded && strcmp(status.overload_range, "Air band") == 0);
    clip_stop(o, s);

    // A clip and then no answer: the clip's second still runs out.
    clip_start(o, s, 62, 1);
    fail_after(o, s, 300, DSD_SIGNAL_MEASURE_CLIPPED);
    wait_after(o, s, 999);
    assert(measuring_span(0) && !status.overloaded);
    wait_after(o, s, 1);
    assert(left_span() && status.overloaded);
    clip_stop(o, s);

    // A clip, then only drops: no second-long clip run, the five seconds skip.
    clip_start(o, s, 63, 1);
    fail_after(o, s, 0, DSD_SIGNAL_MEASURE_CLIPPED);
    for (int i = 0; i < 10; ++i) {
        fail_after(o, s, 500, DSD_SIGNAL_MEASURE_DROPPED);
    }
    assert(measuring_span(0) && !status.overloaded);
    fail_after(o, s, 1, DSD_SIGNAL_MEASURE_DROPPED);
    assert(left_span() && status.overloaded && status.phase != DSD_SEARCH_ERROR);
    clip_stop(o, s);
}

/* Without a clip of its own request, a span tune that completes no measurement in five seconds fails the search, as
   before: drops alone, no answer at all, or clips answering an earlier request. */
static void
test_unclipped_deadline_fails(dsd_opts* o, dsd_state* s) {
    for (int row = 0; row < 3; ++row) {
        clip_start(o, s, 64 + (uint64_t)row, 1);
        for (int i = 0; i < 10; ++i) {
            if (row == 0) {
                fail_after(o, s, 500, DSD_SIGNAL_MEASURE_DROPPED);
            } else if (row == 1) {
                wait_after(o, s, 500);
            } else {
                realtime_ms += 500;
                fail_reason = DSD_SIGNAL_MEASURE_CLIPPED;
                ready = -3;
                tick(o, s);
            }
        }
        assert(measuring_span(0) && !status.overloaded);
        wait_after(o, s, 1);
        assert(status.phase == DSD_SEARCH_ERROR && !status.overloaded);
        assert(strcmp(status.message, "No uninterrupted samples for a measurement") == 0);
        clip_stop(o, s);
    }
}

/* A report that completed is read before any deadline is judged: used, not failed or skipped, when it is read after
   five seconds or at the clip run's second. */
static void
test_completed_report_beats_deadlines(dsd_opts* o, dsd_state* s) {
    clip_start(o, s, 67, 1);
    realtime_ms += 5001;
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id && !status.hit.ended && !status.overloaded);
    clip_stop(o, s);
    clip_start(o, s, 68, 1);
    fail_after(o, s, 0, DSD_SIGNAL_MEASURE_CLIPPED);
    realtime_ms += 1000;
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id && !status.overloaded);
    clip_stop(o, s);
}

/* One span, or every span, clipping for good keeps searching: each skip wraps or moves on, the pass climbs, the
   warning stays and names the pass's first skipped range, and nothing is detected. */
static void
test_clipping_keeps_searching(dsd_opts* o, dsd_state* s) {
    clip_start(o, s, 69, 1);
    const uint32_t tune_hz = target;
    const int hits = hits_published;
    for (uint64_t pass = 1; pass <= 5; ++pass) {
        assert(status.pass == pass && target == tune_hz);
        clip_span_out(o, s);
        assert(strcmp(status.overload_range, "Air band") == 0);
        next_span(o, s);
    }
    assert(status.pass == 6 && status.overloaded && hits_published == hits);
    clip_stop(o, s);

    clip_start(o, s, 70, 2);
    for (uint64_t pass = 1; pass <= 3; ++pass) {
        assert(status.pass == pass && status.range_index == 0);
        clip_span_out(o, s);
        next_span(o, s);
        assert(status.range_index == 1);
        clip_span_out(o, s);
        assert(strcmp(status.overload_range, "Air band") == 0);
        next_span(o, s);
    }
    assert(status.pass == 4 && status.overloaded && hits_published == hits && status.phase != DSD_SEARCH_ERROR);
    clip_stop(o, s);
}

/* The warning stays through Pause and the visits and actions that clear a refusal, and goes at Resume and at a paused
   restart with edited ranges, so a gain lowered there shows its effect; a span that still clips warns again. */
static void
test_overload_clears(dsd_opts* o, dsd_state* s) {
    // A detection visited after the skip, and a Skip taken on it, leave the warning.
    clip_start(o, s, 71, 2);
    clip_span_out(o, s);
    next_span(o, s);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.hit_id && status.overloaded);
    land(o, s);
    assert(status.phase == DSD_SEARCH_LISTENING && status.overloaded);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(status.hit.ended && status.overloaded && strcmp(status.overload_range, "Air band") == 0);

    // Paused, it still shows; Resume clears it, and the span that still clips sets it again with its name.
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && status.overloaded);
    act(o, s, DSD_SEARCH_ACTION_RESUME);
    assert(!status.overloaded && status.overload_range[0] == '\0');
    land(o, s); // the Skip's return tune completes; the span is planned again
    assert(status.phase == DSD_SEARCH_TUNING);
    land(o, s);
    assert(measuring_span(1) && !status.overloaded);
    clip_span_out(o, s);
    assert(strcmp(status.overload_range, "Marine") == 0);
    clip_stop(o, s);

    // A paused restart with edited ranges clears it.
    clip_start(o, s, 72, 1);
    clip_span_out(o, s);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.overloaded);
    dsd_signal_search_config edited;
    DSD_MEMSET(&edited, 0, sizeof(edited));
    edited.session_id = 72;
    edited.range_count = 1;
    dsd_signal_search_range_default(&edited.ranges[0]);
    DSD_SNPRINTF(edited.ranges[0].name, sizeof(edited.ranges[0].name), "%s", "Air band");
    edited.ranges[0].start_hz = 100000000;
    edited.ranges[0].end_hz = 100025000;
    edited.ranges[0].digital = 0;
    edited.ranges[0].gain_set = 1;
    edited.ranges[0].gain_tenth_db = 100;
    char error[192];
    assert(dsd_engine_signal_search_start(o, s, &edited, observer, NULL, error, sizeof(error)) == 0);
    assert(status.phase == DSD_SEARCH_SEARCHING && !status.overloaded && status.overload_range[0] == '\0');
    clip_stop(o, s);
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
    /* Issue #899, round 8: the restore tune went with the input; the stopped search, which nothing polls any more,
       holds no tune the decoder's gate waits for, so a radio reopened in this session gets its gate. */
    assert(!dsd_engine_signal_search_tune_pending(s));
    live = 1;
    assert(!dsd_engine_signal_search_tune_pending(s));
    tick(o, s);
    assert(!dsd_engine_signal_search_tune_pending(s));

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

    /* The input goes while a visit's tune is outstanding: the search fails, and the tune went with the input. Once the
       radio is back, nothing waits on that tune, and a Stop restores at once. */
    one = tag_config(config, 76, 0, 0);
    start_tag_session(o, s, &one);
    land(o, s);
    ready = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && dsd_engine_signal_search_tune_pending(s));
    live = 0;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && !dsd_engine_signal_search_tune_pending(s));
    live = 1;
    tune_pending = 0;
    tick(o, s);
    assert(!dsd_engine_signal_search_tune_pending(s));
    const uint32_t tickets_before = ticket;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(ticket == tickets_before + 1 && target == 100000000U);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && tag_clear() && !dsd_engine_signal_search_tune_pending(s));
}

/* Issue #899: a visit's tune the receiver deferred holds no ticket, and the search retries it from
   the TUNING phase. Until that retry lands, the tune is outstanding for the decoder's gate: the range's squelch, staged
   at the queue, is not written onto the channel being left, so the write stays due for the landing. Before the fix
   only a ticket counted, and the deferral let the gate write the range's squelch there and clear the write due. */
static void
test_deferred_visit_tune_stays_pending(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 77, 1, 0);
    start_tag_session(o, s, &one);
    land(o, s);
    assert(status.phase == DSD_SEARCH_SEARCHING && !dsd_engine_signal_search_tune_pending(s));
    ready = 1;
    defer = 1;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && !tune_pending && !status.hit.ended);
    assert(dsd_engine_signal_search_tune_pending(s));
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && tune_pending && dsd_engine_signal_search_tune_pending(s));
    land(o, s);
    assert(status.phase != DSD_SEARCH_TUNING && !dsd_engine_signal_search_tune_pending(s));
    stop_tag_session(o, s);
    assert(!dsd_engine_signal_search_tune_pending(s));
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

/* Issue #748 B16: Stop tunes back with the radio profile the restored decoder runs, the session's TED override
   included. A -m2 session (6000/4 CQPSK, the P25 Phase 2 profile pinned) used to queue the Auto preset's 4800/4 for
   the radio while its decoder went back to 6000/4; a -mc session (4800/4 C4FM) queued its own by chance. */
static void
test_stop_queues_restored_profile(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    /* -mc and -m2: C4FM, CQPSK, P25 Phase 2 pin; profile, sps, center, rf_mod; symbol rate; the TED override. */
    static const struct {
        int c4fm, qpsk, p2_lock;
        int timing[4];
        int rate, override;
    } locked[] = {
        {1, 0, 0, {DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0}, 4800, 0},
        {0, 1, 1, {DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 8, 3, 1}, 6000, 0},
        {0, 1, 1, {DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 8, 3, 1}, 6000, 8},
    };

    const int c4fm = o->mod_c4fm;
    for (size_t i = 0; i < sizeof(locked) / sizeof(locked[0]); ++i) {
        const dsd_signal_search_config one = tag_config(config, 94 + (uint64_t)i, 1, 0);
        o->mod_cli_lock = 1;
        o->mod_c4fm = locked[i].c4fm;
        o->mod_qpsk = locked[i].qpsk;
        o->mod_gfsk = 0;
        o->mod_p25p2_profile_lock = locked[i].p2_lock;
        const int* t = locked[i].timing;
        put_timing(s, t[0], t[1], t[2], t[3]);
        ted_override = locked[i].override;
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        // A visit's retune replaces the front end's TED override (it queues none).
        ted_override = 0;
        put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_2400_4, 20, 9, 0);
        stop_tag_session(o, s);
        assert(timing_is(s, t[0], t[1], t[2], t[3]));
        assert(queued_profile.hz == 100000000 && queued_profile.rate == locked[i].rate && queued_profile.levels == 4);
        assert(queued_profile.cq == (t[3] == 1));
        if (locked[i].override) {
            assert(queued_profile.sps == locked[i].override && queued_profile.persist);
        } else {
            assert(queued_profile.sps == t[1] && !queued_profile.persist);
        }
    }
    ted_override = 0;
    o->mod_cli_lock = o->mod_qpsk = o->mod_p25p2_profile_lock = 0;
    o->mod_c4fm = c4fm;

    // A fixed preset's session keeps its preset's profile: an NXDN48 session started on the command line, whose
    // decoder runs 2400/4 while the hunt index it never used still names 4800/4.
    dsd_opts* saved = malloc(sizeof(*saved));
    assert(saved);
    DSD_MEMCPY(saved, o, sizeof(*o));
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_NXDN48, DSD_DECODE_PRESET_PROFILE_CONFIG, o, s);
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 20, 9, 0);
    const dsd_signal_search_config nxdn = tag_config(config, 97, 1, 0);
    start_tag_session(o, s, &nxdn);
    visit_strongest(o, s);
    stop_tag_session(o, s);
    assert(timing_is(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 20, 9, 0));
    assert(queued_profile.hz == 100000000 && queued_profile.rate == 2400 && queued_profile.levels == 4);
    assert(queued_profile.sps == 20 && !queued_profile.cq && !queued_profile.persist);
    DSD_MEMCPY(o, saved, sizeof(*o));
    free(saved);
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

/* A DMR Tier III channel the session learned from its site survives the search's visits with its origin and trust: a
   visit's boundary is a tune, which keeps them, and the departure's restore puts the row back with the provenance the
   session saved (discussion #900 F2). A visit that hears another network's checked system code (the entry point a
   checked C_ALOHA or SYS_Parms short LC calls) drops the session's rows; the departure brings the row back as SITE
   with its trust and the session's baseline with it. */
enum { LEARNED_CHANNEL = 9, USER_CHANNEL = 10, OVERLAID_CHANNEL = 11 };

/* The user's entry is still there: no restore takes the live overlay away, and it never reached the shared map. */
static int
user_dmr_row_kept(const dsd_state* s) {
    long hz = 0;
    return s->trunk_chan_map[USER_CHANNEL] == 0 && dmr_t3_chan_effective_hz(s, USER_CHANNEL) == 852112500
           && dmr_t3_chan_origin(s, USER_CHANNEL) == DMR_CHAN_ORIGIN_USER && dmr_t3_overlay_count(s) == 2U
           && dmr_t3_overlay_get(s, USER_CHANNEL, &hz) == DMR_T3_OVERLAY_SET && hz == 852112500;
}

/* The shared row under the user's entry is the session's: the site's SITE row, which a visit that
   heard another network dropped, comes back with the session's rows, while DMR goes on using the user's frequency. */
static int
overlaid_dmr_row_back(const dsd_state* s) {
    const dmr_ext_t* ext = dmr_ext_get(s);
    long hz = 0;
    return dmr_t3_overlay_get(s, OVERLAID_CHANNEL, &hz) == DMR_T3_OVERLAY_SET && hz == 852137500
           && dmr_t3_chan_effective_hz(s, OVERLAID_CHANNEL) == 852137500
           && s->trunk_chan_map[OVERLAID_CHANNEL] == 852025000 && ext != NULL
           && ext->origin[OVERLAID_CHANNEL] == (uint8_t)DMR_CHAN_ORIGIN_SITE
           && dsd_state_dmr_lcn_ota_get(s, OVERLAID_CHANNEL) && s->dmr_lcn_trust[OVERLAID_CHANNEL] == 2U;
}

static int
learned_dmr_row_back(const dsd_state* s, uint32_t session_generation) {
    return s->trunk_chan_map[LEARNED_CHANNEL] == 852012500 && s->dmr_lcn_trust[LEARNED_CHANNEL] == 2U
           && dmr_t3_chan_origin(s, LEARNED_CHANNEL) == DMR_CHAN_ORIGIN_SITE
           && dmr_t3_chan_generation(s) == session_generation;
}

/* Network B's checked code reaches the visit: the session's rows go. */
static void
visit_hears_network_b(dsd_state* s, uint32_t session_generation) {
    dmr_t3_chan_note_checked_syscode(s, 0x2254U); /* Large model: another MODEL than the session's */
    assert(s->trunk_chan_map[LEARNED_CHANNEL] == 0 && s->dmr_lcn_trust[LEARNED_CHANNEL] == 0U);
    assert(dmr_t3_chan_generation(s) == session_generation + 1U);
}

static void
test_learned_dmr_channel_survives_visits(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 85, 1, 0);
    session_site(s, CODE_A, 0, 0);
    dmr_ext_t* ext = dmr_ext_get_or_create(s);
    assert(ext != NULL);
    dmr_t3_chan_note_checked_syscode(s, 0x1234U); /* the session's network: Small model, NET 0x11 */
    /* As the over-the-air writer leaves a SITE row learned on the control channel. */
    dsd_state_set_trunk_chan_freq(s, LEARNED_CHANNEL, 852012500);
    dsd_state_dmr_lcn_ota_set(s, LEARNED_CHANNEL, 1);
    ext->origin[LEARNED_CHANNEL] = (uint8_t)DMR_CHAN_ORIGIN_SITE;
    s->dmr_lcn_trust[LEARNED_CHANNEL] = 2U;
    /* And the user's own entry for another channel, as dmr_t3_overlay_apply() makes it: the session's restore keeps it.
       And an entry over a row the site taught, which the shared map keeps under it. */
    ext->overlay.op[USER_CHANNEL] = (uint8_t)DMR_T3_OVERLAY_SET;
    ext->overlay.hz[USER_CHANNEL] = 852112500;
    dsd_state_set_trunk_chan_freq(s, OVERLAID_CHANNEL, 852025000);
    dsd_state_dmr_lcn_ota_set(s, OVERLAID_CHANNEL, 1);
    ext->origin[OVERLAID_CHANNEL] = (uint8_t)DMR_CHAN_ORIGIN_SITE;
    s->dmr_lcn_trust[OVERLAID_CHANNEL] = 2U;
    ext->overlay.op[OVERLAID_CHANNEL] = (uint8_t)DMR_T3_OVERLAY_SET;
    ext->overlay.hz[OVERLAID_CHANNEL] = 852137500;
    ext->overlay.count = 2U;
    assert(user_dmr_row_kept(s) && overlaid_dmr_row_back(s));
    const uint32_t session_generation = dmr_t3_chan_generation(s);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(boundary_kind == DSD_CARRIER_BOUNDARY_TUNE);
    visit_hears_network_b(s, session_generation);
    /* The visit's network change dropped the SITE row under the entry; DMR still uses the user's frequency. */
    assert(s->trunk_chan_map[OVERLAID_CHANNEL] == 0 && dmr_t3_chan_effective_hz(s, OVERLAID_CHANNEL) == 852137500);
    next_visit(o, s);
    assert(boundary_kind == DSD_CARRIER_BOUNDARY_TUNE);
    assert(learned_dmr_row_back(s, session_generation) && user_dmr_row_kept(s) && overlaid_dmr_row_back(s));
    /* Back on the session's network baseline: its own code is no network change. */
    dmr_t3_chan_note_checked_syscode(s, 0x1223U);
    assert(learned_dmr_row_back(s, session_generation));
    visit_hears_network_b(s, session_generation);
    stop_tag_session(o, s);
    assert(learned_dmr_row_back(s, session_generation));
    assert(user_dmr_row_kept(s) && overlaid_dmr_row_back(s));
    s->dmr_lcn_trust[LEARNED_CHANNEL] = 0U;
    s->dmr_lcn_trust[OVERLAID_CHANNEL] = 0U;
    dsd_state_set_trunk_chan_freq(s, OVERLAID_CHANNEL, 0);
    dmr_t3_overlay_discard(s);
    dmr_t3_chan_note_map_cleared(s);
    ext->syscode_valid = 0U;
    session_site(s, 0, 0, 0);
}

/* A standard group grant for channel @p lpcn with no frequency, as the CSBK handler records it. */
static void
note_missing_dmr_channel(dsd_opts* o, dsd_state* s, uint16_t lpcn) {
    dmr_t3_grant_class cls;
    DSD_MEMSET(&cls, 0, sizeof cls);
    cls.group = 1U;
    cls.recipient = 1001U;
    cls.caller = 2002U;
    dmr_t3_missing_note_grant(o, s, lpcn, &cls, 0U, 0);
}

/* A session that ends during a visit keeps the visit's site, its DMR ledger included, until the
   engine's teardown releases it. Only then is the live ledger the session's own, so the exit summary is logged after
   the release (dsd_engine_cleanup()): before it, the visit's channel would be reported and the session's not. */
static void
test_shutdown_during_visit_returns_the_dmr_ledger(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 88, 1, 0);
    session_site(s, CODE_A, 0, 0);
    note_missing_dmr_channel(o, s, 12U);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    dmr_t3_chan_network_changed(o, s); /* the visit hears another network */
    note_missing_dmr_channel(o, s, 30U);
    dsd_engine_signal_search_shutdown(o, s);
    dmr_t3_missing_ledger ledger;
    dmr_t3_missing_copy(s, &ledger);
    assert(dmr_t3_missing_count(s) == 1U && (ledger.missing[30 >> 3] & (1U << (30 & 7))) != 0U);
    dsd_signal_search_visit_clear(); // the engine's teardown, after its last close
    dsd_engine_signal_search_release_site(o, s);
    dmr_t3_missing_copy(s, &ledger);
    assert(dmr_t3_missing_count(s) == 1U && (ledger.missing[12 >> 3] & (1U << (12 & 7))) != 0U);
    dmr_t3_chan_network_changed(o, s);
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
    dsd_engine_signal_search_release_site(o, s);
    assert(session_back(s, CODE_A, 0, 0));
    s->p2_cc = 0x111;
    dsd_engine_signal_search_release_site(o, s);
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
    dsd_engine_signal_search_release_site(o, s);
    assert(s->p2_cc == 0x111);
    live = 1;
    session_site(s, 0, 0, 0);
}

/* The session's P25 patch table (system A's SG 1400 patching WG 1401) on the session's own listening clock. */
static void
session_patches(dsd_state* s) {
    dsd_p25_patch_clear_all(s);
    (void)dsd_p25_patch_note_system(s, 0xA1001U, 0x101U);
    s->p25_patch_count = 1;
    s->p25_patch_sgid[0] = 1400;
    s->p25_patch_active[0] = 1;
    s->p25_patch_is_patch[0] = 1;
    s->p25_patch_wgid_count[0] = 1;
    s->p25_patch_wgid[0][0] = 1401;
    s->p25_patch_key_valid[0] = 1; // KEY=0: a clear patch
    s->p25_patch_listen_s = 40.0;
    s->p25_patch_seen_listen_s[0] = 39.0;
    s->p25_patch_members_listen_s[0] = 39.0;
    s->p25_patch_seq_counter = 3U;
    s->p25_patch_seq[0] = 2U;
    s->p25_patch_listen_last_m = 1000.0;
}

static int
session_patches_back(const dsd_state* s) {
    return s->p25_patch_count == 1 && s->p25_patch_sgid[0] == 1400 && s->p25_patch_active[0] == 1
           && s->p25_patch_wgid_count[0] == 1 && s->p25_patch_wgid[0][0] == 1401 && s->p25_patch_key_valid[0] == 1
           && s->p25_patch_owner_known == 1 && s->p25_patch_owner_wacn == 0xA1001U && s->p25_patch_owner_sysid == 0x101U
           && s->p25_patch_listen_s > 39.5 && s->p25_patch_seq_counter == 3U
           && s->p25_patch_listen_last_m < 0.5; // restored, continuity broken
}

static int
no_patches(const dsd_state* s) {
    return s->p25_patch_count == 0 && s->p25_patch_active[0] == 0 && s->p25_patch_wgid_count[0] == 0
           && s->p25_patch_owner_known == 0 && s->p25_patch_listen_s < 0.5;
}

/* A voice-only visit to system B, whose calls name the same SG number: an SG-only row, no members, no KEY. */
static void
visit_names_sg(dsd_state* s) {
    s->p25_patch_count = 1;
    s->p25_patch_sgid[0] = 1400;
    s->p25_patch_active[0] = 1;
}

/* The patch table is the session's: every visit starts with an empty, unowned one, so a voice-only visit to another
   system naming the same SG number inherits neither the session's members nor its KEY=0, and the session's comes back
   whole only when the search returns to the session (a Stop, or the teardown of a decoder session that ended during a
   visit), never on a visit. */
static void
test_patches_stay_with_session(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    dsd_signal_search_config one = tag_config(config, 97, 1, 0);
    session_site(s, CODE_A, 0, 0);
    session_patches(s);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    assert(no_patches(s));
    visit_names_sg(s);
    next_visit(o, s);
    assert(no_patches(s));
    visit_names_sg(s);
    stop_tag_session(o, s);
    assert(session_patches_back(s));

    /* The decoder session ends during a visit: the session's table comes back with its site at the teardown. */
    one = tag_config(config, 98, 1, 0);
    session_patches(s);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    visit_names_sg(s);
    dsd_engine_signal_search_shutdown(o, s);
    dsd_signal_search_visit_clear();
    dsd_engine_signal_search_release_site(o, s);
    assert(session_patches_back(s));
    dsd_p25_patch_clear_all(s);
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
    test_learned_dmr_channel_survives_visits(o, s, config);
    test_unknown_identity_restored(o, s, config);
    test_verified_seed_at_start_not_restored_as_proof(o, s, config);
    test_shutdown_during_active_call(o, s, config);
    test_shutdown_during_visit_returns_the_dmr_ledger(o, s, config);
    test_patches_stay_with_session(o, s, config);
    test_departure_mid_recovery_drops_pending(o, s, config);
    test_code_list_bound_dedupe_order(o, s, config);
    test_code_list_pause_resume_and_new_start(o, s, config);
    test_code_list_only_while_receiving(o, s, config);
    test_probe_hook_answer(o, s, config);
}

/* The runtime session list as the engine last published it: its length, with its session in @p session. */
static uint32_t published_session[DSD_SEARCH_MAX_AVOIDS];

static size_t
session_avoids(uint64_t* session) {
    return dsd_signal_search_avoids_session_get(session, published_session, DSD_SEARCH_MAX_AVOIDS);
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
    assert(session_avoids(&session) == DSD_SEARCH_MAX_AVOIDS && session == 97 && published_session[0] == 300000000);
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
    assert(session_avoids(&session) == 1 && session == 98 && published_session[0] == 100012500);
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
    assert(session_avoids(&session) == 1 && session == 46 && published_session[0] == 100012500);
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
    assert(session_avoids(&session) == 1 && published_session[0] == 200000000);
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
    assert(session_avoids(&session) == 1 && session == 77 && published_session[0] == 100000000);
    DSD_SNPRINTF(one.ranges[0].name, sizeof(one.ranges[0].name), "%s", "Edited");
    char error[192];
    assert(dsd_engine_signal_search_start(o, s, &one, observer, NULL, error, sizeof(error)) == 0);
    assert(status.phase == DSD_SEARCH_SEARCHING && strcmp(status.range_name, "Edited") == 0);
    assert(session_avoids(&session) == 1 && session == 77 && published_session[0] == 100000000);
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
    assert(session_avoids(&session) == 1 && published_session[0] == 100012500);
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

/* The native store as the start action reaches it (the app-control store, stubbed): the status it keeps is the one a
   frontend reads (dsd_app_signal_search_status_get()). */
static dsd_signal_search_status store_status;

void
dsd_app_signal_search_publish(const dsd_signal_search_status* s, const dsd_signal_search_hit* h, void* ctx) {
    (void)ctx;
    store_status = *s;
    observer(s, h, NULL);
}

void
dsd_app_signal_search_status_get(dsd_signal_search_status* s, char* error, size_t capacity) {
    if (s) {
        *s = store_status;
    }
    if (error && capacity) {
        error[0] = '\0';
    }
}

void
dsd_app_signal_search_store_flush(void) {}

int
dsd_app_signal_search_permanent_avoid(uint32_t frequency) {
    (void)frequency;
    return DSD_APP_SEARCH_AVOID_ADDED;
}

int
dsd_app_command_submit(int id, const void* data, size_t size) {
    (void)id;
    (void)data;
    (void)size;
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

/* The command queue's -Y take-back, the start's commit hook (issue #748 B13): a -Y resume waiting on its row is taken
   back, leaving its row's scope (here, a voice-only row's setting) for the session's own. */
static int scan_resumes_taken;

void
dsd_app_signal_search_take_back_scan_resume(dsd_opts* opts, dsd_state* state, void* user) {
    (void)user;
    if (state->lcn_scan_resume_pending) {
        state->lcn_scan_resume_pending = 0;
        opts->scan_voice_only = 0;
        ++scan_resumes_taken;
    }
}

double
dsd_analog_rx_clock_s(void) {
    return 0.0;
}

int
dsd_app_rx_tone_view(const dsd_opts* opts, const dsd_state* state, double now, dsd_app_rx_tone* out) {
    (void)opts;
    (void)state;
    (void)now;
    DSD_MEMSET(out, 0, sizeof(*out));
    return 0;
}

/* A frontend's read of the store, as Qt's SignalSearchModel::refresh() makes it: a status whose revision and session
   both match the last one read is the same status, and is skipped. 1 when this read took a new status. */
static uint64_t qt_revision = UINT64_MAX, qt_session;

static int
qt_reads_status(dsd_signal_search_status* out) {
    dsd_signal_search_status s;
    dsd_app_signal_search_status_get(&s, NULL, 0);
    if (s.revision == qt_revision && s.session_id == qt_session) {
        return 0;
    }
    qt_revision = s.revision;
    qt_session = s.session_id;
    *out = s;
    return 1;
}

/* A start through the command queue's handler, as the decoder drains it: 1 when it went on, 2 when it was refused. */
static int
start_action(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    static struct dsd_app_command command;
    DSD_MEMSET(&command, 0, sizeof(command));
    command.id = DSD_APP_CMD_SIGNAL_SEARCH_START;
    command.n = sizeof(*config);
    DSD_MEMCPY(command.data, config, sizeof(*config));
    return dsd_app_actions_signal_search[0].fn(o, s, &command);
}

/* Issue #748 B11: a refused start publishes a status of its own, and every later publication of the session must reach
   a frontend that skips a status whose revision it has read. A refusal took the store's revision plus one, which the
   paused coordinator's next publication (an edited Resume) reused: the frontend skipped the Resume and kept showing the
   refusal on a paused search. */
static void
test_refusal_then_resume_reaches_frontend(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    dsd_signal_search_config one = tag_config(config, 90, 0, 0);
    live = 1;
    carrier = 0;
    observed = 1;
    failed = 0;
    stalled_ms = 0;
    o->rtlsdr_center_freq = 100000000;
    assert(start_action(o, s, &one) == 1);
    tick(o, s);
    visit_strongest(o, s);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    dsd_signal_search_status seen;
    assert(qt_reads_status(&seen) && seen.phase == DSD_SEARCH_PAUSED && seen.session_id == 90);

    // An edited Resume the engine refuses: its message shows on the paused search.
    dsd_signal_search_config refused = one;
    refused.ranges[0].end_hz = refused.ranges[0].start_hz - 1;
    assert(start_action(o, s, &refused) == 2);
    assert(qt_reads_status(&seen) && seen.phase == DSD_SEARCH_PAUSED && seen.message[0] != '\0');

    // The corrected Resume goes on: the frontend reads the running search, not the refusal it already showed.
    one.ranges[0].end_hz = 100012500;
    assert(start_action(o, s, &one) == 1);
    assert(qt_reads_status(&seen) && seen.phase == DSD_SEARCH_SEARCHING && seen.message[0] == '\0');
    tick(o, s);
    assert(qt_reads_status(&seen) && seen.phase == DSD_SEARCH_TUNING);
    land(o, s);
    stop_tag_session(o, s);
    assert(qt_reads_status(&seen) && seen.phase == DSD_SEARCH_STOPPED);

    // A fresh start refused with no coordinator, then one that goes on under the same session: the first publication
    // of the new coordinator reaches the frontend too.
    dsd_signal_search_config fresh = tag_config(config, 91, 0, 0);
    dsd_signal_search_config bad = fresh;
    bad.ranges[0].end_hz = bad.ranges[0].start_hz - 1;
    assert(start_action(o, s, &bad) == 2);
    assert(qt_reads_status(&seen) && seen.phase == DSD_SEARCH_ERROR && seen.session_id == 91);
    assert(start_action(o, s, &fresh) == 1);
    assert(qt_reads_status(&seen) && seen.phase == DSD_SEARCH_SEARCHING && seen.session_id == 91);
    tick(o, s);
    land(o, s);
    stop_tag_session(o, s);
}

/* What a Signal search start changes in the options and the decoder state: the decoder settings with the live symbol
   timing, the -Y row option the test below runs (voice only), the tuned frequency, the scan scope's class and a -Y
   resume still waiting on its row. */
typedef struct {
    dsd_scan_settings settings;
    uint32_t center_hz;
    int resume_pending;
    dsd_scan_mode active;
    dsd_scan_mode row;
} start_view;

static void
start_view_capture(const dsd_opts* o, const dsd_state* s, start_view* v) {
    dsd_scan_settings_capture(o, s, &v->settings);
    v->center_hz = o->rtlsdr_center_freq;
    v->resume_pending = s->lcn_scan_resume_pending;
    v->active = dsd_scan_mode_active(s);
    v->row = dsd_scan_mode_row(s);
}

static int
start_view_same(const start_view* a, const start_view* b) {
    return dsd_scan_settings_equal(&a->settings, &b->settings, 1)
           && a->settings.scan_voice_only == b->settings.scan_voice_only && a->center_hz == b->center_hz
           && a->resume_pending == b->resume_pending && a->active == b->active && a->row == b->row;
}

/* Issue #748 B13: a start is validate-then-commit. Each step a fresh start can fail at fails it with nothing changed:
   the options, the decoder state (a scan owner made for the start goes again), the quiet check a scan owner holds, a -Y
   resume still waiting on its row, the tuner and the carrier. */
static void
test_failed_start_changes_nothing(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    static const int points[] = {DSD_SEARCH_START_FAIL_COORD, DSD_SEARCH_START_FAIL_SCOPE, DSD_SEARCH_START_FAIL_SITE,
                                 DSD_SEARCH_START_FAIL_OWNER};
    static start_view before;
    static start_view after;
    dsd_signal_search_config one = tag_config(config, 92, 1, 0);
    o->rtlsdr_center_freq = 100000000;
    // A session the preset would change, a mono -f1 decoder on a NXDN48 hunt, and a typed -Y row (voice only) whose
    // resume waits on its tune.
    o->dmr_stereo = o->dmr_mono = 0;
    o->frame_dmr = 0;
    o->scan_voice_only = 1;
    s->lcn_scan_resume_pending = 1;
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_2400_4, 20, 9, 0);
    for (int owned = 0; owned < 2; ++owned) {
        for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
            if (owned) {
                assert(dsd_engine_scan_owner_ensure(s) != NULL);
            } else {
                (void)dsd_state_ext_set(s, DSD_STATE_EXT_ENGINE_TRUNK_SCAN, NULL, NULL);
            }
            const dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(s);
            const uint32_t quiet_inv = owner ? owner->quiet_inv_seq : 0U;
            const int before_boundary = boundary;
            start_view_capture(o, s, &before);
            dsd_engine_signal_search_test_fail_start(points[i]);
            assert(start_action(o, s, &one) == 2);
            dsd_engine_signal_search_test_fail_start(DSD_SEARCH_START_FAIL_NONE);
            assert(strcmp(store_status.message, "Not enough memory to start Signal search") == 0);
            assert(store_status.phase == DSD_SEARCH_ERROR && !store_status.owned);
            start_view_capture(o, s, &after);
            assert(start_view_same(&before, &after));
            owner = dsd_engine_scan_owner_get(s);
            assert(owned ? owner && owner->quiet_inv_seq == quiet_inv : owner == NULL);
            assert(!dsd_engine_signal_search_active(s) && !dsd_signal_search_is_running(s));
            assert(boundary == before_boundary && !tuner_owner && !scan_resumes_taken);
        }
    }
    // Nothing was left half made: the same start goes on. It takes the -Y resume back first, so the settings it puts
    // back at Stop are the session's, not the row's.
    live = 1;
    carrier = 0;
    observed = 1;
    failed = 0;
    stalled_ms = 0;
    assert(start_action(o, s, &one) == 1 && scan_resumes_taken == 1 && !s->lcn_scan_resume_pending);
    tick(o, s);
    visit_strongest(o, s);
    // A paused search's edited Resume goes on, through the same commit hook.
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    s->lcn_scan_resume_pending = 1;
    one.ranges[0].end_hz = 100012500;
    assert(start_action(o, s, &one) == 1 && scan_resumes_taken == 2 && status.phase == DSD_SEARCH_SEARCHING);
    tick(o, s);
    land(o, s);
    stop_tag_session(o, s);
    assert(timing_is(s, DSD_FRAME_SYNC_SPS_PROFILE_2400_4, 20, 9, 0) && !o->dmr_stereo && !o->dmr_mono);
    assert(o->scan_voice_only == 0 && !o->frame_dmr);
    o->frame_dmr = 1;
    o->dmr_stereo = 1;
    scan_resumes_taken = 0;
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0);
}

/* A frame sync of @p sync, as the decoder notes it in the received identity (dsd_rxid_note_sync()): another family's
   sync drops the record, an M17 CAN included. */
static void
rx_sync(dsd_state* s, int sync) {
    s->synctype = sync;
    dsd_rxid_note_sync(s, sync);
}

/* An active M17 call on the stream's sync type with CAN @p can in its service options. */
static void
m17_stream_call(uint64_t epoch, uint16_t can) {
    dsd_call_snapshot* call = put_call(0, epoch, 0.0, DSD_SYNC_M17_STR_POS, 0, 0);
    call->service_options = can;
    call->has_service_metadata = 1U;
}

/* What the M17 decoder reports for a CRC-checked LSF of CAN @p can (m17.c, M17decodeLSF() then
   m17_finalize_lsf_crc()): the CAN it carries through the access hook, then the confirmation and the call note. A
   call-free LSF (stream data type 0) opens no call record, so the call note finds none. */
static void
m17_checked_lsf(dsd_state* s, uint16_t can) {
    rx_sync(s, DSD_SYNC_M17_LSF_POS);
    dsd_rxid_note_access(s, DSD_RXID_FAMILY_M17, (uint8_t)DSD_ACCESS_CODE_CAN, can);
    dsd_signal_search_note_access_code(s, DSD_SYNC_M17_LSF_POS, DSD_ACCESS_CODE_CAN, can);
    dsd_signal_search_note_evidence(s, "M17", 0, 0, NULL);
    dsd_signal_search_note_call(s, 0U);
}

/* Issue #748 B14: a CRC-checked M17 LSF of stream data type 0 names its CAN and opens no call. The access text of the
   hit, as the store keeps it, is the latest checked LSF's CAN: a second call-free LSF's replaces the first's, the CAN
   of an older call still running does not come back over it, and the ended hit keeps it. A DMR code the receive policy
   rejects never reaches the hit through the same hook. */
static void
test_m17_call_free_lsf_can(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 93, 1, 0);
    live = 1;
    carrier = 0;
    observed = 1;
    failed = 0;
    stalled_ms = 0;
    o->rtlsdr_center_freq = 100000000;
    DSD_MEMSET(calls, 0, sizeof(calls));
    assert(start_action(o, s, &one) == 1);
    tick(o, s);
    visit_strongest(o, s);
    m17_checked_lsf(s, 3U);
    assert(store_status.hit.confirmed && strcmp(store_status.hit.protocol, "M17") == 0);
    assert(strcmp(store_status.hit.access, "CAN 3") == 0);
    m17_checked_lsf(s, 9U);
    assert(strcmp(store_status.hit.access, "CAN 9") == 0);
    // A periodic refresh keeps it.
    advance(100);
    tick(o, s);
    assert(strcmp(store_status.hit.access, "CAN 9") == 0);
    // A voice LSF of CAN 5 opens a call; a call-free LSF of CAN 9 while that call still runs names CAN 9.
    dsd_call_snapshot* call = put_call(0, 7, 0.0, DSD_SYNC_M17_LSF_POS, 0, 0);
    call->service_options = 5U;
    call->has_service_metadata = 1U;
    m17_checked_lsf(s, 5U);
    assert(strcmp(store_status.hit.access, "CAN 5") == 0);
    m17_checked_lsf(s, 9U);
    assert(strcmp(store_status.hit.access, "CAN 9") == 0);
    advance(100);
    tick(o, s);
    assert(strcmp(store_status.hit.access, "CAN 9") == 0);
    // The stream frames after it publish under another M17 sync type, the call record unchanged: CAN 9 stays.
    s->synctype = DSD_SYNC_M17_STR_POS;
    dsd_signal_search_note_evidence(s, "M17", 0, 0, NULL);
    assert(strcmp(store_status.hit.access, "CAN 9") == 0);
    // The ended hit keeps it.
    advance(600);
    tick(o, s);
    assert(store_status.hit.ended && strcmp(store_status.hit.access, "CAN 9") == 0);
    DSD_MEMSET(calls, 0, sizeof(calls));

    // The next visit: nothing noted on the last one is this one's.
    land(o, s);
    ready = 1;
    tick(o, s);
    land(o, s);
    assert(!store_status.hit.ended && store_status.hit.access[0] == '\0' && !store_status.hit.confirmed);
    // A DMR hit on colour code 7, with colour code 3 blocked: a note of colour code 3 neither reaches the hit nor
    // publishes it.
    block_cc3(o);
    dmr_evidence(s, 7);
    assert(strcmp(store_status.hit.access, "CC 7") == 0);
    const int published = hits_published;
    s->dmr_color_code = 3;
    dsd_signal_search_note_access_code(s, DSD_SYNC_DMR_BS_DATA_POS, DSD_ACCESS_CODE_COLOR_CODE, 3U);
    assert(hits_published == published && strcmp(store_status.hit.access, "CC 7") == 0);
    // An allowed one does.
    s->dmr_color_code = 5;
    dsd_signal_search_note_access_code(s, DSD_SYNC_DMR_BS_DATA_POS, DSD_ACCESS_CODE_COLOR_CODE, 5U);
    assert(hits_published == published + 1 && strcmp(store_status.hit.access, "CC 5") == 0);
    stop_refusal_session(o, s);
}

/* Issue #748 B14: a carried code belongs to the family that sent it. An Auto visit that confirms M17 (CAN 3), then DMR,
   then an M17 transmission that names no CAN (a BERT stream) shows no CAN: the first transmission's went when the
   visit left M17. A code noted for the incoming family before it confirms still shows once it does. */
static void
test_access_note_left_family(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 93, 1, 0);
    live = 1;
    carrier = 0;
    observed = 1;
    failed = 0;
    stalled_ms = 0;
    o->rtlsdr_center_freq = 100000000;
    DSD_MEMSET(calls, 0, sizeof(calls));
    assert(start_action(o, s, &one) == 1);
    tick(o, s);
    visit_strongest(o, s);
    m17_checked_lsf(s, 3U);
    assert(strcmp(store_status.hit.protocol, "M17") == 0 && strcmp(store_status.hit.access, "CAN 3") == 0);
    dmr_evidence(s, 7);
    assert(strcmp(store_status.hit.protocol, "DMR") == 0 && strcmp(store_status.hit.access, "CC 7") == 0);
    s->synctype = DSD_SYNC_M17_BRT_POS;
    dsd_signal_search_note_evidence(s, "M17", 0, 0, NULL);
    assert(strcmp(store_status.hit.protocol, "M17") == 0);
    assert(!store_status.hit.access_noted && strstr(store_status.hit.access, "CAN 3") == NULL);
    // A CAN noted while the hit is still DMR waits for M17, and shows once M17 confirms.
    dmr_evidence(s, 7);
    assert(strcmp(store_status.hit.protocol, "DMR") == 0);
    rx_sync(s, DSD_SYNC_M17_LSF_POS);
    dsd_rxid_note_access(s, DSD_RXID_FAMILY_M17, (uint8_t)DSD_ACCESS_CODE_CAN, 9U);
    dsd_signal_search_note_access_code(s, DSD_SYNC_M17_LSF_POS, DSD_ACCESS_CODE_CAN, 9U);
    assert(strcmp(store_status.hit.access, "CC 7") == 0);
    dsd_signal_search_note_evidence(s, "M17", 0, 0, NULL);
    assert(strcmp(store_status.hit.protocol, "M17") == 0 && strcmp(store_status.hit.access, "CAN 9") == 0);
    advance(600);
    tick(o, s);
    assert(store_status.hit.ended && strcmp(store_status.hit.access, "CAN 9") == 0);
    stop_refusal_session(o, s);
}

/* Start a one-range Auto session (93) and land on its strongest channel. */
static void
start_m17_visit(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    const dsd_signal_search_config one = tag_config(config, 93, 1, 0);
    live = 1;
    carrier = 0;
    observed = 1;
    failed = 0;
    stalled_ms = 0;
    o->rtlsdr_center_freq = 100000000;
    DSD_MEMSET(calls, 0, sizeof(calls));
    assert(start_action(o, s, &one) == 1);
    tick(o, s);
    visit_strongest(o, s);
}

static void
end_m17_visit(dsd_opts* o, dsd_state* s) {
    DSD_MEMSET(calls, 0, sizeof(calls));
    stop_refusal_session(o, s);
    dsd_rxid_reset(s);
}

/* An LSF rebuilt from the stream's LICH: its CAN noted in the received identity and carried to the search, its call
   active under the stream's sync type. */
static void
m17_stream_lsf(dsd_state* s, uint64_t epoch, uint16_t can) {
    rx_sync(s, DSD_SYNC_M17_STR_POS);
    m17_stream_call(epoch, can);
    dsd_rxid_note_access(s, DSD_RXID_FAMILY_M17, (uint8_t)DSD_ACCESS_CODE_CAN, can);
    dsd_signal_search_note_access_code(s, DSD_SYNC_M17_STR_POS, DSD_ACCESS_CODE_CAN, can);
    dsd_signal_search_note_evidence(s, "M17", 0, 0, NULL);
}

/* Two clean M17 stream frames confirm M17, with no LSF: the detection shows no CAN. */
static void
m17_stream_frames_show_no_can(dsd_state* s) {
    for (int frame = 0; frame < 2; ++frame) {
        rx_sync(s, DSD_SYNC_M17_STR_POS);
        dsd_signal_search_note_evidence(s, "M17", 0, 0, NULL);
        assert(strcmp(store_status.hit.protocol, "M17") == 0 && store_status.hit.access[0] == '\0');
    }
}

/* Issue #748: an M17 CAN shows only while the received identity holds it, which only a checked LSF of the current
   reception notes and another family's sync drops. An LSF rebuilt from the stream's LICH names CAN 3, its call still
   active; DMR traffic confirms on colour code 7 while that call stays active; then two clean M17 stream frames confirm
   M17 again before any new LSF: no CAN, live or ended. A call whose checked LSF named CAN 6 shows it. */
static void
test_access_call_left_family(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    start_m17_visit(o, s, config);
    m17_stream_lsf(s, 7, 3U);
    assert(strcmp(store_status.hit.protocol, "M17") == 0 && strcmp(store_status.hit.access, "CAN 3") == 0);
    rx_sync(s, DSD_SYNC_DMR_BS_DATA_POS);
    dmr_evidence(s, 7);
    assert(strcmp(store_status.hit.protocol, "DMR") == 0 && strcmp(store_status.hit.access, "CC 7") == 0);
    m17_stream_frames_show_no_can(s);
    // A call whose checked LSF named CAN 6 (noted in the received identity; carried by no note here) names its CAN.
    m17_stream_call(8, 6U);
    dsd_rxid_note_access(s, DSD_RXID_FAMILY_M17, (uint8_t)DSD_ACCESS_CODE_CAN, 6U);
    dsd_signal_search_note_evidence(s, "M17", 0, 0, NULL);
    assert(strcmp(store_status.hit.access, "CAN 6") == 0);
    advance(600);
    tick(o, s);
    assert(store_status.hit.ended && strcmp(store_status.hit.access, "CAN 6") == 0);
    end_m17_visit(o, s);
}

/* The same with a DMR burst that syncs and never confirms: the hit stays M17, so neither the access text it last
   carried, the carried note nor the call record may bring CAN 3 back when M17 stream frames confirm without an LSF. */
static void
test_access_sync_without_confirm(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    start_m17_visit(o, s, config);
    m17_stream_lsf(s, 7, 3U);
    assert(strcmp(store_status.hit.access, "CAN 3") == 0);
    rx_sync(s, DSD_SYNC_DMR_BS_DATA_POS);
    m17_stream_frames_show_no_can(s);
    advance(100);
    tick(o, s);
    assert(!store_status.hit.ended && store_status.hit.access[0] == '\0');
    advance(600);
    tick(o, s);
    assert(store_status.hit.ended && store_status.hit.access[0] == '\0');
    end_m17_visit(o, s);
}

/* A reacquired call inherits the service options of the epoch it reopens (call_state_seed_reacquired_snapshot()) with
   a fresh reception generation: a named call on CAN 3 ends on sync loss after DMR control traffic confirms; an LSF that
   failed its CRC (decoded under -F) names no CAN; two clean stream frames reopen the call, which still carries CAN 3.
   The next stream confirmation shows no CAN. */
static void
test_access_reacquired_call(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    start_m17_visit(o, s, config);
    m17_stream_lsf(s, 7, 3U);
    assert(strcmp(store_status.hit.access, "CAN 3") == 0);
    calls[0].phase = DSD_CALL_PHASE_ENDED;
    rx_sync(s, DSD_SYNC_DMR_BS_DATA_POS);
    dmr_evidence(s, 7);
    assert(strcmp(store_status.hit.protocol, "DMR") == 0);
    rx_sync(s, DSD_SYNC_M17_STR_POS);
    m17_stream_call(9, 3U);
    calls[0].rx_gen = dsd_state_reception_generation(s);
    m17_stream_frames_show_no_can(s);
    end_m17_visit(o, s);
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

/* ---- TETRA identification (issue #748) ---- */

/* The first TETRA mode a visit confirms names the detection: a later report of the other mode changes nothing (the
   decoder withdraws the site and access instead), and its activity keeps no visit listening. A TETRA detection shows
   the access code its own evidence carries and no call details: a TETRA call record names nothing on it. */
static void
test_tetra_protocol_latched(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    DSD_MEMSET(calls, 0, sizeof(calls));
    for (int first = 0; first < 2; ++first) {
        const char* latched = first ? "TETRA DMO" : "TETRA";
        const char* other = first ? "TETRA" : "TETRA DMO";
        const dsd_signal_search_config one = tag_config(config, 80 + (uint64_t)first, 1, 0);
        start_tag_session(o, s, &one);
        visit_strongest(o, s);
        assert(status.phase == DSD_SEARCH_IDENTIFYING);
        s->synctype = DSD_SYNC_TETRA_SB_POS;
        dsd_signal_search_note_evidence_at(s, latched, DSD_SYNC_TETRA_SB_POS, -1, 0, 0, first ? NULL : "CC 5");
        assert(status.hit.confirmed && strcmp(status.hit.protocol, latched) == 0);
        assert(strcmp(status.hit.access, first ? "" : "CC 5") == 0);
        const uint64_t revision = status.revision;
        advance(400);
        dsd_signal_search_note_evidence_at(s, other, DSD_SYNC_TETRA_NDB_POS, -1, 7, 8, "CC 9");
        assert(strcmp(status.hit.protocol, latched) == 0 && status.revision == revision);
        assert(strcmp(status.hit.access, first ? "" : "CC 5") == 0 && status.hit.target == 0);
        /* No call details: a checked TETRA call record names no target, source or slot. */
        (void)put_call(0, 1, 1.0, DSD_SYNC_TETRA_NDB_POS, 100, 200);
        dsd_signal_search_note_call(s, 0);
        assert(tuple_is(&status.hit, 0, 0, 0, "") && status.revision == revision);
        DSD_MEMSET(calls, 0, sizeof(calls));
        /* The other mode's report refreshed nothing: the visit's own quiet time runs from its own evidence. */
        dsd_signal_search_note_evidence_at(s, latched, DSD_SYNC_TETRA_SB_POS, -1, 0, 0, NULL);
        assert(strcmp(status.hit.protocol, latched) == 0);
        s->synctype = DSD_SYNC_NONE;
        stop_tag_session(o, s);
    }
}

/* One TETRA range of three channels, 100.0 to 100.025 MHz, as the range editor presets it. */
static dsd_signal_search_config
tetra_config(const dsd_signal_search_config* base, uint64_t session) {
    dsd_signal_search_config one = tag_config(base, session, DSD_SEARCH_DIGITAL_TETRA, 0);
    one.ranges[0].bandwidth_hz = 25000;
    one.ranges[0].probe_ms = 4000;
    return one;
}

/* A front end at the 72 kHz DSP rate, every family landing there. */
static void
use_tetra_rate(void) {
    use_front_end(72000, 72000, 72000, 0, 0);
    dsp_rate_hz = 72000;
}

static void
end_tetra_rate(dsd_opts* o, dsd_state* s) {
    dsd_rtl_stream_metrics_hooks_set(NULL);
    DSD_MEMSET(&front_end, 0, sizeof(front_end));
    dsp_rate_hz = 0;
    o->mod_cli_lock = o->mod_qpsk = o->mod_gfsk = o->mod_p25p2_profile_lock = 0;
    o->mod_c4fm = 1;
    put_timing(s, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 10, 4, 0);
}

/* The latest retune queued TETRA's chain: CQPSK on as the search's own choice, 18000/4 with TETRA's filter, the TED at
   the 72 kHz rate's 4 samples per symbol. */
static int
tetra_profile_queued(void) {
    return queued_profile.cq == 1 && queued_profile.rate == 18000 && queued_profile.levels == 4
           && queued_profile.profile == DSD_RTL_STREAM_CHANNEL_PROFILE_TETRA && queued_profile.sps == 4
           && retune_family.family == DSD_RX_FAMILY_DIGITAL && retune_family.cqpsk_explicit == 1;
}

/* The session's modulation lock (or none) is in force, with no TETRA decoder. */
static int
session_modulation_back(const dsd_opts* o, const dsd_state* s, int lock, int c4fm, int qpsk, int p2lock, int rf_mod) {
    return o->frame_tetra == 0 && o->mod_cli_lock == lock && o->mod_c4fm == c4fm && o->mod_qpsk == qpsk
           && o->mod_gfsk == 0 && o->mod_p25p2_profile_lock == p2lock && s->rf_mod == rf_mod;
}

/* A TETRA range's visit runs the TETRA preset with CQPSK and TETRA's chain and timing for the live rate, whatever -m
   lock the session runs (none, -mc, -m2); only that visit does: its span tunes, a span return, the next Auto visit and
   the probe's NFM fallback run the session's own lock and profile, and Stop gives the session its settings back. */
static void
test_tetra_visit_lands_tetra_profile(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    static const struct {
        int lock, c4fm, qpsk, p2lock, rf_mod, profile;
    } locks[] = {
        {0, 1, 0, 0, 0, DSD_FRAME_SYNC_SPS_PROFILE_4800_4},
        {1, 1, 0, 0, 0, DSD_FRAME_SYNC_SPS_PROFILE_4800_4},
        {1, 0, 1, 1, 1, DSD_FRAME_SYNC_SPS_PROFILE_6000_4},
    };

    use_tetra_rate();
    for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); ++i) {
        o->mod_cli_lock = locks[i].lock;
        o->mod_c4fm = locks[i].c4fm;
        o->mod_qpsk = locks[i].qpsk;
        o->mod_gfsk = 0;
        o->mod_p25p2_profile_lock = locks[i].p2lock;
        put_timing(s, locks[i].profile, 15, 7, locks[i].rf_mod);
        dsd_signal_search_config two = tetra_config(config, 82 + (uint64_t)i);
        /* An Auto range after it, at other frequencies. */
        two.range_count = 2;
        two.ranges[1] = two.ranges[0];
        two.ranges[1].digital = 1;
        two.ranges[1].bandwidth_hz = 12500;
        two.ranges[1].start_hz = 100100000;
        two.ranges[1].end_hz = 100125000;
        start_tag_session(o, s, &two);
        /* The span tune runs the session's own decoder. */
        assert(session_modulation_back(o, s, locks[i].lock, locks[i].c4fm, locks[i].qpsk, locks[i].p2lock,
                                       locks[i].rf_mod));
        assert(queued_profile.rate != 18000 && queued_profile.cq == (locks[i].rf_mod == 1));
        visit_strongest(o, s);
        assert(status.phase == DSD_SEARCH_IDENTIFYING && status.hit.unresolved && !status.hit.confirmed);
        assert(tetra_profile_queued() && o->frame_tetra == 1);
        assert(s->rf_mod == 1 && o->mod_qpsk == 1 && o->mod_c4fm == 0 && o->mod_gfsk == 0);
        assert(s->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_18000_4 && s->samplesPerSymbol == 4);
        /* TETRA is no Auto hunt. */
        assert(!dsd_engine_signal_search_probe(o, s));
        /* A span return runs the session's own lock and profile again. */
        act(o, s, DSD_SEARCH_ACTION_SKIP);
        assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
        assert(session_modulation_back(o, s, locks[i].lock, locks[i].c4fm, locks[i].qpsk, locks[i].p2lock,
                                       locks[i].rf_mod));
        assert(queued_profile.rate != 18000 && queued_profile.cq == (locks[i].rf_mod == 1));
        /* The probe's NFM fallback is the analog monitor, no TETRA decoder. */
        visit_strongest(o, s);
        assert(tetra_profile_queued() && status.hit.frequency_hz == 100012500);
        advance(4000);
        tick(o, s);
        assert(status.phase == DSD_SEARCH_TUNING && family == DSD_RX_FAMILY_ANALOG && o->frame_tetra == 0);
        land(o, s);
        assert(status.phase == DSD_SEARCH_LISTENING && status.hit.unresolved);
        act(o, s, DSD_SEARCH_ACTION_SKIP);
        /* The rest of the TETRA range, then the Auto range: its visit runs the session's own lock. */
        for (int n = 0; n < 16 && !(status.phase == DSD_SEARCH_IDENTIFYING && status.range_index == 1); ++n) {
            if (status.phase == DSD_SEARCH_TUNING) {
                land(o, s);
            } else if (status.phase == DSD_SEARCH_SEARCHING) {
                ready = 1;
                tick(o, s);
            } else {
                act(o, s, DSD_SEARCH_ACTION_SKIP);
            }
        }
        assert(status.phase == DSD_SEARCH_IDENTIFYING && status.range_index == 1);
        assert(o->frame_tetra == 0 && queued_profile.rate != 18000);
        assert(s->rf_mod == locks[i].rf_mod && o->mod_cli_lock == locks[i].lock && o->mod_qpsk == locks[i].qpsk);
        stop_tag_session(o, s);
        assert(session_modulation_back(o, s, locks[i].lock, locks[i].c4fm, locks[i].qpsk, locks[i].p2lock,
                                       locks[i].rf_mod));
        assert(s->sps_hunt_idx == locks[i].profile && s->samplesPerSymbol == 15 && !s->tetra_ext_scan_held);
    }
    end_tetra_rate(o, s);
}

/* The departure ran the TETRA state's cold start once more: after its boundary closed the visit under its tag, before
   the session's site (code 0) came back over the visit's (CODE_B). */
static int
tetra_reset_after_close(int before) {
    return tetra_resets == before + 1 && tetra_reset_boundary == boundary && tetra_reset_tagged
           && tetra_reset_code == CODE_B;
}

/* Land on the next TETRA detection, learning the visited site's code. */
static void
tetra_visit(dsd_opts* o, dsd_state* s) {
    for (int n = 0; n < 16 && status.phase != DSD_SEARCH_IDENTIFYING; ++n) {
        if (status.phase == DSD_SEARCH_SEARCHING) {
            ready = 1;
            tick(o, s);
        } else {
            land(o, s);
        }
    }
    assert(status.phase == DSD_SEARCH_IDENTIFYING && tetra_profile_queued());
    hold_code(s, CODE_B);
}

/* Every departure from a TETRA visit starts the TETRA state cold, after the visit's calls and recordings closed and
   before the session's site returns: the probe's fallback, Skip, Avoid, a span return at the quiet time, Pause, Stop,
   a failed search, an input lost, and a decoder session that ends during a visit, whose cold start waits for the
   session's final closes. The landing that follows starts it cold too, and the end of the search lets the hold go. */
static void
test_tetra_visit_resets_on_departures(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    use_tetra_rate();
    session_site(s, 0, 0, 0);
    dsd_signal_search_config one = tetra_config(config, 85);
    int before = tetra_resets;
    start_tag_session(o, s, &one);
    /* The first tune is the search's first landing. */
    assert(tetra_resets == before + 1 && s->tetra_ext_scan_held);

    tetra_visit(o, s);
    before = tetra_resets;
    advance(4000);
    tick(o, s); /* the probe's NFM fallback */
    assert(tetra_reset_after_close(before) && code_held(s) == 0);
    land(o, s);
    hold_code(s, CODE_B);
    before = tetra_resets;
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(tetra_reset_after_close(before) && code_held(s) == 0);

    tetra_visit(o, s);
    before = tetra_resets;
    act(o, s, DSD_SEARCH_ACTION_AVOID);
    assert(tetra_reset_after_close(before));

    tetra_visit(o, s);
    dsd_signal_search_note_evidence_at(s, "TETRA", DSD_SYNC_TETRA_SB_POS, -1, 0, 0, "CC 5");
    before = tetra_resets;
    advance(30000);
    tick(o, s); /* quiet: back to the span */
    assert(status.phase == DSD_SEARCH_TUNING && tetra_reset_after_close(before));

    tetra_visit(o, s);
    before = tetra_resets;
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED && tetra_reset_after_close(before));
    act(o, s, DSD_SEARCH_ACTION_RESUME);
    tetra_visit(o, s);
    before = tetra_resets;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(tetra_reset_after_close(before) && s->tetra_ext_scan_held);
    land(o, s);
    /* The end of the search: one more cold start for the session's own settings, and the hold goes. */
    assert(status.phase == DSD_SEARCH_STOPPED && tetra_resets == before + 2 && !s->tetra_ext_scan_held);

    /* A failed search, then a Stop with the input gone. */
    one = tetra_config(config, 86);
    start_tag_session(o, s, &one);
    tetra_visit(o, s);
    before = tetra_resets;
    stalled_ms = 20000;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && tetra_reset_after_close(before));
    stalled_ms = 0;
    act(o, s, DSD_SEARCH_ACTION_STOP);
    land(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && !s->tetra_ext_scan_held);
    one = tetra_config(config, 87);
    start_tag_session(o, s, &one);
    tetra_visit(o, s);
    before = tetra_resets;
    live = 0;
    tick(o, s);
    assert(status.phase == DSD_SEARCH_ERROR && tetra_reset_after_close(before));
    act(o, s, DSD_SEARCH_ACTION_STOP);
    assert(status.phase == DSD_SEARCH_STOPPED && !s->tetra_ext_scan_held);
    live = 1;

    /* The decoder session ends during a visit: nothing starts cold until its final closes, then before the site. */
    one = tetra_config(config, 88);
    start_tag_session(o, s, &one);
    tetra_visit(o, s);
    before = tetra_resets;
    dsd_engine_signal_search_shutdown(o, s);
    assert(status.phase == DSD_SEARCH_STOPPED && tetra_resets == before && s->tetra_ext_scan_held);
    dsd_signal_search_visit_clear(); /* the engine's teardown, after its last close */
    dsd_engine_signal_search_release_site(o, s);
    assert(tetra_resets == before + 1 && tetra_reset_code == CODE_B && !s->tetra_ext_scan_held);
    assert(code_held(s) == 0);
    dsd_engine_signal_search_release_site(o, s);
    assert(tetra_resets == before + 1);

    /* A search with no TETRA range never takes the hold or starts the TETRA state cold. */
    one = tag_config(config, 89, 1, 0);
    before = tetra_resets;
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    next_visit(o, s);
    stop_tag_session(o, s);
    assert(tetra_resets == before && !s->tetra_ext_scan_held);
    end_tetra_rate(o, s);
}

/* The decoder settings a receive profile puts in force, for comparing a pause's against a span tune's. */
typedef struct {
    int frame_tetra, mod_cli_lock, mod_c4fm, mod_qpsk, mod_gfsk, mod_p2lock;
    int rf_mod, sps_hunt_idx, sps, center;
    dsd_scan_mode row;
} decoder_profile;

static decoder_profile
decoder_profile_of(const dsd_opts* o, const dsd_state* s) {
    const decoder_profile p = {o->frame_tetra,      o->mod_cli_lock,           o->mod_c4fm,         o->mod_qpsk,
                               o->mod_gfsk,         o->mod_p25p2_profile_lock, s->rf_mod,           s->sps_hunt_idx,
                               s->samplesPerSymbol, s->symbolCenter,           dsd_scan_mode_row(s)};
    return p;
}

static int
decoder_profile_is(const dsd_opts* o, const dsd_state* s, const decoder_profile* want) {
    const decoder_profile now = decoder_profile_of(o, s);
    return memcmp(&now, want, sizeof(now)) == 0;
}

/* Issue #748: a Pause or a search error during a TETRA visit closes the visit and puts back the idle receive profile a
   span tune of the range runs (no TETRA decoder or scan class, the session's modulation lock and timing), while the
   search keeps the tuner. Paused while the controller owns the visit's tune, the tune keeps the profile it was queued
   with and its late landing changes nothing; Resume waits for it, then visits TETRA again. Under no lock and -m2. */
static void
test_tetra_pause_restores_idle_profile(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    static const struct {
        int lock, c4fm, qpsk, p2lock, rf_mod, profile;
    } locks[] = {
        {0, 1, 0, 0, 0, DSD_FRAME_SYNC_SPS_PROFILE_4800_4},
        {1, 0, 1, 1, 1, DSD_FRAME_SYNC_SPS_PROFILE_6000_4},
    };

    use_tetra_rate();
    for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); ++i) {
        o->mod_cli_lock = locks[i].lock;
        o->mod_c4fm = locks[i].c4fm;
        o->mod_qpsk = locks[i].qpsk;
        o->mod_gfsk = 0;
        o->mod_p25p2_profile_lock = locks[i].p2lock;
        put_timing(s, locks[i].profile, 15, 7, locks[i].rf_mod);
        const dsd_signal_search_config one = tetra_config(config, 64 + (uint64_t)i);
        start_tag_session(o, s, &one);
        /* The span tune's profile is the idle one. */
        const decoder_profile idle = decoder_profile_of(o, s);
        assert(idle.frame_tetra == 0 && idle.row != DSD_SCAN_MODE_TETRA_DMO);

        tetra_visit(o, s);
        assert(o->frame_tetra == 1 && s->rf_mod == 1 && dsd_scan_mode_row(s) == DSD_SCAN_MODE_TETRA_DMO);
        const int snapshots_before = snapshots;
        act(o, s, DSD_SEARCH_ACTION_PAUSE);
        assert(status.phase == DSD_SEARCH_PAUSED && decoder_profile_is(o, s, &idle));
        assert(snapshots > snapshots_before && tuner_owner == one.session_id && dsd_signal_search_is_running(s));
        act(o, s, DSD_SEARCH_ACTION_RESUME);
        tetra_visit(o, s);
        assert(o->frame_tetra == 1 && s->rf_mod == 1);

        /* Paused while the controller owns the next visit's tune. */
        act(o, s, DSD_SEARCH_ACTION_SKIP);
        for (int n = 0; n < 16 && !(status.phase == DSD_SEARCH_TUNING && !status.hit.ended); ++n) {
            if (status.phase == DSD_SEARCH_TUNING) {
                land(o, s);
            } else {
                ready = 1;
                tick(o, s);
            }
        }
        assert(status.phase == DSD_SEARCH_TUNING && !status.hit.ended && tune_pending && tetra_profile_queued());
        const uint32_t owned = ticket;
        act(o, s, DSD_SEARCH_ACTION_PAUSE);
        assert(status.phase == DSD_SEARCH_PAUSED && decoder_profile_is(o, s, &idle));
        assert(ticket == owned && tetra_profile_queued() && tuner_owner == one.session_id);
        land(o, s);
        assert(status.phase == DSD_SEARCH_PAUSED && decoder_profile_is(o, s, &idle));
        act(o, s, DSD_SEARCH_ACTION_RESUME);
        tetra_visit(o, s);
        assert(o->frame_tetra == 1);

        /* A search error during the visit. */
        stalled_ms = 20000;
        tick(o, s);
        assert(status.phase == DSD_SEARCH_ERROR && decoder_profile_is(o, s, &idle));
        assert(tuner_owner == one.session_id && dsd_signal_search_is_running(s));
        stalled_ms = 0;
        act(o, s, DSD_SEARCH_ACTION_STOP);
        land(o, s);
        assert(status.phase == DSD_SEARCH_STOPPED);
        assert(session_modulation_back(o, s, locks[i].lock, locks[i].c4fm, locks[i].qpsk, locks[i].p2lock,
                                       locks[i].rf_mod));
    }
    end_tetra_rate(o, s);
}

/* A start of @p config is refused with @p why and changes nothing: no boundary, tune, owner or running search. */
static void
start_refused(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config, const char* why) {
    const int before = boundary;
    const uint32_t tickets = ticket;
    char error[192] = "";
    assert(dsd_engine_signal_search_start_refused(o, s, config, error, sizeof(error)) < 0);
    assert(strcmp(error, why) == 0);
    error[0] = '\0';
    assert(dsd_engine_signal_search_start(o, s, config, observer, NULL, error, sizeof(error)) < 0);
    assert(strcmp(error, why) == 0);
    tick(o, s);
    assert(boundary == before && ticket == tickets && !dsd_signal_search_is_running(s) && !tuner_owner);
}

/* Issue #748: a search with an enabled TETRA range starts only where the TETRA chain can run, one reason per cause,
   checked before it changes anything: a published DSP rate TETRA runs at (an unpublished one is no refusal: the visit
   that lands where TETRA cannot run hears nothing and ends unresolved), the channel filter, the downlink and a TED
   timing the TETRA profile published is the rate's. A fresh start on a session that decodes TETRA is refused for any
   ranges, since the Auto baseline would free its decoder; a paused search's Resume never is. */
static void
test_tetra_start_checks(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    live = 1;
    failed = 0;
    stalled_ms = 0;
    const dsd_signal_search_config tetra = tetra_config(config, 90);
    const dsd_signal_search_config auto_only = tag_config(config, 90, 1, 0);
    dsp_rate_hz = 48000;
    start_refused(o, s, &tetra, "Identifying TETRA needs a 72 kHz DSP bandwidth, not 48 kHz");
    dsp_rate_hz = 72000;
    o->tetra_uplink = 1;
    start_refused(o, s, &tetra, "Identifying TETRA decodes the downlink, which --tetra-uplink turns off");
    o->tetra_uplink = 0;
    (void)dsd_test_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    start_refused(o, s, &tetra, "Identifying TETRA needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off");
    (void)dsd_test_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
    tetra_ted_override = 3;
    start_refused(o, s, &tetra,
                  "Identifying TETRA needs the TED timed at the DSP rate over 18000 symbols/s, and the stream runs "
                  "another timing");
    /* An override the rate's own is no refusal, and a rate the stream has not published yet is none either. */
    tetra_ted_override = 4;
    char error[192] = "";
    assert(dsd_engine_signal_search_start_refused(o, s, &tetra, error, sizeof(error)) == 0 && error[0] == '\0');
    tetra_ted_override = 0;
    /* The TED timing the published TETRA profile applied counts as well, with no override: 3 at 72 kHz is refused,
       the rate's own 4 is not. */
    tetra_ted_applied = 3;
    start_refused(o, s, &tetra,
                  "Identifying TETRA needs the TED timed at the DSP rate over 18000 symbols/s, and the stream runs "
                  "another timing");
    tetra_ted_applied = 4;
    assert(dsd_engine_signal_search_start_refused(o, s, &tetra, error, sizeof(error)) == 0 && error[0] == '\0');
    /* A stream settled on another receive profile than TETRA's (an Auto session's FSK chain, the usual start) is no
       refusal, whatever timing it runs: a visit lands TETRA's own chain and timing. */
    tetra_chain_other = 1;
    tetra_ted_applied = 3;
    assert(dsd_engine_signal_search_start_refused(o, s, &tetra, error, sizeof(error)) == 0 && error[0] == '\0');
    tetra_chain_other = 0;
    tetra_ted_applied = 0;
    dsp_rate_hz = 0;
    assert(dsd_engine_signal_search_start_refused(o, s, &tetra, error, sizeof(error)) == 0);
    /* The rate checks are TETRA's: an Auto search at 48 kHz starts. */
    dsp_rate_hz = 48000;
    assert(dsd_engine_signal_search_start_refused(o, s, &auto_only, error, sizeof(error)) == 0);
    /* A disabled TETRA range needs nothing. */
    dsd_signal_search_config off = tetra;
    off.range_count = 2;
    off.ranges[1] = auto_only.ranges[0];
    off.ranges[1].start_hz = 100100000;
    off.ranges[1].end_hz = 100125000;
    off.ranges[0].enabled = 0;
    assert(dsd_engine_signal_search_start_refused(o, s, &off, error, sizeof(error)) == 0);

    /* A session decoding TETRA: any fresh start is refused. */
    o->frame_tetra = 1;
    start_refused(o, s, &auto_only, "Stop TETRA decoding before Signal search");
    dsp_rate_hz = 72000;
    start_refused(o, s, &tetra, "Stop TETRA decoding before Signal search");
    o->frame_tetra = 0;

    /* A 48 kHz search paused with a TETRA range edited in: its Resume says how to identify TETRA, and the search stays
       paused as it was. A paused TETRA search's own Resume is never refused for its TETRA decoder. */
    dsp_rate_hz = 48000;
    start_tag_session(o, s, &auto_only);
    visit_strongest(o, s);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED);
    error[0] = '\0';
    assert(dsd_engine_signal_search_start(o, s, &tetra, observer, NULL, error, sizeof(error)) < 0);
    assert(strcmp(error, "To identify TETRA, stop the search and start it again (this clears the session's avoids "
                         "and learned Phase 2 codes)")
           == 0);
    assert(dsd_engine_signal_search_active(s) && status.phase == DSD_SEARCH_PAUSED);
    stop_tag_session(o, s);
    dsp_rate_hz = 72000;
    use_tetra_rate();
    const dsd_signal_search_config tetra_session = tetra_config(config, 91);
    start_tag_session(o, s, &tetra_session);
    tetra_visit(o, s);
    act(o, s, DSD_SEARCH_ACTION_PAUSE);
    assert(status.phase == DSD_SEARCH_PAUSED);
    o->frame_tetra = 1; /* whatever the decoder runs while paused */
    assert(dsd_engine_signal_search_start(o, s, &tetra_session, observer, NULL, error, sizeof(error)) == 0);
    assert(status.phase == DSD_SEARCH_SEARCHING);
    stop_tag_session(o, s);
    assert(o->frame_tetra == 0);
    o->frame_tetra = 0;
    end_tetra_rate(o, s);
}

/* Measure spans until the next detection is chosen, and land on it. */
static void
next_detection(dsd_opts* o, dsd_state* s) {
    for (int n = 0; n < 24; ++n) {
        if (status.phase == DSD_SEARCH_SEARCHING) {
            ready = 1;
            tick(o, s);
            if (status.phase == DSD_SEARCH_TUNING && status.hit.hit_id && !status.hit.ended) {
                land(o, s);
                return;
            }
        } else {
            assert(status.phase == DSD_SEARCH_TUNING);
            land(o, s);
        }
    }
    assert(0 && "no detection");
}

/* Confirm the visit as @p protocol and let it go quiet, back to its span. */
static void
confirm_and_leave(dsd_opts* o, dsd_state* s, const char* protocol) {
    dsd_signal_search_note_evidence(s, protocol, 0, 0, NULL);
    assert(status.hit.confirmed && strcmp(status.hit.protocol, protocol) == 0);
    advance(3000);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
}

/* Two one-channel ranges on 100.025 MHz: an Auto range and a TETRA range, in @p tetra_first order. */
static dsd_signal_search_config
overlap_config(const dsd_signal_search_config* base, uint64_t session, int tetra_first) {
    dsd_signal_search_config two = tag_config(base, session, 1, 0);
    two.range_count = 2;
    two.ranges[0].start_hz = two.ranges[0].end_hz = 100025000;
    two.ranges[1] = two.ranges[0];
    two.ranges[tetra_first ? 0 : 1].digital = DSD_SEARCH_DIGITAL_TETRA;
    two.ranges[tetra_first ? 0 : 1].bandwidth_hz = 25000;
    return two;
}

/* Issue #748: a pass suppresses a visited frequency per identification kind, TETRA or other, so an overlapping range
   of the other kind still visits it in the same pass, whichever range comes first. Operator Skip keeps its pass-wide
   meaning for every kind, avoids apply to both, and the next pass visits everything again. */
static void
test_tetra_suppression_per_kind(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    for (int tetra_first = 0; tetra_first < 2; ++tetra_first) {
        const char* first = tetra_first ? "TETRA" : "P25";
        const char* second = tetra_first ? "P25" : "TETRA";
        const dsd_signal_search_config two = overlap_config(config, 92 + (uint64_t)tetra_first, tetra_first);
        start_tag_session(o, s, &two);
        next_detection(o, s);
        assert(status.hit.range_index == 0 && status.hit.frequency_hz == 100025000 && status.pass == 1);
        confirm_and_leave(o, s, first);
        /* The other kind's range visits it in this pass. */
        next_detection(o, s);
        assert(status.hit.range_index == 1 && status.hit.frequency_hz == 100025000 && status.pass == 1);
        confirm_and_leave(o, s, second);
        /* Both kinds visited it: the pass wraps, and the next one visits it again. */
        next_detection(o, s);
        assert(status.hit.range_index == 0 && status.pass == 2);
        /* Skip suppresses it for the pass whatever the kind: the other range does not visit it. */
        act(o, s, DSD_SEARCH_ACTION_SKIP);
        next_detection(o, s);
        assert(status.hit.range_index == 0 && status.pass == 3);
        confirm_and_leave(o, s, first);
        next_detection(o, s);
        assert(status.hit.range_index == 1 && status.pass == 3);
        act(o, s, DSD_SEARCH_ACTION_SKIP);
        next_detection(o, s);
        assert(status.hit.range_index == 0 && status.pass == 4);
        /* An avoid takes it from both, for the session. */
        act(o, s, DSD_SEARCH_ACTION_AVOID);
        for (int n = 0; n < 40 && (status.pass < 6 || status.phase == DSD_SEARCH_TUNING); ++n) {
            if (status.phase == DSD_SEARCH_TUNING) {
                land(o, s);
            } else {
                ready = 1;
                tick(o, s);
            }
            assert(status.phase == DSD_SEARCH_TUNING || status.phase == DSD_SEARCH_SEARCHING);
            assert(!(status.phase == DSD_SEARCH_TUNING && !status.hit.ended));
        }
        assert(status.pass >= 6 && status.phase == DSD_SEARCH_SEARCHING);
        stop_tag_session(o, s);
    }
}

/* The visit is still listening (or held) by both expiration paths: the in-frame yield check and the tick. */
static int
still_listening(dsd_opts* o, dsd_state* s) {
    if (dsd_engine_signal_search_should_yield(o, s)) {
        return 0;
    }
    tick(o, s);
    return status.phase == DSD_SEARCH_LISTENING || status.phase == DSD_SEARCH_HELD;
}

/* Issue #748: a confirmed TETRA visit, trunked or direct mode, goes quiet 2.1 s after its last activity (one
   multiframe and a lost frame 18), in the in-frame yield check and the tick alike; the decoder's activity notes refresh
   that time on the decode clock without publishing, only while a confirmed visit receives. Hold keeps the visit, Skip
   leaves it, and a finite maximum visit still ends it; with none, activity keeps it. */
static void
test_tetra_quiet_and_activity(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    static const char* const modes[] = {"TETRA", "TETRA DMO"};
    use_tetra_rate();
    for (int m = 0; m < 2; ++m) {
        for (int yield_path = 0; yield_path < 2; ++yield_path) {
            dsd_signal_search_config one = tetra_config(config, 94 + (uint64_t)(m * 2 + yield_path));
            start_tag_session(o, s, &one);
            tetra_visit(o, s);
            /* Before confirmation an activity note keeps nothing: the probe runs its own time. */
            dsd_signal_search_note_activity(s);
            assert(status.phase == DSD_SEARCH_IDENTIFYING && !status.hit.confirmed);
            dsd_signal_search_note_evidence_at(s, modes[m], DSD_SYNC_TETRA_SB_POS, -1, 0, 0, NULL);
            assert(status.phase == DSD_SEARCH_LISTENING);
            /* A lost multiframe (1.02 s) and more: still the visit's. */
            advance(2099);
            if (yield_path) {
                assert(!dsd_engine_signal_search_should_yield(o, s));
            } else {
                tick(o, s);
                assert(status.phase == DSD_SEARCH_LISTENING);
            }
            /* An activity note refreshes the time without a publication. */
            const uint64_t revision = status.revision;
            const int published = hits_published;
            dsd_signal_search_note_activity(s);
            assert(status.revision == revision && hits_published == published);
            advance(2099);
            if (yield_path) {
                assert(!dsd_engine_signal_search_should_yield(o, s));
            } else {
                tick(o, s);
                assert(status.phase == DSD_SEARCH_LISTENING);
            }
            /* One from a frame an input interruption cut refreshes nothing. */
            s->input_interrupted = 1;
            dsd_signal_search_note_activity(s);
            s->input_interrupted = 0;
            advance(1);
            if (yield_path) {
                assert(dsd_engine_signal_search_should_yield(o, s));
            }
            tick(o, s);
            assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
            land(o, s);
            stop_tag_session(o, s);
        }
    }

    /* Hold keeps a quiet confirmed visit, Skip leaves it. */
    dsd_signal_search_config one = tetra_config(config, 98);
    start_tag_session(o, s, &one);
    tetra_visit(o, s);
    dsd_signal_search_note_evidence_at(s, "TETRA", DSD_SYNC_TETRA_SB_POS, -1, 0, 0, NULL);
    act(o, s, DSD_SEARCH_ACTION_HOLD);
    advance(30000);
    assert(still_listening(o, s) && status.phase == DSD_SEARCH_HELD);
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    land(o, s);
    stop_tag_session(o, s);

    /* A finite maximum visit ends a visit its activity keeps; with none, the activity keeps it. */
    one = tetra_config(config, 99);
    one.ranges[0].max_visit_ms = 5000;
    start_tag_session(o, s, &one);
    tetra_visit(o, s);
    dsd_signal_search_note_evidence_at(s, "TETRA", DSD_SYNC_TETRA_SB_POS, -1, 0, 0, NULL);
    for (int i = 0; i < 4; ++i) {
        advance(1000);
        dsd_signal_search_note_activity(s);
        assert(still_listening(o, s));
    }
    advance(999);
    assert(still_listening(o, s));
    advance(1);
    assert(dsd_engine_signal_search_should_yield(o, s));
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended);
    land(o, s);
    stop_tag_session(o, s);
    one.ranges[0].max_visit_ms = 0;
    start_tag_session(o, s, &one);
    tetra_visit(o, s);
    dsd_signal_search_note_evidence_at(s, "TETRA", DSD_SYNC_TETRA_SB_POS, -1, 0, 0, NULL);
    for (int i = 0; i < 60; ++i) {
        advance(2000);
        dsd_signal_search_note_activity(s);
        assert(still_listening(o, s));
    }
    stop_tag_session(o, s);

    /* Another protocol keeps its own quiet time. */
    one = tag_config(config, 97, 1, 0);
    start_tag_session(o, s, &one);
    visit_strongest(o, s);
    dsd_signal_search_note_evidence(s, "P25", 0, 0, NULL);
    advance(499);
    assert(still_listening(o, s));
    advance(1);
    assert(dsd_engine_signal_search_should_yield(o, s));
    stop_tag_session(o, s);
    end_tetra_rate(o, s);
}

static dsd_signal_search_site_note
tetra_tmo_note(void) {
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_TETRA;
    note.tetra.mode = DSD_SEARCH_TETRA_TMO;
    note.tetra.mcc = 228;
    note.tetra.mnc = 8889;
    note.tetra.la = 12;
    note.tetra.la_valid = 1;
    note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_HZ;
    note.tetra.main_hz = 426706250;
    return note;
}

/* Issue #748: a TETRA detection shows the site its own notes give, by mode ("TETRA DMO" never takes a trunked cell's
   record), with the access code its evidence carries; no receive filter applies to TETRA. A withdrawal clears site
   and access together, publishes at once, and keeps them cleared for the rest of the visit: no later note or evidence
   of the visit, and no repeated withdrawal, brings them back or publishes again. */
static void
test_tetra_site_and_withdraw(dsd_opts* o, dsd_state* s, const dsd_signal_search_config* config) {
    static const char tmo_text[] = "Main carrier 426.70625 MHz \xC2\xB7 MCC 228 \xC2\xB7 MNC 8889 \xC2\xB7 LA 12";
    use_tetra_rate();
    /* Filters on every kind that rejects everything they can name: none names TETRA. */
    o->receive.filters[DSD_RECEIVE_DCC].enabled = 1;
    o->receive.filters[DSD_RECEIVE_DCC].allow.bits[0] = UINT64_C(1) << 15;
    o->receive.filters[DSD_RECEIVE_NAC].enabled = 1;
    o->receive.filters[DSD_RECEIVE_NAC].allow.bits[0] = UINT64_C(1) << 1;
    o->receive.filters[DSD_RECEIVE_RAN].enabled = 1;
    o->receive.filters[DSD_RECEIVE_RAN].allow.bits[0] = UINT64_C(1) << 1;
    o->receive.filters[DSD_RECEIVE_SLOT].enabled = 1;
    o->receive.filters[DSD_RECEIVE_SLOT].allow.bits[0] = UINT64_C(1) << 2;
    dsd_signal_search_config one = tetra_config(config, 70);
    start_tag_session(o, s, &one);
    tetra_visit(o, s);
    s->synctype = DSD_SYNC_TETRA_SB_POS;
    dsd_signal_search_site_note tmo = tetra_tmo_note();
    /* A site noted before the confirmation shows with it. */
    dsd_signal_search_note_site(s, &tmo);
    assert(status.hit.site[0] == '\0');
    dsd_signal_search_note_evidence_at(s, "TETRA", DSD_SYNC_TETRA_SB_POS, 1, 0, 0, "CC 5");
    assert(status.hit.confirmed && strcmp(status.hit.site, tmo_text) == 0 && strcmp(status.hit.access, "CC 5") == 0);
    assert(strcmp(published_hit.site, tmo_text) == 0);
    /* A changed SYSINFO publishes the new line; a repeat publishes nothing. */
    tmo.tetra.main_state = DSD_SEARCH_TETRA_MAIN_NO_CA;
    int published = hits_published;
    dsd_signal_search_note_site(s, &tmo);
    assert(hits_published == published + 1);
    assert(strcmp(status.hit.site, "No main carrier \xC2\xB7 MCC 228 \xC2\xB7 MNC 8889") == 0);
    dsd_signal_search_note_site(s, &tmo);
    assert(hits_published == published + 1);

    /* Withdrawn: site and access together, at once. */
    dsd_signal_search_site_note withdraw;
    DSD_MEMSET(&withdraw, 0, sizeof(withdraw));
    withdraw.kind = DSD_SEARCH_SITE_NOTE_TETRA_WITHDRAW;
    published = hits_published;
    dsd_signal_search_note_site(s, &withdraw);
    assert(hits_published == published + 1 && published_hit.site[0] == '\0' && published_hit.access[0] == '\0');
    assert(status.hit.confirmed && strcmp(status.hit.protocol, "TETRA") == 0);
    /* Nothing of the visit brings them back, and a repeat publishes nothing. */
    dsd_signal_search_note_site(s, &withdraw);
    tmo = tetra_tmo_note();
    dsd_signal_search_note_site(s, &tmo);
    assert(hits_published == published + 1);
    dsd_signal_search_note_evidence_at(s, "TETRA", DSD_SYNC_TETRA_NDB_POS, -1, 0, 0, "CC 5");
    assert(status.hit.site[0] == '\0' && status.hit.access[0] == '\0' && published_hit.access[0] == '\0');
    /* Nor the ended hit. */
    advance(2100);
    tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && published_hit.ended);
    assert(published_hit.site[0] == '\0' && published_hit.access[0] == '\0');
    /* The next detection starts with nothing withdrawn. */
    land(o, s);
    tetra_visit(o, s);
    dsd_signal_search_note_evidence_at(s, "TETRA", DSD_SYNC_TETRA_SB_POS, -1, 0, 0, "CC 6");
    dsd_signal_search_note_site(s, &tmo);
    assert(strcmp(status.hit.site, tmo_text) == 0 && strcmp(status.hit.access, "CC 6") == 0);
    /* A withdrawal with no site shown still clears the access. */
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    tetra_visit(o, s);
    dsd_signal_search_note_evidence_at(s, "TETRA", DSD_SYNC_TETRA_SB_POS, -1, 0, 0, "CC 7");
    published = hits_published;
    dsd_signal_search_note_site(s, &withdraw);
    assert(hits_published == published + 1 && status.hit.access[0] == '\0' && published_hit.access[0] == '\0');

    /* Direct mode: the route, never a trunked cell's record. */
    act(o, s, DSD_SEARCH_ACTION_SKIP);
    tetra_visit(o, s);
    dsd_signal_search_note_site(s, &tmo);
    dsd_signal_search_note_evidence_at(s, "TETRA DMO", DSD_SYNC_TETRA_SB_POS, -1, 0, 0, NULL);
    assert(strcmp(status.hit.protocol, "TETRA DMO") == 0 && status.hit.site[0] == '\0');
    dsd_signal_search_site_note dmo;
    DSD_MEMSET(&dmo, 0, sizeof(dmo));
    dmo.kind = DSD_SEARCH_SITE_NOTE_TETRA;
    dmo.tetra.mode = DSD_SEARCH_TETRA_DMO;
    dmo.tetra.route = DSD_SEARCH_TETRA_ROUTE_REPEATER;
    dsd_signal_search_note_site(s, &dmo);
    assert(strcmp(status.hit.site, "Via repeater") == 0 && status.hit.access[0] == '\0');
    dsd_signal_search_note_site(s, &withdraw);
    assert(status.hit.site[0] == '\0');
    dmo.tetra.route = DSD_SEARCH_TETRA_ROUTE_DIRECT;
    dsd_signal_search_note_site(s, &dmo);
    assert(status.hit.site[0] == '\0');

    s->synctype = DSD_SYNC_NONE;
    DSD_MEMSET(&o->receive, 0, sizeof(o->receive));
    stop_tag_session(o, s);
    end_tetra_rate(o, s);
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
    test_fixed_tuning_refused_before_a_boundary(o, s, &config);
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
    test_restore_hands_back_the_analog_default_first(o, s, &config);
    test_capture_freshness(o, s, &config);
    test_request_failure_messages(o, s, &config);
    test_clipping_span_skipped(o, s);
    test_clipping_deadline_skips(o, s);
    test_unclipped_deadline_fails(o, s);
    test_completed_report_beats_deadlines(o, s);
    test_clipping_keeps_searching(o, s);
    test_overload_clears(o, s);
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
    test_deferred_visit_tune_stays_pending(o, s, &config);
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
    test_refusal_then_resume_reaches_frontend(o, s, &config);
    test_failed_start_changes_nothing(o, s, &config);
    test_m17_call_free_lsf_can(o, s, &config);
    test_access_note_left_family(o, s, &config);
    test_access_call_left_family(o, s, &config);
    test_access_sync_without_confirm(o, s, &config);
    test_access_reacquired_call(o, s, &config);
    test_stop_queues_restored_profile(o, s, &config);
    test_tetra_protocol_latched(o, s, &config);
    test_tetra_visit_lands_tetra_profile(o, s, &config);
    test_tetra_visit_resets_on_departures(o, s, &config);
    test_tetra_pause_restores_idle_profile(o, s, &config);
    test_tetra_start_checks(o, s, &config);
    test_tetra_suppression_per_kind(o, s, &config);
    test_tetra_quiet_and_activity(o, s, &config);
    test_tetra_site_and_withdraw(o, s, &config);
    s->rtl_ctx = NULL;
    dsd_state_ext_free_all(s);
    free(s);
    free(o);
    dsd_decode_clock_use_system();
    return 0;
}
