// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The scan quiet check (issue #834): see scan_quiet.h. */

#include "scan_quiet.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "scan_owner.h"
#ifdef USE_RADIO
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include "scan_landing.h"
#endif

/* Width classes (the values of DSD_SIGNAL_QUIET_CLASS_*, which signal_measure.h, a radio header, names). */
enum { QUIET_CLASS_12K5 = 1, QUIET_CLASS_6K25 = 2, QUIET_CLASS_ANALOG = 4 };

static const dsd_engine_scan_owner*
quiet_owner_const(const dsd_state* state) {
    return state ? (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN) : NULL;
}

void
scan_quiet_revisions_read(const dsd_opts* opts, const dsd_state* state, scan_quiet_revisions* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    if (!opts || !state) {
        return;
    }
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    out->chan_map_seq = state->trunk_chan_map_seq;
    out->lcn_list_rev = state->lcn_list_rev;
    out->lcn_avoid_rev = state->lcn_avoid_rev;
    out->trunk_scan_avoid_rev = state->trunk_scan_avoid_rev;
    out->quiet_inv_seq = owner ? owner->quiet_inv_seq : 0U;
    out->lcn_scan_session = state->lcn_scan_session;
    out->trunk_scan_lists_seq = state->trunk_scan_lists_seq;
    out->trunk_edit_seq = dsd_engine_trunk_scan_quiet_edit_seq(state);
    out->y_edits_epoch = dsd_engine_channel_scan_quiet_edits_epoch(state);
    out->pcm_input_generation = opts->pcm_input_generation;
#ifdef USE_RADIO
    /* A receive-profile or width request still in flight changes what the channel is received as. */
    out->receive_request_seq = rtl_stream_receive_request_seq();
#endif
    out->nfm_width_hz = opts->analog_nfm_bandwidth_hz;
    out->am_width_hz = opts->analog_am_bandwidth_hz;
}

uint8_t
scan_quiet_mode_classes(dsd_scan_mode mode) {
    switch (mode) {
        case DSD_SCAN_MODE_P25:
        case DSD_SCAN_MODE_DMR:
        case DSD_SCAN_MODE_NXDN96:
        case DSD_SCAN_MODE_YSF:
        case DSD_SCAN_MODE_M17: return QUIET_CLASS_12K5;
        case DSD_SCAN_MODE_NXDN48:
        case DSD_SCAN_MODE_DPMR:
        case DSD_SCAN_MODE_DSTAR: return QUIET_CLASS_6K25;
        case DSD_SCAN_MODE_NFM:
        case DSD_SCAN_MODE_AM: return QUIET_CLASS_ANALOG;
        case DSD_SCAN_MODE_INHERIT: return 0U;
    }
    return 0U;
}

/* Whether the decoder set enables a mode of the 12.5 kHz class, and of the 6.25 kHz class. */
static int
quiet_decoder_wide(const dsd_opts* opts) {
    return opts->frame_p25p1 || opts->frame_p25p2 || opts->frame_dmr || opts->frame_nxdn96 || opts->frame_ysf
           || opts->frame_m17;
}

static int
quiet_decoder_narrow(const dsd_opts* opts) {
    return opts->frame_nxdn48 || opts->frame_dpmr || opts->frame_dstar;
}

uint32_t
scan_quiet_analog_width_hz(int kind, int explicit_width_hz) {
#ifdef USE_RADIO
    /* As the stream requests it: an explicit width as typed, the AM default (a definite filtered channel) when unset,
       and none for an unset NFM width, which keeps the historical enable rule and WIDE fallback. */
    const int width_hz = rtl_demod_analog_requested_width_hz(kind, explicit_width_hz);
#else
    (void)kind;
    const int width_hz = explicit_width_hz; /* no stream: no default to resolve, and no check is ever made */
#endif
    return width_hz > 0 ? (uint32_t)width_hz : 0U;
}

void
scan_quiet_analog_facts(const dsd_opts* opts, const dsd_state* state, int kind, const dsd_scan_option_values* values,
                        scan_quiet_eligibility* e) {
    if (!e) {
        return;
    }
    const int explicit_hz = (values && (values->present & DSD_SCAN_OPT_BANDWIDTH))
                                ? values->channel_bw_hz
                                : dsd_scan_mode_configured_analog_width(opts, state, kind);
    e->analog_kind = kind;
    e->analog_width_hz = scan_quiet_analog_width_hz(kind, explicit_hz);
}

/* The analog class's kind and the width the stream runs it at (scan_quiet_analog_width_hz() of the configured width),
   into @p analog_kind and @p analog_width_hz when given; 0 when that width is none, so the set gets no check. */
static uint8_t
quiet_decoder_analog(const dsd_opts* opts, int* analog_kind, uint32_t* analog_width_hz) {
    const uint32_t width_hz = scan_quiet_analog_width_hz(opts->analog_demod, dsd_opts_analog_width_hz(opts));
    if (width_hz == 0U) {
        return 0U;
    }
    if (analog_kind) {
        *analog_kind = opts->analog_demod;
    }
    if (analog_width_hz) {
        *analog_width_hz = width_hz;
    }
    return QUIET_CLASS_ANALOG;
}

uint8_t
scan_quiet_decoder_classes(const dsd_opts* opts, int* analog_kind, uint32_t* analog_width_hz) {
    if (analog_kind) {
        *analog_kind = 0;
    }
    if (analog_width_hz) {
        *analog_width_hz = 0U;
    }
    if (!opts || opts->m17encoder == 1) {
        return 0U;
    }
    if (dsd_opts_is_analog_family(opts)) {
        return quiet_decoder_analog(opts, analog_kind, analog_width_hz);
    }
    /* ProVoice and X2-TDMA have no sourced width and time structure, nor TETRA a class: a set holding one gets no
       check. */
    if (opts->frame_x2tdma || opts->frame_provoice || opts->frame_tetra) {
        return 0U;
    }
    const uint8_t digital = (uint8_t)((quiet_decoder_wide(opts) ? QUIET_CLASS_12K5 : 0U)
                                      | (quiet_decoder_narrow(opts) ? QUIET_CLASS_6K25 : 0U));
    if (opts->monitor_input_audio != 1 || digital == 0U) {
        return digital;
    }
    /* The analog monitor runs beside the digital decoders (-8): a mixed set, judged in the analog class too; a
       monitor the stream runs at no definite width (an unset NFM width) gets no check. */
    const uint8_t analog = quiet_decoder_analog(opts, analog_kind, analog_width_hz);
    return analog ? (uint8_t)(digital | analog) : 0U;
}

int
scan_quiet_format_skip(char* out, size_t out_size, const char* name, int row, uint32_t hz, float power_dbfs,
                       float floor_dbfs, float ratio_db) {
    char mhz[24];
    (void)dsd_freq_hz_mhz_text(hz, mhz, sizeof mhz);
    char row_name[24];
    if (!name || name[0] == '\0') {
        DSD_SNPRINTF(row_name, sizeof row_name, "row %d", row + 1);
        name = row_name;
    }
    return DSD_SNPRINTF(out, out_size, "NOTICE: scan: skipped quiet %s %s MHz: %.1f dBFS, floor %.1f dBFS (%.1f dB)\n",
                        name, mhz, (double)power_dbfs, (double)floor_dbfs, (double)ratio_db);
}

#ifdef USE_RADIO

static int
quiet_revisions_equal(const scan_quiet_revisions* a, const scan_quiet_revisions* b) {
    return a->chan_map_seq == b->chan_map_seq && a->lcn_list_rev == b->lcn_list_rev
           && a->lcn_avoid_rev == b->lcn_avoid_rev && a->trunk_scan_avoid_rev == b->trunk_scan_avoid_rev
           && a->quiet_inv_seq == b->quiet_inv_seq && a->lcn_scan_session == b->lcn_scan_session
           && a->trunk_scan_lists_seq == b->trunk_scan_lists_seq && a->trunk_edit_seq == b->trunk_edit_seq
           && a->y_edits_epoch == b->y_edits_epoch && a->pcm_input_generation == b->pcm_input_generation
           && a->receive_request_seq == b->receive_request_seq && a->nfm_width_hz == b->nfm_width_hz
           && a->am_width_hz == b->am_width_hz;
}

typedef struct {
    scan_quiet_phase phase;
    scan_quiet_visit_key key;
    /* The owner's landing epoch counter when a park still in flight (key.epoch 0) was published: its landing is the
       next epoch, and any other note or forget since ends the check. */
    uint64_t epoch_counter;
    /* As measured: the published classes, width, rate and frequency the request judged. */
    uint8_t classes;
    int analog_kind;
    uint32_t analog_width_hz;
    uint32_t dsp_rate_hz;
    uint32_t frequency_hz;
    scan_quiet_revisions revisions;
    uint32_t generation_before;
    /* The WAIT_CAPTURE bound's origin (realtime): the landing's note, or the in-flight park's completion as the hunt
       first read it; origin_set 0 while that park is still in flight. */
    int origin_set;
    uint64_t origin_ms;
    uint64_t next_capture_check_ms;
    int retries;
    uint64_t request_seq;
    uint32_t ticket;
    uint64_t deadline_ms;
    /* The visit's activity baseline: the sync sequence and the decode time at its landing's note (a park still in
       flight: at its first publication, kept when its landing adopts it), kept for that visit across its suspensions,
       restarts and retries. A sync, a carrier or allowed activity since then ends the visit's check, whatever phase the
       check was in. visit_set 0 before the visit's first publication. */
    int visit_set;
    scan_quiet_visit_key visit_key;
    uint64_t sync_seq;
    double visit_decode_m;
    uint64_t ready_ms;
    dsd_signal_quiet_result result;
} scan_quiet_check;

static void
quiet_check_free(void* ptr) {
    scan_quiet_check* c = (scan_quiet_check*)ptr;
    if (c && c->ticket != 0U) {
        rtl_signal_quiet_cancel(c->ticket);
    }
    free(c);
}

static scan_quiet_check*
quiet_check_get(dsd_engine_scan_owner* owner) {
    return owner ? (scan_quiet_check*)owner->quiet : NULL;
}

static scan_quiet_check*
quiet_check_ensure(dsd_engine_scan_owner* owner) {
    if (!owner->quiet) {
        owner->quiet = calloc(1, sizeof(scan_quiet_check));
        owner->quiet_free = owner->quiet ? quiet_check_free : NULL;
    }
    return (scan_quiet_check*)owner->quiet;
}

/* Let go of the tap's request, if one is outstanding, and end in @p phase. */
static void
quiet_end(scan_quiet_check* c, scan_quiet_phase phase) {
    if (c->ticket != 0U) {
        rtl_signal_quiet_cancel(c->ticket);
        c->ticket = 0U;
    }
    c->phase = phase;
}

static int
quiet_check_active(const scan_quiet_check* c) {
    return c->phase == SCAN_QUIET_WAIT_CAPTURE || c->phase == SCAN_QUIET_REQUESTED || c->phase == SCAN_QUIET_READY;
}

static int
quiet_key_equal(const scan_quiet_visit_key* a, const scan_quiet_visit_key* b) {
    return a->owner == b->owner && a->index == b->index && a->session == b->session && a->epoch == b->epoch
           && a->tune_request == b->tune_request;
}

static uint8_t
quiet_landing_by(int owner) {
    return (uint8_t)(owner == SCAN_QUIET_OWNER_TRUNK ? DSD_ENGINE_SCAN_LANDING_TARGET : DSD_ENGINE_SCAN_LANDING_ROW);
}

/* Whether @p landing is the one the in-flight park of @p c lands: its row or target, session, tune and frequency, and
   the very next epoch after the check was published. */
static int
quiet_landing_adopts(const scan_quiet_check* c, const dsd_engine_scan_landing* landing) {
    return landing->by == quiet_landing_by(c->key.owner) && landing->index == c->key.index
           && landing->session == c->key.session && landing->tune_request == c->key.tune_request
           && landing->hz == c->frequency_hz && landing->epoch == c->epoch_counter + 1U;
}

/* Whether the visit @p c measures is still the one on air; a park in flight it measured is adopted (@p adopt) once
   its own landing is noted. */
static int
quiet_visit_current(const dsd_engine_scan_owner* owner, scan_quiet_check* c, int adopt) {
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (c->key.epoch != 0U) {
        return landing->by == quiet_landing_by(c->key.owner) && landing->epoch == c->key.epoch;
    }
    if (landing->by == DSD_ENGINE_SCAN_LANDING_NONE) {
        return owner->landing_epoch == c->epoch_counter;
    }
    if (!quiet_landing_adopts(c, landing)) {
        return 0;
    }
    if (adopt) {
        if (c->visit_set && quiet_key_equal(&c->visit_key, &c->key)) {
            c->visit_key.epoch = landing->epoch; /* the park's landing is the same visit */
        }
        c->key.epoch = landing->epoch;
        if (!c->origin_set) {
            /* The hunt never read the park's completion (another landing succeeded first): the wait is bounded from
               the landing's note. */
            c->origin_set = 1;
            c->origin_ms = landing->landed_realtime_ms;
        }
    }
    return 1;
}

static uint64_t
quiet_age_ms(uint64_t now_ms, uint64_t since_ms) {
    return now_ms >= since_ms ? now_ms - since_ms : 0U;
}

/* Whether a sync or a carrier was heard since the visit's activity baseline. */
static int
quiet_visit_heard_sync(const dsd_engine_scan_owner* owner, const scan_quiet_check* c) {
    return c->visit_set && quiet_key_equal(&c->visit_key, &c->key) && owner->sync_seq != c->sync_seq;
}

/* Reconcile: a changed visit or revision suspends the check; a sync since the visit's activity baseline, or a READY
   result past its age, ends it. */
static void
quiet_reconcile(const dsd_opts* opts, const dsd_state* state, const dsd_engine_scan_owner* owner, scan_quiet_check* c,
                uint64_t now_ms) {
    if (!quiet_check_active(c)) {
        return;
    }
    scan_quiet_revisions now_rev;
    scan_quiet_revisions_read(opts, state, &now_rev);
    if (!quiet_visit_current(owner, c, 1) || !quiet_revisions_equal(&now_rev, &c->revisions)) {
        quiet_end(c, SCAN_QUIET_SUSPENDED);
        return;
    }
    if (quiet_visit_heard_sync(owner, c)) {
        quiet_end(c, SCAN_QUIET_DONE);
        return;
    }
    if (c->phase == SCAN_QUIET_READY && quiet_age_ms(now_ms, c->ready_ms) > SCAN_QUIET_READY_MAX_AGE_MS) {
        c->phase = SCAN_QUIET_DONE;
    }
}

/* The capture of the landed channel, or 0 while there is none yet: a capture of the stream's live output generation,
   newer than the one read at the tune request, centred where the visit landed. */
static int
quiet_landed_capture(const scan_quiet_check* c, dsd_signal_capture* cap) {
    if (!rtl_signal_capture_get(cap)) {
        return 0;
    }
    return cap->generation != 0U && cap->generation == dsd_rtl_stream_metrics_hook_stream_generation()
           && cap->generation != c->generation_before
           && (int64_t)cap->capture_hz - cap->capture_offset_hz == (int64_t)c->frequency_hz;
}

/* Whether the calibration covers the capture's geometry: an Fs/4-shifted capture (centred I/Q, an offset-tuned E4000
   or the shift off put the channel on DC), at a validated rate, with the analog width validated for its kind there. */
static int
quiet_geometry_supported(const scan_quiet_check* c, const dsd_signal_capture* cap) {
    if (cap->rotation_hz == 0 || !dsd_signal_quiet_rate_supported(cap->rate_hz)) {
        return 0;
    }
    return !(c->classes & QUIET_CLASS_ANALOG)
           || dsd_signal_quiet_analog_supported(c->analog_kind, c->analog_width_hz, cap->rate_hz);
}

static void
quiet_request(scan_quiet_check* c, const dsd_signal_capture* cap, uint64_t now_ms) {
    dsd_signal_quiet_request request;
    DSD_MEMSET(&request, 0, sizeof request);
    request.request_id = ++c->request_seq;
    request.capture = *cap;
    request.frequency_hz = c->frequency_hz;
    request.classes = c->classes;
    request.analog_width_hz = (c->classes & QUIET_CLASS_ANALOG) ? c->analog_width_hz : 0U;
    uint32_t ticket = 0U;
    if (rtl_signal_quiet_request(&request, &ticket, NULL) != 0) {
        /* Refused (a Signal search holds the tap, or no memory): no check this visit. */
        c->phase = SCAN_QUIET_DONE;
        return;
    }
    c->ticket = ticket;
    c->deadline_ms = now_ms + SCAN_QUIET_REQUEST_DEADLINE_MS;
    c->phase = SCAN_QUIET_REQUESTED;
}

static void
quiet_wait_capture(scan_quiet_check* c, uint64_t now_ms) {
    if (!c->origin_set) {
        /* A park still in flight: its wait starts once the tuning ledger published its completion. */
        if (!dsd_trunk_tuning_request_landed(c->key.tune_request)) {
            return;
        }
        c->origin_set = 1;
        c->origin_ms = now_ms;
    }
    if (quiet_age_ms(now_ms, c->origin_ms) >= SCAN_QUIET_CAPTURE_WAIT_MS) {
        c->phase = SCAN_QUIET_DONE;
        return;
    }
    if (now_ms < c->next_capture_check_ms) {
        return;
    }
    c->next_capture_check_ms = now_ms + SCAN_QUIET_CAPTURE_RECHECK_MS;
    dsd_signal_capture cap;
    if (!quiet_landed_capture(c, &cap)) {
        return;
    }
    if (!quiet_geometry_supported(c, &cap)) {
        c->phase = SCAN_QUIET_DONE;
        return;
    }
    quiet_request(c, &cap, now_ms);
}

static int
quiet_failure_retries(dsd_signal_measure_failure failure) {
    return failure == DSD_SIGNAL_MEASURE_SUPERSEDED || failure == DSD_SIGNAL_MEASURE_CAPTURE_CHANGED
           || failure == DSD_SIGNAL_MEASURE_DROPPED;
}

static void
quiet_collect(scan_quiet_check* c, uint64_t now_ms) {
    const int status = rtl_signal_quiet_status(c->ticket);
    if (status == 0) {
        if (now_ms >= c->deadline_ms) {
            quiet_end(c, SCAN_QUIET_DONE);
        }
        return;
    }
    dsd_signal_quiet_result result;
    DSD_MEMSET(&result, 0, sizeof result);
    const int taken = rtl_signal_quiet_take(c->ticket, &result);
    if (taken == 0) {
        return;
    }
    c->ticket = 0U;
    if (taken > 0) {
        if (result.request_id == c->request_seq && result.valid && result.quiet) {
            c->result = result;
            c->ready_ms = result.completed_realtime_ns != 0U ? result.completed_realtime_ns / 1000000U : now_ms;
            c->phase = SCAN_QUIET_READY;
        } else {
            c->phase = SCAN_QUIET_DONE; /* busy, or not measurable */
        }
        return;
    }
    if (quiet_failure_retries(result.failure) && c->retries < SCAN_QUIET_MAX_RETRIES) {
        c->retries++;
        c->next_capture_check_ms = 0U;
        c->phase = SCAN_QUIET_WAIT_CAPTURE;
        return;
    }
    c->phase = SCAN_QUIET_DONE;
}

/* Start, collect or expire, by phase. */
static void
quiet_step(scan_quiet_check* c, uint64_t now_ms) {
    if (c->phase == SCAN_QUIET_WAIT_CAPTURE) {
        quiet_wait_capture(c, now_ms);
    } else if (c->phase == SCAN_QUIET_REQUESTED) {
        quiet_collect(c, now_ms);
    }
}

static int
quiet_capture_equal(const dsd_signal_capture* a, const dsd_signal_capture* b) {
    return a->generation == b->generation && a->rate_hz == b->rate_hz && a->capture_hz == b->capture_hz
           && a->capture_offset_hz == b->capture_offset_hz && a->rotation_hz == b->rotation_hz && a->gain == b->gain
           && a->drops == b->drops;
}

/* For a reconciled READY result past the class and revision checks: the tap's capture and the live output
   generation are still the result's, no tune is pending, no analog carrier is heard and, under trunk scan, no allowed
   activity came since the visit's activity baseline. */
static int
quiet_ready_passes(const dsd_opts* opts, const dsd_state* state, const scan_quiet_check* c) {
    dsd_signal_capture cap;
    if (!rtl_signal_capture_get(&cap) || !quiet_capture_equal(&cap, &c->result.capture)
        || cap.generation != dsd_rtl_stream_metrics_hook_stream_generation()) {
        return 0;
    }
    /* Lock-free: no tune in flight or failed-gated anywhere in the ledger. */
    if (!dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation())) {
        return 0;
    }
    /* An analog carrier heard, whatever its tone verdict. */
    if (dsd_scan_analog_carrier_open(opts, state)
        || dsd_scan_analog_tone_gate(opts, state) != DSD_ANALOG_TONE_GATE_OFF) {
        return 0;
    }
    return c->key.owner != SCAN_QUIET_OWNER_TRUNK
           || !dsd_engine_trunk_scan_quiet_activity_since(state, c->visit_decode_m);
}

int
scan_quiet_session_supported(const dsd_opts* opts) {
    return opts && opts->scan_skip_silent && opts->audio_in_type == AUDIO_IN_RTL && !opts->iq_replay_active
           && !opts->iq_replay_requested && !dsd_opts_input_is_iq_replay(opts)
           && dsd_decode_clock_source() != DSD_DECODE_CLOCK_REPLAY;
}

int
scan_quiet_wants_eligibility(const dsd_opts* opts, const dsd_state* state, int owner) {
    if (!opts || !opts->scan_skip_silent) {
        return 0;
    }
    const dsd_engine_scan_owner* scan_owner = quiet_owner_const(state);
    const scan_quiet_check* c = scan_owner ? (const scan_quiet_check*)scan_owner->quiet : NULL;
    if (!c || c->phase != SCAN_QUIET_DONE || c->key.owner != owner) {
        return 1;
    }
    scan_quiet_check probe = *c;
    return !quiet_visit_current(scan_owner, &probe, 0);
}

/* Whether @p e can be measured: the owner's conditions, the session's, a class to judge, and a key that is the visit
   on air (a landed one) or a park in flight (no landing). */
static int
quiet_publication_eligible(const dsd_opts* opts, const dsd_engine_scan_owner* owner, int owner_kind,
                           const scan_quiet_eligibility* e) {
    if (!e->eligible || e->key.owner != owner_kind || !scan_quiet_session_supported(opts) || e->classes == 0U
        || e->frequency_hz == 0U || ((e->classes & QUIET_CLASS_ANALOG) && e->analog_width_hz == 0U)) {
        return 0;
    }
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (e->key.epoch == 0U) {
        return owner_kind == SCAN_QUIET_OWNER_TRUNK && e->key.tune_request != 0U
               && landing->by == DSD_ENGINE_SCAN_LANDING_NONE;
    }
    return landing->by == quiet_landing_by(owner_kind) && landing->epoch == e->key.epoch
           && landing->index == e->key.index && landing->session == e->key.session && landing->hz == e->frequency_hz;
}

static int
quiet_published_as_measured(const scan_quiet_check* c, const scan_quiet_eligibility* e) {
    return quiet_key_equal(&c->key, &e->key) && c->classes == e->classes && c->analog_kind == e->analog_kind
           && c->analog_width_hz == e->analog_width_hz && c->dsp_rate_hz == e->dsp_rate_hz
           && c->frequency_hz == e->frequency_hz && quiet_revisions_equal(&c->revisions, &e->revisions);
}

/* The effective classes, the analog kind and width (with the analog class) and the DSP rate in force
   (@p current, this pass's facts) are still the ones measured. */
static int
quiet_measured_as_current(const scan_quiet_check* c, const scan_quiet_eligibility* current) {
    if (!current || current->classes != c->classes || current->dsp_rate_hz != c->dsp_rate_hz) {
        return 0;
    }
    return !(c->classes & QUIET_CLASS_ANALOG)
           || (current->analog_kind == c->analog_kind && current->analog_width_hz == c->analog_width_hz);
}

static void
quiet_start(const dsd_engine_scan_owner* owner, scan_quiet_check* c, const scan_quiet_eligibility* e, uint64_t now_ms) {
    const int landed = e->key.epoch != 0U;
    /* A landed visit's first check waits for its capture from the landing's note. One that starts again on a visit
       already published (after a suspension, eligible only now, or with other facts) waits from now: the stream may
       only now make the capture it needs (a receive-profile or width change moves the output generation). */
    const int restart = landed && quiet_key_equal(&c->key, &e->key);
    quiet_end(c, SCAN_QUIET_WAIT_CAPTURE);
    c->key = e->key;
    c->epoch_counter = owner->landing_epoch;
    c->classes = e->classes;
    c->analog_kind = e->analog_kind;
    c->analog_width_hz = e->analog_width_hz;
    c->dsp_rate_hz = e->dsp_rate_hz;
    c->frequency_hz = e->frequency_hz;
    c->revisions = e->revisions;
    c->generation_before = landed ? owner->landing.generation_before_tune : owner->tune_generation_before;
    c->origin_set = landed;
    c->origin_ms = restart ? now_ms : (landed ? owner->landing.landed_realtime_ms : 0U);
    c->next_capture_check_ms = 0U;
    c->retries = 0;
}

/* The visit's activity baseline for @p key: its landing's note, or a park still in flight's first publication. A later
   publication of the same visit keeps it, so what was heard while its check was suspended still counts. */
static void
quiet_visit_baseline(const dsd_engine_scan_owner* owner, scan_quiet_check* c, const scan_quiet_visit_key* key) {
    if (c->visit_set && quiet_key_equal(&c->visit_key, key)) {
        return;
    }
    c->visit_set = 1;
    c->visit_key = *key;
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (key->epoch != 0U && landing->by == quiet_landing_by(key->owner) && landing->epoch == key->epoch) {
        /* A landed visit counts from its landing's note. */
        c->sync_seq = landing->sync_seq;
        c->visit_decode_m = landing->landed_decode_m;
        return;
    }
    /* A park still in flight counts from its first publication. */
    c->sync_seq = owner->sync_seq;
    c->visit_decode_m = dsd_decode_now_mono_s();
}

/* A publication's reconcile, and whether @p e's visit has its check ended already: DONE for the visit, or a sync or a
   carrier heard in it since its activity baseline, its check suspended or not (then DONE now: none is made for it
   again). */
static int
quiet_publication_ends_visit(const dsd_opts* opts, const dsd_state* state, const dsd_engine_scan_owner* owner,
                             scan_quiet_check* c, const scan_quiet_eligibility* e, uint64_t now_ms) {
    quiet_reconcile(opts, state, owner, c, now_ms);
    if ((c->phase == SCAN_QUIET_DONE || c->phase == SCAN_QUIET_SUSPENDED) && c->key.epoch == 0U) {
        /* A park measured or suspended in flight whose landing has been noted: the landing is still its visit. */
        (void)quiet_visit_current(owner, c, 1);
    }
    if (c->phase == SCAN_QUIET_DONE && quiet_key_equal(&c->key, &e->key)) {
        return 1; /* terminal for the visit */
    }
    quiet_visit_baseline(owner, c, &e->key);
    if (owner->sync_seq == c->sync_seq) {
        return 0;
    }
    quiet_end(c, SCAN_QUIET_DONE);
    c->key = e->key;
    return 1;
}

void
scan_quiet_publish_eligibility(const dsd_opts* opts, dsd_state* state, int owner_kind,
                               const scan_quiet_eligibility* e) {
    if (!opts || !state || !e || !opts->scan_skip_silent) {
        return;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_ensure(state);
    scan_quiet_check* c = owner ? quiet_check_ensure(owner) : NULL;
    if (!c) {
        return;
    }
    const uint64_t now_ms = dsd_realtime_mono_ms();
    if (quiet_publication_ends_visit(opts, state, owner, c, e, now_ms)) {
        return;
    }
    if (!quiet_publication_eligible(opts, owner, owner_kind, e)) {
        quiet_end(c, SCAN_QUIET_SUSPENDED);
        c->key = e->key;
        /* What a park in flight is adopted by at its landing, so the visit keeps its baseline even when no check of
           it ever started. */
        c->epoch_counter = owner->landing_epoch;
        c->frequency_hz = e->frequency_hz;
        return;
    }
    if (c->phase == SCAN_QUIET_SUSPENDED || !quiet_published_as_measured(c, e)) {
        quiet_start(owner, c, e, now_ms);
    }
    quiet_step(c, now_ms);
}

int
dsd_engine_scan_quiet_service(const dsd_opts* opts, dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!c || c->phase == SCAN_QUIET_SUSPENDED || c->phase == SCAN_QUIET_DONE) {
        return 0;
    }
    const uint64_t now_ms = dsd_realtime_mono_ms();
    quiet_reconcile(opts, state, owner, c, now_ms);
    quiet_step(c, now_ms);
    return c->phase == SCAN_QUIET_READY && quiet_ready_passes(opts, state, c);
}

int
scan_quiet_apply(const dsd_opts* opts, dsd_state* state, int owner_kind, const scan_quiet_eligibility* current) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!opts || !opts->scan_skip_silent || !c || c->phase != SCAN_QUIET_READY || c->key.owner != owner_kind) {
        return 0;
    }
    quiet_reconcile(opts, state, owner, c, dsd_realtime_mono_ms());
    if (c->phase != SCAN_QUIET_READY || c->key.epoch == 0U) {
        /* Declined by the reconcile, or a park whose landing the coordinator has not noted yet. */
        return 0;
    }
    c->phase = SCAN_QUIET_DONE; /* consumed, or declined: one skip per visit */
    return quiet_measured_as_current(c, current) && quiet_ready_passes(opts, state, c);
}

void
scan_quiet_reconcile_declined(const dsd_opts* opts, dsd_state* state, int owner_kind, int guard_lost) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!opts || !c) {
        return;
    }
    quiet_reconcile(opts, state, owner, c, dsd_realtime_mono_ms());
    if (c->phase == SCAN_QUIET_READY && c->key.owner == owner_kind && !guard_lost) {
        c->phase = SCAN_QUIET_DONE;
    }
}

void
dsd_engine_scan_quiet_invalidate(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (!owner) {
        return;
    }
    owner->quiet_inv_seq++;
    scan_quiet_check* c = quiet_check_get(owner);
    if (c && quiet_check_active(c)) {
        quiet_end(c, SCAN_QUIET_SUSPENDED);
    }
}

void
scan_quiet_log_skip(const dsd_state* state, const char* name, int row) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    const scan_quiet_check* c = owner ? (const scan_quiet_check*)owner->quiet : NULL;
    if (!c) {
        return;
    }
    char line[192];
    (void)scan_quiet_format_skip(line, sizeof line, name, row, c->frequency_hz, c->result.power_dbfs,
                                 c->result.floor_dbfs, c->result.max_ratio_db);
    LOG_INFO("%s", line);
}

double
scan_quiet_visit_decode_m(const dsd_state* state) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    const scan_quiet_check* c = owner ? (const scan_quiet_check*)owner->quiet : NULL;
    return c ? c->visit_decode_m : 0.0;
}

scan_quiet_phase
scan_quiet_phase_get(const dsd_state* state) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    const scan_quiet_check* c = owner ? (const scan_quiet_check*)owner->quiet : NULL;
    return c ? c->phase : SCAN_QUIET_SUSPENDED;
}

scan_quiet_visit_key
scan_quiet_key_get(const dsd_state* state) {
    const dsd_engine_scan_owner* owner = quiet_owner_const(state);
    const scan_quiet_check* c = owner ? (const scan_quiet_check*)owner->quiet : NULL;
    if (c) {
        return c->key;
    }
    const scan_quiet_visit_key none = {0};
    return none;
}

#else /* !USE_RADIO: no tap, so no check is ever made. */

int
scan_quiet_session_supported(const dsd_opts* opts) {
    (void)opts;
    return 0;
}

int
scan_quiet_wants_eligibility(const dsd_opts* opts, const dsd_state* state, int owner) {
    (void)opts;
    (void)state;
    (void)owner;
    return 0;
}

void
scan_quiet_publish_eligibility(const dsd_opts* opts, dsd_state* state, int owner_kind,
                               const scan_quiet_eligibility* e) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    (void)e;
}

int
dsd_engine_scan_quiet_service(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

int
scan_quiet_apply(const dsd_opts* opts, dsd_state* state, int owner_kind, const scan_quiet_eligibility* current) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    (void)current;
    return 0;
}

void
scan_quiet_reconcile_declined(const dsd_opts* opts, dsd_state* state, int owner_kind, int guard_lost) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    (void)guard_lost;
}

void
dsd_engine_scan_quiet_invalidate(dsd_state* state) {
    dsd_engine_scan_quiet_note_change(state);
}

void
scan_quiet_log_skip(const dsd_state* state, const char* name, int row) {
    (void)state;
    (void)name;
    (void)row;
}

double
scan_quiet_visit_decode_m(const dsd_state* state) {
    (void)state;
    return 0.0;
}

scan_quiet_phase
scan_quiet_phase_get(const dsd_state* state) {
    (void)state;
    return SCAN_QUIET_SUSPENDED;
}

scan_quiet_visit_key
scan_quiet_key_get(const dsd_state* state) {
    (void)state;
    const scan_quiet_visit_key none = {0};
    return none;
}

#endif

#ifdef USE_RADIO
/* The setting turned on mid-visit: the visit landed now gets no check, since the syncs it heard while the setting was
   off moved no sync sequence. The setting applies from the next visit. A park still in flight has heard nothing of its
   channel yet, and is checked. */
static void
quiet_skip_landed_visit(dsd_engine_scan_owner* owner) {
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (landing->by != DSD_ENGINE_SCAN_LANDING_ROW && landing->by != DSD_ENGINE_SCAN_LANDING_TARGET) {
        return;
    }
    scan_quiet_check* c = quiet_check_ensure(owner);
    if (!c) {
        return;
    }
    quiet_end(c, SCAN_QUIET_DONE);
    c->key.owner = landing->by == DSD_ENGINE_SCAN_LANDING_ROW ? SCAN_QUIET_OWNER_Y : SCAN_QUIET_OWNER_TRUNK;
    c->key.index = landing->index;
    c->key.session = landing->session;
    c->key.epoch = landing->epoch;
    c->key.tune_request = landing->tune_request;
    c->epoch_counter = owner->landing_epoch;
    c->frequency_hz = landing->hz;
}
#endif

void
dsd_engine_scan_quiet_setting_changed(dsd_state* state, int turned_on) {
    dsd_engine_scan_quiet_invalidate(state);
#ifdef USE_RADIO
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    /* The setting turned: the visit's activity baseline is set again at its next publication (a landed visit's from
       its landing note, a park in flight's from then). Turned on, a landed visit is not checked: the setting applies
       from the next visit. */
    scan_quiet_check* c = quiet_check_get(owner);
    if (c) {
        c->visit_set = 0;
    }
    if (turned_on && owner) {
        quiet_skip_landed_visit(owner);
    }
#else
    (void)turned_on;
#endif
}
