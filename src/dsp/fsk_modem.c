// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/fsk_modem.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        memset(&out, 0, sizeof(out));
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
    memset(st, 0, sizeof(*st));
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
    memset(st, 0, sizeof(*st));
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

static float*
pending_symbol_data(dsd_fsk_modem_state* st) {
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
        float* pending = pending_symbol_data(st);
        memmove(pending, pending + st->pending_pos, (size_t)remaining * sizeof(float));
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
            memcpy(next, st->pending_symbols, (size_t)st->pending_len * sizeof(float));
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
    pending_symbol_data(st)[st->pending_len++] = symbol;
    return 1;
}

static int
emit_symbol(dsd_fsk_modem_state* st, float raw_symbol, float* out_symbols, int out_count, int max_symbols) {
    float sym = normalize_symbol(st, raw_symbol);
    st->last_symbol = sym;
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
acquire_symbol_phase(const float* freq, int n, int clock_i, float* out_score) {
    int best_phase = 0;
    float best_score = -1.0f;

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
            best_score = (float)score;
            best_phase = phase;
        }
    }

    if (out_score) {
        *out_score = best_score;
    }
    return best_phase;
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

    if (debug_fsk_acq_enabled()) {
        fprintf(stderr, "[FSKACQ] clock=%d phase=%d score=%.5f lead=%d carry=%d emitted=%llu\n", clock_i, phase, score,
                emitted_leading, st->symbol_count, (unsigned long long)st->symbols_emitted);
    }
    return out_count;
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

    int pairs = len_interleaved >> 1;
    float prev_i = st->prev_i;
    float prev_q = st->prev_q;
    int have_prev = st->have_prev;
    float phase = st->symbol_phase;
    float accum = st->symbol_accum;
    int accum_count = st->symbol_count;
    float dc = st->dc_est;
    float clock = st->symbol_clock > 0.0f ? st->symbol_clock : modem_symbol_clock(&st->cfg);
    int clock_i = modem_clock_int(clock);
    int acq_target = (clock_i > 0) ? modem_acq_target_samples(clock_i) : 0;

    for (int n = 0; n < pairs; n++) {
        float cur_i = iq_interleaved[(n << 1) + 0];
        float cur_q = iq_interleaved[(n << 1) + 1];

        if (!have_prev) {
            prev_i = cur_i;
            prev_q = cur_q;
            have_prev = 1;
            continue;
        }

        float re = cur_i * prev_i + cur_q * prev_q;
        float im = cur_q * prev_i - cur_i * prev_q;
        float freq = phase_delta_small_angle_or_atan2(im, re);

        /* Very slow carrier centering. The symbol stream is expected to be
         * roughly balanced over frame-sync windows; keeping this slow prevents
         * long same-symbol runs from being mistaken for CFO. */
        dc += 0.00025f * (freq - dc);
        float centered = freq - dc;

        if (!st->timing_acquired && clock_i > 0) {
            if (st->acq_len < DSD_FSK_MODEM_ACQ_MAX_SAMPLES) {
                st->acq_freq[st->acq_len++] = centered;
            }
            if (st->acq_len >= acq_target) {
                out_count = emit_acquisition_symbols(st, clock_i, out_symbols, out_count, max_symbols);
                phase = st->symbol_phase;
                accum = st->symbol_accum;
                accum_count = st->symbol_count;
            }
            prev_i = cur_i;
            prev_q = cur_q;
            continue;
        }

        accum += centered;
        accum_count++;
        phase += 1.0f;
        if (phase >= clock) {
            float raw_symbol = (accum_count > 0) ? (accum / (float)accum_count) : 0.0f;
            out_count = emit_symbol(st, raw_symbol, out_symbols, out_count, max_symbols);
            accum = 0.0f;
            accum_count = 0;
            phase -= clock;
            if (phase >= clock) {
                phase = fmodf(phase, clock);
            }
        }

        prev_i = cur_i;
        prev_q = cur_q;
    }

    st->prev_i = prev_i;
    st->prev_q = prev_q;
    st->have_prev = have_prev;
    st->symbol_phase = phase;
    st->symbol_accum = accum;
    st->symbol_count = accum_count;
    st->dc_est = dc;
    st->symbol_clock = clock;
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
    st->prev_i = 0.0f;
    st->prev_q = 0.0f;
    st->have_prev = 0;
    st->acq_len = 0;
    st->timing_acquired = modem_should_acquire_timing(&st->cfg, clock) ? 0 : 1;
    st->symbols_emitted += (uint64_t)n;
    return n;
}
