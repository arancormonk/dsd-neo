// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/dsp/symbol_levels.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef TRACE_DSD
#include <dsd-neo/platform/file_compat.h>
#endif

#ifdef USE_RADIO
#endif

#ifdef USE_RADIO
#define DSD_RTL_OUTPUT_KIND_SYMBOL_FSK 1
#endif

static void DSD_ATTR_USED
throttle_symbol_bin_replay(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || state->use_throttle != 1) {
        return;
    }

    /* Keep legacy microsecond override support when explicitly configured. */
    if (state->symbol_throttle > 0) {
        dsd_sleep_us((uint64_t)state->symbol_throttle);
        return;
    }

    int input_rate_hz = dsd_opts_current_input_timing_rate(opts);
    if (input_rate_hz <= 0) {
        input_rate_hz = SAMPLE_RATE_IN;
    }

    int sps = state->samplesPerSymbol;
    if (sps <= 0) {
        sps = 10;
    }

    uint64_t period_ns = ((uint64_t)sps * 1000000000ULL) / (uint64_t)input_rate_hz;
    if (period_ns == 0ULL) {
        dsd_sleep_ms(0U);
        return;
    }

    uint64_t now_ns = dsd_time_monotonic_ns();
    uint64_t deadline_ns = state->symbol_replay_next_deadline_ns;
    if (deadline_ns == 0ULL) {
        state->symbol_replay_next_deadline_ns = now_ns + period_ns;
        return;
    }

    if (now_ns < deadline_ns) {
        dsd_sleep_ns(deadline_ns - now_ns);
    } else if ((now_ns - deadline_ns) > 250000000ULL) {
        /* Rebase pacing after long scheduler stalls/debug pauses. */
        state->symbol_replay_next_deadline_ns = now_ns + period_ns;
        return;
    }

    state->symbol_replay_next_deadline_ns = deadline_ns + period_ns;
}

static void DSD_ATTR_USED
datascope_modulation_label(const dsd_state* state, char modulation[8]) {
    if (state->rf_mod == 0) {
        DSD_SNPRINTF(modulation, 8, "C4FM");
    } else if (state->rf_mod == 1) {
        DSD_SNPRINTF(modulation, 8, "QPSK");
    } else if (state->rf_mod == 2) {
        DSD_SNPRINTF(modulation, 8, "GFSK");
    }
}

static int
clamp_datascope_bin(int bin) {
    if (bin < 0) {
        return 0;
    }
    if (bin > 63) {
        return 63;
    }
    return bin;
}

static void
build_datascope_spectrum(const float* sbuf2, int count, float scale, int spectrum[64]) {
    for (int i = 0; i < 64; i++) {
        spectrum[i] = 0;
    }

    for (int i = 0; i < count; i++) {
        int bin = (int)lrintf((sbuf2[i] * scale) + 32.0f);
        spectrum[clamp_datascope_bin(bin)]++;
    }
}

static void DSD_ATTR_USED
build_datascope_bins(const dsd_state* state, float scale, int bins[5]) {
    bins[0] = clamp_datascope_bin((int)lrintf(state->min * scale + 32.0f));
    bins[1] = clamp_datascope_bin((int)lrintf(state->max * scale + 32.0f));
    bins[2] = clamp_datascope_bin((int)lrintf(state->lmid * scale + 32.0f));
    bins[3] = clamp_datascope_bin((int)lrintf(state->umid * scale + 32.0f));
    bins[4] = clamp_datascope_bin((int)lrintf(state->center * scale + 32.0f));
}

static char
datascope_char_for_cell(int row, int col, const int spectrum[64], const int bins[5]) {
    if (row == 0) {
        if (col == bins[0] || col == bins[1]) {
            return '#';
        }
        if (col == bins[2] || col == bins[3]) {
            return '^';
        }
        if (col == bins[4]) {
            return '!';
        }
    } else if (spectrum[col] > 9 - row) {
        return '*';
    }

    if (col == 32) {
        return '|';
    }
    return ' ';
}

static void
print_datascope_grid(const int spectrum[64], const int bins[5]) {
    for (int row = 0; row < 10; row++) {
        printf("|");
        for (int col = 0; col < 64; col++) {
            DSD_FPRINTF(stderr, "%c", datascope_char_for_cell(row, col, spectrum, bins));
        }
        DSD_FPRINTF(stderr, "|\n");
    }
}

static void DSD_ATTR_USED
print_datascope(const dsd_opts* opts, dsd_state* state, const float* sbuf2, int count) {
    char modulation[8];
    int spectrum[64];

    datascope_modulation_label(state, modulation);
    float span = fmaxf(fabsf(state->max), fabsf(state->min));
    if (span < 1e-3f) {
        span = 1.0f;
    }
    const float scale = 32.0f / span;
    build_datascope_spectrum(sbuf2, count, scale, spectrum);
    if (state->symbolcnt > (4800 / opts->scoperate)) {
        state->symbolcnt = 0;
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, "Demod mode:     %s                Nac:                     %4X\n", modulation, state->nac);
        DSD_FPRINTF(stderr, "Frame Type:    %s        Talkgroup:            %7i\n", state->ftype, state->lasttg);
        DSD_FPRINTF(stderr, "Frame Subtype: %s       Source:          %12i\n", state->fsubtype, state->lastsrc);
        DSD_FPRINTF(stderr, "TDMA activity:  %s %s     Voice errors: %s\n", state->slot0light, state->slot1light,
                    state->err_str);
        DSD_FPRINTF(stderr, "+----------------------------------------------------------------+\n");
        int bins[5];
        build_datascope_bins(state, scale, bins);
        print_datascope_grid(spectrum, bins);
        DSD_FPRINTF(stderr, "+----------------------------------------------------------------+\n");
    }
}

static void
symbol_window_extrema_avg2(const float* samples, int count, float* out_min, float* out_max) {
    if (!samples || !out_min || !out_max || count < 2) {
        if (out_min) {
            *out_min = 0.0f;
        }
        if (out_max) {
            *out_max = 0.0f;
        }
        return;
    }

    float min1 = samples[0];
    float min2 = samples[1];
    if (min2 < min1) {
        const float tmp = min1;
        min1 = min2;
        min2 = tmp;
    }

    float max1 = samples[0];
    float max2 = samples[1];
    if (max2 > max1) {
        const float tmp = max1;
        max1 = max2;
        max2 = tmp;
    }

    for (int i = 2; i < count; i++) {
        const float v = samples[i];
        if (v < min1) {
            min2 = min1;
            min1 = v;
        } else if (v < min2) {
            min2 = v;
        }

        if (v > max1) {
            max2 = max1;
            max1 = v;
        } else if (v > max2) {
            max2 = v;
        }
    }

    *out_min = (min1 + min2) * 0.5f;
    *out_max = (max1 + max2) * 0.5f;
}

static void DSD_ATTR_USED
use_symbol(const dsd_opts* opts, dsd_state* state, float symbol) {
    UNUSED(symbol);

    if (opts == NULL || state == NULL) {
        return;
    }

    float lmin, lmax;

    int cap = opts->ssize;
    if (cap < 0) {
        cap = 0;
    }
    if (cap > (int)(sizeof(state->sbuf) / sizeof(state->sbuf[0]))) {
        cap = (int)(sizeof(state->sbuf) / sizeof(state->sbuf[0]));
    }

    // Continuous update of min/max
    // - QPSK: always (as before)
    // - C4FM: enable for P25 Phase 1 (+/-) to keep slicer thresholds fresh during calls
    if (state->rf_mod == 1 || (state->rf_mod == 0 && DSD_SYNC_IS_P25P1(state->lastsynctype))) {
        symbol_window_extrema_avg2(state->sbuf, cap, &lmin, &lmax);
        dsd_state_push_minmax_window(state, opts->msize, lmin, lmax);
        state->center = ((state->max) + (state->min)) / 2.0f;
        state->umid = (((state->max) - state->center) * 5.0f / 8.0f) + state->center;
        state->lmid = (((state->min) - state->center) * 5.0f / 8.0f) + state->center;
        state->maxref = (state->max) * 0.80F;
        state->minref = (state->min) * 0.80F;
    } else {
        state->maxref = state->max;
        state->minref = state->min;
    }

    // Increase sidx
    if (cap <= 0) {
        // nothing to do
    } else if (state->sidx >= (cap - 1)) {
        state->sidx = 0;

        if (opts->datascope == 1) {
            print_datascope(opts, state, state->sbuf, cap);
        }
    } else {
        state->sidx++;
    }

    if (state->dibit_buf_p > state->dibit_buf + 900000) {
        state->dibit_buf_p = state->dibit_buf + 200;
    }

    //dmr buffer
    if (state->dmr_payload_p > state->dmr_payload_buf + 900000) {
        state->dmr_payload_p = state->dmr_payload_buf + 200;
    }
    //dmr buffer end
}

static int
invert_dibit(int dibit) {
    switch (dibit) {
        case 0: return 2;
        case 1: return 3;
        case 2: return 0;
        case 3: return 1;
        default: break;
    }

    return -1;
}

/**
 * @brief CQPSK 4-level slicer matching OP25's fsk4_slicer_fb.
 *
 * When the RTL-SDR CQPSK demodulation path is active, qpsk_differential_demod
 * outputs phase values scaled by 4/π. For π/4-DQPSK, the differential phases
 * are at ±45° and ±135°, which map to symbol values of ±1 and ±3 respectively.
 *
 * Symbol-to-dibit mapping (same as OP25 fsk4_slicer_fb):
 *   sym >= +2.0  → dibit 1 (symbol +3, phase +135°)
 *   0 <= sym < +2.0 → dibit 0 (symbol +1, phase +45°)
 *   -2.0 <= sym < 0 → dibit 2 (symbol -1, phase -45°)
 *   sym < -2.0  → dibit 3 (symbol -3, phase -135°)
 *
 * @param symbol Input symbol value (scaled phase, ~[-4,+4]).
 * @return Dibit value [0,3].
 */
static inline int DSD_ATTR_USED
cqpsk_slice(float symbol) {
    /* Fixed threshold slicer for CQPSK symbols, matching OP25's fsk4_slicer_fb.
     * qpsk_differential_demod outputs phase * 4/π, giving symbols at ±1, ±3.
     * Threshold at ±2.0 is the midpoint between these symbol levels. */
    const float kUpperThresh = 2.0f;  /* boundary between +1 and +3 symbols */
    const float kLowerThresh = -2.0f; /* boundary between -1 and -3 symbols */
    int dibit;

    if (symbol >= kUpperThresh) {
        dibit = 1; /* +3 (phase +135°) */
    } else if (symbol >= 0.0f) {
        dibit = 0; /* +1 (phase +45°) */
    } else if (symbol >= kLowerThresh) {
        dibit = 2; /* -1 (phase -45°) */
    } else {
        dibit = 3; /* -3 (phase -135°) */
    }

    return dibit;
}

/*
 * CQPSK slicer with optional debug inversion for sync alignment.
 *
 * The CQPSK DSP path mirrors OP25, so constellation rotation is already resolved
 * by the differential Costas loop.
 */
static inline int DSD_ATTR_USED
cqpsk_slice_aligned(float symbol) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(NULL);
        cfg = dsd_neo_get_config();
    }
    int inv = (cfg && cfg->cqpsk_sync_inv) ? 1 : 0;
    int negate = (cfg && cfg->cqpsk_sync_neg) ? 1 : 0;

    float s = negate ? -symbol : symbol;
    int raw_dibit = cqpsk_slice(s);
    if (inv) {
        raw_dibit = invert_dibit(raw_dibit);
    }
    return raw_dibit;
}

static float
cqpsk_reliability_ideal(float sym_c) {
    if (sym_c >= 2.0f) {
        return 3.0f;
    }
    if (sym_c >= 0.0f) {
        return 1.0f;
    }
    if (sym_c >= -2.0f) {
        return -1.0f;
    }
    return -3.0f;
}

static int
cqpsk_reliability_raw(float sym_c) {
    float error = fabsf(sym_c - cqpsk_reliability_ideal(sym_c));
    if (error > 1.0f) {
        error = 1.0f;
    }

    int rel = (int)((1.0f - error) * 255.0f + 0.5f);
    if (rel < 0) {
        rel = 0;
    }
    if (rel > 255) {
        rel = 255;
    }
    return rel;
}

#ifdef USE_RADIO
static int
apply_cqpsk_snr_weight(int rel) {
    double snr_db = dsd_rtl_stream_metrics_hook_snr_cqpsk_db();
    if (snr_db <= -50.0) {
        return rel;
    }

    int w256 = 0;
    if (snr_db >= 25.0) {
        w256 = 255;
    } else if (snr_db > 0.0) {
        w256 = (int)((snr_db / 25.0) * 255.0 + 0.5);
    }
    int scale_num = 204 + (w256 >> 2);
    int scaled = (rel * scale_num) >> 8;
    if (scaled > 255) {
        scaled = 255;
    }
    if (scaled < 0) {
        scaled = 0;
    }
    return scaled;
}
#endif

static int DSD_ATTR_USED
fsk_soft_metric_available(int* out_levels) {
#ifdef USE_RADIO
    int symbol_rate_hz = 0;
    int levels = 0;
    int channel_profile = 0;
    if (dsd_rtl_stream_metrics_hook_stream_active()
        && dsd_rtl_stream_metrics_hook_output_kind() == DSD_RTL_OUTPUT_KIND_SYMBOL_FSK
        && dsd_rtl_stream_metrics_hook_symbol_profile(&symbol_rate_hz, &levels, &channel_profile) == 0) {
        (void)symbol_rate_hz;
        (void)channel_profile;
        if (out_levels) {
            *out_levels = (levels == 2) ? 2 : 4;
        }
        return 1;
    }
#else
    (void)out_levels;
#endif
    return 0;
}

static int
c4fm_reliability_from_thresholds(const dsd_state* st, float sym) {
    const float eps = 1e-6f;
    float lmid = st->lmid;
    float center = st->center;
    float umid = st->umid;
    int rel;

    if (sym > umid) {
        float span = st->max - umid;
        if (span < eps) {
            span = eps;
        }
        rel = (int)lrintf(((sym - umid) * 255.0f) / span);
    } else if (sym > center) {
        float d1 = sym - center;
        float d2 = umid - sym;
        float span = umid - center;
        if (span < eps) {
            span = eps;
        }
        float m = d1 < d2 ? d1 : d2;
        rel = (int)lrintf((m * 510.0f) / span);
    } else if (sym >= lmid) {
        float d1 = center - sym;
        float d2 = sym - lmid;
        float span = center - lmid;
        if (span < eps) {
            span = eps;
        }
        float m = d1 < d2 ? d1 : d2;
        rel = (int)lrintf((m * 510.0f) / span);
    } else {
        float span = lmid - st->min;
        if (span < eps) {
            span = eps;
        }
        rel = (int)lrintf(((lmid - sym) * 255.0f) / span);
    }

    if (rel < 0) {
        rel = 0;
    }
    if (rel > 255) {
        rel = 255;
    }
    return rel;
}

#ifdef USE_RADIO
static int
apply_c4fm_snr_weight(int rel, int rtl_fsk_soft, int rtl_fsk_levels) {
    double snr_db = -100.0;
    if (rtl_fsk_soft && rtl_fsk_levels == 2) {
        snr_db = dsd_rtl_stream_metrics_hook_snr_gfsk_db();
        if (snr_db < -50.0) {
            snr_db = dsd_rtl_stream_metrics_hook_snr_c4fm_db();
        }
    } else {
        snr_db = dsd_rtl_stream_metrics_hook_snr_c4fm_db();
    }
    if (snr_db < -50.0) {
        snr_db = dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db();
    }

    int w256 = 0;
    if (snr_db > -13.0) {
        if (snr_db >= 12.0) {
            w256 = 255;
        } else {
            double w = (snr_db + 13.0) / 25.0;
            if (w < 0.0) {
                w = 0.0;
            }
            if (w > 1.0) {
                w = 1.0;
            }
            w256 = (int)(w * 255.0 + 0.5);
        }
    }

    int scale_num = 204 + (w256 >> 2);
    int scaled = (rel * scale_num) >> 8;
    if (scaled > 255) {
        scaled = 255;
    }
    if (scaled < 0) {
        scaled = 0;
    }
    return scaled;
}
#endif

uint8_t
dmr_compute_reliability(const dsd_state* st, float sym) {
    if (st->rf_mod == 1) {
        float sym_c = sym - st->center;
        int rel = cqpsk_reliability_raw(sym_c);
#ifdef USE_RADIO
        rel = apply_cqpsk_snr_weight(rel);
#endif
        return (uint8_t)rel;
    }

    int rel = c4fm_reliability_from_thresholds(st, sym);
#ifdef USE_RADIO
    int rtl_fsk_levels = 4;
    int rtl_fsk_soft = fsk_soft_metric_available(&rtl_fsk_levels);
    if (rtl_fsk_soft) {
        rel = (int)dsd_fsk_symbol_reliability(sym, rtl_fsk_levels);
    }
#endif
#ifdef USE_RADIO
    rel = apply_c4fm_snr_weight(rel, rtl_fsk_soft, rtl_fsk_levels);
#endif
    return (uint8_t)rel;
}

static int
clamp_u8_int(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

static int
abs_i16_to_int(int16_t v) {
    return v < 0 ? -(int)v : (int)v;
}

static int16_t
llr_from_magnitude_and_bit(int magnitude, int bit) {
    magnitude = clamp_u8_int(magnitude);
    return (int16_t)(bit ? magnitude : -magnitude);
}

static void
fallback_soft_from_dibit(int dibit, uint8_t reliability, dsd_dibit_soft_t* out) {
    if (!out) {
        return;
    }
    int hard_dibit = dibit & 3;
    reliability = (uint8_t)clamp_u8_int(reliability);
    out->reliability = reliability;
    out->llr[0] = llr_from_magnitude_and_bit(reliability, (hard_dibit >> 1) & 1);
    out->llr[1] = llr_from_magnitude_and_bit(reliability, hard_dibit & 1);
}

static float
square_float(float v) {
    return v * v;
}

static int
soft_metric_for_bit(float symbol, const float ideal[4], int bit_index) {
    float best0 = 3.4028234663852886e38f;
    float best1 = 3.4028234663852886e38f;
    float min_spacing = 3.4028234663852886e38f;

    for (int i = 0; i < 4; i++) {
        float d = square_float(symbol - ideal[i]);
        if (((i >> (1 - bit_index)) & 1) != 0) {
            if (d < best1) {
                best1 = d;
            }
        } else if (d < best0) {
            best0 = d;
        }
        for (int j = i + 1; j < 4; j++) {
            float spacing = fabsf(ideal[i] - ideal[j]);
            if (spacing > 1e-6f && spacing < min_spacing) {
                min_spacing = spacing;
            }
        }
    }

    if (min_spacing == 3.4028234663852886e38f) {
        min_spacing = 2.0f;
    }
    float scale = 255.0f / (min_spacing * min_spacing);
    int magnitude = (int)lrintf(fabsf(best0 - best1) * scale);
    return clamp_u8_int(magnitude);
}

static void DSD_ATTR_USED
build_standard_dibit_ideals(const dsd_state* state, int inverted, float ideal[4]) {
    float plus_one = 0.5f * (state->center + state->umid);
    float minus_one = 0.5f * (state->lmid + state->center);

    if (inverted) {
        ideal[0] = minus_one;
        ideal[1] = state->min;
        ideal[2] = plus_one;
        ideal[3] = state->max;
    } else {
        ideal[0] = plus_one;
        ideal[1] = state->max;
        ideal[2] = minus_one;
        ideal[3] = state->min;
    }
}

static void DSD_ATTR_USED
build_cqpsk_dibit_ideals(const dsd_state* state, float ideal[4]) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(NULL);
        cfg = dsd_neo_get_config();
    }

    int inv = (cfg && cfg->cqpsk_sync_inv) ? 1 : 0;
    int negate = (cfg && cfg->cqpsk_sync_neg) ? 1 : 0;
    const float base_ideal[4] = {1.0f, 3.0f, -1.0f, -3.0f};

    for (int dibit = 0; dibit < 4; dibit++) {
        int base = inv ? invert_dibit(dibit) : dibit;
        if (base < 0 || base >= 4) {
            base = 0;
        }
        float level = 1.0f;
        switch (base) {
            case 0: level = base_ideal[0]; break;
            case 1: level = base_ideal[1]; break;
            case 2: level = base_ideal[2]; break;
            case 3: level = base_ideal[3]; break;
            default: level = base_ideal[0]; break;
        }
        if (negate) {
            level = -level;
        }
        ideal[dibit] = state->center + level;
    }
}

static void DSD_ATTR_USED
compute_dibit_soft_metric(const dsd_state* state, float symbol, int dibit, int inverted, int cqpsk_aligned,
                          dsd_dibit_soft_t* out) {
    if (!state || !out || dibit < 0 || dibit > 3) {
        fallback_soft_from_dibit(dibit & 3, 255, out);
        return;
    }

    float ideal[4];
    if (cqpsk_aligned) {
        build_cqpsk_dibit_ideals(state, ideal);
    } else {
        build_standard_dibit_ideals(state, inverted, ideal);
    }

    int mag0 = soft_metric_for_bit(symbol, ideal, 0);
    int mag1 = soft_metric_for_bit(symbol, ideal, 1);

    uint8_t rel = dmr_compute_reliability(state, symbol);
    int min_mag = mag0 < mag1 ? mag0 : mag1;
    if (min_mag > 0 && rel < min_mag) {
        mag0 = (mag0 * (int)rel) / min_mag;
        mag1 = (mag1 * (int)rel) / min_mag;
    }

    out->llr[0] = llr_from_magnitude_and_bit(mag0, (dibit >> 1) & 1);
    out->llr[1] = llr_from_magnitude_and_bit(mag1, dibit & 1);
    int rel_from_llr = abs_i16_to_int(out->llr[0]);
    int rel1 = abs_i16_to_int(out->llr[1]);
    if (rel1 < rel_from_llr) {
        rel_from_llr = rel1;
    }
    out->reliability = (uint8_t)clamp_u8_int(rel_from_llr);
}

static void DSD_ATTR_USED
wrap_soft_buffer(dsd_state* state) {
    if (state && state->dmr_soft_buf && state->dmr_soft_p > state->dmr_soft_buf + 900000) {
        state->dmr_soft_p = state->dmr_soft_buf + 200;
    }
}

static void DSD_ATTR_USED
write_dibit_soft_metric(dsd_state* state, const dsd_dibit_soft_t* soft) {
    if (!state || !soft || !state->dmr_soft_p) {
        return;
    }
    wrap_soft_buffer(state);
    *state->dmr_soft_p = *soft;
    state->dmr_soft_p++;
}

static int DSD_ATTR_USED
read_previous_dibit_soft(const dsd_state* state, dsd_dibit_soft_t* out) {
    if (!state || !out || !state->dmr_soft_buf || !state->dmr_soft_p) {
        return 0;
    }
    const dsd_dibit_soft_t* sp = state->dmr_soft_p;
    if (sp <= state->dmr_soft_buf + 200 || sp > state->dmr_soft_buf + 1000000) {
        return 0;
    }
    *out = *(sp - 1);
    return 1;
}

static void DSD_ATTR_USED
replace_previous_dibit_soft(dsd_state* state, const dsd_dibit_soft_t* soft) {
    if (!state || !soft) {
        return;
    }

    if (state->dmr_soft_buf != NULL && state->dmr_soft_p != NULL) {
        dsd_dibit_soft_t* sp = state->dmr_soft_p;
        if (sp > state->dmr_soft_buf + 200 && sp <= state->dmr_soft_buf + 1000000) {
            *(sp - 1) = *soft;
        }
    }

    if (state->dmr_reliab_buf != NULL && state->dmr_reliab_p != NULL) {
        uint8_t* rp = state->dmr_reliab_p;
        if (rp > state->dmr_reliab_buf + 200 && rp <= state->dmr_reliab_buf + 1000000) {
            *(rp - 1) = soft->reliability;
        }
    }
}

static void DSD_ATTR_USED
replace_symbol_bin_soft_metric(dsd_state* state, int dibit) {
    dsd_dibit_soft_t soft;
    if (state != NULL && state->symbol_replay_has_soft) {
        soft = state->symbol_replay_soft;
        state->symbol_replay_has_soft = 0;
    } else {
        fallback_soft_from_dibit(dibit, 255, &soft);
    }
    replace_previous_dibit_soft(state, &soft);
}

static void
write_le_i16(unsigned char* out, int16_t value) {
    uint16_t u = (uint16_t)value;
    out[0] = (unsigned char)(u & 0xFFU);
    out[1] = (unsigned char)((u >> 8) & 0xFFU);
}

static void
write_le_u32(unsigned char* out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xFFU);
    out[1] = (unsigned char)((value >> 8) & 0xFFU);
    out[2] = (unsigned char)((value >> 16) & 0xFFU);
    out[3] = (unsigned char)((value >> 24) & 0xFFU);
}

static void DSD_ATTR_USED
write_symbol_capture_record_with_soft(dsd_opts* opts, dsd_state* state, int dibit, float symbol,
                                      const dsd_dibit_soft_t* soft_in) {
    if (opts == NULL || state == NULL || opts->symbol_out_f == NULL) {
        return;
    }

    if (opts->symbol_capture_format != DSD_SYMBOL_CAPTURE_FORMAT_SOFT) {
        fputc(dibit, opts->symbol_out_f);
        return;
    }

    dsd_dibit_soft_t soft;
    if (soft_in != NULL) {
        soft = *soft_in;
    } else {
        fallback_soft_from_dibit(dibit, 255, &soft);
    }

    unsigned char record[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    record[0] = (unsigned char)(dibit & 3);
    record[1] = soft.reliability;
    write_le_i16(record + 2, soft.llr[0]);
    write_le_i16(record + 4, soft.llr[1]);
    uint32_t raw_symbol = 0;
    DSD_MEMCPY(&raw_symbol, &symbol, sizeof(raw_symbol));
    write_le_u32(record + 6, raw_symbol);
    if (fwrite(record, 1, sizeof(record), opts->symbol_out_f) == sizeof(record)) {
        state->symbol_capture_soft_records++;
    }
}

void
write_symbol_capture_record(dsd_opts* opts, dsd_state* state, int dibit, float symbol) {
    write_symbol_capture_record_with_soft(opts, state, dibit, symbol, NULL);
}

#ifdef DSD_NEO_TEST_HOOKS
uint8_t
dsd_test_compute_cqpsk_reliability(float sym) {
    // Use static to avoid stack overflow - dsd_state is ~1.5MB
    static dsd_state dummy;
    static int initialized = 0;
    if (!initialized) {
        DSD_MEMSET(&dummy, 0, sizeof(dummy));
        initialized = 1;
    }
    dummy.rf_mod = 1;
    return dmr_compute_reliability(&dummy, sym);
}
#endif

/**
 * @brief Check if CQPSK demodulation path is active.
 *
 * Returns 1 if the RTL-SDR CQPSK path with TED is active, meaning symbols
 * are pre-scaled phase values suitable for the CQPSK slicer.
 *
 * @param opts Decoder options (checks audio_in_type).
 * @return 1 if CQPSK active, 0 otherwise.
 */
static inline int DSD_ATTR_USED
is_cqpsk_active(const dsd_opts* opts) {
#ifdef USE_RADIO
    if (opts && opts->audio_in_type == AUDIO_IN_RTL) {
        int cqpsk = 0, fll = 0, ted = 0;
        dsd_rtl_stream_metrics_hook_dsp_get(&cqpsk, &fll, &ted);
        if (cqpsk && ted) {
            return 1;
        }
    }
#else
    UNUSED(opts);
#endif
    return 0;
}

#ifdef USE_RADIO
/* Optional histogram of CQPSK slicer output during decoding. */
static void DSD_ATTR_USED
debug_log_cqpsk_slice(int dibit, float symbol, const dsd_state* state) {
    static int hist[4] = {0, 0, 0, 0};
    static int sample_count = 0;
    static float sym_min = 1e9f, sym_max = -1e9f, sym_sum = 0.0f;
    (void)state;

    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(NULL);
        cfg = dsd_neo_get_config();
    }
    if (!cfg || !cfg->debug_cqpsk_enable) {
        return;
    }

    int idx = dibit & 0x3;
    if (idx >= 0 && idx < 4) {
        hist[idx]++;
    }
    sym_sum += symbol;
    if (symbol < sym_min) {
        sym_min = symbol;
    }
    if (symbol > sym_max) {
        sym_max = symbol;
    }
    if (++sample_count >= 4800) {
        float n = (float)sample_count;
        float avg = sym_sum / n;
        DSD_FPRINTF(stderr,
                    "[SLICE-DECODE] d0:%.1f%% d1:%.1f%% d2:%.1f%% d3:%.1f%% avg:%.2f range:[%.2f,%.2f] (n=%d)\n",
                    100.0f * hist[0] / n, 100.0f * hist[1] / n, 100.0f * hist[2] / n, 100.0f * hist[3] / n, avg,
                    sym_min, sym_max, sample_count);
        hist[0] = hist[1] = hist[2] = hist[3] = 0;
        sample_count = 0;
        sym_sum = 0.0f;
        sym_min = 1e9f;
        sym_max = -1e9f;
    }
}
#else
static inline void DSD_ATTR_USED
debug_log_cqpsk_slice(int dibit, float symbol, const dsd_state* state) {
    UNUSED3(dibit, symbol, state);
}
#endif

static int
is_two_level_pos_synctype(int synctype) {
    switch (synctype) {
        case DSD_SYNC_DSTAR_VOICE_POS:
        case DSD_SYNC_PROVOICE_POS:
        case DSD_SYNC_DSTAR_HD_POS:
        case DSD_SYNC_EDACS_POS: return 1;
        default: break;
    }
    return 0;
}

static int
is_two_level_neg_synctype(int synctype) {
    switch (synctype) {
        case DSD_SYNC_DSTAR_VOICE_NEG:
        case DSD_SYNC_PROVOICE_NEG:
        case DSD_SYNC_DSTAR_HD_NEG:
        case DSD_SYNC_EDACS_NEG: return 1;
        default: break;
    }
    return 0;
}

static int
is_four_level_neg_synctype(int synctype) {
    switch (synctype) {
        case DSD_SYNC_P25P1_NEG:
        case DSD_SYNC_X2TDMA_VOICE_NEG:
        case DSD_SYNC_X2TDMA_DATA_NEG:
        case DSD_SYNC_M17_STR_NEG:
        case DSD_SYNC_DMR_BS_VOICE_NEG:
        case DSD_SYNC_DMR_BS_DATA_NEG:
        case DSD_SYNC_M17_LSF_NEG:
        case DSD_SYNC_NXDN_NEG:
        case DSD_SYNC_YSF_NEG:
        case DSD_SYNC_M17_BRT_NEG:
        case DSD_SYNC_M17_PKT_NEG:
        case DSD_SYNC_P25P2_NEG:
        case DSD_SYNC_M17_PRE_NEG:
        case DSD_SYNC_M17_EOT_NEG: return 1;
        default: break;
    }
    return 0;
}

static int DSD_ATTR_USED
store_two_level_dibit(dsd_state* state, float symbol, int high_symbol_return) {
    if (symbol > state->center) {
        *state->dibit_buf_p = 1;
        state->dibit_buf_p++;
        return high_symbol_return;
    }

    *state->dibit_buf_p = 3;
    state->dibit_buf_p++;
    return high_symbol_return ? 0 : 1;
}

static int DSD_ATTR_USED
want_cqpsk_p25_slice(const dsd_opts* opts, const dsd_state* state, int is_negative) {
    int p25p1_sync = is_negative ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;
    int p25p2_sync = is_negative ? DSD_SYNC_P25P2_NEG : DSD_SYNC_P25P2_POS;
#ifdef USE_RADIO
    return is_cqpsk_active(opts) && state->rf_mod == 1
           && (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || state->synctype == p25p1_sync
               || state->synctype == p25p2_sync || state->lastsynctype == p25p1_sync
               || state->lastsynctype == p25p2_sync);
#else
    UNUSED(opts);
    UNUSED(state);
    UNUSED(p25p1_sync);
    UNUSED(p25p2_sync);
    return 0;
#endif
}

static int DSD_ATTR_USED
slice_dibit_from_symbol_regions(const dsd_state* state, float symbol, int is_negative) {
    if (symbol > state->center) {
        if (symbol > state->umid) {
            return is_negative ? 3 : 1;
        }
        return is_negative ? 2 : 0;
    }

    if (symbol < state->lmid) {
        return is_negative ? 1 : 3;
    }
    return is_negative ? 0 : 2;
}

static int DSD_ATTR_USED
select_four_level_dibit(const dsd_opts* opts, const dsd_state* state, float symbol, int is_negative,
                        int* used_cqpsk_slice) {
    if (used_cqpsk_slice) {
        *used_cqpsk_slice = 0;
    }

#ifdef USE_RADIO
    if (want_cqpsk_p25_slice(opts, state, is_negative)) {
        int dibit = cqpsk_slice_aligned(symbol - state->center);
        if (used_cqpsk_slice) {
            *used_cqpsk_slice = 1;
        }
        debug_log_cqpsk_slice(dibit, symbol, state);
        return dibit;
    }
#else
    UNUSED(opts);
#endif

    return slice_dibit_from_symbol_regions(state, symbol, is_negative);
}

static void DSD_ATTR_USED
store_dibit_with_soft(dsd_state* state, int stored_dibit, const dsd_dibit_soft_t* soft) {
    *state->dibit_buf_p = stored_dibit;
    state->dibit_buf_p++;

    *state->dmr_payload_p = stored_dibit;
    if (state->dmr_reliab_p) {
        if (state->dmr_reliab_p > state->dmr_reliab_buf + 900000) {
            state->dmr_reliab_p = state->dmr_reliab_buf + 200;
        }
        *state->dmr_reliab_p = soft->reliability;
        state->dmr_reliab_p++;
    }
    write_dibit_soft_metric(state, soft);
    state->dmr_payload_p++;
}

int
digitize(const dsd_opts* opts, dsd_state* state, float symbol) {
    if (opts == NULL || state == NULL) {
        return -1;
    }

    if (is_two_level_pos_synctype(state->synctype)) {
        return store_two_level_dibit(state, symbol, 0);
    }
    if (is_two_level_neg_synctype(state->synctype)) {
        return store_two_level_dibit(state, symbol, 1);
    }

    int is_negative = is_four_level_neg_synctype(state->synctype);
    int used_cqpsk_slice = 0;
    int dibit = select_four_level_dibit(opts, state, symbol, is_negative, &used_cqpsk_slice);
    int stored_dibit = is_negative ? invert_dibit(dibit) : dibit;

    dsd_dibit_soft_t soft;
    compute_dibit_soft_metric(state, symbol, dibit, is_negative && !used_cqpsk_slice, used_cqpsk_slice, &soft);
    state->last_dibit = dibit;
    store_dibit_with_soft(state, stored_dibit, &soft);
    return dibit;
}

int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    if (opts == NULL || state == NULL) {
        return -1;
    }

    float symbol;
    int dibit;

#ifdef TRACE_DSD
    unsigned int l, r;
#endif

#ifdef TRACE_DSD
    l = state->debug_sample_index;
#endif

    symbol = getSymbol(opts, state, 1);

#ifdef TRACE_DSD
    r = state->debug_sample_index;
#endif

    state->sbuf[state->sidx] = symbol;

    if (out_analog_signal != NULL) {
        *out_analog_signal = (int)lrintf(symbol);
    }

    use_symbol(opts, state, symbol);

    dibit = digitize(opts, state, symbol);

    if (opts->audio_in_type == AUDIO_IN_SYMBOL_BIN) {
        //assign dibit from last symbol/dibit read from capture bin
        dibit = state->symbolc;
        replace_symbol_bin_soft_metric(state, dibit);
        throttle_symbol_bin_replay(opts, state);
    }

    dsd_dibit_soft_t capture_soft;
    const dsd_dibit_soft_t* capture_soft_p = read_previous_dibit_soft(state, &capture_soft) ? &capture_soft : NULL;
    write_symbol_capture_record_with_soft(opts, state, dibit, symbol, capture_soft_p);

#ifdef TRACE_DSD
    {
        if (state->debug_label_dibit_file == NULL) {
            state->debug_label_dibit_file = dsd_fopen_private("pp_label_dibit.txt", "w");
        }
        if (state->debug_label_dibit_file != NULL) {
            float left = l / 48000.0f;
            float right = r / 48000.0f;
            DSD_FPRINTF(state->debug_label_dibit_file, "%f\t%f\t%i\n", left, right, dibit);
        }
    }
#endif

    return dibit;
}

/**
 * \brief This important method reads the last analog signal value (getSymbol call) and digitizes it.
 * Depending of the ongoing transmission it in converted into a bit (0/1) or a di-bit (00/01/10/11).
 */
int
getDibit(dsd_opts* opts, dsd_state* state) {
    return get_dibit_and_analog_signal(opts, state, NULL);
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    int dibit = get_dibit_and_analog_signal(opts, state, NULL);

    if (out_soft != NULL) {
        if (!read_previous_dibit_soft(state, out_soft)) {
            uint8_t rel = 255;
            if (state && state->dmr_reliab_p != NULL && state->dmr_reliab_buf != NULL) {
                rel = *(state->dmr_reliab_p - 1);
            }
            fallback_soft_from_dibit(dibit, rel, out_soft);
        }
    }

    return dibit;
}

/**
 * \brief Get the next dibit along with its reliability value.
 *
 * This function reads the next dibit and returns the associated reliability
 * (0=uncertain, 255=confident) via out_reliability. The reliability is computed
 * based on the symbol's position relative to decision thresholds.
 *
 * @param opts Decoder options.
 * @param state Decoder state containing symbol buffers and thresholds.
 * @param out_reliability [out] Reliability value when non-NULL.
 * @return Dibit value [0,3]; negative on shutdown/EOF.
 */
int
getDibitWithReliability(dsd_opts* opts, dsd_state* state, uint8_t* out_reliability) {
    dsd_dibit_soft_t soft;
    int dibit = getDibitSoft(opts, state, &soft);

    if (out_reliability != NULL) {
        *out_reliability = soft.reliability;
    }

    return dibit;
}

/**
 * \brief Get the next dibit and store the soft symbol for Viterbi decoding.
 *
 * This function reads the next dibit while also recording the raw float
 * symbol value in state->soft_symbol_buf for soft-decision FEC.
 */
int
getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol) {
    if (opts == NULL || state == NULL) {
        return -1;
    }

    float symbol;
    int dibit;

    symbol = getSymbol(opts, state, 1);

    // Store soft symbol in ring buffer
    state->soft_symbol_buf[state->soft_symbol_head] = symbol;
    state->soft_symbol_head = (state->soft_symbol_head + 1) & 511; // Wrap at 512

    state->sbuf[state->sidx] = symbol;
    use_symbol(opts, state, symbol);
    dibit = digitize(opts, state, symbol);

    if (opts->audio_in_type == AUDIO_IN_SYMBOL_BIN) {
        dibit = state->symbolc;
        replace_symbol_bin_soft_metric(state, dibit);
        throttle_symbol_bin_replay(opts, state);
    }

    dsd_dibit_soft_t capture_soft;
    const dsd_dibit_soft_t* capture_soft_p = read_previous_dibit_soft(state, &capture_soft) ? &capture_soft : NULL;
    write_symbol_capture_record_with_soft(opts, state, dibit, symbol, capture_soft_p);

    if (out_soft_symbol != NULL) {
        *out_soft_symbol = symbol;
    }

    return dibit;
}

/**
 * \brief Mark the start of a new frame for soft symbol collection.
 */
void
soft_symbol_frame_begin(dsd_state* state) {
    state->soft_symbol_frame_start = state->soft_symbol_head;
}

/**
 * \brief Map log-likelihood ratio proxy to 16-bit Viterbi metric.
 *
 * This decoder family expects costs in [0, 65535], where:
 *   - 0      = strong bit 0
 *   - 65535  = strong bit 1
 *   - ~32768 = uncertain
 */
static inline uint16_t
llr_to_viterbi_cost(float llr_log_p0_over_p1) {
    /* Clamp LLR to keep expf numerically stable and preserve monotonicity. */
    if (llr_log_p0_over_p1 >= 16.0f) {
        return 0;
    }
    if (llr_log_p0_over_p1 <= -16.0f) {
        return 65535;
    }
    float p1 = 1.0f / (1.0f + expf(llr_log_p0_over_p1));
    long q = lrintf(p1 * 65535.0f);
    if (q < 0L) {
        q = 0L;
    }
    if (q > 65535L) {
        q = 65535L;
    }
    return (uint16_t)q;
}

static inline float
min_sq_dist2(float x, float a, float b) {
    float da = x - a;
    float db = x - b;
    float d2a = da * da;
    float d2b = db * db;
    return (d2a < d2b) ? d2a : d2b;
}

/**
 * \brief Convert a soft 4-level symbol to a Viterbi cost metric.
 *
 * For 4-level modulations (C4FM, GFSK, QPSK), each dibit encodes 2 bits.
 * We use a max-log MAP proxy:
 *   LLR ~= (d1^2 - d0^2) / (2*sigma^2)
 * where d0/d1 are distances to nearest ideal levels for each bit hypothesis.
 *
 * This is more mathematically consistent than direct linear confidence maps,
 * while preserving the decoder's expected [0..65535] metric domain.
 */
uint16_t
soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state, int bit_position) {
    if (!state) {
        return 32768U;
    }

    float min_val = state->min;
    float lmid = state->lmid;
    float center = state->center;
    float umid = state->umid;
    float max_val = state->max;

    /* Guard against degenerate/unsorted thresholds by synthesizing a symmetric set. */
    if (!(min_val < lmid && lmid < center && center < umid && umid < max_val)) {
        float span = max_val - min_val;
        if (span < 1e-3f) {
            span = 2.0f;
        }
        float half = span * 0.5f;
        min_val = center - half;
        max_val = center + half;
        lmid = center - (span / 6.0f);
        umid = center + (span / 6.0f);
    }

    /* Estimate ideal 4-PAM levels from decision-region midpoints. */
    float n3 = 0.5f * (min_val + lmid);
    float n1 = 0.5f * (lmid + center);
    float p1 = 0.5f * (center + umid);
    float p3 = 0.5f * (umid + max_val);

    /* Noise scale proxy: 4-PAM span is roughly 6*sigma in this normalized model. */
    float sigma = (max_val - min_val) / 6.0f;
    if (sigma < 1e-3f) {
        sigma = 1e-3f;
    }
    float inv_2sigma2 = 0.5f / (sigma * sigma);

    bit_position &= 1;
    float d0 = 0.0f;
    float d1 = 0.0f;
    if (bit_position == 0) {
        /* MSB: positive cluster (0) vs negative cluster (1). */
        d0 = min_sq_dist2(symbol, p1, p3);
        d1 = min_sq_dist2(symbol, n1, n3);
    } else {
        /* LSB: inner levels (0) vs outer levels (1). */
        d0 = min_sq_dist2(symbol, n1, p1);
        d1 = min_sq_dist2(symbol, n3, p3);
    }

    float llr = (d1 - d0) * inv_2sigma2; /* log(P(bit=0)/P(bit=1)) proxy */
    return llr_to_viterbi_cost(llr);
}

/**
 * GMSK (binary) soft symbol to Viterbi cost.
 *
 * For GMSK modulation where each symbol represents a single bit.
 * Uses the same max-log MAP proxy from distances to the two class means.
 */
uint16_t
gmsk_soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state) {
    if (!state) {
        return 32768U;
    }

    float center = state->center;
    float min_val = state->min;
    float max_val = state->max;
    if (!(min_val < center && center < max_val)) {
        float span = max_val - min_val;
        if (span < 1e-3f) {
            span = 2.0f;
        }
        float half = span * 0.5f;
        min_val = center - half;
        max_val = center + half;
    }

    float mu0 = 0.5f * (min_val + center); /* bit 0 hypothesis */
    float mu1 = 0.5f * (center + max_val); /* bit 1 hypothesis */
    float sigma = (max_val - min_val) / 4.0f;
    if (sigma < 1e-3f) {
        sigma = 1e-3f;
    }
    float inv_2sigma2 = 0.5f / (sigma * sigma);

    float d0 = symbol - mu0;
    float d1 = symbol - mu1;
    float llr = ((d1 * d1) - (d0 * d0)) * inv_2sigma2;
    return llr_to_viterbi_cost(llr);
}

void
skipDibit(dsd_opts* opts, dsd_state* state, int count) {

    int i;
    for (i = 0; i < (count); i++) {
        (void)getDibit(opts, state);
    }
}
