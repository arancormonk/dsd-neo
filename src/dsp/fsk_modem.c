// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/fsk_modem.h>
#include <dsd-neo/dsp/symbol_levels.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

#define DSD_FSK_TRACK_SYMBOLS        64
#define DSD_FSK_TRACK_MIN_SCORE      1.0e-4f
#define DSD_FSK_TRACK_MIN_RATIO      1.10f
#define DSD_FSK_TRACK_GAIN           0.125f
#define DSD_FSK_TRACK_MAX_CORRECTION 0.25f
#define DSD_FSK_METRICS_WINDOW       256U
#define DSD_FSK_LOW_RELIABILITY      128U

static int
clamp_levels(int levels) {
    return (levels == 2) ? 2 : 4;
}

static int
valid_rate(int rate_hz, int fallback_hz) {
    return (rate_hz > 0) ? rate_hz : fallback_hz;
}

static float
modem_symbol_clock(const dsd_fsk_modem_config* cfg) {
    int sample_rate = valid_rate(cfg ? cfg->sample_rate_hz : 0, 48000);
    int symbol_rate = valid_rate(cfg ? cfg->symbol_rate_hz : 0, 4800);
    float clock = (float)sample_rate / (float)symbol_rate;
    if (clock < 1.0f) {
        clock = 1.0f;
    }
    if (clock > 256.0f) {
        clock = 256.0f;
    }
    return clock;
}

static int
modem_clock_int(float clock) {
    int ci = (int)(clock + 0.5f);
    if (ci < 1) {
        ci = 1;
    }
    if (fabsf(clock - (float)ci) > 0.01f) {
        return 0;
    }
    return ci;
}

static int
modem_should_acquire_timing(const dsd_fsk_modem_config* cfg, float clock) {
    int ci = modem_clock_int(clock);
    if (!cfg || ci < 4 || ci > 32) {
        return 0;
    }
    return (cfg->levels == 2 || cfg->levels == 4) ? 1 : 0;
}

static int
modem_acq_target_samples(int clock_i) {
    int target = clock_i * 96;
    if (target > DSD_FSK_MODEM_ACQ_MAX_SAMPLES) {
        target = DSD_FSK_MODEM_ACQ_MAX_SAMPLES - (DSD_FSK_MODEM_ACQ_MAX_SAMPLES % clock_i);
    }
    if (target < clock_i * 12) {
        target = clock_i * 12;
    }
    return target;
}

static int
debug_fsk_acq_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* dbg = getenv("DSD_NEO_DEBUG_SYNC");
        cached = (dbg && *dbg && strcmp(dbg, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static inline float
phase_delta_small_angle_or_atan2(float im, float re) {
    float abs_im = fabsf(im);
    if (re > 1.0e-7f && abs_im <= (0.35f * re)) {
        float x = im / re;
        float x2 = x * x;
        /* atan(x) Maclaurin through x^5. In the gated region the next term is
         * below discriminator noise for the RTL FSK symbol path; outside it we
         * keep atan2f's quadrant and large-angle behavior. */
        return x * (1.0f + x2 * (-0.3333333333333333f + x2 * 0.2f));
    }
    return atan2f(im, re);
}

static dsd_fsk_modem_config
normalized_config(const dsd_fsk_modem_config* cfg) {
    dsd_fsk_modem_config out;
    if (cfg) {
        out = *cfg;
    } else {
        DSD_MEMSET(&out, 0, sizeof(out));
    }
    out.sample_rate_hz = valid_rate(out.sample_rate_hz, 48000);
    out.symbol_rate_hz = valid_rate(out.symbol_rate_hz, 4800);
    out.levels = clamp_levels(out.levels);
    return out;
}

static void
release_pending_storage(dsd_fsk_modem_state* st) {
    if (st->pending_heap) {
        free(st->pending_heap);
        st->pending_heap = NULL;
    }
    st->pending_cap = 0;
}

void
dsd_fsk_modem_reset(dsd_fsk_modem_state* st) {
    if (!st) {
        return;
    }
    dsd_fsk_modem_config cfg = st->cfg;
    release_pending_storage(st);
    DSD_MEMSET(st, 0, sizeof(*st));
    st->cfg = normalized_config(&cfg);
    st->symbol_clock = modem_symbol_clock(&st->cfg);
    st->abs_est = 0.0f;
    st->timing_acquired = modem_should_acquire_timing(&st->cfg, st->symbol_clock) ? 0 : 1;
}

void
dsd_fsk_modem_configure(dsd_fsk_modem_state* st, const dsd_fsk_modem_config* cfg) {
    if (!st) {
        return;
    }
    dsd_fsk_modem_config next = normalized_config(cfg);
    int changed = (st->cfg.sample_rate_hz != next.sample_rate_hz || st->cfg.symbol_rate_hz != next.symbol_rate_hz
                   || st->cfg.levels != next.levels || st->cfg.channel_profile != next.channel_profile);
    st->cfg = next;
    st->symbol_clock = modem_symbol_clock(&st->cfg);
    if (changed) {
        dsd_fsk_modem_reset(st);
    }
}

void
dsd_fsk_modem_init(dsd_fsk_modem_state* st, const dsd_fsk_modem_config* cfg) {
    if (!st) {
        return;
    }
    DSD_MEMSET(st, 0, sizeof(*st));
    st->cfg = normalized_config(cfg);
    st->symbol_clock = modem_symbol_clock(&st->cfg);
    st->timing_acquired = modem_should_acquire_timing(&st->cfg, st->symbol_clock) ? 0 : 1;
}

void
dsd_fsk_modem_release(dsd_fsk_modem_state* st) {
    if (!st) {
        return;
    }
    release_pending_storage(st);
    st->pending_pos = 0;
    st->pending_len = 0;
}

static void
reset_soft_metrics(dsd_fsk_modem_state* st) {
    if (!st) {
        return;
    }
    DSD_MEMSET(&st->metrics, 0, sizeof(st->metrics));
    st->metrics_rel_sum = 0.0f;
    st->metrics_err2_sum = 0.0f;
    st->metrics_ref2_sum = 0.0f;
    st->metrics_low_count = 0;
    st->metrics_clip_count = 0;
    st->metrics_min_reliability = 0;
}

static void
refresh_soft_metrics_snapshot(dsd_fsk_modem_state* st) {
    if (!st || st->metrics.window_symbols == 0U) {
        if (st) {
            st->metrics.valid = 0;
        }
        return;
    }

    const unsigned int n = st->metrics.window_symbols;
    st->metrics.valid = 1;
    st->metrics.levels = st->cfg.levels;
    st->metrics.symbol_rate_hz = st->cfg.symbol_rate_hz;
    st->metrics.mean_reliability = (unsigned int)((st->metrics_rel_sum / (float)n) + 0.5f);
    if (st->metrics.mean_reliability > 255U) {
        st->metrics.mean_reliability = 255U;
    }
    st->metrics.min_reliability = st->metrics_min_reliability;
    st->metrics.rms_error = sqrtf(st->metrics_err2_sum / (float)n);
    if (st->metrics_err2_sum > 1.0e-12f && st->metrics_ref2_sum > 1.0e-12f) {
        st->metrics.evm_snr_db = 10.0f * log10f(st->metrics_ref2_sum / st->metrics_err2_sum);
    } else if (st->metrics_ref2_sum > 1.0e-12f) {
        st->metrics.evm_snr_db = 99.0f;
    } else {
        st->metrics.evm_snr_db = -100.0f;
    }
    st->metrics.low_reliability_pct = (100.0f * (float)st->metrics_low_count) / (float)n;
    st->metrics.clip_pct = (100.0f * (float)st->metrics_clip_count) / (float)n;
    st->metrics.timing_acquired = st->timing_acquired;
    st->metrics.track_last_error = st->track_last_error;
    st->metrics.track_last_score = st->track_last_score;
    st->metrics.track_updates = st->track_updates;
    st->metrics.track_skips = st->track_skips;
    st->metrics.abs_est = st->abs_est;
    st->metrics.dc_est = st->dc_est;
    st->metrics.last_symbol = st->last_symbol;
}

static void
update_soft_metrics(dsd_fsk_modem_state* st, float symbol) {
    if (!st) {
        return;
    }
    if (st->metrics.window_symbols >= DSD_FSK_METRICS_WINDOW) {
        st->metrics.window_symbols = 0;
        st->metrics_rel_sum = 0.0f;
        st->metrics_err2_sum = 0.0f;
        st->metrics_ref2_sum = 0.0f;
        st->metrics_low_count = 0;
        st->metrics_clip_count = 0;
    }
    if (st->metrics.window_symbols == 0U) {
        st->metrics_min_reliability = 255U;
    }

    dsd_fsk_soft_symbol_metrics sm = dsd_fsk_soft_symbol_metrics_from_symbol(symbol, st->cfg.levels);
    st->metrics.window_symbols++;
    st->metrics.symbols_total++;
    st->metrics_rel_sum += (float)sm.reliability;
    st->metrics_err2_sum += sm.error * sm.error;
    st->metrics_ref2_sum += sm.ideal * sm.ideal;
    if ((unsigned int)sm.reliability < st->metrics_min_reliability) {
        st->metrics_min_reliability = (unsigned int)sm.reliability;
    }
    if ((unsigned int)sm.reliability < DSD_FSK_LOW_RELIABILITY) {
        st->metrics_low_count++;
    }
    if (sm.clipped) {
        st->metrics_clip_count++;
    }
    refresh_soft_metrics_snapshot(st);
}

static float
clamp_symbol(float sym, int levels) {
    float limit = (levels == 2) ? 2.25f : 4.5f;
    if (sym > limit) {
        return limit;
    }
    if (sym < -limit) {
        return -limit;
    }
    return sym;
}

static float
normalize_symbol(dsd_fsk_modem_state* st, float raw_symbol) {
    float abs_raw = fabsf(raw_symbol);
    if (abs_raw > 1e-7f) {
        if (st->abs_est <= 1e-7f) {
            st->abs_est = abs_raw;
        } else {
            /* Track level slowly so short symbol runs do not pump the slicer. */
            st->abs_est += 0.0125f * (abs_raw - st->abs_est);
        }
    }

    float ref_abs = (st->cfg.levels == 2) ? 1.0f : 2.0f;
    float denom = (st->abs_est > 1e-7f) ? st->abs_est : (abs_raw > 1e-7f ? abs_raw : ref_abs);
    float sym = raw_symbol * (ref_abs / denom);
    return clamp_symbol(sym, st->cfg.levels);
}

static const float*
pending_symbol_data(const dsd_fsk_modem_state* st) {
    return st->pending_heap ? st->pending_heap : st->pending_symbols;
}

static int
pending_symbol_capacity(const dsd_fsk_modem_state* st) {
    return st->pending_heap ? st->pending_cap : DSD_FSK_MODEM_PENDING_INLINE_SYMBOLS;
}

static void
clear_pending_symbols(dsd_fsk_modem_state* st) {
    st->pending_pos = 0;
    st->pending_len = 0;
    release_pending_storage(st);
}

static void
compact_pending_symbols(dsd_fsk_modem_state* st) {
    if (st->pending_pos <= 0) {
        return;
    }

    int remaining = st->pending_len - st->pending_pos;
    if (remaining > 0) {
        float* pending = st->pending_heap ? st->pending_heap : st->pending_symbols;
        DSD_MEMMOVE(pending, pending + st->pending_pos, (size_t)remaining * sizeof(float));
    }
    st->pending_pos = 0;
    st->pending_len = remaining;
}

static int
ensure_pending_capacity(dsd_fsk_modem_state* st, int needed) {
    int cap = pending_symbol_capacity(st);
    if (needed <= cap) {
        return 1;
    }

    int new_cap = cap;
    while (new_cap < needed) {
        if (new_cap > 1073741823) {
            return 0;
        }
        new_cap *= 2;
    }

    float* next = NULL;
    if (st->pending_heap) {
        next = (float*)realloc(st->pending_heap, (size_t)new_cap * sizeof(float));
    } else {
        next = (float*)malloc((size_t)new_cap * sizeof(float));
        if (next) {
            DSD_MEMCPY(next, st->pending_symbols, (size_t)st->pending_len * sizeof(float));
        }
    }
    if (!next) {
        return 0;
    }

    st->pending_heap = next;
    st->pending_cap = new_cap;
    return 1;
}

static int
drain_pending_symbols(dsd_fsk_modem_state* st, float* out_symbols, int out_count, int max_symbols) {
    const float* pending = pending_symbol_data(st);
    while (st->pending_pos < st->pending_len && out_count < max_symbols) {
        out_symbols[out_count++] = pending[st->pending_pos++];
        st->symbols_emitted++;
    }
    if (st->pending_pos >= st->pending_len) {
        clear_pending_symbols(st);
    }
    return out_count;
}

static int
queue_pending_symbol(dsd_fsk_modem_state* st, float symbol) {
    if (st->pending_pos > 0 && st->pending_len >= pending_symbol_capacity(st)) {
        compact_pending_symbols(st);
    }
    if (!ensure_pending_capacity(st, st->pending_len + 1)) {
        return 0;
    }
    float* pending = st->pending_heap ? st->pending_heap : st->pending_symbols;
    pending[st->pending_len++] = symbol;
    return 1;
}

static int
emit_symbol(dsd_fsk_modem_state* st, float raw_symbol, float* out_symbols, int out_count, int max_symbols) {
    float sym = normalize_symbol(st, raw_symbol);
    st->last_symbol = sym;
    update_soft_metrics(st, sym);
    if (out_count < max_symbols) {
        out_symbols[out_count++] = sym;
        st->symbols_emitted++;
    } else {
        (void)queue_pending_symbol(st, sym);
    }
    return out_count;
}

static float
sum_freq_window(const float* freq, int start, int len) {
    float sum = 0.0f;
    for (int i = 0; i < len; i++) {
        sum += freq[start + i];
    }
    return sum;
}

static int
acquire_symbol_phase_scored(const float* freq, int n, int clock_i, float* out_score, float* out_second_score) {
    int best_phase = 0;
    float best_score = -1.0f;
    float second_score = -1.0f;

    for (int phase = 0; phase < clock_i; phase++) {
        double score = 0.0;
        int count = 0;
        for (int start = phase; start + clock_i <= n; start += clock_i) {
            float sum = sum_freq_window(freq, start, clock_i);
            score += fabs((double)sum / (double)clock_i);
            count++;
        }
        if (count > 0) {
            score /= (double)count;
        }
        if ((float)score > best_score) {
            second_score = best_score;
            best_score = (float)score;
            best_phase = phase;
        } else if ((float)score > second_score) {
            second_score = (float)score;
        }
    }

    if (out_score) {
        *out_score = best_score;
    }
    if (out_second_score) {
        *out_second_score = second_score;
    }
    return best_phase;
}

static int
acquire_symbol_phase(const float* freq, int n, int clock_i, float* out_score) {
    return acquire_symbol_phase_scored(freq, n, clock_i, out_score, NULL);
}

static int
tracking_target_samples(int clock_i) {
    int target = clock_i * DSD_FSK_TRACK_SYMBOLS;
    if (target > DSD_FSK_MODEM_TRACK_MAX_SAMPLES) {
        target = DSD_FSK_MODEM_TRACK_MAX_SAMPLES - (DSD_FSK_MODEM_TRACK_MAX_SAMPLES % clock_i);
    }
    if (target < clock_i * 12) {
        target = clock_i * 12;
    }
    return target;
}

static void
clear_tracking_window(dsd_fsk_modem_state* st) {
    st->track_len = 0;
    st->track_start_phase = 0.0f;
}

static float
wrap_timing_error(float err, int clock_i) {
    float half = (float)clock_i * 0.5f;
    while (err > half) {
        err -= (float)clock_i;
    }
    while (err < -half) {
        err += (float)clock_i;
    }
    return err;
}

typedef struct {
    int best_phase;
    int expected_phase;
    float best_score;
    float ratio;
    float err;
    int accept;
} dsd_fsk_track_eval;

static int track_boundary_phase_scored(const float* freq, int n, int clock_i, float* out_score,
                                       float* out_second_score);

static int
track_start_phase_mod(float start_phase, int clock_i) {
    int phase = (int)floorf(start_phase);
    phase %= clock_i;
    if (phase < 0) {
        phase += clock_i;
    }
    return phase;
}

static float
track_score_ratio(float best_score, float second_score) {
    if (second_score > 1.0e-12f) {
        return best_score / second_score;
    }
    return (best_score > 0.0f) ? 1000.0f : 0.0f;
}

static int
track_timing_ready(const dsd_fsk_modem_state* st, const float* phase_io, int clock_i) {
    if (!st || !phase_io || clock_i < 4 || clock_i > 32 || !st->timing_acquired) {
        return 0;
    }
    return 1;
}

static int
track_push_sample(dsd_fsk_modem_state* st, float centered, float sample_phase, int target) {
    if (target <= 0) {
        return 0;
    }
    if (st->track_len == 0) {
        st->track_start_phase = sample_phase;
    }
    if (st->track_len < DSD_FSK_MODEM_TRACK_MAX_SAMPLES) {
        st->track_freq[st->track_len++] = centered;
    }
    return (st->track_len >= target) ? 1 : 0;
}

static dsd_fsk_track_eval
track_compute_eval(const dsd_fsk_modem_state* st, int clock_i) {
    dsd_fsk_track_eval eval;
    float second_score = 0.0f;
    eval.best_score = 0.0f;
    eval.best_phase =
        track_boundary_phase_scored(st->track_freq, st->track_len, clock_i, &eval.best_score, &second_score);
    int start_phase = track_start_phase_mod(st->track_start_phase, clock_i);
    eval.expected_phase = (clock_i - start_phase) % clock_i;
    eval.ratio = track_score_ratio(eval.best_score, second_score);
    eval.err = wrap_timing_error((float)(eval.best_phase - eval.expected_phase), clock_i);
    eval.accept = (eval.best_score >= DSD_FSK_TRACK_MIN_SCORE && eval.ratio >= DSD_FSK_TRACK_MIN_RATIO
                   && fabsf(eval.err) <= 2.0f);
    return eval;
}

static float
track_gain_correction(float err) {
    float correction = err * DSD_FSK_TRACK_GAIN;
    if (correction > DSD_FSK_TRACK_MAX_CORRECTION) {
        correction = DSD_FSK_TRACK_MAX_CORRECTION;
    } else if (correction < -DSD_FSK_TRACK_MAX_CORRECTION) {
        correction = -DSD_FSK_TRACK_MAX_CORRECTION;
    }
    return correction;
}

static float
track_apply_correction(const dsd_fsk_modem_state* st, float* phase_io, float correction) {
    float corrected_phase = *phase_io - correction;
    if (corrected_phase < 0.0f) {
        correction = *phase_io;
        corrected_phase = 0.0f;
    } else if (corrected_phase >= st->symbol_clock) {
        float phase_limit = st->symbol_clock - 1.0e-4f;
        if (phase_limit < 0.0f) {
            phase_limit = 0.0f;
        }
        correction = *phase_io - phase_limit;
        corrected_phase = phase_limit;
    }
    *phase_io = corrected_phase;
    return correction;
}

static void
track_log_accept(const dsd_fsk_modem_state* st, int clock_i, const dsd_fsk_track_eval* eval, float correction) {
    if (debug_fsk_acq_enabled()) {
        DSD_FPRINTF(stderr,
                    "[FSKTRACK] clock=%d best=%d expect=%d err=%.3f corr=%.3f score=%.5f ratio=%.3f updates=%llu\n",
                    clock_i, eval->best_phase, eval->expected_phase, eval->err, correction, eval->best_score,
                    eval->ratio, (unsigned long long)st->track_updates);
    }
}

static void
track_log_skip(const dsd_fsk_modem_state* st, int clock_i, const dsd_fsk_track_eval* eval) {
    if (debug_fsk_acq_enabled()) {
        DSD_FPRINTF(stderr, "[FSKTRACK] skip clock=%d best=%d expect=%d score=%.5f ratio=%.3f skips=%llu\n", clock_i,
                    eval->best_phase, eval->expected_phase, eval->best_score, eval->ratio,
                    (unsigned long long)st->track_skips);
    }
}

static int
track_boundary_phase_scored(const float* freq, int n, int clock_i, float* out_score, float* out_second_score) {
    int best_phase = 0;
    float best_score = -1.0f;
    float second_score = -1.0f;

    for (int phase = 0; phase < clock_i; phase++) {
        double score = 0.0;
        int count = 0;
        for (int pos = phase; pos < n; pos += clock_i) {
            if (pos <= 0) {
                continue;
            }
            score += fabs((double)freq[pos] - (double)freq[pos - 1]);
            count++;
        }
        if (count > 0) {
            score /= (double)count;
        }
        if ((float)score > best_score) {
            second_score = best_score;
            best_score = (float)score;
            best_phase = phase;
        } else if ((float)score > second_score) {
            second_score = (float)score;
        }
    }

    if (out_score) {
        *out_score = best_score;
    }
    if (out_second_score) {
        *out_second_score = second_score;
    }
    return best_phase;
}

static void
maybe_track_symbol_timing(dsd_fsk_modem_state* st, float centered, int clock_i, float sample_phase, float* phase_io) {
    if (!track_timing_ready(st, phase_io, clock_i)) {
        return;
    }
    if (!track_push_sample(st, centered, sample_phase, tracking_target_samples(clock_i))) {
        return;
    }

    dsd_fsk_track_eval eval = track_compute_eval(st, clock_i);
    if (eval.accept) {
        float correction = track_gain_correction(eval.err);
        correction = track_apply_correction(st, phase_io, correction);
        st->track_last_error = eval.err;
        st->track_last_score = eval.best_score;
        st->track_updates++;
        track_log_accept(st, clock_i, &eval, correction);
    } else {
        st->track_last_error = 0.0f;
        st->track_last_score = eval.best_score;
        st->track_skips++;
        track_log_skip(st, clock_i, &eval);
    }
    clear_tracking_window(st);
}

static int
emit_acquisition_symbols(dsd_fsk_modem_state* st, int clock_i, float* out_symbols, int out_count, int max_symbols) {
    float score = 0.0f;
    int phase = acquire_symbol_phase(st->acq_freq, st->acq_len, clock_i, &score);
    if (phase < 0) {
        phase = 0;
    } else if (phase >= clock_i) {
        phase = clock_i - 1;
    }
    int start = phase;
    int emitted_leading = 0;

    if (phase >= clock_i - 1 && phase > 0) {
        float sum = sum_freq_window(st->acq_freq, 0, phase);
        out_count = emit_symbol(st, sum / (float)phase, out_symbols, out_count, max_symbols);
        emitted_leading = 1;
    }

    while (start + clock_i <= st->acq_len) {
        float sum = sum_freq_window(st->acq_freq, start, clock_i);
        out_count = emit_symbol(st, sum / (float)clock_i, out_symbols, out_count, max_symbols);
        start += clock_i;
    }

    st->symbol_accum = 0.0f;
    st->symbol_count = 0;
    st->symbol_phase = 0.0f;
    for (int i = start; i < st->acq_len; i++) {
        st->symbol_accum += st->acq_freq[i];
        st->symbol_count++;
        st->symbol_phase += 1.0f;
    }
    if (st->symbol_phase >= st->symbol_clock) {
        st->symbol_phase = fmodf(st->symbol_phase, st->symbol_clock);
    }
    st->acq_len = 0;
    st->timing_acquired = 1;
    clear_tracking_window(st);

    if (debug_fsk_acq_enabled()) {
        DSD_FPRINTF(stderr, "[FSKACQ] clock=%d phase=%d score=%.5f lead=%d carry=%d emitted=%llu\n", clock_i, phase,
                    score, emitted_leading, st->symbol_count, (unsigned long long)st->symbols_emitted);
    }
    return out_count;
}

typedef struct {
    float prev_i;
    float prev_q;
    int have_prev;
    float phase;
    float accum;
    int accum_count;
    float dc;
    float clock;
    int clock_i;
    int acq_target;
} dsd_fsk_process_ctx;

static float
modem_clock_or_default(const dsd_fsk_modem_state* st) {
    if (st->symbol_clock > 0.0f) {
        return st->symbol_clock;
    }
    return modem_symbol_clock(&st->cfg);
}

static int
modem_acquisition_target(int clock_i) {
    if (clock_i <= 0) {
        return 0;
    }
    return modem_acq_target_samples(clock_i);
}

static float
symbol_average_or_zero(float accum, int count) {
    if (count > 0) {
        return accum / (float)count;
    }
    return 0.0f;
}

static dsd_fsk_process_ctx
process_ctx_load(const dsd_fsk_modem_state* st) {
    dsd_fsk_process_ctx ctx;
    ctx.prev_i = st->prev_i;
    ctx.prev_q = st->prev_q;
    ctx.have_prev = st->have_prev;
    ctx.phase = st->symbol_phase;
    ctx.accum = st->symbol_accum;
    ctx.accum_count = st->symbol_count;
    ctx.dc = st->dc_est;
    ctx.clock = modem_clock_or_default(st);
    ctx.clock_i = modem_clock_int(ctx.clock);
    ctx.acq_target = modem_acquisition_target(ctx.clock_i);
    return ctx;
}

static void
process_ctx_store(dsd_fsk_modem_state* st, const dsd_fsk_process_ctx* ctx) {
    st->prev_i = ctx->prev_i;
    st->prev_q = ctx->prev_q;
    st->have_prev = ctx->have_prev;
    st->symbol_phase = ctx->phase;
    st->symbol_accum = ctx->accum;
    st->symbol_count = ctx->accum_count;
    st->dc_est = ctx->dc;
    st->symbol_clock = ctx->clock;
}

static float
discriminator_frequency(float cur_i, float cur_q, float prev_i, float prev_q) {
    float re = cur_i * prev_i + cur_q * prev_q;
    float im = cur_q * prev_i - cur_i * prev_q;
    return phase_delta_small_angle_or_atan2(im, re);
}

static float
update_centered_frequency(float* dc_io, float freq) {
    /* Very slow carrier centering. The symbol stream is expected to be
     * roughly balanced over frame-sync windows; keeping this slow prevents
     * long same-symbol runs from being mistaken for CFO. */
    *dc_io += 0.00025f * (freq - *dc_io);
    return freq - *dc_io;
}

static int
process_acquisition_window(dsd_fsk_modem_state* st, dsd_fsk_process_ctx* ctx, float centered, float* out_symbols,
                           int* out_count, int max_symbols) {
    if (st->timing_acquired || ctx->clock_i <= 0) {
        return 0;
    }
    if (st->acq_len < DSD_FSK_MODEM_ACQ_MAX_SAMPLES) {
        st->acq_freq[st->acq_len++] = centered;
    }
    if (st->acq_len >= ctx->acq_target) {
        *out_count = emit_acquisition_symbols(st, ctx->clock_i, out_symbols, *out_count, max_symbols);
        ctx->phase = st->symbol_phase;
        ctx->accum = st->symbol_accum;
        ctx->accum_count = st->symbol_count;
    }
    return 1;
}

static void
normalize_symbol_phase(float* phase_io, float clock) {
    if (*phase_io >= clock) {
        *phase_io -= clock;
        if (*phase_io >= clock) {
            *phase_io = fmodf(*phase_io, clock);
        }
    }
}

static void
process_tracking_window(dsd_fsk_modem_state* st, dsd_fsk_process_ctx* ctx, float centered, float* out_symbols,
                        int* out_count, int max_symbols) {
    float sample_phase = ctx->phase;
    ctx->accum += centered;
    ctx->accum_count++;
    ctx->phase += 1.0f;
    if (ctx->phase >= ctx->clock) {
        float raw_symbol = symbol_average_or_zero(ctx->accum, ctx->accum_count);
        *out_count = emit_symbol(st, raw_symbol, out_symbols, *out_count, max_symbols);
        ctx->accum = 0.0f;
        ctx->accum_count = 0;
        normalize_symbol_phase(&ctx->phase, ctx->clock);
    }
    maybe_track_symbol_timing(st, centered, ctx->clock_i, sample_phase, &ctx->phase);
}

int
dsd_fsk_modem_process(dsd_fsk_modem_state* st, const float* iq_interleaved, int len_interleaved, float* out_symbols,
                      int max_symbols) {
    if (!st || !out_symbols || max_symbols <= 0) {
        return 0;
    }

    int out_count = drain_pending_symbols(st, out_symbols, 0, max_symbols);
    if (!iq_interleaved || len_interleaved < 2) {
        return out_count;
    }

    dsd_fsk_process_ctx ctx = process_ctx_load(st);
    int pairs = len_interleaved >> 1;
    for (int n = 0; n < pairs; n++) {
        float cur_i = iq_interleaved[(n << 1) + 0];
        float cur_q = iq_interleaved[(n << 1) + 1];

        if (!ctx.have_prev) {
            ctx.prev_i = cur_i;
            ctx.prev_q = cur_q;
            ctx.have_prev = 1;
            continue;
        }

        float freq = discriminator_frequency(cur_i, cur_q, ctx.prev_i, ctx.prev_q);
        float centered = update_centered_frequency(&ctx.dc, freq);

        if (process_acquisition_window(st, &ctx, centered, out_symbols, &out_count, max_symbols)) {
            ctx.prev_i = cur_i;
            ctx.prev_q = cur_q;
            continue;
        }

        process_tracking_window(st, &ctx, centered, out_symbols, &out_count, max_symbols);
        ctx.prev_i = cur_i;
        ctx.prev_q = cur_q;
    }

    process_ctx_store(st, &ctx);
    return out_count;
}

int
dsd_fsk_modem_zero_symbols(dsd_fsk_modem_state* st, int input_complex_samples, float* out_symbols, int max_symbols) {
    if (!st || !out_symbols || input_complex_samples <= 0 || max_symbols <= 0) {
        return 0;
    }
    float clock = st->symbol_clock > 0.0f ? st->symbol_clock : modem_symbol_clock(&st->cfg);
    int n = (int)ceilf((float)input_complex_samples / clock);
    if (n < 1) {
        n = 1;
    }
    if (n > max_symbols) {
        n = max_symbols;
    }
    for (int i = 0; i < n; i++) {
        out_symbols[i] = 0.0f;
    }
    st->symbol_phase = 0.0f;
    st->symbol_accum = 0.0f;
    st->symbol_count = 0;
    st->last_symbol = 0.0f;
    clear_pending_symbols(st);
    clear_tracking_window(st);
    st->track_last_error = 0.0f;
    st->track_last_score = 0.0f;
    st->prev_i = 0.0f;
    st->prev_q = 0.0f;
    st->have_prev = 0;
    st->acq_len = 0;
    st->timing_acquired = modem_should_acquire_timing(&st->cfg, clock) ? 0 : 1;
    reset_soft_metrics(st);
    st->symbols_emitted += (uint64_t)n;
    return n;
}

int
dsd_fsk_modem_get_metrics(const dsd_fsk_modem_state* st, dsd_fsk_modem_metrics* out) {
    if (!st || !out) {
        return -1;
    }
    *out = st->metrics;
    out->levels = st->cfg.levels;
    out->symbol_rate_hz = st->cfg.symbol_rate_hz;
    out->timing_acquired = st->timing_acquired;
    out->track_last_error = st->track_last_error;
    out->track_last_score = st->track_last_score;
    out->track_updates = st->track_updates;
    out->track_skips = st->track_skips;
    out->abs_est = st->abs_est;
    out->dc_est = st->dc_est;
    out->last_symbol = st->last_symbol;
    if (out->window_symbols == 0U) {
        out->valid = 0;
    }
    return 0;
}
