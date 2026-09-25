// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Analog receive tap: sub-audible front end, carrier hangover and tone publication.
 *
 * Issue #522. The pure core (front end, detectors, carrier) is the first half of this file;
 * the decoder-thread glue that owns DSD_STATE_EXT_DSP_ANALOG_RX, dsd_state::analog_rx and the
 * "Received tone:" log line is the second. See analog_rx_internal.h for the signal path.
 */

#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/firdes.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "analog_rx_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Front-end design (issue #522): stage 2 keeps 0-DSD_ANALOG_RX_BAND_HZ with a 60 Hz Blackman
   transition, about 28 ms of delay at 2.4 kHz; stage 1 only has to stop what would fold into
   that band. */
static const double k_stage2_transition_hz = 60.0;
static const double k_dc_corner_hz = 10.0;

/* ------------------------------------------------------------------------------------------
 * Sub-audible front end
 * ---------------------------------------------------------------------------------------- */

void
dsd_analog_subaudible_fe_clear(dsd_analog_subaudible_fe* fe) {
    if (!fe) {
        return;
    }
    DSD_MEMSET(fe->hist1, 0, sizeof(fe->hist1));
    DSD_MEMSET(fe->hist2, 0, sizeof(fe->hist2));
    DSD_MEMSET(fe->wide_delay, 0, sizeof(fe->wide_delay));
    DSD_MEMSET(fe->full_delay, 0, sizeof(fe->full_delay));
    fe->pos1 = 0;
    fe->pos2 = 0;
    fe->wide_delay_pos = 0;
    fe->full_delay_pos = 0;
    fe->full_acc = 0.0;
    fe->phase = 0;
    fe->dc_x1 = 0.0;
    fe->dc_y1 = 0.0;
}

static int
fe_design_stage1(dsd_analog_subaudible_fe* fe) {
    if (fe->decim <= 1) {
        fe->n1 = 0;
        return 1;
    }
    /* Anything within DSD_ANALOG_RX_BAND_HZ of a multiple of the output rate folds into the
       band, so the stop band has to start by out_rate - DSD_ANALOG_RX_BAND_HZ while the pass
       band only reaches DSD_ANALOG_RX_BAND_HZ. firdes sizes a Blackman design from the
       requested transition assuming its full attenuation is reached there, but the window only
       gets there over about 1.65 times that width; asking for 0.6 of the gap puts the real
       -74 dB edge inside it. */
    const double transition = 0.6 * (fe->out_rate_hz - (2.0 * DSD_ANALOG_RX_BAND_HZ));
    fe->n1 = dsd_firdes_low_pass(1.0, (double)fe->in_rate_hz, fe->out_rate_hz / 2.0, transition, DSD_WIN_BLACKMAN,
                                 fe->taps1, DSD_ANALOG_RX_STAGE1_MAX_TAPS);
    return fe->n1 > 0;
}

int
dsd_analog_subaudible_fe_configure(dsd_analog_subaudible_fe* fe, int rate_hz) {
    if (!fe) {
        return 0;
    }
    DSD_MEMSET(fe, 0, sizeof(*fe));
    fe->in_rate_hz = rate_hz;
    if (rate_hz < DSD_ANALOG_RX_MIN_RATE_HZ || rate_hz > DSD_ANALOG_RX_MAX_RATE_HZ) {
        return 0;
    }
    fe->decim = rate_hz / DSD_ANALOG_RX_TARGET_RATE_HZ;
    fe->out_rate_hz = (double)rate_hz / (double)fe->decim;
    if (!fe_design_stage1(fe)) {
        return 0;
    }
    fe->n2 = dsd_firdes_low_pass(1.0, fe->out_rate_hz, DSD_ANALOG_RX_BAND_HZ, k_stage2_transition_hz, DSD_WIN_BLACKMAN,
                                 fe->taps2, DSD_ANALOG_RX_STAGE2_MAX_TAPS);
    if (fe->n2 <= 0) {
        return 0;
    }
    /* Stage 2 is linear phase, so its delay is exactly (n2 - 1) / 2 decimated samples. */
    fe->wide_delay_len = (fe->n2 - 1) / 2;
    /* The full stream also has stage 1's delay to make up: (n1 - 1) / 2 input samples, less
       the (decim - 1) / 2 by which the span an output averages already lags its newest sample. */
    int full_len = fe->wide_delay_len;
    if (fe->decim > 1) {
        full_len += (int)lround((double)(fe->n1 - fe->decim) / (2.0 * (double)fe->decim));
    }
    const int full_cap = (int)(sizeof(fe->full_delay) / sizeof(fe->full_delay[0]));
    if (full_len > full_cap) {
        full_len = full_cap;
    }
    fe->full_delay_len = full_len > 0 ? full_len : 0;
    fe->dc_alpha = exp(-2.0 * M_PI * k_dc_corner_hz / fe->out_rate_hz);
    dsd_analog_subaudible_fe_clear(fe);
    fe->active = 1;
    return 1;
}

/* Push into a doubled delay line and return the start of the newest n samples, oldest first.
   The Blackman designs are symmetric, so tap order against that run does not matter. */
static const float*
fe_push(float* hist, int n, int* pos, float x) {
    hist[*pos] = x;
    hist[*pos + n] = x;
    *pos = (*pos + 1) % n;
    return hist + *pos;
}

static float
fe_dot(const float* taps, const float* x, int n) {
    float acc = 0.0f;
    for (int k = 0; k < n; k++) {
        acc += taps[k] * x[k];
    }
    return acc;
}

/* Stage 1 at the input rate, evaluated only when a decimated output is due. */
static int
fe_stage1(dsd_analog_subaudible_fe* fe, float x, float* y) {
    if (fe->decim <= 1) {
        *y = x;
        return 1;
    }
    const float* run = fe_push(fe->hist1, fe->n1, &fe->pos1, x);
    if (++fe->phase < fe->decim) {
        return 0;
    }
    fe->phase = 0;
    *y = fe_dot(fe->taps1, run, fe->n1);
    return 1;
}

/* The value pushed @p len outputs ago on a delay line of that length (the value itself when
   the line is empty). */
static float
fe_delay(float* line, int len, int* pos, float x) {
    if (len <= 0) {
        return x;
    }
    const float delayed = line[*pos];
    line[*pos] = x;
    *pos = (*pos + 1) % len;
    return delayed;
}

int
dsd_analog_subaudible_fe_process(dsd_analog_subaudible_fe* fe, const float* in, int count, float* band, float* wide,
                                 float* full, int out_cap) {
    if (!fe || !fe->active || !in || !band || count <= 0) {
        return 0;
    }
    const double span = (fe->decim > 1) ? (double)fe->decim : 1.0;
    int produced = 0;
    for (int i = 0; i < count && produced < out_cap; i++) {
        fe->full_acc += (double)in[i] * (double)in[i];
        float y1 = 0.0f;
        if (!fe_stage1(fe, in[i], &y1)) {
            continue;
        }
        const float full_now = (float)(fe->full_acc / span);
        fe->full_acc = 0.0;
        const float* run = fe_push(fe->hist2, fe->n2, &fe->pos2, y1);
        const double y2 = (double)fe_dot(fe->taps2, run, fe->n2);
        const double y = y2 - fe->dc_x1 + (fe->dc_alpha * fe->dc_y1);
        fe->dc_x1 = y2;
        fe->dc_y1 = y;
        const float delayed_wide = fe_delay(fe->wide_delay, fe->wide_delay_len, &fe->wide_delay_pos, y1);
        const float delayed_full = fe_delay(fe->full_delay, fe->full_delay_len, &fe->full_delay_pos, full_now);
        if (wide) {
            wide[produced] = delayed_wide;
        }
        if (full) {
            full[produced] = delayed_full;
        }
        band[produced++] = (float)y;
    }
    return produced;
}

/* ------------------------------------------------------------------------------------------
 * Core: front end + detectors + carrier hangover
 * ---------------------------------------------------------------------------------------- */

/* The detectors the core runs, in the order their reports are merged. A detector is its ops
   and the core member holding its working state; the DCS detector (#523) adds a row here. */
typedef struct {
    const dsd_analog_rx_detector_ops* ops;
    size_t ctx_offset;
} analog_rx_detector_slot;

static const analog_rx_detector_slot k_detectors[] = {
    {&dsd_analog_ctcss_ops, offsetof(dsd_analog_rx_core, ctcss)},
};

enum { ANALOG_RX_DETECTOR_COUNT = (int)(sizeof(k_detectors) / sizeof(k_detectors[0])) };

static void*
core_detector(dsd_analog_rx_core* core, int i) {
    return (char*)core + k_detectors[i].ctx_offset;
}

static const void*
core_detector_const(const dsd_analog_rx_core* core, int i) {
    return (const char*)core + k_detectors[i].ctx_offset;
}

void
dsd_analog_rx_core_init(dsd_analog_rx_core* core) {
    if (!core) {
        return;
    }
    DSD_MEMSET(core, 0, sizeof(*core));
}

void
dsd_analog_rx_core_reset(dsd_analog_rx_core* core) {
    if (!core) {
        return;
    }
    dsd_analog_subaudible_fe_clear(&core->fe);
    for (int i = 0; i < ANALOG_RX_DETECTOR_COUNT; i++) {
        k_detectors[i].ops->reset(core_detector(core, i));
    }
    core->carrier_open = 0;
    core->closed_samples = 0;
    core->resets++;
}

static void
core_configure(dsd_analog_rx_core* core, int rate_hz) {
    if (dsd_analog_subaudible_fe_configure(&core->fe, rate_hz)) {
        for (int i = 0; i < ANALOG_RX_DETECTOR_COUNT; i++) {
            k_detectors[i].ops->configure(core_detector(core, i), core->fe.out_rate_hz);
        }
    }
    core->carrier_open = 0;
    core->closed_samples = 0;
    core->resets++;
}

static double
core_mean_square(const float* block, int count) {
    double acc = 0.0;
    for (int i = 0; i < count; i++) {
        acc += (double)block[i] * (double)block[i];
    }
    return acc / (double)count;
}

static void
core_feed(dsd_analog_rx_core* core, const float* block, int count, int freeze) {
    /* Size each slice so its decimated output always fits the scratch buffer. */
    const int slice = (DSD_ANALOG_RX_SCRATCH - 1) * core->fe.decim;
    for (int start = 0; start < count; start += slice) {
        const int n = (count - start < slice) ? count - start : slice;
        const int produced = dsd_analog_subaudible_fe_process(
            &core->fe, block + start, n, core->scratch, core->scratch_wide, core->scratch_full, DSD_ANALOG_RX_SCRATCH);
        for (int i = 0; i < ANALOG_RX_DETECTOR_COUNT; i++) {
            k_detectors[i].ops->process(core_detector(core, i), core->scratch, core->scratch_wide, core->scratch_full,
                                        produced, freeze);
        }
    }
}

/* Carrier bookkeeping for one block; returns 1 when the block should reach the detectors. */
static int
core_update_carrier(dsd_analog_rx_core* core, int carrier_now, int count) {
    if (carrier_now) {
        core->closed_samples = 0;
        core->carrier_open = 1;
        return 1;
    }
    if (!core->carrier_open) {
        return 0;
    }
    core->closed_samples += count;
    const int64_t hangover = ((int64_t)core->fe.in_rate_hz * DSD_ANALOG_CARRIER_HANGOVER_MS) / 1000;
    if (core->closed_samples >= hangover) {
        dsd_analog_rx_core_reset(core);
        return 0;
    }
    return 1;
}

int
dsd_analog_rx_core_process(dsd_analog_rx_core* core, const float* block, int count, int rate_hz, int squelch_open) {
    if (!core || !block || count <= 0) {
        return 0;
    }
    if (rate_hz != core->fe.in_rate_hz) {
        core_configure(core, rate_hz);
    }
    if (!core->fe.active) {
        return 0;
    }
    const int carrier_now = squelch_open && core_mean_square(block, count) > DSD_ANALOG_RX_FLOOR_MEAN_SQUARE;
    if (core_update_carrier(core, carrier_now, count)) {
        /* Inside the hangover the detectors keep time but may not change their verdict on
           these samples alone. */
        core_feed(core, block, count, !carrier_now);
    }
    return 1;
}

/* One verdict from every detector's: the first detector that locked names the tone; otherwise
   the carrier is still being evaluated while any detector is, and carries no tone once every
   detector has said so. */
static void
core_merge_reports(const dsd_analog_rx_core* core, dsd_analog_rx_report* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->state = DSD_ANALOG_TONE_STATE_NONE;
    out->kind = DSD_ANALOG_TONE_KIND_NONE;
    for (int i = 0; i < ANALOG_RX_DETECTOR_COUNT; i++) {
        dsd_analog_rx_report report;
        k_detectors[i].ops->report(core_detector_const(core, i), &report);
        if (report.state == DSD_ANALOG_TONE_STATE_LOCKED) {
            *out = report;
            return;
        }
        if (report.state == DSD_ANALOG_TONE_STATE_ACQUIRING) {
            out->state = DSD_ANALOG_TONE_STATE_ACQUIRING;
        }
    }
}

void
dsd_analog_rx_core_publish(const dsd_analog_rx_core* core, dsd_analog_rx_publication* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->tone_state = DSD_ANALOG_TONE_STATE_INACTIVE;
    out->gate = DSD_ANALOG_TONE_GATE_OFF;
    if (!core) {
        return;
    }
    out->generation = core->resets;
    if (!core->fe.active) {
        /* Designed for a rate the front end cannot use: detection is on but hears nothing.
           An unconfigured core (no block yet) is still INACTIVE. */
        if (core->fe.in_rate_hz != 0) {
            out->tone_state = DSD_ANALOG_TONE_STATE_UNAVAILABLE;
        }
        return;
    }
    out->carrier_open = core->carrier_open;
    if (!core->carrier_open) {
        out->tone_state = DSD_ANALOG_TONE_STATE_IDLE;
        return;
    }
    dsd_analog_rx_report report;
    core_merge_reports(core, &report);
    out->tone_state = report.state;
    out->tone_kind = report.kind;
    out->ctcss_tenths_hz = report.ctcss_tenths_hz;
    out->dcs_code = report.dcs_code;
    out->dcs_inverted = report.dcs_inverted;
}

/* ------------------------------------------------------------------------------------------
 * Decoder-thread glue
 * ---------------------------------------------------------------------------------------- */

/* Log key for "Received tone:" lines: nothing logged yet this reception, "none", or a tone. */
enum { ANALOG_RX_LOG_UNSET = -1, ANALOG_RX_LOG_NONE = 0 };

typedef struct {
    dsd_analog_rx_core core;
    uint32_t rtl_generation;
    uint64_t tune_generation;
    int log_key;              /**< ANALOG_RX_LOG_* or the logged tone in tenths of a hertz */
    uint32_t log_generation;  /**< the publication generation log_key belongs to */
    int unusable_rate_logged; /**< the unusable rate reported for the current stretch of such input; 0 = none */
    /** Input that may pause: the monotonic ms past which the next read arrives after a pause
        (published as dsd_analog_rx_publication::stale_after_ms); 0 = no read to measure from. */
    uint64_t stale_after_ms;
    /** Samples of the monitor block the symbol path is assembling that the tap has read, or
        set aside at a reset or when detection started. */
    unsigned int block_taken;
    /** The input rate at the tap's last read, which the samples it has not read yet arrived
        at; 0 once a reset has set the rest of the monitor block aside, when nothing unread is
        left. */
    int read_rate_hz;
    /** 1 from a boundary until a read shows the input ran dry (analog_rx_backlog_skipped()). */
    int backlog_armed;
    int backlog_span_open;           /**< 1 once a read since the boundary started a measured span */
    uint64_t backlog_span_start_ms;  /**< monotonic ms at which the span being measured started */
    uint64_t backlog_span_us;        /**< input read in that span, in sample time */
    uint64_t backlog_span_played_ms; /**< of that span, the time the decoder spent playing monitor audio */
    uint64_t backlog_skipped_us;     /**< input skipped since the boundary, in sample time */
    int playback_open;               /**< 1 between dsd_analog_rx_playback_begin() and _end() */
    uint64_t playback_started_ms;    /**< monotonic ms at dsd_analog_rx_playback_begin() */
} analog_rx_session;

#ifdef DSD_NEO_TEST_HOOKS
static uint64_t (*g_analog_rx_test_clock)(void) = NULL;

/* Test-hook entry point (declared in analog_rx_internal.h): replace the clock the stream-pause
   check reads; NULL restores the monotonic clock. */
void
dsd_analog_rx_test_set_clock(uint64_t (*now_ms)(void)) {
    g_analog_rx_test_clock = now_ms;
}
#endif

static uint64_t
analog_rx_now_ms(void) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_analog_rx_test_clock) {
        return g_analog_rx_test_clock();
    }
#endif
    return dsd_time_monotonic_ms();
}

static analog_rx_session*
analog_rx_session_get(const dsd_state* state) {
    return DSD_STATE_EXT_GET_AS(analog_rx_session, state, DSD_STATE_EXT_DSP_ANALOG_RX);
}

static void
analog_rx_note_generations(const dsd_opts* opts, analog_rx_session* session) {
    session->rtl_generation =
        (opts && opts->audio_in_type == AUDIO_IN_RTL) ? dsd_rtl_stream_metrics_hook_stream_generation() : 0U;
    session->tune_generation = dsd_trunk_tuning_generation();
}

static int
analog_rx_rate_hz(const dsd_opts* opts) {
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        const unsigned int rtl_rate = dsd_rtl_stream_metrics_hook_output_rate_hz();
        if (rtl_rate > 0U) {
            return (int)rtl_rate;
        }
    }
    const int rate = dsd_opts_current_input_timing_rate(opts);
    return rate > 0 ? rate : 48000;
}

/* DSD_ANALOG_RX_TAP_READ_MS of input at @p rate_hz, rounded up as the symbol path sizes an RTL
   block; at least one sample. */
static unsigned int
analog_rx_read_samples(int rate_hz) {
    if (rate_hz <= 0) {
        return 1U;
    }
    const uint64_t samples = (((uint64_t)rate_hz * DSD_ANALOG_RX_TAP_READ_MS) + 999U) / 1000U;
    return samples > 0U ? (unsigned int)samples : 1U;
}

static analog_rx_session*
analog_rx_session_create(const dsd_opts* opts, dsd_state* state) {
    analog_rx_session* session = (analog_rx_session*)calloc(1, sizeof(*session));
    if (!session) {
        return NULL;
    }
    dsd_analog_rx_core_init(&session->core);
    /* Continue the published generation rather than restarting it, so a reader comparing
       generations never sees one repeat. */
    session->core.resets = state->analog_rx.generation;
    session->log_key = ANALOG_RX_LOG_UNSET;
    session->log_generation = session->core.resets;
    session->read_rate_hz = analog_rx_rate_hz(opts);
    analog_rx_note_generations(opts, session);
    if (dsd_state_ext_set(state, DSD_STATE_EXT_DSP_ANALOG_RX, session, free) != 0) {
        free(session);
        return NULL;
    }
    return session;
}

static void
analog_rx_log_change(analog_rx_session* session, const dsd_analog_rx_publication* pub) {
    if (pub->generation != session->log_generation) {
        /* Every reset moves the generation on -- a retune, a stream pause, the carrier
           hangover, a new input rate -- and starts a new reception, which reports its tone
           afresh, even the same tone. */
        session->log_generation = pub->generation;
        session->log_key = ANALOG_RX_LOG_UNSET;
    }
    int key = session->log_key;
    if (pub->tone_state == DSD_ANALOG_TONE_STATE_LOCKED && pub->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS) {
        key = pub->ctcss_tenths_hz;
    } else if (pub->tone_state == DSD_ANALOG_TONE_STATE_NONE) {
        key = ANALOG_RX_LOG_NONE;
    } else if (pub->tone_state != DSD_ANALOG_TONE_STATE_ACQUIRING) {
        /* No verdict to report: no carrier, or no detection at this input rate. */
        return;
    }
    if (key == session->log_key) {
        return;
    }
    session->log_key = key;
    if (key == ANALOG_RX_LOG_NONE) {
        LOG_INFO("Received tone: none\n");
        return;
    }
    char label[DSD_CTCSS_LABEL_SIZE];
    if (dsd_ctcss_format_label(key, label, sizeof(label)) > 0) {
        LOG_INFO("Received tone: %s\n", label);
    }
}

static void
analog_rx_publish(dsd_state* state, analog_rx_session* session) {
    dsd_analog_rx_core_publish(&session->core, &state->analog_rx);
    state->analog_rx.stale_after_ms = session->stale_after_ms;
    analog_rx_log_change(session, &state->analog_rx);
}

/*
 * Inputs whose samples can stop arriving while the decoder waits for them. Files and Pulse
 * deliver continuously, and a closed squelch arrives as zeroed blocks, which the sample-time
 * hangover counts. But stdin, UDP and TCP producers may squelch by sending nothing (rtl_fm
 * without `-E pad`, a sender that stops), and a live radio stream stops when its source does
 * (an rtl_tcp server that went away, whose client then retries without end; a stalled device):
 * then no block arrives to count. IQ replay is a file, so a replay reads the same however
 * slowly it is read.
 */
static int
analog_rx_input_may_pause(const dsd_opts* opts) {
    switch (opts->audio_in_type) {
        case AUDIO_IN_STDIN:
        case AUDIO_IN_UDP:
        case AUDIO_IN_TCP: return 1;
        case AUDIO_IN_RTL: return opts->iq_replay_active == 0U;
        default: return 0;
    }
}

/*
 * On an input that may pause, each read sets a deadline: its arrival plus its own duration and
 * the hangover, and at least DSD_ANALOG_STREAM_PAUSE_MIN_MS. A read arriving past the previous
 * deadline follows a pause as long as a dropped carrier, and returns 1. The frontends read the
 * same deadline, so the row stops claiming a carrier while the input is quiet.
 */
static int
analog_rx_input_paused(const dsd_opts* opts, analog_rx_session* session, unsigned int count, int rate_hz) {
    if (!analog_rx_input_may_pause(opts) || rate_hz <= 0) {
        session->stale_after_ms = 0;
        return 0;
    }
    const uint64_t now = analog_rx_now_ms();
    const int paused = session->stale_after_ms != 0U && now > session->stale_after_ms;
    uint64_t allowed = (((uint64_t)count * 1000U) / (uint64_t)rate_hz) + (uint64_t)DSD_ANALOG_CARRIER_HANGOVER_MS;
    if (allowed < (uint64_t)DSD_ANALOG_STREAM_PAUSE_MIN_MS) {
        allowed = (uint64_t)DSD_ANALOG_STREAM_PAUSE_MIN_MS;
    }
    session->stale_after_ms = now + allowed;
    return paused;
}

/*
 * Live inputs that can queue audio before the decoder reads it: the UDP ring, the TCP socket,
 * the Pulse record buffer and the stdin pipe. What they hold at a boundary arrived before it --
 * a rigctl retune holds the decoder while the old channel keeps arriving -- and the part of the
 * monitor block a reset sets aside is only what the symbol path had already taken. An
 * RTL-family stream clears its own output at a retune and moves its generation, and a file
 * queues no other channel.
 */
static int
analog_rx_input_queues(const dsd_opts* opts) {
    switch (opts->audio_in_type) {
        case AUDIO_IN_PULSE:
        case AUDIO_IN_STDIN:
        case AUDIO_IN_UDP:
        case AUDIO_IN_TCP: return 1;
        default: return 0;
    }
}

/* Skip what the input had queued at the boundary just crossed. */
static void
analog_rx_arm_backlog_skip(analog_rx_session* session) {
    session->backlog_armed = 1;
    session->backlog_span_open = 0;
    session->backlog_span_start_ms = 0U;
    session->backlog_span_us = 0U;
    session->backlog_span_played_ms = 0U;
    session->backlog_skipped_us = 0U;
}

/* Start measuring a new span of the backlog skip at @p now_ms. */
static void
analog_rx_backlog_span_start(analog_rx_session* session, uint64_t now_ms) {
    session->backlog_span_open = 1;
    session->backlog_span_start_ms = now_ms;
    session->backlog_span_us = 0U;
    session->backlog_span_played_ms = 0U;
}

/*
 * After a boundary, on an input that queues, every read is skipped until one shows the input ran
 * dry. The decoder drains a backlog far faster than real time, so the test is a span of at least
 * DSD_ANALOG_RX_TAP_READ_MS of input that took at least half as long to arrive: the decoder had
 * to wait for it. Time the decoder spent playing monitor audio does not count: synchronous
 * playback holds it for each block's playing time once the output buffer is full, and it then
 * reads a backlog at real-time pace without ever waiting for the input. The first read after the
 * boundary only starts the clock, since it may have waited in the input for any length of time
 * (after a generation move that is the read the move is seen on, dropped already), and the read
 * that ends the waiting span is skipped too, since it can still open with the backlog's last
 * samples; the next read holds only audio that arrived after the boundary. An input that never
 * runs dry (stdin fed from a file) is heard again after DSD_ANALOG_RX_BACKLOG_MAX_MS of it.
 * Returns 1 when this read is skipped.
 */
static int
analog_rx_backlog_skipped(const dsd_opts* opts, analog_rx_session* session, unsigned int count, int rate_hz) {
    if (!session->backlog_armed) {
        return 0;
    }
    if (!analog_rx_input_queues(opts) || rate_hz <= 0) {
        session->backlog_armed = 0;
        return 0;
    }
    const uint64_t now = analog_rx_now_ms();
    const uint64_t read_us = ((uint64_t)count * 1000000U) / (uint64_t)rate_hz;
    session->backlog_skipped_us += read_us;
    if (!session->backlog_span_open) {
        analog_rx_backlog_span_start(session, now);
    } else {
        session->backlog_span_us += read_us;
        if (session->backlog_span_us >= (uint64_t)DSD_ANALOG_RX_TAP_READ_MS * 1000U) {
            const uint64_t elapsed_ms =
                now > session->backlog_span_start_ms ? now - session->backlog_span_start_ms : 0U;
            const uint64_t waited_ms =
                elapsed_ms > session->backlog_span_played_ms ? elapsed_ms - session->backlog_span_played_ms : 0U;
            if (2U * waited_ms * 1000U >= session->backlog_span_us) {
                session->backlog_armed = 0;
            }
            analog_rx_backlog_span_start(session, now);
        }
    }
    if (session->backlog_skipped_us >= (uint64_t)DSD_ANALOG_RX_BACKLOG_MAX_MS * 1000U) {
        session->backlog_armed = 0;
    }
    return 1;
}

/*
 * The session for detection that starts now. With no session there was nothing for a reset to
 * arm the backlog skip in, and one did happen when the publication's generation, which every
 * reset moves, is no longer 0: a retune, say, then the switch from a digital mode to the analog
 * monitor. What the input holds now may then be the old channel's, so the first reads skip it
 * as after a reset with detection running. With no reset at all (the engine started on the
 * analog monitor) nothing is skipped, so stdin fed from a file is heard from its start.
 */
static analog_rx_session*
analog_rx_session_start(const dsd_opts* opts, dsd_state* state) {
    const int after_reset = state->analog_rx.generation != 0U;
    analog_rx_session* session = analog_rx_session_create(opts, state);
    if (session && after_reset) {
        analog_rx_arm_backlog_skip(session);
    }
    return session;
}

/* Publish a read the backlog skip passed over. The core is still designed for the read's rate,
   so the publication says at once whether detection can use it: IDLE meanwhile, or UNAVAILABLE. */
static void
analog_rx_publish_skipped(dsd_state* state, analog_rx_session* session, int rate_hz) {
    if (session->core.fe.in_rate_hz != rate_hz) {
        core_configure(&session->core, rate_hz);
    }
    analog_rx_publish(state, session);
}

/* A retune nobody told the tap about shows up as a generation change: the RTL stream's for
   a direct or UDP-driven retune, the trunk-tuning one for a hook-driven or rigctl retune. */
static int
analog_rx_generation_moved(const dsd_opts* opts, analog_rx_session* session) {
    const uint32_t rtl = session->rtl_generation;
    const uint64_t tune = session->tune_generation;
    analog_rx_note_generations(opts, session);
    return rtl != session->rtl_generation || tune != session->tune_generation;
}

static void
analog_rx_log_unusable_rate(analog_rx_session* session, int rate_hz) {
    if (session->unusable_rate_logged == rate_hz) {
        return;
    }
    session->unusable_rate_logged = rate_hz;
    if (rate_hz < DSD_ANALOG_RX_MIN_RATE_HZ) {
        LOG_WARN("Received tone detection inactive: %d Hz input is below %d Hz\n", rate_hz, DSD_ANALOG_RX_MIN_RATE_HZ);
    } else if (rate_hz > DSD_ANALOG_RX_MAX_RATE_HZ) {
        LOG_WARN("Received tone detection inactive: %d Hz input is above the %d Hz the front end supports\n", rate_hz,
                 DSD_ANALOG_RX_MAX_RATE_HZ);
    } else {
        LOG_WARN("Received tone detection inactive: no front-end filter design for a %d Hz input\n", rate_hz);
    }
}

/*
 * Whether the squelch is open over these samples. RTL input compares the receiver power, which
 * the stream keeps current. PCM input has no receiver power: dsd_input_level_publish() sets
 * opts->rtl_pwr from the level of each whole monitor block once the block is complete, which is
 * after the tap has read most of it. So on PCM each read is judged on its own level, the same
 * measurement and the same dB-to-power step; for a read that is the whole block, exactly the
 * value opts->rtl_pwr is about to hold.
 */
static int
analog_rx_squelch_open(const dsd_opts* opts, const float* samples, unsigned int count) {
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return opts->rtl_pwr > opts->rtl_squelch_level;
    }
    dsd_input_level_snapshot level;
    if (dsd_input_level_metrics_from_pcm_f32_i16_scale(samples, count, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, &level) != 0) {
        return opts->rtl_pwr > opts->rtl_squelch_level;
    }
    /* rms_dbfs never exceeds 0 dBFS, full scale, which is a mean power of 1. */
    const double power = (level.rms_dbfs < 0.0) ? dsd_squelch_level_from_sql(level.rms_dbfs) : 1.0;
    return power > opts->rtl_squelch_level;
}

/* Forget the received tone: every detector's state and the publication. */
static void
analog_rx_forget(dsd_state* state) {
    analog_rx_session* session = analog_rx_session_get(state);
    if (!session) {
        const uint32_t generation = state->analog_rx.generation + 1U;
        DSD_MEMSET(&state->analog_rx, 0, sizeof(state->analog_rx));
        state->analog_rx.generation = generation;
        return;
    }
    dsd_analog_rx_core_reset(&session->core);
    session->stale_after_ms = 0;
    dsd_analog_rx_core_publish(&session->core, &state->analog_rx);
    /* Nothing has been processed since the reset, whatever the front end's design says; at an
       unusable rate the next read publishes UNAVAILABLE again. */
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_INACTIVE;
}

void
dsd_analog_rx_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    analog_rx_session* session = analog_rx_session_get(state);
    if (session) {
        /* The symbol path assembles the monitor block this tap reads sample by sample
           (dsd_state::analog_out_f, dsd_symbol.c), and what it holds now arrived before the
           boundary: at a low input rate, a block's worth of the old channel is enough to lock
           its tone again. The tap sets those samples aside rather than reading them; they stay
           in the block for the raw WAV and the monitor output, whose audio a reset leaves alone.
           With no session, detection has not run since the engine started: the first read that
           creates one starts at the newest sample (dsd_analog_rx_tap_partial()) and, after this
           reset, arms the backlog skip (analog_rx_session_start()). */
        const int pending = state->analog_sample_counter;
        session->block_taken = pending > 0 ? (unsigned int)pending : 0U;
        session->read_rate_hz = 0;
        /* Nor may what a live input still holds, which arrived before the boundary too. */
        analog_rx_arm_backlog_skip(session);
    }
    analog_rx_forget(state);
}

/* Whether the input rate moved since the tap's last read, so that the samples in hand may
   have arrived at either rate; notes @p rate_hz as the rate of this read. */
static int
analog_rx_rate_moved(analog_rx_session* session, int rate_hz) {
    const int previous = session->read_rate_hz;
    session->read_rate_hz = rate_hz;
    return previous != 0 && previous != rate_hz;
}

/* The core's own resets act on the samples in hand: a retune nobody announced, an input that
   paused, or an input rate that moved. Each way these samples may straddle the boundary -- the
   first of them can have arrived before it, and after a rate change they are another signal
   at the new rate (1920 Hz at 48 kHz read as 2500 Hz input is a 100 Hz tone) -- so they are
   dropped, and the new reception starts with the next read. Returns 1 when that happened. */
static int
analog_rx_boundary_dropped(const dsd_opts* opts, dsd_state* state, analog_rx_session* session, unsigned int count,
                           int rate_hz) {
    const int moved = analog_rx_generation_moved(opts, session);
    const int paused = analog_rx_input_paused(opts, session, count, rate_hz);
    const int rate_moved = analog_rx_rate_moved(session, rate_hz);
    if (!moved && !paused && !rate_moved) {
        return 0;
    }
    if (rate_moved) {
        /* Designed for the new rate at once, so the publication says straight away whether
           detection can use it. */
        core_configure(&session->core, rate_hz);
    } else {
        dsd_analog_rx_core_reset(&session->core);
    }
    if (moved) {
        /* A retune starts the stream afresh: no deadline until it delivers again, and what the
           input had queued is the old channel's. This read, dropped here, starts the clock the
           skip measures the next ones against. */
        session->stale_after_ms = 0;
        analog_rx_arm_backlog_skip(session);
        analog_rx_backlog_span_start(session, analog_rx_now_ms());
    }
    analog_rx_publish(state, session);
    return 1;
}

/* Hand the detectors @p count consecutive raw samples of the monitor block. */
static void
analog_rx_read(const dsd_opts* opts, dsd_state* state, const float* samples, unsigned int count) {
    /* The same question every frontend's row asks (runtime/analog_tones.h), so the row is on
       screen exactly while this tap listens. */
    if (!dsd_analog_tone_detection_active(opts)) {
        /* Forget only: these samples go on to the voice filters and the monitor output as
           they are. */
        if (state->analog_rx.tone_state != DSD_ANALOG_TONE_STATE_INACTIVE || state->analog_rx.carrier_open) {
            analog_rx_forget(state);
        }
        return;
    }
    analog_rx_session* session = analog_rx_session_get(state);
    const int rate_hz = analog_rx_rate_hz(opts);
    if (!session) {
        session = analog_rx_session_start(opts, state);
        if (!session) {
            return;
        }
    } else if (analog_rx_boundary_dropped(opts, state, session, count, rate_hz)) {
        return;
    }
    if (analog_rx_backlog_skipped(opts, session, count, rate_hz)) {
        analog_rx_publish_skipped(state, session, rate_hz);
        return;
    }
    if (!dsd_analog_rx_core_process(&session->core, samples, (int)count, rate_hz,
                                    analog_rx_squelch_open(opts, samples, count))) {
        analog_rx_log_unusable_rate(session, rate_hz);
    } else {
        /* A usable block ends the stretch: going back to an unusable rate says so again. A
           reset does not, so a scan step or retune at an unchanged unusable rate stays quiet. */
        session->unusable_rate_logged = 0;
    }
    analog_rx_publish(state, session);
}

/* Where the tap's next read starts in a block now holding @p filled samples: after what it has
   read of this block, or at its start when the symbol path began a new block unannounced. */
static unsigned int
analog_rx_block_start(const analog_rx_session* session, unsigned int filled) {
    if (!session || session->block_taken > filled) {
        return 0U;
    }
    return session->block_taken;
}

void
dsd_analog_rx_tap_partial(const dsd_opts* opts, dsd_state* state, const float* block, unsigned int filled) {
    if (!opts || !state || !block || filled == 0U) {
        return;
    }
    analog_rx_session* session = analog_rx_session_get(state);
    if (!session) {
        /* Nothing to catch up on until detection runs; the first read creates the session. */
        if (!dsd_analog_tone_detection_active(opts)) {
            return;
        }
        session = analog_rx_session_start(opts, state);
        if (!session) {
            return;
        }
        /* Detection starts listening with the sample just added. The ones before it in the
           block arrived while it was not, and perhaps before a boundary that had no session to
           set them aside (dsd_analog_rx_reset()). */
        session->block_taken = filled - 1U;
    }
    /* The quota follows the input rate as it is now, not as it was at the last read: after a
       drop in rate, the old rate's quota would hold the first read at the new one back for up
       to a whole block (384 ms from 48 kHz to 2500 Hz), with the old reception still shown. */
    const unsigned int start = analog_rx_block_start(session, filled);
    if (filled - start < analog_rx_read_samples(analog_rx_rate_hz(opts))) {
        return;
    }
    analog_rx_read(opts, state, block + start, filled - start);
    session->block_taken = filled;
}

void
dsd_analog_rx_tap(const dsd_opts* opts, dsd_state* state, const float* block, unsigned int count) {
    if (!opts || !state || !block || count == 0U) {
        return;
    }
    analog_rx_session* session = analog_rx_session_get(state);
    const unsigned int start = analog_rx_block_start(session, count);
    if (session) {
        /* The symbol path starts a new block after this one. */
        session->block_taken = 0U;
    }
    if (start < count) {
        analog_rx_read(opts, state, block + start, count - start);
    }
}

void
dsd_analog_rx_block_restart(const dsd_state* state) {
    analog_rx_session* session = state ? analog_rx_session_get(state) : NULL;
    if (session) {
        /* Whatever the tap had read, or set aside, of the emptied block is gone with it. */
        session->block_taken = 0U;
    }
}

void
dsd_analog_rx_playback_begin(const dsd_state* state) {
    analog_rx_session* session = state ? analog_rx_session_get(state) : NULL;
    if (!session || !session->backlog_armed) {
        return;
    }
    session->playback_open = 1;
    session->playback_started_ms = analog_rx_now_ms();
}

void
dsd_analog_rx_playback_end(const dsd_state* state) {
    analog_rx_session* session = state ? analog_rx_session_get(state) : NULL;
    if (!session || !session->playback_open) {
        return;
    }
    session->playback_open = 0;
    const uint64_t now = analog_rx_now_ms();
    if (session->backlog_armed && session->backlog_span_open && now > session->playback_started_ms) {
        session->backlog_span_played_ms += now - session->playback_started_ms;
    }
}
