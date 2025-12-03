// SPDX-License-Identifier: ISC
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

#include <assert.h>
#include <math.h>

#include <dsd-neo/core/dsd.h>
#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

#include <dsd-neo/protocol/p25/p25p1_heuristics.h>

static void
print_datascope(dsd_opts* opts, dsd_state* state, const float* sbuf2, int count) {
    int i, j, o;
    char modulation[8];
    int spectrum[64];

    if (state->rf_mod == 0) {
        snprintf(modulation, sizeof modulation, "C4FM");
    } else if (state->rf_mod == 1) {
        snprintf(modulation, sizeof modulation, "QPSK");
    } else if (state->rf_mod == 2) {
        snprintf(modulation, sizeof modulation, "GFSK");
    }

    for (i = 0; i < 64; i++) {
        spectrum[i] = 0;
    }
    float span = fmaxf(fabsf(state->max), fabsf(state->min));
    if (span < 1e-3f) {
        span = 1.0f;
    }
    const float scale = 32.0f / span;
    for (i = 0; i < count; i++) {
        float v = sbuf2[i];
        o = (int)lrintf((v * scale) + 32.0f);
        if (o < 0) {
            o = 0;
        }
        if (o > 63) {
            o = 63;
        }
        spectrum[o]++;
    }
    if (state->symbolcnt > (4800 / opts->scoperate)) {
        state->symbolcnt = 0;
        fprintf(stderr, "\n");
        fprintf(stderr, "Demod mode:     %s                Nac:                     %4X\n", modulation, state->nac);
        fprintf(stderr, "Frame Type:    %s        Talkgroup:            %7i\n", state->ftype, state->lasttg);
        fprintf(stderr, "Frame Subtype: %s       Source:          %12i\n", state->fsubtype, state->lastsrc);
        fprintf(stderr, "TDMA activity:  %s %s     Voice errors: %s\n", state->slot0light, state->slot1light,
                state->err_str);
        fprintf(stderr, "+----------------------------------------------------------------+\n");
        int bin_min = (int)lrintf(state->min * scale + 32.0f);
        int bin_max = (int)lrintf(state->max * scale + 32.0f);
        int bin_lmid = (int)lrintf(state->lmid * scale + 32.0f);
        int bin_umid = (int)lrintf(state->umid * scale + 32.0f);
        int bin_center = (int)lrintf(state->center * scale + 32.0f);
        int bins[] = {bin_min, bin_max, bin_lmid, bin_umid, bin_center};
        for (size_t b = 0; b < sizeof(bins) / sizeof(bins[0]); b++) {
            if (bins[b] < 0) {
                bins[b] = 0;
            }
            if (bins[b] > 63) {
                bins[b] = 63;
            }
        }
        for (i = 0; i < 10; i++) {
            printf("|");
            for (j = 0; j < 64; j++) {
                if (i == 0) {
                    if (j == bins[0] || j == bins[1]) {
                        fprintf(stderr, "#");
                    } else if (j == bins[2] || j == bins[3]) {
                        fprintf(stderr, "^");
                    } else if (j == bins[4]) {
                        fprintf(stderr, "!");
                    } else {
                        if (j == 32) {
                            fprintf(stderr, "|");
                        } else {
                            fprintf(stderr, " ");
                        }
                    }
                } else {
                    if (spectrum[j] > 9 - i) {
                        fprintf(stderr, "*");
                    } else {
                        if (j == 32) {
                            fprintf(stderr, "|");
                        } else {
                            fprintf(stderr, " ");
                        }
                    }
                }
            }
            fprintf(stderr, "|\n");
        }
        fprintf(stderr, "+----------------------------------------------------------------+\n");
    }
}

static void
use_symbol(dsd_opts* opts, dsd_state* state, float symbol) {
    UNUSED(symbol);

    int i;
    float sbuf2[128];
    float lmin, lmax, lsum;

    int cap = opts->ssize;
    if (cap < 0) {
        cap = 0;
    }
    if (cap > (int)(sizeof(sbuf2) / sizeof(sbuf2[0]))) {
        cap = (int)(sizeof(sbuf2) / sizeof(sbuf2[0]));
    }
    for (i = 0; i < cap; i++) {
        sbuf2[i] = state->sbuf[i];
    }

    qsort(sbuf2, (size_t)cap, sizeof(float), comp);

    // Continuous update of min/max
    // - QPSK: always (as before)
    // - C4FM: enable for P25 Phase 1 (+/-) to keep slicer thresholds fresh during calls
    if (state->rf_mod == 1 || (state->rf_mod == 0 && (state->lastsynctype == 0 || state->lastsynctype == 1))) {
        if (cap >= 2) {
            lmin = (sbuf2[0] + sbuf2[1]) / 2.0f;
            lmax = (sbuf2[(cap - 1)] + sbuf2[(cap - 2)]) / 2.0f;
        } else {
            lmin = lmax = 0.0f;
        }
        state->minbuf[state->midx] = lmin;
        state->maxbuf[state->midx] = lmax;
        if (state->midx == (opts->msize - 1)) {
            state->midx = 0;
        } else {
            state->midx++;
        }
        lsum = 0.0f;
        for (i = 0; i < opts->msize; i++) {
            lsum += state->minbuf[i];
        }
        state->min = lsum / (float)opts->msize;
        lsum = 0.0f;
        for (i = 0; i < opts->msize; i++) {
            lsum += state->maxbuf[i];
        }
        state->max = lsum / (float)opts->msize;
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
            print_datascope(opts, state, sbuf2, cap);
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
    }

    // Error, shouldn't be here
    assert(0);
    return -1;
}

static inline int
apply_p25_cqpsk_map(const dsd_state* state, int dibit) {
    if (!state || state->rf_mod != 1) {
        return dibit;
    }
    int idx = dibit & 0x3;
    if (idx < 0 || idx > 3) {
        return dibit;
    }
    return (int)(state->p25_cqpsk_map[idx] & 0x3);
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
static inline int
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
static inline int
cqpsk_slice_aligned(float symbol) {
    static int init = 0;
    static int inv = 0;
    static int negate = 0;
    if (!init) {
        const char* e_inv = getenv("DSD_NEO_CQPSK_SYNC_INV");
        const char* e_neg = getenv("DSD_NEO_CQPSK_SYNC_NEG");
        if (e_inv && (*e_inv == '1' || *e_inv == 'y' || *e_inv == 'Y' || *e_inv == 't' || *e_inv == 'T')) {
            inv = 1;
        }
        if (e_neg && (*e_neg == '1' || *e_neg == 'y' || *e_neg == 'Y' || *e_neg == 't' || *e_neg == 'T')) {
            negate = 1;
        }
        init = 1;
    }
    float s = negate ? -symbol : symbol;
    int raw_dibit = cqpsk_slice(s);
    if (inv) {
        raw_dibit = invert_dibit(raw_dibit);
    }
    return raw_dibit;
}

static inline uint8_t
dmr_compute_reliability(const dsd_state* st, float sym) {
    const float eps = 1e-6f;
    float min = st->min, max = st->max, lmid = st->lmid, center = st->center, umid = st->umid;
    int rel = 0;
    if (sym > umid) {
        float span = max - umid;
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
        rel = (int)lrintf((m * 510.0f) / span); // scale to 0..255 (2x since max margin is span/2)
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
        float span = lmid - min;
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

    // Refine using demod SNR when available from RTL stream: scale reliability
    // into [~0.8x, ~1.2x] based on SNR in a coarse [ -5dB .. +20dB ] window.
#ifdef USE_RTLSDR
    double snr_db = rtl_stream_get_snr_c4fm();
    if (snr_db < -50.0) {
        // fallback estimate when smooth SNR not available
        snr_db = rtl_stream_estimate_snr_c4fm_eye();
    }
    // Map unbiased C4FM SNR [-13, 12] dB to [0, 255] (shifted from [-5,20])
    int w256 = 0;
    if (snr_db > -13.0) {
        if (snr_db >= 12.0) {
            w256 = 255;
        } else {
            double w = (snr_db + 13.0) / 25.0; // 0..1
            if (w < 0.0) {
                w = 0.0;
            }
            if (w > 1.0) {
                w = 1.0;
            }
            w256 = (int)(w * 255.0 + 0.5);
        }
    }
    // Base scale 0.8 (204/256) + up to +0.25 (64/256) with SNR
    int scale_num = 204 + (w256 >> 2); // 204..(204+63)=267
    int scaled = (rel * scale_num) >> 8;
    if (scaled > 255) {
        scaled = 255;
    }
    if (scaled < 0) {
        scaled = 0;
    }
    rel = scaled;
#endif
    return (uint8_t)rel;
}

/**
 * @brief Check if CQPSK demodulation path is active.
 *
 * Returns 1 if the RTL-SDR CQPSK path with TED is active, meaning symbols
 * are pre-scaled phase values suitable for the CQPSK slicer.
 *
 * @param opts Decoder options (checks audio_in_type).
 * @return 1 if CQPSK active, 0 otherwise.
 */
static inline int
is_cqpsk_active(dsd_opts* opts) {
#ifdef USE_RTLSDR
    if (opts && opts->audio_in_type == 3) {
        int cqpsk = 0, fll = 0, ted = 0;
        rtl_stream_dsp_get(&cqpsk, &fll, &ted);
        if (cqpsk && ted) {
            return 1;
        }
    }
#else
    UNUSED(opts);
#endif
    return 0;
}

#ifdef USE_RTLSDR
/* Optional histogram of CQPSK slicer output during decoding. */
static void
debug_log_cqpsk_slice(int dibit, float symbol, const dsd_state* state) {
    static int init = 0;
    static int enabled = 0;
    static int hist[4] = {0, 0, 0, 0};
    static int sample_count = 0;
    static float sym_min = 1e9f, sym_max = -1e9f, sym_sum = 0.0f;
    (void)state;

    if (!init) {
        const char* env = getenv("DSD_NEO_DEBUG_CQPSK");
        enabled = (env && *env == '1') ? 1 : 0;
        init = 1;
    }
    if (!enabled) {
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
        fprintf(stderr, "[SLICE-DECODE] d0:%.1f%% d1:%.1f%% d2:%.1f%% d3:%.1f%% avg:%.2f range:[%.2f,%.2f] (n=%d)\n",
                100.0f * hist[0] / n, 100.0f * hist[1] / n, 100.0f * hist[2] / n, 100.0f * hist[3] / n, avg, sym_min,
                sym_max, sample_count);
        hist[0] = hist[1] = hist[2] = hist[3] = 0;
        sample_count = 0;
        sym_sum = 0.0f;
        sym_min = 1e9f;
        sym_max = -1e9f;
    }
}
#else
static inline void
debug_log_cqpsk_slice(int dibit, float symbol, const dsd_state* state) {
    UNUSED3(dibit, symbol, state);
}
#endif

int
digitize(dsd_opts* opts, dsd_state* state, float symbol) {
    // determine dibit state
    if ((state->synctype == 6) || (state->synctype == 14) || (state->synctype == 18) || (state->synctype == 37))

    {
        //  6 +D-STAR
        // 14 +ProVoice
        // 18 +D-STAR_HD
        // 37 +EDACS

        if (symbol > state->center) {
            *state->dibit_buf_p = 1;
            state->dibit_buf_p++;
            return (0); // +1
        } else {
            *state->dibit_buf_p = 3;
            state->dibit_buf_p++;
            return (1); // +3
        }
    } else if ((state->synctype == 7) || (state->synctype == 15) || (state->synctype == 19)
               || (state->synctype == 38)) {
        //  7 -D-STAR
        // 15 -ProVoice
        // 19 -D-STAR_HD
        // 38 -EDACS

        if (symbol > state->center) {
            *state->dibit_buf_p = 1;
            state->dibit_buf_p++;
            return (1); // +3
        } else {
            *state->dibit_buf_p = 3;
            state->dibit_buf_p++;
            return (0); // +1
        }
    }

    else if ((state->synctype == 1) || (state->synctype == 3) || (state->synctype == 5) || (state->synctype == 9)
             || (state->synctype == 11) || (state->synctype == 13) || (state->synctype == 17) || (state->synctype == 29)
             || (state->synctype == 31) || (state->synctype == 77) || (state->synctype == 87) || (state->synctype == 36)
             || (state->synctype == 99))

    {
        //  1 -P25p1
        //  3 -X2-TDMA (inverted signal voice frame)
        //  5 -X2-TDMA (inverted signal data frame)
        //  9 -M17 LSR
        // 11 -DMR (inverted signal voice frame)
        // 13 -DMR (inverted signal data frame)
        // 17 -M17 STR
        // 29 -NXDN (inverted FSW)
        // 31 -YSF
        // 36 -P25p2
        // 77 -M17 BRT
        // 87 -M17 PKT
        // 99 -M17 Preamble

        int valid;
        int dibit;

        valid = 0;
        /* Prefer the fixed CQPSK slicer whenever the CQPSK DSP path is active and
         * we are hunting/decoding P25 (Phase 1 or 2). This keeps the sync search
         * aligned even before synctype is fully resolved. */
        int want_cqpsk_slice = is_cqpsk_active(opts) && state->rf_mod == 1
                               && (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || state->synctype == 1
                                   || state->synctype == 36 || state->lastsynctype == 1 || state->lastsynctype == 36);
        if (want_cqpsk_slice) {
            float sym = symbol - state->center; /* remove DC bias before fixed-threshold slice */
            dibit = cqpsk_slice_aligned(sym);
            valid = 1;
            debug_log_cqpsk_slice(dibit, symbol, state);
        }

        //testing again, either on Voice channels only (when tuned) or with trunk disabled
        // if (state->synctype == 1 && (opts->p25_is_tuned == 1 || opts->p25_trunk == 0) && opts->use_heuristics == 1)
        if (valid == 0 && state->synctype == 1 && opts->use_heuristics == 1) {
            // Use the P25p1 heuristics if available
            valid = estimate_symbol(state->rf_mod, &(state->inv_p25_heuristics), state->last_dibit, symbol, &dibit);
        }

        if (valid == 0) {
            // Revert to the original approach: choose the symbol according to the regions delimited
            // by center, umid and lmid
            if (symbol > state->center) {
                if (symbol > state->umid) {
                    dibit = 3; // -3
                } else {
                    dibit = 2; // -1
                }
            } else {
                if (symbol < state->lmid) {
                    dibit = 1; // +3
                } else {
                    dibit = 0; // +1
                }
            }
        }

        dibit = apply_p25_cqpsk_map(state, dibit);
        int out_dibit = invert_dibit(dibit);

        state->last_dibit = dibit;

        *state->dibit_buf_p = out_dibit;
        state->dibit_buf_p++;

        //dmr buffer
        *state->dmr_payload_p = out_dibit;
        if (state->dmr_reliab_p) {
            if (state->dmr_reliab_p > state->dmr_reliab_buf + 900000) {
                state->dmr_reliab_p = state->dmr_reliab_buf + 200;
            }
            *state->dmr_reliab_p = dmr_compute_reliability(state, symbol);
            state->dmr_reliab_p++;
        }
        state->dmr_payload_p++;
        //dmr buffer end

        return dibit;
    } else {
        //  0 +P25p1
        //  2 +X2-TDMA (non inverted signal data frame)
        //  4 +X2-TDMA (non inverted signal voice frame)
        //  8 +M17 LSF
        // 10 +DMR (non inverted signal data frame)
        // 12 +DMR (non inverted signal voice frame)
        // 16 +M17 STR
        // 28 +NXND (FSW)
        // 30 +YSF
        // 35 +p25p2
        // 76 +M17 BRT
        // 86 +M17 PKT
        // 98 +M17 Preamble

        int valid;
        int dibit;

        valid = 0;
        /* Prefer the fixed CQPSK slicer whenever the CQPSK DSP path is active and
         * we are hunting/decoding P25 (Phase 1 or 2). This keeps the sync search
         * aligned even before synctype is fully resolved. */
        int want_cqpsk_slice = is_cqpsk_active(opts) && state->rf_mod == 1
                               && (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || state->synctype == 0
                                   || state->synctype == 35 || state->lastsynctype == 0 || state->lastsynctype == 35);
        if (want_cqpsk_slice) {
            float sym = symbol - state->center; /* remove DC bias before fixed-threshold slice */
            dibit = cqpsk_slice_aligned(sym);
            valid = 1;
            debug_log_cqpsk_slice(dibit, symbol, state);
        }

        //testing again, either on Voice channels only (when tuned) or with trunk disabled
        // if (state->synctype == 0 && (opts->p25_is_tuned == 1 || opts->p25_trunk == 0) && opts->use_heuristics == 1)
        if (valid == 0 && state->synctype == 0 && opts->use_heuristics == 1) {
            // Use the P25p1 heuristics if available
            valid = estimate_symbol(state->rf_mod, &(state->p25_heuristics), state->last_dibit, symbol, &dibit);
        }

        if (valid == 0) {
            // Revert to the original approach: choose the symbol according to the regions delimited
            // by center, umid and lmid
            if (symbol > state->center) {
                if (symbol > state->umid) {
                    dibit = 1; // +3
                } else {
                    dibit = 0; // +1
                }
            } else {
                if (symbol < state->lmid) {
                    dibit = 3; // -3
                } else {
                    dibit = 2; // -1
                }
            }
        }

        dibit = apply_p25_cqpsk_map(state, dibit);

        state->last_dibit = dibit;

        *state->dibit_buf_p = dibit;
        state->dibit_buf_p++;

        //dmr buffer
        //note to self, perceived bug with initial dibit buffer appears to be caused by
        //media player, when playing back from audacity, the initial few dmr frames are
        //decoded properly, need to investigate the root cause of what audacity is doing
        //vs other audio sources...perhaps just the audio level itself?
        *state->dmr_payload_p = dibit;
        if (state->dmr_reliab_p) {
            if (state->dmr_reliab_p > state->dmr_reliab_buf + 900000) {
                state->dmr_reliab_p = state->dmr_reliab_buf + 200;
            }
            *state->dmr_reliab_p = dmr_compute_reliability(state, symbol);
            state->dmr_reliab_p++;
        }
        state->dmr_payload_p++;
        //dmr buffer end

        return dibit;
    }
}

int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
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

    if (opts->audio_in_type == 4) {
        //assign dibit from last symbol/dibit read from capture bin
        dibit = state->symbolc;
        if (state->use_throttle == 1) {
            usleep(0);
        }
    }

    //symbol/dibit file capture/writing
    if (opts->symbol_out_f) {
        //fprintf (stderr, "%d", dibit);
        fputc(dibit, opts->symbol_out_f);
    }

#ifdef TRACE_DSD
    {
        float left, right;
        if (state->debug_label_dibit_file == NULL) {
            state->debug_label_dibit_file = fopen("pp_label_dibit.txt", "w");
        }
        left = l / 48000.0;
        right = r / 48000.0;
        fprintf(state->debug_label_dibit_file, "%f\t%f\t%i\n", left, right, dibit);
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
    int dibit = get_dibit_and_analog_signal(opts, state, NULL);

    if (out_reliability != NULL) {
        /* Reliability was written at dmr_reliab_p - 1 (pointer already advanced).
         * We read from there rather than caching the pointer before the call,
         * because get_dibit_and_analog_signal() may wrap dmr_reliab_p back to
         * dmr_reliab_buf + 200 when it exceeds 900000 elements. */
        if (state->dmr_reliab_p != NULL && state->dmr_reliab_buf != NULL) {
            *out_reliability = *(state->dmr_reliab_p - 1);
        } else {
            /* Fallback: max reliability if buffer not available */
            *out_reliability = 255;
        }
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
    float symbol;
    int dibit;

    symbol = getSymbol(opts, state, 1);

    // Store soft symbol in ring buffer
    state->soft_symbol_buf[state->soft_symbol_head] = symbol;
    state->soft_symbol_head = (state->soft_symbol_head + 1) & 511; // Wrap at 512

    state->sbuf[state->sidx] = symbol;
    use_symbol(opts, state, symbol);
    dibit = digitize(opts, state, symbol);

    if (opts->audio_in_type == 4) {
        dibit = state->symbolc;
        if (state->use_throttle == 1) {
            usleep(0);
        }
    }

    if (opts->symbol_out_f) {
        fputc(dibit, opts->symbol_out_f);
    }

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
 * \brief Convert a soft symbol to Viterbi cost metric.
 *
 * For 4-level modulations (C4FM, GFSK, QPSK), each dibit encodes 2 bits.
 * The MSB (bit_position=0) determines upper/lower half (+3/+1 vs -1/-3).
 * The LSB (bit_position=1) determines inner/outer level (+3/-3 vs +1/-1).
 *
 * Returns 0x0000 for confident '0', 0xFFFF for confident '1', ~0x7FFF for uncertain.
 *
 * M17 uses GFSK with dibit mapping: 01->+3, 00->+1, 10->-1, 11->-3
 * So for M17: MSB=0 means positive (>center), MSB=1 means negative (<center)
 *             LSB=0 means inner (±1), LSB=1 means outer (±3)
 */
uint16_t
soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state, int bit_position) {
    float center = state->center;
    float umid = state->umid;
    float lmid = state->lmid;
    float max_val = state->max;
    float min_val = state->min;
    float confidence;
    int bit_value;

    // Compute span for normalization, guard against zero
    float span = max_val - min_val;
    if (span < 1e-6f) {
        span = 1.0f;
    }

    if (bit_position == 0) {
        // MSB: determines if symbol is in upper half (0) or lower half (1)
        // For M17 GFSK: positive symbols -> bit 0, negative -> bit 1
        if (symbol > center) {
            bit_value = 0;
            // Confidence based on distance from center toward max
            confidence = (symbol - center) / (max_val - center + 1e-6f);
        } else {
            bit_value = 1;
            // Confidence based on distance from center toward min
            confidence = (center - symbol) / (center - min_val + 1e-6f);
        }
    } else {
        // LSB: determines inner (0) or outer (1) level
        // For M17 GFSK: ±1 -> bit 0, ±3 -> bit 1
        float abs_sym = fabsf(symbol - center);
        float mid_threshold = (fabsf(umid - center) + fabsf(lmid - center)) / 2.0f;

        if (abs_sym < mid_threshold) {
            // Inner level (±1)
            bit_value = 0;
            confidence = (mid_threshold - abs_sym) / (mid_threshold + 1e-6f);
        } else {
            // Outer level (±3)
            bit_value = 1;
            confidence = (abs_sym - mid_threshold) / (span / 2.0f - mid_threshold + 1e-6f);
        }
    }

    // Clamp confidence to [0, 1]
    if (confidence < 0.0f) {
        confidence = 0.0f;
    }
    if (confidence > 1.0f) {
        confidence = 1.0f;
    }

    // Map to Viterbi cost:
    // bit_value=0: confident -> 0x0000, uncertain -> 0x7FFF
    // bit_value=1: confident -> 0xFFFF, uncertain -> 0x7FFF
    uint16_t cost;
    if (bit_value == 0) {
        // Strong 0 = 0x0000, weak 0 = 0x7FFF
        cost = (uint16_t)((1.0f - confidence) * 32767.0f);
    } else {
        // Strong 1 = 0xFFFF, weak 1 = 0x7FFF
        cost = (uint16_t)(32767.0f + confidence * 32768.0f);
    }

    return cost;
}

/**
 * GMSK (binary) soft symbol to Viterbi cost.
 *
 * For GMSK modulation where each symbol represents a single bit.
 * D-STAR uses GMSK where: symbol > center -> bit 1, symbol < center -> bit 0
 * Distance from center indicates confidence in the decision.
 */
uint16_t
gmsk_soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state) {
    float center = state->center;
    float max_val = state->max;
    float min_val = state->min;
    float confidence;
    int bit_value;

    float upper_span = max_val - center;
    if (upper_span < 1e-6f) {
        upper_span = 1e-6f;
    }
    float lower_span = center - min_val;
    if (lower_span < 1e-6f) {
        lower_span = 1e-6f;
    }

    // GMSK: above center = 1, below center = 0
    if (symbol > center) {
        bit_value = 1;
        // Confidence based on distance from center toward max
        confidence = (symbol - center) / upper_span;
    } else {
        bit_value = 0;
        // Confidence based on distance from center toward min
        confidence = (center - symbol) / lower_span;
    }

    // Clamp confidence to [0, 1]
    if (confidence < 0.0f) {
        confidence = 0.0f;
    }
    if (confidence > 1.0f) {
        confidence = 1.0f;
    }

    // Map to Viterbi cost:
    // bit_value=0: confident -> 0x0000, uncertain -> 0x7FFF
    // bit_value=1: confident -> 0xFFFF, uncertain -> 0x7FFF
    uint16_t cost;
    if (bit_value == 0) {
        // Strong 0 = 0x0000, weak 0 = 0x7FFF
        cost = (uint16_t)((1.0f - confidence) * 32767.0f);
    } else {
        // Strong 1 = 0xFFFF, weak 1 = 0x7FFF
        cost = (uint16_t)(32767.0f + confidence * 32768.0f);
    }

    return cost;
}

void
skipDibit(dsd_opts* opts, dsd_state* state, int count) {

    int i;
    for (i = 0; i < (count); i++) {
        (void)getDibit(opts, state);
    }
}
