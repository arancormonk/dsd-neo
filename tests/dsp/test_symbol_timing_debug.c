// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The symbol-timing diagnostic has to report the sub-symbol phase the decoder's
 * grid settled on, because that phase is fixed at sync acquisition and held for
 * the whole call -- under AUTO it starts wherever the previous profile left off,
 * which is the mechanism #404 could only measure with a throwaway build.
 *
 * These cases drive the measurement over a synthetic four-level stream whose true
 * symbol boundaries are known, once per sub-symbol phase the grid could have
 * started on, and require the reported offset to be exactly the phase injected.
 *
 * Symbol transitions are ramped rather than square, as a filtered stream's are, so
 * an off-centre window reads a blend of two symbols; that is what makes one
 * alignment score highest instead of a plateau of equally good ones.
 *
 * The spans the measurement walks are deliberately irregular in two cases: the
 * pre-sync timing nudge makes symbols consume samplesPerSymbol +/- 1, and a
 * measurement that assumed a fixed stride would smear across a sync word.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/symbol_timing_debug.h>

#define MAX_SYMS    32
#define MAX_SAMPLES 4096

/* Leading symbols exist only to give the measurement room to shift the template
   earlier; the template itself is the trailing DSD_SYMBOL_TIMING_TEMPLATE_SYMS. */
#define LEAD_SYMS   4

static float g_waveform[MAX_SAMPLES];
static int g_waveform_len;

static float
level_for_dibit(char dibit) {
    switch (dibit) {
        case '0': return 8000.0f;
        case '1': return 24000.0f;
        case '2': return -8000.0f;
        case '3': return -24000.0f;
        default: break;
    }
    return 0.0f;
}

/*
 * Build a waveform whose symbol k occupies spans[k] samples, then smooth it with a
 * boxcar so each boundary becomes a ramp. Variable spans are the point: they let a
 * case describe a grid that nudged, and the measurement is handed the same spans.
 */
static void
build_waveform(const char* dibits, const uint8_t* spans, int count, int ramp_samples) {
    static float nrz[MAX_SAMPLES];
    int n = 0;

    for (int k = 0; k < count; k++) {
        const float level = level_for_dibit(dibits[k]);
        for (int s = 0; s < (int)spans[k]; s++) {
            assert(n < MAX_SAMPLES);
            nrz[n++] = level;
        }
    }

    for (int i = 0; i < n; i++) {
        float sum = 0.0f;
        for (int k = 0; k < ramp_samples; k++) {
            int idx = i + k - (ramp_samples / 2);
            if (idx < 0) {
                idx = 0;
            }
            if (idx >= n) {
                idx = n - 1;
            }
            sum += nrz[idx];
        }
        g_waveform[i] = sum / (float)ramp_samples;
    }
    g_waveform_len = n;
}

/*
 * Measure a grid that started `phase` samples after the true symbol boundaries and
 * then consumed exactly `spans`. The reported offset is how far back the template
 * had to move to fit, which is the phase that was injected.
 */
static int
measure_with_phase(const char* dibits, const uint8_t* spans, int count, int phase, int* out_offset) {
    int total = 0;
    for (int k = 0; k < count; k++) {
        total += (int)spans[k];
    }
    /* The grid reads `phase` samples late, so it needs that many beyond the last symbol. */
    assert(phase + total <= g_waveform_len);

    const int template_syms = DSD_SYMBOL_TIMING_TEMPLATE_SYMS;
    assert(count > template_syms);

    return dsd_symbol_timing_measure_sync_offset(&g_waveform[phase], total, &spans[count - template_syms],
                                                 template_syms, &dibits[count - template_syms], out_offset);
}

/* Report which case failed before tripping the assert: every case runs the same
   measurement, so the bare assertion would not say which phase went wrong. */
static void
expect_offset(int ok, int offset, int expected, const char* label, int sps) {
    if (ok && offset == expected) {
        return;
    }
    DSD_FPRINTF(stderr, "%s: sps=%i expected offset %i, got ok=%i offset=%i\n", label, sps, expected, ok, offset);
    assert(0);
}

/* A sync-like run of outer symbols, as the sync search's binary slicer produces. */
static const char kDibits[] = "13311313113313311331131311331131";

static void
fill_uniform_spans(uint8_t* spans, int count, int sps) {
    for (int k = 0; k < count; k++) {
        spans[k] = (uint8_t)sps;
    }
}

static void
test_uniform_spans_recover_phase(int sps) {
    const int count = LEAD_SYMS + DSD_SYMBOL_TIMING_TEMPLATE_SYMS;
    uint8_t spans[MAX_SYMS];
    fill_uniform_spans(spans, count, sps);

    /* One extra symbol of waveform so a late grid still has samples to read. */
    uint8_t build_spans[MAX_SYMS];
    fill_uniform_spans(build_spans, count + 1, sps);
    build_waveform(kDibits, build_spans, count + 1, sps / 4 > 1 ? sps / 4 : 2);

    /* Past half a symbol the slicer would decide the next symbol instead, so the
       phase stops being this template's to report. */
    for (int phase = 0; phase <= sps / 2; phase++) {
        int offset = -1;
        const int ok = measure_with_phase(kDibits, spans, count, phase, &offset);
        expect_offset(ok, offset, phase, "uniform spans", sps);
    }
}

static void
test_nudged_spans_recover_phase(void) {
    const int sps = 20;
    const int count = LEAD_SYMS + DSD_SYMBOL_TIMING_TEMPLATE_SYMS;
    uint8_t spans[MAX_SYMS];
    uint8_t build_spans[MAX_SYMS];

    /* The nudge moves a symbol by one sample either way; a fixed-stride correlation
       would walk off the boundaries by the end of the template. */
    for (int k = 0; k < count + 1; k++) {
        int span = sps;
        if ((k % 3) == 1) {
            span = sps - 1;
        } else if ((k % 3) == 2) {
            span = sps + 1;
        }
        build_spans[k] = (uint8_t)span;
        if (k < count) {
            spans[k] = (uint8_t)span;
        }
    }
    build_waveform(kDibits, build_spans, count + 1, sps / 4);

    for (int phase = 0; phase <= sps / 2; phase++) {
        int offset = -1;
        const int ok = measure_with_phase(kDibits, spans, count, phase, &offset);
        expect_offset(ok, offset, phase, "nudged spans", sps);
    }
}

static void
test_declines_degenerate_input(void) {
    const int count = LEAD_SYMS + DSD_SYMBOL_TIMING_TEMPLATE_SYMS;
    const int template_syms = DSD_SYMBOL_TIMING_TEMPLATE_SYMS;
    uint8_t spans[MAX_SYMS];
    int offset = -1;

    fill_uniform_spans(spans, count + 1, 20);
    build_waveform(kDibits, spans, count + 1, 5);

    const float* samples = g_waveform;
    const uint8_t* tail_spans = &spans[count - template_syms];
    const char* tail_window = &kDibits[count - template_syms];
    const int total = 20 * count;

    /* Null arguments. */
    assert(dsd_symbol_timing_measure_sync_offset(NULL, total, tail_spans, template_syms, tail_window, &offset) == 0);
    assert(dsd_symbol_timing_measure_sync_offset(samples, total, NULL, template_syms, tail_window, &offset) == 0);
    assert(dsd_symbol_timing_measure_sync_offset(samples, total, tail_spans, template_syms, NULL, &offset) == 0);
    assert(dsd_symbol_timing_measure_sync_offset(samples, total, tail_spans, template_syms, tail_window, NULL) == 0);
    assert(dsd_symbol_timing_measure_sync_offset(samples, total, tail_spans, 0, tail_window, &offset) == 0);

    /* One sample per symbol: the CQPSK and RTL symbol-rate grids, which have no
       sub-symbol structure left to localise. */
    uint8_t unit_spans[MAX_SYMS];
    fill_uniform_spans(unit_spans, count, 1);
    assert(dsd_symbol_timing_measure_sync_offset(samples, count, unit_spans, template_syms, tail_window, &offset) == 0);

    /* No room to shift the template: nothing to compare the one candidate against. */
    assert(dsd_symbol_timing_measure_sync_offset(samples, 20 * template_syms, tail_spans, template_syms, tail_window,
                                                 &offset)
           == 0);

    /* Silence, which is what a symbol-file replay leaves in the trace: those inputs
       never hand the grid samples at all. Static, so it is already zeroed. */
    static float silence[MAX_SAMPLES];
    assert(dsd_symbol_timing_measure_sync_offset(silence, total, tail_spans, template_syms, tail_window, &offset) == 0);

    /* A window that is not dibits describes no levels to correlate against. */
    const char* bad_window = "abcdefgh";
    assert(dsd_symbol_timing_measure_sync_offset(samples, total, tail_spans, template_syms, bad_window, &offset) == 0);
}

int
main(void) {
    /* Both ends of the range dsd_opts_compute_sps_rate produces, plus NXDN48's 20. */
    test_uniform_spans_recover_phase(8);
    test_uniform_spans_recover_phase(10);
    test_uniform_spans_recover_phase(20);
    test_uniform_spans_recover_phase(40);

    test_nudged_spans_recover_phase();
    test_declines_degenerate_input();

    printf("SYMBOL_TIMING_DEBUG: OK\n");
    return 0;
}
