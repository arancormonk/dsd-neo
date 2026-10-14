// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "scan_owner.h"
#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#endif

enum {
    /* A listening visit ends this long after its last relevant activity. */
    SEARCH_QUIET_MS = 500,
    /* An analog visit whose monitor has not judged the landed channel yet keeps waiting for a verdict this long:
       the front end may be designing a noise-squelch plan before it delivers any sample. */
    SEARCH_ANALOG_VERDICT_MAX_MS = 3000,
    /* Live strength while listening: one measurement this often; longer after a geometry that cannot be measured. */
    SEARCH_LIVE_INTERVAL_MS = 250,
    SEARCH_LIVE_RETRY_MS = 1000
};

typedef struct {
    dsd_signal_search_config config;
    dsd_signal_search_status status;
    dsd_scan_settings saved;
    dsd_signal_search_publish_fn publish;
    void* user;
    uint32_t saved_frequency;
    int saved_gain, saved_gain_auto, saved_autogain;
    dsd_airspy_config saved_airspy;
    uint32_t suppressed[DSD_SEARCH_MAX_CHANNELS];
    size_t suppressed_count;
    uint64_t request, next_hit, acquired, probe_started, activity, tune_started, last_publish;
    uint64_t measurement_started;
    /* Decode-clock landing of the current visit; quiet timing of an analog visit starts at the monitor's first verdict. */
    uint64_t landed;
    int hold, skip, need_span, visit, measuring, stopping, probing, restored, restore_queued;
    int analog_visit, listen_ready;
    /* Paused or failed, the search has ended what the decoder was receiving (search_end_reception()). */
    int reception_ended;
    uint32_t ticket, target, before_generation;
#ifdef USE_RADIO
    dsd_signal_search_span span;
    dsd_signal_measure_report report;
    int report_pending;
    int live_measuring;
    uint64_t live_request, live_next;
#endif
} search_coord;

static search_coord*
search_get(const dsd_state* state) {
    return (search_coord*)dsd_engine_scan_owned(state, 1);
}

static void
search_publish(search_coord* c, int hit_changed) {
#ifdef USE_RADIO
    if (c->report_pending && c->report.received_realtime_ns) {
        c->status.measurement_latency_us = (dsd_realtime_mono_ns() - c->report.received_realtime_ns) / 1000;
    }
    c->report_pending = 0;
#endif
    c->status.identifying = c->probing && !c->status.hit.ended;
    c->status.owned = c->status.phase != DSD_SEARCH_STOPPED;
    ++c->status.revision;
    c->last_publish = dsd_realtime_mono_ms();
    if (c->publish) {
        c->publish(&c->status, hit_changed ? &c->status.hit : NULL, c->user);
    }
}

int
dsd_engine_signal_search_active(const dsd_state* state) {
    const search_coord* c = search_get(state);
    return c && c->status.phase != DSD_SEARCH_STOPPED;
}

static void
search_end_hit(search_coord* c) {
    if (c->status.hit.hit_id && !c->status.hit.ended) {
        c->status.hit.ended = 1;
        c->status.hit.ended_ms = (uint64_t)(dsd_decode_now_realtime_s() * 1000);
        search_publish(c, 1);
    }
}

#ifdef USE_RADIO
/* Retire every outstanding detector request, the search measurement and the live strength alike. */
static void
search_cancel_measurements(search_coord* c) {
    rtl_signal_measure_cancel();
    c->measuring = 0;
    c->live_measuring = 0;
    c->live_next = 0;
    c->status.live_power_valid = 0;
}

static void
search_error(search_coord* c, const char* message) {
    search_end_hit(c);
    search_cancel_measurements(c);
    c->stopping = 0;
    rtl_signal_search_bypass(1);
    c->status.phase = DSD_SEARCH_ERROR;
    DSD_SNPRINTF(c->status.message, sizeof(c->status.message), "%s", message);
    search_publish(c, 0);
}

static int
search_contains(const uint32_t* values, size_t n, uint32_t value) {
    for (size_t i = 0; i < n; ++i) {
        if (values[i] == value) {
            return 1;
        }
    }
    return 0;
}

static int
search_excluded(const search_coord* c, uint32_t f) {
    return search_contains(c->config.avoids, c->config.avoid_count, f)
           || search_contains(c->suppressed, c->suppressed_count, f);
}

static void
search_suppress(search_coord* c, uint32_t f) {
    if (!search_contains(c->suppressed, c->suppressed_count, f) && c->suppressed_count < DSD_SEARCH_MAX_CHANNELS) {
        c->suppressed[c->suppressed_count++] = f;
    }
}

static int
search_select_range(search_coord* c) {
    for (uint32_t tries = 0; tries <= c->config.range_count; ++tries) {
        if (c->status.range_index == c->config.range_count) {
            c->status.range_index = 0;
            ++c->status.pass;
            c->suppressed_count = 0;
        }
        const dsd_signal_search_range* r = &c->config.ranges[c->status.range_index];
        if (r->enabled && c->status.channel_index < dsd_signal_search_channel_count(r)) {
            c->status.channel_count = dsd_signal_search_channel_count(r);
            return 1;
        }
        ++c->status.range_index;
        c->status.channel_index = 0;
    }
    return 0;
}

static int
search_apply_airspy(const search_coord* c, dsd_opts* opts) {
    const dsd_signal_search_range* r = &c->config.ranges[c->status.range_index];
    if (dsd_opts_audio_in_dev_is_airspy_spec(opts->audio_in_dev)) {
        dsd_airspy_config gain = r->airspy_gain_set ? r->airspy : c->saved_airspy;
        DSD_MEMCPY(gain.serial, c->saved_airspy.serial, sizeof(gain.serial));
        gain.sample_rate = c->saved_airspy.sample_rate;
        gain.bias_tee = c->saved_airspy.bias_tee;
        if (rtl_stream_airspy_controls(&gain)) {
            return -1;
        }
        opts->airspy = gain;
    }
    return 0;
}

static void
search_stage_gain(const search_coord* c, const dsd_opts* opts, const dsd_state* state, uint32_t target) {
    const dsd_signal_search_range* r = &c->config.ranges[c->status.range_index];
    dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(opts, state);
    rtl_stream_retune_gain_profile gain = {0};
    gain.tuner_gain_is_set = 1;
    gain.tuner_gain_tenth_db = r->gain_set ? r->gain_tenth_db : c->saved_gain;
    gain.tuner_gain_is_auto = r->gain_set ? r->gain_auto : c->saved_gain_auto;
    gain.tuner_autogain_is_set = 1;
    gain.tuner_autogain_on = c->visit ? c->saved_autogain : 0;
    if (r->gain_set && !r->gain_auto) {
        gain.tuner_autogain_on = 0;
    }
    rtl_stream_prepare_retune_profile_for_target_with_gain(
        target, state->rf_mod == 1, profile.symbol_rate_hz, profile.levels,
        dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, state->rf_mod),
        state->samplesPerSymbol, 0, &gain);
}

static int
search_open_audio(const search_coord* c, dsd_opts* opts, int analog) {
    if (!c->visit) {
        return 0;
    }
    return analog ? dsd_audio_ensure_analog_output(opts) : dsd_audio_ensure_digital_output(opts);
}

static int
search_prepare_profile(search_coord* c, dsd_opts* opts, dsd_state* state, uint32_t target, int analog) {
    const dsd_signal_search_range* r = &c->config.ranges[c->status.range_index];
    if (search_apply_airspy(c, opts)) {
        return -1;
    }
    dsd_scan_mode mode = analog ? (r->am ? DSD_SCAN_MODE_AM : DSD_SCAN_MODE_NFM)
                                : (r->digital > 1 ? (dsd_scan_mode)(r->digital - 1) : DSD_SCAN_MODE_INHERIT);
    if (dsd_scan_mode_enter(opts, state, mode)) {
        return -1;
    }
    dsd_scan_option_values values;
    DSD_MEMSET(&values, 0, sizeof(values));
    values.present = DSD_SCAN_OPT_SQUELCH | DSD_SCAN_OPT_TONE;
    values.squelch_mode = analog ? r->squelch_mode : DSD_SQUELCH_MODE_LEVEL;
    values.squelch_margin_db = r->squelch_margin_db;
    values.squelch_db = analog ? r->squelch_dbfs : 0;
    if (analog) {
        values.present |= DSD_SCAN_OPT_BANDWIDTH;
        values.channel_bw_hz = (int)r->bandwidth_hz;
        values.channel_bw_kind = r->am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
    }
    if (dsd_scan_mode_options(opts, state, &values)) {
        return -1;
    }
    if (search_open_audio(c, opts, analog) < 0) {
        return -1;
    }
    search_stage_gain(c, opts, state, target);
    rtl_stream_retune_analog_profile family = {0};
    family.family = analog ? DSD_RX_FAMILY_ANALOG : DSD_RX_FAMILY_DIGITAL;
    family.kind = r->am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
    family.width_hz = (int)r->bandwidth_hz;
    family.cqpsk_explicit = 1;
    return rtl_stream_prepare_retune_analog_profile_for_target(target, &family);
}

static int
search_queue_tune(search_coord* c, dsd_opts* opts, dsd_state* state, uint32_t target, int visit, int analog) {
    search_cancel_measurements(c);
    c->measurement_started = 0;
    rtl_signal_search_bypass(1);
    c->visit = visit;
    c->target = target;
    c->probing = visit && !analog;
    c->analog_visit = visit && analog;
    c->listen_ready = 0;
    c->before_generation = rtl_stream_output_generation();
    c->ticket = 0;
    c->tune_started = dsd_realtime_mono_ms();
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    dsd_frame_sync_reset_acquisition(opts, state, 1);
    c->reception_ended = 0;
    if (search_prepare_profile(c, opts, state, target, analog)) {
        search_error(c, "The receiver cannot apply this range's receive profile");
        return -1;
    }
    int rc = rtl_stream_tune_async(target, c->status.session_id, &c->ticket);
    if (rc < 0) {
        search_error(c, "The receiver refused the tune");
        return -1;
    }
    c->status.phase = DSD_SEARCH_TUNING;
    search_publish(c, 0);
    return 0;
}

static void
search_return_to_span(search_coord* c, dsd_opts* opts, dsd_state* state) {
    search_end_hit(c);
    c->hold = c->skip = 0;
    (void)search_queue_tune(c, opts, state, c->span.tune_hz, 0, 0);
}

static int
search_begin_measurement(search_coord* c, const dsd_signal_capture* capture) {
    dsd_signal_measure_request r;
    DSD_MEMSET(&r, 0, sizeof(r));
    r.session_id = c->status.session_id;
    r.request_id = ++c->request;
    r.capture = *capture;
    r.range = c->config.ranges[c->status.range_index];
    r.count = c->span.count;
    DSD_MEMCPY(r.frequencies, c->span.frequencies, r.count * sizeof(r.frequencies[0]));
    if (rtl_signal_measure_request(&r)) {
        search_error(c, "Unsupported FFT measurement plan");
        return -1;
    }
    c->measuring = 1;
    if (!c->measurement_started) {
        c->measurement_started = dsd_realtime_mono_ms();
    }
    return 0;
}

static void
search_restore(search_coord* c, dsd_opts* opts, dsd_state* state) {
    if (c->restored) {
        return;
    }
    dsd_scan_mode_leave(opts, state);
    dsd_scan_settings_restore(&c->saved, opts, state);
    rtl_stream_enforce_tuner_autogain(c->saved_autogain);
    if (dsd_opts_audio_in_dev_is_airspy_spec(opts->audio_in_dev)) {
        (void)rtl_stream_airspy_controls(&c->saved_airspy);
        opts->airspy = c->saved_airspy;
    }
    const int analog = dsd_opts_is_analog_family(opts);
    (void)rtl_stream_request_analog_profile(
        analog ? DSD_RX_FAMILY_ANALOG : DSD_RX_FAMILY_DIGITAL, opts->analog_demod,
        analog
            ? (opts->analog_demod == DSD_ANALOG_DEMOD_AM ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz)
            : 0);
    c->restored = 1;
}

static int
search_valid_config(const dsd_signal_search_config* config) {
    return config && config->session_id && config->range_count && config->range_count <= DSD_SEARCH_MAX_RANGES
           && config->avoid_count <= DSD_SEARCH_MAX_AVOIDS;
}

static int
search_live_receiver(const dsd_opts* opts, const dsd_state* state) {
    return opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx && rtl_stream_is_active() && !opts->trunk_enable
           && !opts->scanner_mode && !opts->trunk_scan_enabled && dsd_decode_clock_source() != DSD_DECODE_CLOCK_REPLAY;
}

static dsd_signal_search_phase
search_reception_phase(const search_coord* c) {
    return c->hold ? DSD_SEARCH_HELD : (c->probing ? DSD_SEARCH_IDENTIFYING : DSD_SEARCH_LISTENING);
}

static int
search_validate_start(const dsd_opts* opts, const dsd_state* state, const dsd_signal_search_config* config, char* error,
                      size_t capacity) {
    const char* why = NULL;
    if (!opts || !state || !search_valid_config(config)) {
        why = "Invalid signal search configuration";
    } else if (!search_live_receiver(opts, state)) {
        why = "Signal search needs exclusive use of a live tunable SDR";
    } else if (dsd_engine_signal_search_active(state)
               && (search_get(state)->status.phase != DSD_SEARCH_PAUSED
                   || search_get(state)->status.session_id != config->session_id)) {
        why = "Pause Signal search before applying edited ranges";
    }
    if (why) {
        if (error && capacity) {
            DSD_SNPRINTF(error, capacity, "%s", why);
        }
        return -1;
    }
    return dsd_signal_search_validate_config(config, error, capacity);
}

#endif

int
dsd_engine_signal_search_start(dsd_opts* opts, dsd_state* state, const dsd_signal_search_config* config,
                               dsd_signal_search_publish_fn observer, void* user, char* error, size_t capacity) {
#ifndef USE_RADIO
    (void)opts;
    (void)state;
    (void)config;
    (void)observer;
    (void)user;
    if (error && capacity) {
        DSD_SNPRINTF(error, capacity, "%s", "Signal search needs a radio-enabled build");
    }
    return -1;
#else
    if (search_validate_start(opts, state, config, error, capacity)) {
        return -1;
    }
    search_coord* previous = search_get(state);
    if (previous && previous->status.phase == DSD_SEARCH_PAUSED) {
        /* Keep the native session's avoids, including those made before an Activity recreation. */
        previous->config.range_count = config->range_count;
        DSD_MEMCPY(previous->config.ranges, config->ranges, sizeof(config->ranges));
        previous->status.range_index = previous->status.channel_index = 0;
        ++previous->status.pass;
        previous->suppressed_count = 0;
        previous->need_span = 1;
        previous->visit = previous->skip = previous->hold = 0;
        previous->status.phase = DSD_SEARCH_SEARCHING;
        search_publish(previous, 0);
        return 0;
    }
    search_coord* c = (search_coord*)calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    c->config = *config;
    c->publish = observer;
    c->user = user;
    c->status.session_id = config->session_id;
    c->status.pass = 1;
    c->status.phase = DSD_SEARCH_SEARCHING;
    c->need_span = 1;
    c->tune_started = dsd_realtime_mono_ms();
    c->saved_frequency = opts->rtlsdr_center_freq;
    dsd_scan_settings_capture(opts, state, &c->saved);
    (void)rtl_stream_get_gain(&c->saved_gain, &c->saved_gain_auto);
    c->saved_autogain = rtl_stream_get_tuner_autogain();
    c->saved_airspy = opts->airspy;
    /* Explicit Auto is the session baseline; ranges constrain it to existing receive classes. The CONFIG profile leaves
       the DMR dispatch flags as the session had them, and a mono preset (-f1) turns both off, which reads no direct-mode
       voice frame: Auto then takes the dual-slot dispatch every other Auto path selects. The saved settings restore the
       session's own flags. */
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CONFIG, opts, state);
    if (!opts->dmr_stereo && !opts->dmr_mono) {
        opts->dmr_stereo = 1;
    }
    if (dsd_scan_mode_begin(opts, state) || dsd_engine_scan_own(state, 1, c, free)) {
        dsd_scan_mode_leave(opts, state);
        dsd_scan_settings_restore(&c->saved, opts, state);
        free(c);
        return -1;
    }
    dsd_signal_search_evidence_set(dsd_engine_signal_search_evidence);
    dsd_signal_search_active_set(dsd_engine_signal_search_active);
    rtl_signal_search_set_owner(c->status.session_id);
    rtl_signal_search_bypass(1);
    search_publish(c, 0);
    return 0;
#endif
}

static void
search_identified_protocol(search_coord* c, const dsd_state* state, const char* protocol) {
    /* NXDN's sync word is shared by both rates. Preserve the profile that actually
       confirmed so saving an NXDN48 hit cannot select the NXDN96 decoder. */
    const char* identified = protocol;
    if (strcmp(protocol, "NXDN") == 0) {
        identified = state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4 ? "NXDN48" : "NXDN96";
    }
    dsd_signal_search_hit* hit = &c->status.hit;
    if (strcmp(hit->protocol, identified) != 0) {
        hit->target = hit->source = 0;
        hit->access[0] = hit->source_text[0] = hit->target_text[0] = '\0';
    }
    DSD_SNPRINTF(hit->protocol, sizeof(hit->protocol), "%s", identified);
}

void
dsd_engine_signal_search_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                                  const char* access) {
    search_coord* c = search_get(state);
    /* Evidence noted while an input interruption stands comes from a frame it cut, whose rest is erasures getSymbol()
       stood in: the decoders drop such a frame, and this refuses whatever one still reports. */
    if (!c || !c->visit || c->status.hit.ended || state->input_interrupted
        || (c->status.phase != DSD_SEARCH_IDENTIFYING && c->status.phase != DSD_SEARCH_LISTENING
            && c->status.phase != DSD_SEARCH_HELD)) {
        return;
    }
    c->activity = dsd_decode_now_mono_ms();
    c->probing = 0;
    c->status.hit.confirmed = 1;
    c->status.hit.unresolved = 0;
    search_identified_protocol(c, state, protocol);
    if (!c->hold) {
        c->status.phase = DSD_SEARCH_LISTENING;
    }
    if (target) {
        c->status.hit.target = target;
    }
    if (source) {
        c->status.hit.source = source;
    }
    if (access && access[0]) {
        DSD_SNPRINTF(c->status.hit.access, sizeof(c->status.hit.access), "%s", access);
    }
    search_publish(c, 1);
}

#ifdef USE_RADIO
/* The landed channel's own RF power while listening, measured by the capture-rate detector on the same dBFS scale as
   the detection. Only a capture that is the landed tune is measured; a report is accepted for its own request. Both the
   main-loop tick and the in-frame yield check service it: a continuous digital transmission keeps the decoder in its
   synced-frame loop, where only the latter runs. */
static void
search_live_strength(search_coord* c) {
    const uint64_t now = dsd_realtime_mono_ms();
    if (c->live_measuring) {
        dsd_signal_measure_report report;
        const int rc = rtl_signal_measure_get(&report);
        if (rc == 0) {
            return;
        }
        c->live_measuring = 0;
        c->live_next = now + SEARCH_LIVE_INTERVAL_MS;
        if (rc == 1 && report.session_id == c->status.session_id && report.request_id == c->live_request
            && report.count == 1) {
            if (report.candidates[0].valid) {
                c->status.live_power_dbfs = report.candidates[0].power_dbfs;
                c->status.live_power_valid = 1;
                search_publish(c, 0);
            } else {
                c->live_next = now + SEARCH_LIVE_RETRY_MS;
            }
        }
        return;
    }
    if (now < c->live_next) {
        return;
    }
    dsd_signal_capture capture;
    if (!rtl_signal_capture_get(&capture) || capture.generation != rtl_stream_output_generation()
        || (int64_t)capture.capture_hz - capture.capture_offset_hz != (int64_t)c->status.hit.frequency_hz) {
        c->live_next = now + SEARCH_LIVE_INTERVAL_MS;
        return;
    }
    dsd_signal_measure_request r;
    DSD_MEMSET(&r, 0, sizeof(r));
    r.session_id = c->status.session_id;
    r.request_id = c->live_request = ++c->request;
    r.capture = capture;
    r.range = c->config.ranges[c->status.range_index];
    r.count = 1;
    r.frequencies[0] = c->status.hit.frequency_hz;
    if (rtl_signal_measure_request(&r) == 0) {
        c->live_measuring = 1;
    } else {
        c->live_next = now + SEARCH_LIVE_RETRY_MS;
    }
}

#endif

/* Listening activity, refreshed on the decoder thread by both expiration paths. A carrier the monitor holds is
   activity. An analog visit starts quiet timing only once the monitor has judged the landed channel, or at a bounded
   deadline: until its first samples reach the tap (a front end still designing its noise-squelch plan delivers none),
   a closed carrier says nothing about the channel. */
static void
search_note_activity(search_coord* c, const dsd_opts* opts, const dsd_state* state, uint64_t now) {
    if (dsd_scan_analog_carrier_open(opts, state)) {
        c->activity = now;
        c->listen_ready = 1;
        return;
    }
    if (!c->analog_visit || c->listen_ready) {
        return;
    }
    if (dsd_analog_rx_carrier_observed_now(opts, state) || now - c->landed >= SEARCH_ANALOG_VERDICT_MAX_MS) {
        c->listen_ready = 1;
    }
    c->activity = now;
}

static int
search_visit_expired(const search_coord* c, uint64_t now) {
    if (c->hold) {
        return 0;
    }
    const dsd_signal_search_range* r = &c->config.ranges[c->status.range_index];
    return (r->max_visit_ms && now - c->acquired >= (uint64_t)r->max_visit_ms)
           || (!c->probing && now - c->activity >= SEARCH_QUIET_MS);
}

int
// cppcheck-suppress constParameterPointer -- Fixed runtime hook signature uses mutable decoder state.
dsd_engine_signal_search_should_yield(const dsd_opts* opts, dsd_state* state) {
    search_coord* c = search_get(state);
    if (!c || c->status.phase == DSD_SEARCH_STOPPED) {
        return 0;
    }
    if (dsd_runtime_controls_pending() || c->skip || c->stopping) {
        return 1;
    }
    if (c->status.phase == DSD_SEARCH_PAUSED || c->status.phase == DSD_SEARCH_TUNING
        || c->status.phase == DSD_SEARCH_SEARCHING || c->status.phase == DSD_SEARCH_ERROR) {
        return 1;
    }
    uint64_t now = dsd_decode_now_mono_ms();
    const dsd_signal_search_range* r = &c->config.ranges[c->status.range_index];
    search_note_activity(c, opts, state, now);
#ifdef USE_RADIO
    if (c->visit) {
        search_live_strength(c);
    }
#endif
    if (c->probing && now - c->probe_started >= (uint64_t)r->probe_ms) {
        return 1;
    }
    return search_visit_expired(c, now);
}

static int
search_valid_action(const search_coord* c, const dsd_signal_search_control* action) {
    if (!action || action->session_id != c->status.session_id || c->status.phase == DSD_SEARCH_STOPPED) {
        return 0;
    }
    if (action->action == DSD_SEARCH_ACTION_UNAVOID) {
        return action->frequency_hz != 0;
    }
    if (action->action >= DSD_SEARCH_ACTION_HOLD
        && (!c->visit || action->hit_id != c->status.hit.hit_id || c->status.hit.ended)) {
        return 0;
    }
    return 1;
}

#ifdef USE_RADIO
/* Avoid the current hit for the rest of the session and leave it; -1 when the avoid list is full. */
static int
search_avoid_hit(search_coord* c) {
    if (!search_contains(c->config.avoids, c->config.avoid_count, c->status.hit.frequency_hz)) {
        if (c->config.avoid_count == DSD_SEARCH_MAX_AVOIDS) {
            return -1;
        }
        c->config.avoids[c->config.avoid_count++] = c->status.hit.frequency_hz;
    }
    c->skip = 1;
    return 0;
}

/* The next tick ends what the decoder was receiving (search_end_reception()). */
static void
search_pause(search_coord* c) {
    search_end_hit(c);
    search_cancel_measurements(c);
    c->hold = c->skip = c->visit = 0;
    rtl_signal_search_bypass(1);
    c->status.phase = DSD_SEARCH_PAUSED;
}

/* Lifting an avoid that is not there is not an error: the caller's list and the session's agree afterwards. */
static void
search_unavoid(search_coord* c, uint32_t frequency) {
    for (uint32_t i = 0; i < c->config.avoid_count; ++i) {
        if (c->config.avoids[i] == frequency) {
            c->config.avoids[i] = c->config.avoids[--c->config.avoid_count];
            return;
        }
    }
}
#endif

int
// cppcheck-suppress constParameterPointer -- Radio-enabled action handling mutates the coordinator attached to state.
dsd_engine_signal_search_control(dsd_opts* opts, dsd_state* state, const dsd_signal_search_control* action) {
    // cppcheck-suppress constVariablePointer -- Mutated below when USE_RADIO is enabled.
    search_coord* c = search_get(state);
    if (!c || !search_valid_action(c, action)) {
        return -1;
    }
    (void)opts;
#ifdef USE_RADIO
    switch (action->action) {
        case DSD_SEARCH_ACTION_STOP:
            c->stopping = 1;
            c->tune_started = dsd_realtime_mono_ms();
            break;
        case DSD_SEARCH_ACTION_PAUSE: search_pause(c); break;
        case DSD_SEARCH_ACTION_RESUME:
            if (c->status.phase != DSD_SEARCH_PAUSED) {
                return -1;
            }
            c->need_span = 1;
            c->visit = 0;
            c->status.phase = DSD_SEARCH_SEARCHING;
            break;
        case DSD_SEARCH_ACTION_HOLD:
            c->hold = !c->hold;
            if (c->status.phase != DSD_SEARCH_TUNING) {
                c->status.phase = search_reception_phase(c);
            }
            break;
        case DSD_SEARCH_ACTION_AVOID:
        case DSD_SEARCH_ACTION_AVOID_PERMANENT:
            if (search_avoid_hit(c)) {
                return -1;
            }
            break;
        case DSD_SEARCH_ACTION_SKIP: c->skip = 1; break;
        case DSD_SEARCH_ACTION_UNAVOID: search_unavoid(c, action->frequency_hz); break;
        default: return -1;
    }
    search_publish(c, 0);
    return 0;
#else
    return -1;
#endif
}

#ifdef USE_RADIO
static int
search_restore_tune(search_coord* c, dsd_opts* opts, dsd_state* state) {
    search_end_hit(c);
    search_cancel_measurements(c);
    rtl_signal_search_bypass(1);
    rtl_stream_clear_pending_retune_profile();
    /* Leaving the channel the search left the receiver on is a carrier change like any visit's: the call heard there
       ends at the boundary, so nothing received on the restored frequency joins it. */
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    dsd_frame_sync_reset_acquisition(opts, state, 1);
    search_restore(c, opts, state);
    dsd_decode_mode_profile profile = dsd_decode_mode_profile_for(dsd_infer_decode_mode_preset(opts));
    rtl_stream_retune_gain_profile gain = {1, c->saved_gain, c->saved_gain_auto, 1, c->saved_autogain};
    rtl_stream_prepare_retune_profile_for_target_with_gain(
        c->saved_frequency, state->rf_mod == 1, profile.symbol_rate_hz, profile.levels,
        dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, state->rf_mod),
        state->samplesPerSymbol, 0, &gain);
    /* The retune carries the session's receive family, as each visit's does: search_restore() also requests it live,
       but a retune landing first retires that older request, which would leave an analog session on the digital family
       the search last used. */
    const int analog = dsd_opts_is_analog_family(opts);
    const rtl_stream_retune_analog_profile family = {analog ? DSD_RX_FAMILY_ANALOG : DSD_RX_FAMILY_DIGITAL,
                                                     analog ? opts->analog_demod : DSD_ANALOG_DEMOD_FM,
                                                     analog ? dsd_opts_analog_width_hz(opts) : 0, 0};
    (void)rtl_stream_prepare_retune_analog_profile_for_target(c->saved_frequency, &family);
    int rc = rtl_stream_tune_async(c->saved_frequency, c->status.session_id, &c->ticket);
    if (rc < 0) {
        search_error(c, "Unable to restore the previous frequency");
    } else if (rc == RTL_STREAM_TUNE_OK) {
        c->restore_queued = 1;
    } else if (dsd_realtime_mono_ms() - c->tune_started > 10000) {
        search_error(c, "Timed out restoring the receiver; stop the radio session");
    }
    return 1;
}

static int
search_stop_tick(search_coord* c, dsd_opts* opts, dsd_state* state) {
    int result = 0;
    if (c->ticket && rtl_stream_tune_poll(c->ticket, &result) == 0) {
        if (dsd_realtime_mono_ms() - c->tune_started > 10000) {
            search_error(c, "Timed out restoring the receiver; stop the radio session");
        }
        return 1;
    }
    c->ticket = 0;
    if (!c->restore_queued) {
        return search_restore_tune(c, opts, state);
    }
    c->restore_queued = 0;
    if (result != RTL_STREAM_TUNE_OK) {
        search_error(c, "Restoring the previous tune failed");
        return 1;
    }
    opts->rtlsdr_center_freq = c->saved_frequency;
    dsd_engine_signal_search_shutdown(opts, state);
    return 0;
}

/* A pause may arrive while the controller owns a tune. Drain that tagged completion
   before staging a new profile, including when edited ranges restart the pass. */
static int
search_previous_tune_ready(search_coord* c) {
    if (!c->ticket || c->status.phase == DSD_SEARCH_TUNING) {
        return 1;
    }
    int result = 0;
    if (rtl_stream_tune_poll(c->ticket, &result) == 0) {
        if (dsd_realtime_mono_ms() - c->tune_started > 10000) {
            search_error(c, "The previous tune did not finish");
        }
        return 0;
    }
    c->ticket = 0;
    return 1;
}

static int
search_capture_ready(search_coord* c, dsd_signal_capture* capture) {
    if (!rtl_signal_capture_get(capture)) {
        if (dsd_realtime_mono_ms() - c->tune_started > 10000) {
            search_error(c, "No samples from the radio input");
        }
        return 0;
    }
    if (dsd_realtime_mono_ms() - rtl_signal_capture_last_ms() > 10000) {
        search_error(c, "The radio input stopped delivering samples");
        return 0;
    }
    c->status.capture_rate_hz = capture->rate_hz;
    return 1;
}

static int
search_next_span(search_coord* c, dsd_opts* opts, dsd_state* state, const dsd_signal_capture* capture) {
    if (!search_select_range(c)) {
        search_error(c, "No enabled ranges");
        return 1;
    }
    int rc = dsd_signal_search_plan(&c->config.ranges[c->status.range_index], c->status.channel_index, capture->rate_hz,
                                    capture->capture_offset_hz, &c->span);
    if (rc != 1) {
        search_error(c, "This range cannot be covered at the receiver's capture rate");
        return 1;
    }
    c->need_span = 0;
    (void)search_queue_tune(c, opts, state, c->span.tune_hz, 0, 0);
    return 1;
}

static int
search_wait_tune(search_coord* c, dsd_opts* opts, const dsd_signal_capture* capture) {
    uint64_t now = dsd_decode_now_mono_ms();
    int result = 0;
    if (!c->ticket) {
        if (rtl_stream_tune_async(c->target, c->status.session_id, &c->ticket) < 0) {
            search_error(c, "The receiver refused the tune");
            return 1;
        }
    }
    int complete = c->ticket ? rtl_stream_tune_poll(c->ticket, &result) : 0;
    if (complete < 0 || (complete && result < 0)) {
        search_error(c, "The receiver failed to tune");
        return 1;
    }
    if (!complete || capture->generation == c->before_generation
        || (int64_t)capture->capture_hz - capture->capture_offset_hz != c->target
        || capture->generation != rtl_stream_output_generation()) {
        if (dsd_realtime_mono_ms() - c->tune_started > 10000) {
            search_error(c, "Timed out waiting for fresh radio samples");
        }
        return 1;
    }
    c->ticket = 0;
    opts->rtlsdr_center_freq = c->target;
    if (c->visit) {
        if (!c->acquired) {
            c->acquired = now;
        }
        c->probe_started = c->activity = c->landed = now;
        c->listen_ready = 0;
        c->status.phase = search_reception_phase(c);
        rtl_signal_search_bypass(0);
    } else {
        c->status.phase = DSD_SEARCH_SEARCHING;
    }
    search_publish(c, c->visit);
    return 0;
}

static void
search_visit_candidate(search_coord* c, dsd_opts* opts, dsd_state* state, const dsd_signal_measure_candidate* found) {
    const dsd_signal_search_range* r = &c->config.ranges[c->status.range_index];
    dsd_signal_search_hit* hit = &c->status.hit;
    DSD_MEMSET(hit, 0, sizeof(*hit));
    hit->session_id = c->status.session_id;
    hit->hit_id = ++c->next_hit;
    hit->frequency_hz = found->frequency_hz;
    hit->bandwidth_hz = r->bandwidth_hz;
    hit->step_num = r->step_num;
    hit->step_den = r->step_den;
    hit->power_dbfs = found->power_dbfs;
    hit->noise_dbfs = found->noise_dbfs;
    hit->started_ms = (uint64_t)(dsd_decode_now_realtime_s() * 1000);
    hit->range_index = c->status.range_index;
    hit->am = r->am;
    hit->unresolved = r->digital != 0;
    hit->gain_tenth_db = r->gain_set ? r->gain_tenth_db : c->saved_gain;
    hit->gain_auto = r->gain_set ? r->gain_auto : c->saved_gain_auto;
    (void)rtl_stream_get_gain(&hit->gain_tenth_db, &hit->gain_auto);
    hit->airspy = opts->airspy;
    hit->ppm = opts->rtlsdr_ppm_error;
    hit->bias_tee = opts->rtl_bias_tee;
    DSD_SNPRINTF(hit->input_spec, sizeof(hit->input_spec), "%s", opts->audio_in_dev);
    DSD_SNPRINTF(hit->range_name, sizeof(hit->range_name), "%s", r->name);
    search_suppress(c, hit->frequency_hz);
    search_publish(c, 1);
    c->acquired = 0;
    (void)search_queue_tune(c, opts, state, hit->frequency_hz, 1, !r->digital);
}

static int
search_strongest_candidate(search_coord* c) {
    int best = -1;
    for (uint32_t i = 0; i < c->report.count; ++i) {
        const dsd_signal_measure_candidate* candidate = &c->report.candidates[i];
        if (!candidate->valid) {
            search_error(c, "Capture geometry changed; restart the search");
            return -2;
        }
        if (candidate->detected && !search_excluded(c, candidate->frequency_hz)
            && (best < 0 || candidate->power_dbfs > c->report.candidates[best].power_dbfs)) {
            best = (int)i;
        }
    }
    return best;
}

static int
search_measure_tick(search_coord* c, dsd_opts* opts, dsd_state* state, const dsd_signal_capture* capture) {
    if (c->measurement_started && dsd_realtime_mono_ms() - c->measurement_started > 5000) {
        search_error(c, "No uninterrupted samples for a measurement");
        return 1;
    }
    if (!c->measuring) {
        (void)search_begin_measurement(c, capture);
        return 1;
    }
    int rc = rtl_signal_measure_get(&c->report);
    if (rc < 0) {
        c->measuring = 0;
        return 1;
    }
    if (!rc) {
        return 1;
    }
    c->measuring = 0;
    if (c->report.session_id != c->status.session_id || c->report.request_id != c->request
        || c->report.generation != capture->generation) {
        return 1;
    }
    c->report_pending = 1;
    int best = search_strongest_candidate(c);
    if (best == -2) {
        return 1;
    }
    if (best < 0) {
        c->status.channel_index = c->span.next;
        c->need_span = 1;
        search_publish(c, 0);
        return 1;
    }
    search_visit_candidate(c, opts, state, &c->report.candidates[best]);
    return 1;
}

static int
search_visit_tick(search_coord* c, dsd_opts* opts, dsd_state* state) {
    uint64_t now = dsd_decode_now_mono_ms();
    const dsd_signal_search_range* r = &c->config.ranges[c->status.range_index];
    search_note_activity(c, opts, state, now);
    if (c->skip || (!c->hold && r->max_visit_ms && now - c->acquired >= (uint64_t)r->max_visit_ms)) {
        search_return_to_span(c, opts, state);
        return 1;
    }
    if (c->probing && now - c->probe_started >= (uint64_t)r->probe_ms) {
        (void)search_queue_tune(c, opts, state, c->status.hit.frequency_hz, 1, 1);
        return 1;
    }
    if (!c->hold && !c->probing && now - c->activity >= SEARCH_QUIET_MS) {
        search_return_to_span(c, opts, state);
        return 1;
    }
    search_live_strength(c);
    if (dsd_realtime_mono_ms() - c->last_publish >= 100) {
        search_publish(c, 0);
    }
    return 0;
}

/* Paused or failed, the search reads nothing from the channel, and the engine loop runs no noCarrier() until a Resume
   or Stop: the carrier boundary and acquisition reset each tune runs end the reception once, on the tick that finds
   the search there, so a call heard on the channel ends then rather than at a later tune. A visit lands only through
   search_queue_tune(), which runs them again and lets the next pause or failure end its reception. */
static void
search_end_reception(search_coord* c, dsd_opts* opts, dsd_state* state) {
    if (c->reception_ended) {
        return;
    }
    c->reception_ended = 1;
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    dsd_frame_sync_reset_acquisition(opts, state, 1);
    /* The engine loop publishes no snapshot while the search is paused or failed (frame sync does, and it is not
       running): frontends learn here that the call ended. */
    dsd_telemetry_publish_both_and_redraw(opts, state);
}

static int
search_input_lost(search_coord* c, dsd_opts* opts, dsd_state* state) {
    if (c->stopping) {
        dsd_engine_signal_search_shutdown(opts, state);
        return 0;
    }
    if (c->status.phase != DSD_SEARCH_ERROR) {
        search_error(c, "The radio input is no longer available");
    }
    return 1;
}
#endif

#ifdef USE_RADIO
static int
search_tick(search_coord* c, dsd_opts* opts, dsd_state* state) {
    if (opts->audio_in_type != AUDIO_IN_RTL || !rtl_stream_is_active()) {
        return search_input_lost(c, opts, state);
    }
    if (c->stopping) {
        return search_stop_tick(c, opts, state);
    }
    if (c->status.phase == DSD_SEARCH_PAUSED || c->status.phase == DSD_SEARCH_ERROR) {
        return 1;
    }
    dsd_signal_capture capture;
    if (!search_previous_tune_ready(c) || !search_capture_ready(c, &capture)) {
        return 1;
    }
    if (c->need_span) {
        return search_next_span(c, opts, state, &capture);
    }
    if (c->status.phase == DSD_SEARCH_TUNING && search_wait_tune(c, opts, &capture)) {
        return 1;
    }
    if (c->status.phase == DSD_SEARCH_SEARCHING) {
        return search_measure_tick(c, opts, state, &capture);
    }
    return c->visit ? search_visit_tick(c, opts, state) : 0;
}
#endif

int
// cppcheck-suppress constParameterPointer -- Radio-enabled tuning applies receive profiles to state.
dsd_engine_signal_search_tick(dsd_opts* opts, dsd_state* state) {
    // cppcheck-suppress constVariablePointer -- Mutated below when USE_RADIO is enabled.
    search_coord* c = search_get(state);
    if (!c || c->status.phase == DSD_SEARCH_STOPPED) {
        return 0;
    }
#ifndef USE_RADIO
    (void)opts;
    return 0;
#else
    const int rc = search_tick(c, opts, state);
    if (c->status.phase == DSD_SEARCH_PAUSED || c->status.phase == DSD_SEARCH_ERROR) {
        search_end_reception(c, opts, state);
    }
    return rc;
#endif
}

void
// cppcheck-suppress constParameterPointer -- Radio-enabled shutdown restores the saved receive profile.
dsd_engine_signal_search_shutdown(dsd_opts* opts, dsd_state* state) {
    search_coord* c = search_get(state);
    if (!c || c->status.phase == DSD_SEARCH_STOPPED) {
        return;
    }
    search_end_hit(c);
#ifdef USE_RADIO
    search_cancel_measurements(c);
    search_restore(c, opts, state);
    /* The session's frequency is the one the search started from, as after a Stop's restore tune: a session ending
       under the search autosaves its configuration next, which must not keep a visit's frequency. */
    opts->rtlsdr_center_freq = c->saved_frequency;
    rtl_stream_clear_pending_retune_profile();
    rtl_signal_search_bypass(0);
    rtl_signal_search_set_owner(0);
#else
    (void)opts;
#endif
    c->status.phase = DSD_SEARCH_STOPPED;
    search_publish(c, 0);
}
