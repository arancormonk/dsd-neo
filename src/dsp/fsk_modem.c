// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/fsk_modem.h>

#include <math.h>
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

void
dsd_fsk_modem_reset(dsd_fsk_modem_state* st) {
    if (!st) {
        return;
    }
    dsd_fsk_modem_config cfg = st->cfg;
    memset(st, 0, sizeof(*st));
    st->cfg = normalized_config(&cfg);
    st->symbol_clock = modem_symbol_clock(&st->cfg);
    st->abs_est = 0.0f;
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

static int
emit_symbol(dsd_fsk_modem_state* st, float raw_symbol, float* out_symbols, int out_count, int max_symbols) {
    if (out_count >= max_symbols) {
        return out_count;
    }
    float sym = normalize_symbol(st, raw_symbol);
    out_symbols[out_count++] = sym;
    st->last_symbol = sym;
    st->symbols_emitted++;
    return out_count;
}

int
dsd_fsk_modem_process(dsd_fsk_modem_state* st, const float* iq_interleaved, int len_interleaved, float* out_symbols,
                      int max_symbols) {
    if (!st || !iq_interleaved || !out_symbols || len_interleaved < 2 || max_symbols <= 0) {
        return 0;
    }

    int pairs = len_interleaved >> 1;
    int out_count = 0;
    float prev_i = st->prev_i;
    float prev_q = st->prev_q;
    int have_prev = st->have_prev;
    float phase = st->symbol_phase;
    float accum = st->symbol_accum;
    int accum_count = st->symbol_count;
    float dc = st->dc_est;
    float clock = st->symbol_clock > 0.0f ? st->symbol_clock : modem_symbol_clock(&st->cfg);

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
        float freq = atan2f(im, re);

        /* Very slow carrier centering. The symbol stream is expected to be
         * roughly balanced over frame-sync windows; keeping this slow prevents
         * long same-symbol runs from being mistaken for CFO. */
        dc += 0.00025f * (freq - dc);
        float centered = freq - dc;

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
            if (out_count >= max_symbols) {
                break;
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
    st->symbols_emitted += (uint64_t)n;
    return n;
}
