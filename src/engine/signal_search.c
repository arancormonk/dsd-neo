// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/manual_tune.h>
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
    SEARCH_LIVE_RETRY_MS = 1000,
    /* A live strength no accepted measurement has refreshed for three intervals and a retry is no current level. */
    SEARCH_LIVE_STALE_MS = 3 * SEARCH_LIVE_INTERVAL_MS + SEARCH_LIVE_RETRY_MS
};

/* The call tuple: what one checked canonical call record says, copied whole when its call is noted. */
typedef struct {
    /* The record's protocol as the search names it ("DMR", "P25", "NXDN", ...); "" for no tuple. */
    char protocol[8];
    uint8_t record_slot;
    uint64_t epoch;
    double started_m;
    uint32_t target, source;
    int slot;
    char target_text[64], source_text[64];
    char crypt[48];
} search_call;

typedef struct {
    dsd_signal_search_config config;
    dsd_signal_search_status status;
    /* The decoder's options, which outlive the search: its receive policy judges what the search may report and hold
       on (issue #637). */
    const dsd_opts* opts;
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
    /* The current visit's checked site identity, from site notes alone; cleared at each landing. */
    dsd_signal_search_site site;
    /* The visit's call tuple, from call notes alone; cleared at each landing and when the protocol changes. */
    search_call call;
#ifdef USE_RADIO
    dsd_signal_search_span span;
    dsd_signal_measure_report report;
    int report_pending;
    int live_measuring;
    uint64_t live_request, live_next;
    /* When the shown live strength was measured. */
    uint64_t live_accepted;
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
        /* An ended visit has no current level. */
        c->status.live_power_valid = 0;
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
            DSD_SNPRINTF(c->status.range_name, sizeof(c->status.range_name), "%s", r->name);
            c->status.range_start_hz = r->start_hz;
            c->status.range_end_hz = r->end_hz;
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
search_open_audio(const search_coord* c, dsd_opts* opts, const dsd_state* state, int analog) {
    if (!c->visit) {
        return 0;
    }
    return analog ? dsd_audio_ensure_analog_output(opts) : dsd_audio_ensure_digital_output(opts, state);
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
    if (search_open_audio(c, opts, state, analog) < 0) {
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
    /* The search now decides where the receiver goes, not a manual tune still pending (issue #671). */
    dsd_engine_manual_tune_supersede(state);
    c->status.phase = DSD_SEARCH_TUNING;
    /* A refusal (a full avoid list) belonged to the detection being left; the next one starts clean. */
    if (visit) {
        c->status.message[0] = '\0';
    }
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
        /* The edited ranges show from this Resume on, not while they were being edited. */
        (void)search_select_range(previous);
        search_publish(previous, 0);
        return 0;
    }
    search_coord* c = (search_coord*)calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    c->config = *config;
    c->opts = opts;
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
    dsd_signal_search_evidence_set(dsd_engine_signal_search_evidence_at);
    dsd_signal_search_site_set(dsd_engine_signal_search_site);
    dsd_signal_search_call_set(dsd_engine_signal_search_call);
    dsd_signal_search_active_set(dsd_engine_signal_search_active);
    rtl_signal_search_set_owner(c->status.session_id);
    rtl_signal_search_bypass(1);
    /* The first enabled range: validation guarantees one, so this neither wraps the pass nor fails. */
    (void)search_select_range(c);
    search_publish(c, 0);
    return 0;
#endif
}

/* Whether the visit's site record belongs to @p protocol ("NXDN" for NXDN48 and NXDN96). */
static int
search_site_is(const search_coord* c, const char* protocol) {
    const size_t n = strlen(c->site.protocol);
    return n && strncmp(protocol, c->site.protocol, n) == 0;
}

/* The hit shows the visit's site record only while it belongs to the hit's protocol: a site noted before the protocol
   is identified shows once it is. 1 when the shown site changed. */
static int
search_refresh_site(search_coord* c) {
    dsd_signal_search_hit* hit = &c->status.hit;
    char site[sizeof(hit->site)] = "";
    if (search_site_is(c, hit->protocol)) {
        (void)dsd_signal_search_site_format(&c->site, site, sizeof(site));
    }
    if (strcmp(site, hit->site) == 0) {
        return 0;
    }
    DSD_SNPRINTF(hit->site, sizeof(hit->site), "%s", site);
    return 1;
}

/* Whether the visit's call tuple belongs to @p protocol, as the site record does. */
static int
search_call_is(const search_coord* c, const char* protocol) {
    const size_t n = strlen(c->call.protocol);
    return n && strncmp(protocol, c->call.protocol, n) == 0;
}

/* The hit takes the call tuple whole while it belongs to the hit's protocol. 1 when the shown tuple changed. */
static int
search_refresh_call(search_coord* c) {
    dsd_signal_search_hit* hit = &c->status.hit;
    if (!search_call_is(c, hit->protocol)) {
        return 0;
    }
    const search_call* t = &c->call;
    if (hit->target == t->target && hit->source == t->source && hit->slot == t->slot
        && strcmp(hit->target_text, t->target_text) == 0 && strcmp(hit->source_text, t->source_text) == 0
        && strcmp(hit->crypt, t->crypt) == 0) {
        return 0;
    }
    hit->target = t->target;
    hit->source = t->source;
    hit->slot = t->slot;
    DSD_SNPRINTF(hit->target_text, sizeof(hit->target_text), "%s", t->target_text);
    DSD_SNPRINTF(hit->source_text, sizeof(hit->source_text), "%s", t->source_text);
    DSD_SNPRINTF(hit->crypt, sizeof(hit->crypt), "%s", t->crypt);
    return 1;
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
        hit->slot = 0;
        hit->access[0] = hit->source_text[0] = hit->target_text[0] = hit->crypt[0] = '\0';
        /* Another protocol's identity is not this one's site or call: they clear, and only a new note shows one again.
           A site or call noted before any protocol was identified waits for it. */
        if (hit->protocol[0] && !search_site_is(c, identified)) {
            dsd_signal_search_site_clear(&c->site);
        }
        if (hit->protocol[0] && !search_call_is(c, identified)) {
            DSD_MEMSET(&c->call, 0, sizeof(c->call));
        }
    }
    DSD_SNPRINTF(hit->protocol, sizeof(hit->protocol), "%s", identified);
    (void)search_refresh_site(c);
    (void)search_refresh_call(c);
}

/* Whether a protocol observation may join the visit: one is being received, and no input interruption stands (an
   observation noted then comes from a frame it cut, whose rest is erasures getSymbol() stood in). */
static int
search_receiving(const search_coord* c, const dsd_state* state) {
    return c && c->visit && !c->status.hit.ended && !state->input_interrupted
           && (c->status.phase == DSD_SEARCH_IDENTIFYING || c->status.phase == DSD_SEARCH_LISTENING
               || c->status.phase == DSD_SEARCH_HELD);
}

void
dsd_engine_signal_search_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                                  const char* access) {
    dsd_engine_signal_search_evidence_at(state, protocol, -1, -1, target, source, access);
}

void
dsd_engine_signal_search_evidence_at(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                                     uint32_t source, const char* access) {
    search_coord* c = search_get(state);
    /* The decoders drop a frame an input interruption cut; this refuses whatever one still reports. Traffic the receive
       policy rejects or is still checking neither confirms the hit nor keeps the search listening (issue #637), judged
       by the protocol and slot the report names when it names them. */
    if (!c || !search_receiving(c, state)) {
        return;
    }
    const int admitted = sync >= 0 ? dsd_receive_allows(c->opts, state, sync, slot)
                                   : dsd_receive_current_allows(c->opts, state, state->currentslot);
    if (!admitted) {
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
    /* A protocol that names its call in a call record names it in the tuple; evidence IDs only stand in for a hit no
       record has named. */
    if (!search_call_is(c, c->status.hit.protocol)) {
        if (target) {
            c->status.hit.target = target;
        }
        if (source) {
            c->status.hit.source = source;
        }
    }
    if (access && access[0]) {
        DSD_SNPRINTF(c->status.hit.access, sizeof(c->status.hit.access), "%s", access);
    }
    search_publish(c, 1);
}

void
dsd_engine_signal_search_site(const dsd_state* state, const dsd_signal_search_site_note* note) {
    search_coord* c = search_get(state);
    /* A repeated broadcast changes nothing and publishes nothing; a new identity publishes the hit at once, so the
       last broadcast before the channel falls silent still reaches it. */
    if (!c || !search_receiving(c, state) || !dsd_signal_search_site_apply(&c->site, note)) {
        return;
    }
    if (search_refresh_site(c)) {
        search_publish(c, 1);
    }
}

/* The search's name for a call record's protocol, or NULL for one the search does not identify by its calls. */
static const char*
search_call_protocol(int protocol) {
    if (DSD_SYNC_IS_DMR(protocol)) {
        return "DMR";
    }
    if (DSD_SYNC_IS_P25(protocol)) {
        return "P25";
    }
    if (DSD_SYNC_IS_NXDN(protocol)) {
        return "NXDN";
    }
    if (DSD_SYNC_IS_DPMR(protocol)) {
        return "dPMR";
    }
    if (DSD_SYNC_IS_M17(protocol)) {
        return "M17";
    }
    if (DSD_SYNC_IS_YSF(protocol)) {
        return "YSF";
    }
    if (DSD_SYNC_IS_DSTAR(protocol)) {
        return "D-STAR";
    }
    return NULL;
}

/* "ALG 84 · KID 0001" with only the parts the call received over the air, "Encrypted" when its classification says
   encrypted, neither was received and checked signalling of the call itself says encrypted (enc_received: never a
   forced, inferred or key-loaded classification), "" for a call not known to be encrypted. */
static void
search_call_crypt(const dsd_call_snapshot* call, char* out, size_t capacity) {
    out[0] = '\0';
    if (call->crypto != DSD_CALL_CRYPTO_ENCRYPTED_PENDING && call->crypto != DSD_CALL_CRYPTO_ENCRYPTED
        && call->crypto != DSD_CALL_CRYPTO_DECRYPTABLE) {
        return;
    }
    const int alg = (call->crypto_received & DSD_CALL_CRYPTO_RX_ALG) != 0U;
    const int kid = (call->crypto_received & DSD_CALL_CRYPTO_RX_KID) != 0U;
    if (alg && kid) {
        DSD_SNPRINTF(out, capacity, "ALG %02X \xC2\xB7 KID %04X", (unsigned)call->received_algid,
                     (unsigned)call->received_kid);
    } else if (alg) {
        DSD_SNPRINTF(out, capacity, "ALG %02X", (unsigned)call->received_algid);
    } else if (kid) {
        DSD_SNPRINTF(out, capacity, "KID %04X", (unsigned)call->received_kid);
    } else if (call->enc_received != 0U) {
        DSD_SNPRINTF(out, capacity, "%s", "Encrypted");
    }
}

static uint32_t
search_call_id(uint64_t id) {
    return id <= UINT32_MAX ? (uint32_t)id : 0U;
}

static int
search_call_equal(const search_call* a, const search_call* b) {
    return strcmp(a->protocol, b->protocol) == 0 && a->record_slot == b->record_slot && a->epoch == b->epoch
           && a->target == b->target && a->source == b->source && a->slot == b->slot
           && strcmp(a->target_text, b->target_text) == 0 && strcmp(a->source_text, b->source_text) == 0
           && strcmp(a->crypt, b->crypt) == 0;
}

/* The protocol name of @p slot's active, checked call when it may be shown on the hit, NULL otherwise. Another
   protocol's call is not this hit's once the hit's protocol is known. */
static const char*
search_call_admit(const search_coord* c, const dsd_state* state, uint8_t slot, dsd_call_snapshot* record) {
    if (dsd_call_state_get(state, slot, record) <= 0 || record->phase != DSD_CALL_PHASE_ACTIVE
        || record->crc_invalid != 0U || !dsd_receive_allows(c->opts, state, record->protocol, slot)) {
        return NULL;
    }
    const char* protocol = search_call_protocol(record->protocol);
    const dsd_signal_search_hit* hit = &c->status.hit;
    if (!protocol || (hit->protocol[0] && strncmp(hit->protocol, protocol, strlen(protocol)) != 0)) {
        return NULL;
    }
    return protocol;
}

/* The tuple is the most recent call heard on the hit: an older call still running on the other slot re-announcing
   itself leaves the newer one shown, so the tuple never mixes two calls. */
static int
search_call_superseded(const search_coord* c, const char* protocol, uint8_t slot, const dsd_call_snapshot* record) {
    if (!c->call.protocol[0] || strcmp(c->call.protocol, protocol) != 0) {
        return 0;
    }
    const int same_call = c->call.record_slot == slot && c->call.epoch == record->epoch;
    return !same_call && record->started_m < c->call.started_m;
}

static void
search_call_from_record(search_call* next, const dsd_call_snapshot* record, const char* protocol, uint8_t slot) {
    DSD_MEMSET(next, 0, sizeof(*next));
    DSD_SNPRINTF(next->protocol, sizeof(next->protocol), "%s", protocol);
    next->record_slot = slot;
    next->epoch = record->epoch;
    next->started_m = record->started_m;
    next->target = search_call_id(record->ota_target_id != 0U ? record->ota_target_id : record->policy_target_id);
    next->source = search_call_id(record->ota_source_id);
    next->slot = (DSD_SYNC_IS_DMR(record->protocol) || DSD_SYNC_IS_P25P2(record->protocol)) ? slot + 1 : 0;
    DSD_SNPRINTF(next->target_text, sizeof(next->target_text), "%s", record->target_text);
    DSD_SNPRINTF(next->source_text, sizeof(next->source_text), "%s", record->source_text);
    search_call_crypt(record, next->crypt, sizeof(next->crypt));
}

void
dsd_engine_signal_search_call(const dsd_state* state, uint8_t slot) {
    search_coord* c = search_get(state);
    if (!c || !search_receiving(c, state) || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return;
    }
    dsd_call_snapshot record;
    const char* protocol = search_call_admit(c, state, slot, &record);
    if (!protocol || search_call_superseded(c, protocol, slot, &record)) {
        return;
    }
    search_call next;
    search_call_from_record(&next, &record, protocol, slot);
    if (search_call_equal(&next, &c->call)) {
        return;
    }
    c->call = next;
    if (search_refresh_call(c)) {
        search_publish(c, 1);
    }
}

#ifdef USE_RADIO
/* The landed channel's own RF power while listening, measured by the capture-rate detector on the same dBFS scale as
   the detection. Only a capture that is the landed tune is measured; a report is accepted for its own request. Both the
   main-loop tick and the in-frame yield check service it: a continuous digital transmission keeps the decoder in its
   synced-frame loop, where only the latter runs. A level that no accepted measurement has refreshed for
   SEARCH_LIVE_STALE_MS (the capture keeps changing, its samples clip, or the channel cannot be measured) is withdrawn
   and published, so a frontend never shows an old level as the current one. */
static void
search_live_strength(search_coord* c) {
    const uint64_t now = dsd_realtime_mono_ms();
    if (c->status.live_power_valid && now - c->live_accepted >= SEARCH_LIVE_STALE_MS) {
        c->status.live_power_valid = 0;
        search_publish(c, 0);
    }
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
                ++c->status.live_seq;
                c->live_accepted = now;
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
    if (action->action == DSD_SEARCH_ACTION_UNAVOID || action->action == DSD_SEARCH_ACTION_AVOID_FREQUENCY) {
        return action->frequency_hz != 0;
    }
    if (action->action >= DSD_SEARCH_ACTION_HOLD
        && (!c->visit || action->hit_id != c->status.hit.hit_id || c->status.hit.ended)) {
        return 0;
    }
    return 1;
}

int
dsd_engine_signal_search_target(const dsd_state* state, const dsd_signal_search_control* action, uint32_t* frequency) {
    const search_coord* c = state ? search_get(state) : NULL;
    if (!c || !search_valid_action(c, action)) {
        return -1;
    }
    if (frequency) {
        const int by_frequency =
            action->action == DSD_SEARCH_ACTION_UNAVOID || action->action == DSD_SEARCH_ACTION_AVOID_FREQUENCY;
        *frequency = by_frequency ? action->frequency_hz : c->status.hit.frequency_hz;
    }
    return 0;
}

#ifdef USE_RADIO
/* Avoid @p frequency for the rest of the session, leaving it if it is being visited; -1 when the avoid list is full.
   Avoiding a frequency already avoided succeeds. */
static int
search_avoid(search_coord* c, uint32_t frequency) {
    if (!search_contains(c->config.avoids, c->config.avoid_count, frequency)) {
        if (c->config.avoid_count == DSD_SEARCH_MAX_AVOIDS) {
            return -1;
        }
        c->config.avoids[c->config.avoid_count++] = frequency;
    }
    if (c->visit && !c->status.hit.ended && c->status.hit.frequency_hz == frequency) {
        c->skip = 1;
    }
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

/* Avoid the hit being visited, or the action's frequency; a refusal is the frontends' to show: the queue only said the
   command was taken. A failed search keeps its error, which says more. */
static int
search_avoid_action(search_coord* c, const dsd_signal_search_control* action) {
    /* A valid AVOID names the hit being visited, so it is left as well. */
    if (search_avoid(c, action->action == DSD_SEARCH_ACTION_AVOID_FREQUENCY ? action->frequency_hz
                                                                            : c->status.hit.frequency_hz)) {
        if (c->status.phase != DSD_SEARCH_ERROR) {
            DSD_SNPRINTF(c->status.message, sizeof(c->status.message), "%s", "The avoid list is full");
            search_publish(c, 0);
        }
        return -1;
    }
    return 0;
}

static int
search_resume(search_coord* c) {
    if (c->status.phase != DSD_SEARCH_PAUSED) {
        return -1;
    }
    c->need_span = 1;
    c->visit = 0;
    c->status.phase = DSD_SEARCH_SEARCHING;
    return 0;
}

static void
search_toggle_hold(search_coord* c) {
    c->hold = !c->hold;
    if (c->status.phase != DSD_SEARCH_TUNING) {
        c->status.phase = search_reception_phase(c);
    }
}

/* Carry out a valid @p action; -1 when it is refused. */
static int
search_apply_action(search_coord* c, const dsd_signal_search_control* action) {
    switch (action->action) {
        case DSD_SEARCH_ACTION_STOP:
            c->stopping = 1;
            c->tune_started = dsd_realtime_mono_ms();
            return 0;
        case DSD_SEARCH_ACTION_PAUSE: search_pause(c); return 0;
        case DSD_SEARCH_ACTION_RESUME: return search_resume(c);
        case DSD_SEARCH_ACTION_HOLD: search_toggle_hold(c); return 0;
        case DSD_SEARCH_ACTION_AVOID:
        case DSD_SEARCH_ACTION_AVOID_PERMANENT:
        case DSD_SEARCH_ACTION_AVOID_FREQUENCY: return search_avoid_action(c, action);
        case DSD_SEARCH_ACTION_SKIP: c->skip = 1; return 0;
        case DSD_SEARCH_ACTION_UNAVOID: search_unavoid(c, action->frequency_hz); return 0;
        default: return -1;
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
    if (search_apply_action(c, action) != 0) {
        return -1;
    }
    /* An action that took effect replaces an earlier refusal; a search error stays until the session ends. */
    if (c->status.phase != DSD_SEARCH_ERROR) {
        c->status.message[0] = '\0';
    }
    search_publish(c, 0);
    return 0;
#else
    return -1;
#endif
}

void
// cppcheck-suppress constParameterPointer -- The coordinator attached to state publishes the message.
dsd_engine_signal_search_notice(dsd_state* state, uint64_t session_id, const char* message) {
    search_coord* c = state ? search_get(state) : NULL;
    if (!c || !message || c->status.session_id != session_id || c->status.phase == DSD_SEARCH_STOPPED
        || c->status.phase == DSD_SEARCH_ERROR) {
        return;
    }
    DSD_SNPRINTF(c->status.message, sizeof(c->status.message), "%s", message);
    search_publish(c, 0);
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
    /* Snapshot the producer's stamp before reading now: a capture arriving between reads must not underflow its age. */
    const uint64_t last_capture_ms = rtl_signal_capture_last_ms();
    const uint64_t now = dsd_realtime_mono_ms();
    if (now >= last_capture_ms && now - last_capture_ms > 10000) {
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
    /* Nothing noted before this landing, on the span or an earlier visit, is this detection's. */
    dsd_signal_search_site_clear(&c->site);
    DSD_MEMSET(&c->call, 0, sizeof(c->call));
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
