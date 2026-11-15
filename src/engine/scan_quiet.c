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
#include <dsd-neo/runtime/frame_sync_hooks.h>
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

#ifdef DSD_NEO_TEST_HOOKS
uint64_t (*scan_quiet_test_realtime_ms)(void) = NULL;
#endif

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
    /* A receive-profile or width request still in flight changes what the channel is received as; one that repeats the
       profile the stream runs (the hunt re-asking for its own) changes nothing and moves no change number (issue #834
       B8). */
    out->receive_change_seq = rtl_stream_receive_change_seq();
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

int
scan_quiet_decoder_set_unchecked(const dsd_opts* opts) {
    return opts && (opts->frame_x2tdma || opts->frame_provoice || opts->frame_tetra);
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
    if (scan_quiet_decoder_set_unchecked(opts)) {
        return 0U;
    }
    const uint8_t digital = (uint8_t)((quiet_decoder_wide(opts) ? QUIET_CLASS_12K5 : 0U)
                                      | (quiet_decoder_narrow(opts) ? QUIET_CLASS_6K25 : 0U));
    return scan_quiet_monitor_classes(opts, digital, analog_kind, analog_width_hz);
}

uint8_t
scan_quiet_monitor_classes(const dsd_opts* opts, uint8_t digital, int* analog_kind, uint32_t* analog_width_hz) {
    if (!opts || opts->monitor_input_audio != 1 || digital == 0U) {
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

/* The check's realtime clock: a test's, when it set one (DSD_NEO_TEST_HOOKS builds only). */
static uint64_t
quiet_realtime_ms(void) {
#ifdef DSD_NEO_TEST_HOOKS
    if (scan_quiet_test_realtime_ms) {
        return scan_quiet_test_realtime_ms();
    }
#endif
    return dsd_realtime_mono_ms();
}

static int
quiet_revisions_equal(const scan_quiet_revisions* a, const scan_quiet_revisions* b) {
    return a->chan_map_seq == b->chan_map_seq && a->lcn_list_rev == b->lcn_list_rev
           && a->lcn_avoid_rev == b->lcn_avoid_rev && a->trunk_scan_avoid_rev == b->trunk_scan_avoid_rev
           && a->quiet_inv_seq == b->quiet_inv_seq && a->lcn_scan_session == b->lcn_scan_session
           && a->trunk_scan_lists_seq == b->trunk_scan_lists_seq && a->trunk_edit_seq == b->trunk_edit_seq
           && a->y_edits_epoch == b->y_edits_epoch && a->pcm_input_generation == b->pcm_input_generation
           && a->receive_change_seq == b->receive_change_seq && a->nfm_width_hz == b->nfm_width_hz
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
    /* The frame-boundary settle wait (issue #834 D8): a request a frame-boundary service starts waits
       SCAN_QUIET_FRAME_SETTLE_MS (realtime) from settle_from_ms, the first qualifying capture the service saw (the first
       request) or the retryable failure it collected (a retry); settle_set 0 while there is neither. capture_seen: a
       qualifying capture was seen in this wait before its deadline, so the wait is no longer for a capture and its
       deadline expires it only once no qualifying capture is left. */
    int settle_set;
    uint64_t settle_from_ms;
    int capture_seen;
    /* Suspended while the visit's landing was still in force (issue #834 B8): the next frame boundary publishes it
       again, once (dsd_engine_scan_quiet_take_republish()); a publication or a new start clears it. */
    int republish_owed;
    int retries;
    uint64_t request_seq;
    uint32_t ticket;
    uint64_t deadline_ms;
    /* The visit's activity baseline: the activity sequence and the decode time when its tune was issued (a landed
       visit carries them on its landing's note; a park still in flight takes them at its first publication, and keeps
       them when its landing adopts it), kept for that visit across its suspensions, restarts and retries. Checked
       content, a carrier or allowed activity since then ends the visit's check, whatever phase the check was in.
       visit_set 0 before the visit's first publication. */
    int visit_set;
    scan_quiet_visit_key visit_key;
    uint64_t activity_seq;
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

/* Whether a frame's verdict is still awaited (issue #834 D8): a sync whose frame has not ended, or a PARTIAL frame's
   hold not over yet (both sides on the decode clock). A quiet result is neither handed back, applied nor declined while
   one is; the check's reconcile, collection and deadlines go on. */
static int
quiet_pending(const dsd_engine_scan_owner* owner) {
    if (owner->frame_pending) {
        return 1;
    }
    return owner->pending_until_m > 0.0 && dsd_decode_now_mono_s() < owner->pending_until_m;
}

/* Whether checked content or a carrier was heard since the visit's activity baseline. */
static int
quiet_visit_heard_activity(const dsd_engine_scan_owner* owner, const scan_quiet_check* c) {
    return c->visit_set && quiet_key_equal(&c->visit_key, &c->key) && owner->activity_seq != c->activity_seq;
}

/* Reconcile: a changed visit or revision suspends the check; activity since the visit's activity baseline, or a READY
   result past its age, ends it. A pending frame verdict ends nothing: a READY result it holds back still ages out. */
static void
quiet_reconcile(const dsd_opts* opts, const dsd_state* state, const dsd_engine_scan_owner* owner, scan_quiet_check* c,
                uint64_t now_ms) {
    if (!quiet_check_active(c)) {
        return;
    }
    scan_quiet_revisions now_rev;
    scan_quiet_revisions_read(opts, state, &now_rev);
    const int current = quiet_visit_current(owner, c, 1);
    if (!current || !quiet_revisions_equal(&now_rev, &c->revisions)) {
        quiet_end(c, SCAN_QUIET_SUSPENDED);
        c->republish_owed = current;
        return;
    }
    if (quiet_visit_heard_activity(owner, c)) {
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

/* The WAIT_CAPTURE deadline: 1000 ms from its origin, the bound on obtaining a qualifying capture. Where the
   frame-boundary settle wait runs (a frame-boundary call, @p at_frame, or a capture already seen), it is
   max(origin + 1000 ms, settle start + SCAN_QUIET_FRAME_SETTLE_MS), so waiting out the settle never expires a check. A
   legacy opportunity with no capture seen keeps the plain bound: a visit with no frame-boundary service times every
   request and expiry exactly as before. */
static uint64_t
quiet_capture_deadline_ms(const scan_quiet_check* c, int at_frame) {
    const uint64_t bound_ms = c->origin_ms + SCAN_QUIET_CAPTURE_WAIT_MS;
    if (!c->settle_set || !(at_frame || c->capture_seen)) {
        return bound_ms;
    }
    const uint64_t settled_ms = c->settle_from_ms + SCAN_QUIET_FRAME_SETTLE_MS;
    return settled_ms > bound_ms ? settled_ms : bound_ms;
}

/* WAIT_CAPTURE, from a legacy opportunity (the hunt's 300-symbol point, an engine publication: @p at_frame 0) or a
   frame boundary (@p at_frame 1). A legacy call reads the capture at most every 50 ms and requests as soon as it finds
   a qualifying one. A frame-boundary call reads it without moving that schedule and requests only once the settle wait
   has run since the first qualifying capture the service saw (or since the retryable failure, for a retry). */
static void
quiet_wait_capture(scan_quiet_check* c, uint64_t now_ms, int at_frame) {
    if (!c->origin_set) {
        /* A park still in flight: its wait starts once the tuning ledger published its completion. */
        if (!dsd_trunk_tuning_request_landed(c->key.tune_request)) {
            return;
        }
        c->origin_set = 1;
        c->origin_ms = now_ms;
    }
    const int expired = now_ms >= quiet_capture_deadline_ms(c, at_frame);
    if (expired && !c->capture_seen) {
        c->phase = SCAN_QUIET_DONE;
        return;
    }
    if (!at_frame) {
        if (now_ms < c->next_capture_check_ms) {
            return;
        }
        c->next_capture_check_ms = now_ms + SCAN_QUIET_CAPTURE_RECHECK_MS;
    }
    dsd_signal_capture cap;
    if (!quiet_landed_capture(c, &cap)) {
        if (expired) {
            c->phase = SCAN_QUIET_DONE; /* the capture seen is gone, past the deadline */
        }
        return;
    }
    if (!c->capture_seen) {
        c->capture_seen = 1;
        if (!c->settle_set) {
            c->settle_set = 1;
            c->settle_from_ms = now_ms;
        }
    }
    if (at_frame && quiet_age_ms(now_ms, c->settle_from_ms) < SCAN_QUIET_FRAME_SETTLE_MS) {
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
    dsd_signal_quiet_result result;
    DSD_MEMSET(&result, 0, sizeof result);
    const int taken = rtl_signal_quiet_status(c->ticket) == 0 ? 0 : rtl_signal_quiet_take(c->ticket, &result);
    if (taken == 0) {
        if (now_ms >= c->deadline_ms) {
            quiet_end(c, SCAN_QUIET_DONE);
        }
        return;
    }
    c->ticket = 0U;
    /* The answer bound holds for an answer too: one that completed past the deadline (on the realtime clock the
       deadline is on; the poll that finds it when it carries no completion time) is no answer in time, whatever it
       says, and ends the visit as the timeout does, with no retry. */
    const uint64_t completed_ms = result.completed_realtime_ns != 0U ? result.completed_realtime_ns / 1000000U : now_ms;
    if (completed_ms >= c->deadline_ms) {
        c->phase = SCAN_QUIET_DONE;
        return;
    }
    if (taken > 0) {
        if (result.request_id == c->request_seq && result.valid && result.quiet) {
            c->result = result;
            c->ready_ms = completed_ms;
            c->phase = SCAN_QUIET_READY;
        } else {
            c->phase = SCAN_QUIET_DONE; /* busy, or not measurable */
        }
        return;
    }
    if (quiet_failure_retries(result.failure) && c->retries < SCAN_QUIET_MAX_RETRIES) {
        c->retries++;
        c->next_capture_check_ms = 0U;
        /* A retry started at a frame boundary waits out the settle from this failure. */
        c->settle_set = 1;
        c->settle_from_ms = now_ms;
        c->capture_seen = 0;
        c->phase = SCAN_QUIET_WAIT_CAPTURE;
        return;
    }
    c->phase = SCAN_QUIET_DONE;
}

/* Start, collect or expire, by phase; @p at_frame for a frame-boundary call (quiet_wait_capture()). */
static void
quiet_step(scan_quiet_check* c, uint64_t now_ms, int at_frame) {
    if (c->phase == SCAN_QUIET_WAIT_CAPTURE) {
        quiet_wait_capture(c, now_ms, at_frame);
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
    return opts && opts->scan_skip_silent && dsd_opts_radio_can_tune(opts)
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
    c->settle_set = 0;
    c->settle_from_ms = 0U;
    c->capture_seen = 0;
    c->republish_owed = 0;
    c->retries = 0;
}

/* The visit's activity baseline for @p key: its tune's issue, which its landing's note carries, or which a park still
   in flight reads at its first publication. A later publication of the same visit keeps it, so what was heard while
   its check was suspended still counts. */
static void
quiet_visit_baseline(const dsd_engine_scan_owner* owner, scan_quiet_check* c, const scan_quiet_visit_key* key) {
    if (c->visit_set && quiet_key_equal(&c->visit_key, key)) {
        return;
    }
    c->visit_set = 1;
    c->visit_key = *key;
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (key->epoch != 0U && landing->by == quiet_landing_by(key->owner) && landing->epoch == key->epoch) {
        /* A landed visit counts from its tune's issue, which its landing's note carries. */
        c->activity_seq = landing->activity_seq;
        c->visit_decode_m = landing->tune_decode_m;
        return;
    }
    /* A park still in flight counts from its tune's issue, the scan's last tune request. */
    c->activity_seq = owner->tune_activity_seq;
    c->visit_decode_m = owner->tune_decode_m;
}

/* The scan tune that put @p key's visit on air (dsd_engine_scan_owner::tune_seq): its landing's, or for a park still
   in flight, or one whose landing is not noted yet, the scan's last. */
static uint64_t
quiet_visit_tune_seq(const dsd_engine_scan_owner* owner, const scan_quiet_visit_key* key) {
    const dsd_engine_scan_landing* landing = &owner->landing;
    if (key->epoch != 0U && landing->by == quiet_landing_by(key->owner) && landing->epoch == key->epoch) {
        return landing->tune_seq;
    }
    return owner->tune_seq;
}

/* A publication's reconcile, and whether @p e's visit has its check ended already: DONE for the visit, a visit tuned
   before the setting was turned on (then DONE now: the setting applies from the next tune), or checked content or a
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
    if (quiet_visit_tune_seq(owner, &e->key) < owner->quiet_first_tune_seq) {
        /* Tuned before the setting turned on: what it heard while off moved nothing. A park in flight keeps what its
           landing adopts it by, as an ineligible one does. */
        quiet_end(c, SCAN_QUIET_DONE);
        c->key = e->key;
        c->epoch_counter = owner->landing_epoch;
        c->frequency_hz = e->frequency_hz;
        return 1;
    }
    quiet_visit_baseline(owner, c, &e->key);
    if (owner->activity_seq == c->activity_seq) {
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
    const uint64_t now_ms = quiet_realtime_ms();
    c->republish_owed = 0; /* this is its publication */
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
    quiet_step(c, now_ms, 0);
}

/* The service of a check under way (@p c, reconciled and stepped here), for the hunt or a frame boundary. */
static int
quiet_service(const dsd_opts* opts, const dsd_state* state, const dsd_engine_scan_owner* owner, scan_quiet_check* c,
              int at_frame) {
    const uint64_t now_ms = quiet_realtime_ms();
    quiet_reconcile(opts, state, owner, c, now_ms);
    quiet_step(c, now_ms, at_frame);
    if (!quiet_check_active(c)) {
        return DSD_SCAN_QUIET_NONE;
    }
    /* A READY result a frame's pending verdict holds back is LIVE: it stays READY, and still ages out. */
    if (c->phase == SCAN_QUIET_READY && !quiet_pending(owner) && quiet_ready_passes(opts, state, c)) {
        return DSD_SCAN_QUIET_HAND_BACK;
    }
    return DSD_SCAN_QUIET_LIVE;
}

int
dsd_engine_scan_quiet_service(const dsd_opts* opts, dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!c || c->phase == SCAN_QUIET_SUSPENDED || c->phase == SCAN_QUIET_DONE) {
        return DSD_SCAN_QUIET_NONE;
    }
    return quiet_service(opts, state, owner, c, 0);
}

int
dsd_engine_scan_quiet_frame_service(const dsd_opts* opts, dsd_state* state, int owner_kind) {
    /* The setting before anything else: with it off, the null check and one load. */
    if (!opts || !opts->scan_skip_silent) {
        return DSD_SCAN_QUIET_NONE;
    }
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!c || c->key.owner != owner_kind || c->phase == SCAN_QUIET_SUSPENDED || c->phase == SCAN_QUIET_DONE) {
        return DSD_SCAN_QUIET_NONE;
    }
    return quiet_service(opts, state, owner, c, 1);
}

int
dsd_engine_scan_quiet_frame_yield(const dsd_opts* opts, dsd_state* state) {
    /* The setting before anything else: with it off, the null check and one load. */
    return opts && opts->scan_skip_silent && opts->scanner_mode == 1
           && dsd_engine_scan_quiet_frame_service(opts, state, SCAN_QUIET_OWNER_Y) == DSD_SCAN_QUIET_HAND_BACK;
}

int
dsd_engine_scan_quiet_take_republish(const dsd_opts* opts, dsd_state* state, int owner_kind) {
    if (!opts || !opts->scan_skip_silent) {
        return 0;
    }
    scan_quiet_check* c = quiet_check_get(dsd_engine_scan_owner_get(state));
    if (!c || !c->republish_owed || c->phase != SCAN_QUIET_SUSPENDED || c->key.owner != owner_kind) {
        return 0;
    }
    c->republish_owed = 0;
    return 1;
}

int
scan_quiet_apply(const dsd_opts* opts, dsd_state* state, int owner_kind, const scan_quiet_eligibility* current) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    scan_quiet_check* c = quiet_check_get(owner);
    if (!opts || !opts->scan_skip_silent || !c || c->phase != SCAN_QUIET_READY || c->key.owner != owner_kind) {
        return 0;
    }
    quiet_reconcile(opts, state, owner, c, quiet_realtime_ms());
    if (c->phase != SCAN_QUIET_READY || c->key.epoch == 0U || quiet_pending(owner)) {
        /* Declined by the reconcile, a park whose landing the coordinator has not noted yet, or held while a frame's
           verdict is pending (kept READY: neither applied nor declined). */
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
    quiet_reconcile(opts, state, owner, c, quiet_realtime_ms());
    if (c->phase == SCAN_QUIET_READY && c->key.owner == owner_kind && !guard_lost && !quiet_pending(owner)) {
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
        c->republish_owed = quiet_visit_current(owner, c, 0);
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
    return DSD_SCAN_QUIET_NONE;
}

int
dsd_engine_scan_quiet_frame_service(const dsd_opts* opts, dsd_state* state, int owner_kind) {
    (void)opts;
    (void)state;
    (void)owner_kind;
    return DSD_SCAN_QUIET_NONE;
}

int
dsd_engine_scan_quiet_frame_yield(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

int
dsd_engine_scan_quiet_take_republish(const dsd_opts* opts, dsd_state* state, int owner_kind) {
    (void)opts;
    (void)state;
    (void)owner_kind;
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
   off moved no sync sequence, and it asks for no eligibility from now. A visit tuned before the setting turned on whose
   landing is not noted yet (in flight, or landed before the coordinator noted it) gets none either, at its publication
   (quiet_publication_ends_visit()): the setting applies from the next tune. */
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
    /* Nothing noted while the setting was on is waited for once it changes. */
    dsd_engine_scan_quiet_pending_reset(state);
#ifdef USE_RADIO
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    /* The setting turned: the visit's activity baseline is set again at its next publication (a landed visit's from
       its landing note, a park in flight's from then). Turned on, it applies from the next scan tune: no visit tuned
       before it is checked, landed or not. */
    scan_quiet_check* c = quiet_check_get(owner);
    if (c) {
        c->visit_set = 0;
    }
    if (turned_on && owner) {
        owner->quiet_first_tune_seq = owner->tune_seq + 1U;
        quiet_skip_landed_visit(owner);
    }
#else
    (void)turned_on;
#endif
}

/* Wait for a PARTIAL verdict for @p hold_ms more (capped), from now on the decode clock; a longer wait already running
   stays. */
static void
quiet_pending_extend(dsd_engine_scan_owner* owner, uint32_t hold_ms) {
    const uint32_t capped = hold_ms < SCAN_QUIET_PENDING_MAX_MS ? hold_ms : SCAN_QUIET_PENDING_MAX_MS;
    const double until_m = dsd_decode_now_mono_s() + (double)capped / 1000.0;
    if (until_m > owner->pending_until_m) {
        owner->pending_until_m = until_m;
    }
}

void
dsd_engine_scan_quiet_note_evidence(dsd_state* state, int level, uint32_t hold_ms) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (!owner) {
        return;
    }
    if (level == DSD_CONTENT_EVIDENCE_PROVEN) {
        /* Activity, once a frame however many of its checks pass; it dominates: nothing is waited for any more. */
        if (!owner->frame_pending || owner->frame_evidence != (uint8_t)DSD_CONTENT_EVIDENCE_PROVEN) {
            owner->activity_seq++;
        }
        if (owner->frame_pending) {
            owner->frame_evidence = (uint8_t)DSD_CONTENT_EVIDENCE_PROVEN;
        }
        owner->frame_hold_ms = 0U;
        owner->pending_until_m = 0.0;
        return;
    }
    if (level != DSD_CONTENT_EVIDENCE_PARTIAL) {
        return;
    }
    if (!owner->frame_pending) {
        quiet_pending_extend(owner, hold_ms);
        return;
    }
    if (owner->frame_evidence == (uint8_t)DSD_CONTENT_EVIDENCE_PROVEN) {
        return; /* the frame proved its content already */
    }
    owner->frame_evidence = (uint8_t)DSD_CONTENT_EVIDENCE_PARTIAL;
    if (hold_ms > owner->frame_hold_ms) {
        owner->frame_hold_ms = hold_ms;
    }
}

void
dsd_engine_scan_quiet_note_frame_sync(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (owner) {
        owner->frame_pending = 1U;
        owner->frame_evidence = 0U;
        owner->frame_hold_ms = 0U;
    }
}

void
dsd_engine_scan_quiet_frame_end(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (!owner || !owner->frame_pending) {
        return;
    }
    /* PROVEN cleared the wait at its note. A frame that checked nothing leaves a wait another frame started running:
       a rejected sync never cancels a verdict the protocol still holds (an NXDN LICH reject keeps the weak streak). */
    if (owner->frame_evidence == (uint8_t)DSD_CONTENT_EVIDENCE_PARTIAL) {
        quiet_pending_extend(owner, owner->frame_hold_ms);
    }
    owner->frame_pending = 0U;
    owner->frame_evidence = 0U;
    owner->frame_hold_ms = 0U;
}

void
dsd_engine_scan_quiet_pending_reset(dsd_state* state) {
    dsd_engine_scan_owner* owner = dsd_engine_scan_owner_get(state);
    if (owner) {
        dsd_engine_scan_owner_pending_clear(owner);
    }
}
