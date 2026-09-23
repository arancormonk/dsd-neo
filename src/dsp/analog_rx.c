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

#include <dsd-neo/core/opts.h>
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
    fe->pos1 = 0;
    fe->pos2 = 0;
    fe->wide_delay_pos = 0;
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

/* The stage-1 sample from wide_delay_len outputs ago, which lines up with stage 2's output. */
static float
fe_delay_wide(dsd_analog_subaudible_fe* fe, float y1) {
    if (fe->wide_delay_len <= 0) {
        return y1;
    }
    const float delayed = fe->wide_delay[fe->wide_delay_pos];
    fe->wide_delay[fe->wide_delay_pos] = y1;
    fe->wide_delay_pos = (fe->wide_delay_pos + 1) % fe->wide_delay_len;
    return delayed;
}

int
dsd_analog_subaudible_fe_process(dsd_analog_subaudible_fe* fe, const float* in, int count, float* band, float* wide,
                                 int out_cap) {
    if (!fe || !fe->active || !in || !band || count <= 0) {
        return 0;
    }
    int produced = 0;
    for (int i = 0; i < count && produced < out_cap; i++) {
        float y1 = 0.0f;
        if (!fe_stage1(fe, in[i], &y1)) {
            continue;
        }
        const float* run = fe_push(fe->hist2, fe->n2, &fe->pos2, y1);
        const double y2 = (double)fe_dot(fe->taps2, run, fe->n2);
        const double y = y2 - fe->dc_x1 + (fe->dc_alpha * fe->dc_y1);
        fe->dc_x1 = y2;
        fe->dc_y1 = y;
        const float delayed = fe_delay_wide(fe, y1);
        if (wide) {
            wide[produced] = delayed;
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
        const int produced = dsd_analog_subaudible_fe_process(&core->fe, block + start, n, core->scratch,
                                                              core->scratch_wide, DSD_ANALOG_RX_SCRATCH);
        for (int i = 0; i < ANALOG_RX_DETECTOR_COUNT; i++) {
            k_detectors[i].ops->process(core_detector(core, i), core->scratch, core->scratch_wide, produced, freeze);
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
        /* Inside the hangover the detectors keep time but may not change their verdict. */
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
    int unusable_rate_logged; /**< the unusable rate already reported, so it is said once */
    /** Live stream input: the monotonic ms past which the next block arrives after a pause
        (published as dsd_analog_rx_publication::stale_after_ms); 0 = no block to measure from. */
    uint64_t stale_after_ms;
} analog_rx_session;

#ifdef DSD_NEO_TEST_HOOKS
void dsd_analog_rx_test_set_clock(uint64_t (*now_ms)(void));

static uint64_t (*g_analog_rx_test_clock)(void) = NULL;

/* Replace the clock the stream-pause check reads; NULL restores the monotonic clock. */
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
    analog_rx_note_generations(opts, session);
    if (dsd_state_ext_set(state, DSD_STATE_EXT_DSP_ANALOG_RX, session, free) != 0) {
        free(session);
        return NULL;
    }
    return session;
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

static void
analog_rx_log_change(analog_rx_session* session, const dsd_analog_rx_publication* pub) {
    int key = session->log_key;
    if (pub->tone_state == DSD_ANALOG_TONE_STATE_LOCKED && pub->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS) {
        key = pub->ctcss_tenths_hz;
    } else if (pub->tone_state == DSD_ANALOG_TONE_STATE_NONE) {
        key = ANALOG_RX_LOG_NONE;
    } else if (pub->tone_state != DSD_ANALOG_TONE_STATE_ACQUIRING) {
        /* No carrier: the next reception reports its tone afresh. */
        session->log_key = ANALOG_RX_LOG_UNSET;
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
 * A live stream input whose producer went quiet. Files, Pulse and RTL deliver samples
 * continuously -- a closed squelch arrives as zeroed blocks, which the sample-time hangover
 * counts -- but stdin, UDP and TCP producers may squelch by sending nothing (rtl_fm without
 * `-E pad`, a sender that stops), and then no block arrives to count. So on those inputs each
 * block sets a deadline: its arrival plus its own duration and the hangover, and at least
 * DSD_ANALOG_STREAM_PAUSE_MIN_MS. A block arriving past the previous deadline follows a pause
 * as long as a dropped carrier, and returns 1. The frontends read the same deadline, so the
 * row stops claiming a carrier while the stream is quiet.
 */
static int
analog_rx_stream_paused(const dsd_opts* opts, analog_rx_session* session, unsigned int count, int rate_hz) {
    const int live_stream = opts->audio_in_type == AUDIO_IN_STDIN || opts->audio_in_type == AUDIO_IN_UDP
                            || opts->audio_in_type == AUDIO_IN_TCP;
    if (!live_stream || rate_hz <= 0) {
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

void
dsd_analog_rx_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    analog_rx_session* session = analog_rx_session_get(state);
    if (!session) {
        const uint32_t generation = state->analog_rx.generation + 1U;
        DSD_MEMSET(&state->analog_rx, 0, sizeof(state->analog_rx));
        state->analog_rx.generation = generation;
        return;
    }
    dsd_analog_rx_core_reset(&session->core);
    session->log_key = ANALOG_RX_LOG_UNSET;
    session->stale_after_ms = 0;
    dsd_analog_rx_core_publish(&session->core, &state->analog_rx);
    /* Nothing has been processed since the reset, whatever the front end's design says; at an
       unusable rate the next block publishes UNAVAILABLE again. */
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_INACTIVE;
}

void
dsd_analog_rx_tap(const dsd_opts* opts, dsd_state* state, const float* block, unsigned int count) {
    if (!opts || !state || !block || count == 0U) {
        return;
    }
    /* The same question every frontend's row asks (runtime/analog_tones.h), so the row is on
       screen exactly while this tap listens. */
    if (!dsd_analog_tone_detection_active(opts)) {
        if (state->analog_rx.tone_state != DSD_ANALOG_TONE_STATE_INACTIVE || state->analog_rx.carrier_open) {
            dsd_analog_rx_reset(state);
        }
        return;
    }
    analog_rx_session* session = analog_rx_session_get(state);
    if (!session) {
        session = analog_rx_session_create(opts, state);
        if (!session) {
            return;
        }
    } else if (analog_rx_generation_moved(opts, session)) {
        dsd_analog_rx_core_reset(&session->core);
        session->stale_after_ms = 0;
        analog_rx_publish(state, session);
        return;
    }
    const int rate_hz = analog_rx_rate_hz(opts);
    if (analog_rx_stream_paused(opts, session, count, rate_hz)) {
        /* The pause outlasted the hangover: whatever arrives now is a new reception, maybe on
           another channel, and inherits nothing -- the same reset the hangover makes. */
        dsd_analog_rx_core_reset(&session->core);
        session->log_key = ANALOG_RX_LOG_UNSET;
    }
    const int squelch_open = opts->rtl_pwr > opts->rtl_squelch_level;
    if (!dsd_analog_rx_core_process(&session->core, block, (int)count, rate_hz, squelch_open)) {
        analog_rx_log_unusable_rate(session, rate_hz);
    }
    analog_rx_publish(state, session);
}
