// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Symbol-timing observability: sub-symbol offset measurement and reporting.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/symbol_timing_debug.h>
#include <dsd-neo/runtime/config.h>
#include "dsd-neo/core/state_fwd.h"

/* Enough for the template plus a symbol of shift room, at any samplesPerSymbol
   the decoder produces. Sized in symbols rather than samples so it does not have
   to grow if the template does. */
#define TRACE_CAPACITY DSD_SYMBOL_TIMING_SPAN_SAMPLES

int
dsd_symbol_timing_debug_level(void) {
    const dsdneoRuntimeConfig* config = dsd_neo_get_config();
    if (config == NULL) {
        dsd_neo_config_init();
        config = dsd_neo_get_config();
    }
    return config ? config->debug_symbol_timing : DSD_NEO_SYMBOL_TIMING_OFF;
}

void
dsd_symbol_timing_trace_push_sample(dsd_state* state, float sample) {
    if (state == NULL) {
        return;
    }
    dsd_symbol_timing_trace* trace = &state->timing_trace;
    if (trace->samples == NULL) {
        return;
    }

    trace->samples[trace->sample_head] = sample;
    trace->sample_head = (trace->sample_head + 1) % TRACE_CAPACITY;
    if (trace->sample_count < TRACE_CAPACITY) {
        trace->sample_count++;
    }
}

void
dsd_symbol_timing_trace_push_span(dsd_state* state, int samples_consumed) {
    if (state == NULL) {
        return;
    }
    dsd_symbol_timing_trace* trace = &state->timing_trace;
    if (trace->spans == NULL || samples_consumed <= 0 || samples_consumed > DSD_SYMBOL_TIMING_MAX_SPAN) {
        return;
    }

    trace->spans[trace->span_head] = (uint8_t)samples_consumed;
    trace->span_head = (trace->span_head + 1) % DSD_SYMBOL_TIMING_TEMPLATE_SYMS;
    if (trace->span_count < DSD_SYMBOL_TIMING_TEMPLATE_SYMS) {
        trace->span_count++;
    }
}

void
dsd_symbol_timing_trace_reset(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->timing_trace.sample_head = 0;
    state->timing_trace.sample_count = 0;
    state->timing_trace.span_head = 0;
    state->timing_trace.span_count = 0;
}

/**
 * @brief Level a decided dibit stands for, in the units P25's CQPSK raw fit uses.
 * @return +/-1 for an inner symbol, +/-3 for an outer one; 0 when not a dibit.
 */
static int
unit_level_for_dibit(char c) {
    static const int units[4] = {1, 3, -1, -3};
    if (c < '0' || c > '3') {
        return 0;
    }
    return units[c - '0'];
}

/**
 * @brief Total and largest sample counts the template's symbols occupy.
 * @return 1 when every span and window symbol is usable.
 */
static int
template_geometry(const uint8_t* spans, int span_count, const char* window, int* out_total, int* out_max_span) {
    int total = 0;
    int max_span = 0;
    for (int k = 0; k < span_count; k++) {
        const int span = (int)spans[k];
        if (span <= 0 || unit_level_for_dibit(window[k]) == 0) {
            return 0;
        }
        if (span > max_span) {
            max_span = span;
        }
        total += span;
    }
    *out_total = total;
    *out_max_span = max_span;
    return 1;
}

/**
 * @brief Correlate the template against the samples starting @p off earlier than the grid did.
 * @return The correlation score; higher means the symbols line up better.
 */
static double
score_template_at_offset(const float* samples, int lead, int off, const uint8_t* spans, int span_count,
                         const char* window) {
    double score = 0.0;
    int pos = lead - off;
    for (int k = 0; k < span_count; k++) {
        const int span = (int)spans[k];
        double sum = 0.0;
        for (int n = 0; n < span; n++) {
            sum += (double)samples[pos + n];
        }
        score += (double)unit_level_for_dibit(window[k]) * (sum / (double)span);
        pos += span;
    }
    return score;
}

/**
 * @brief Pick the offset scoring highest, or decline if no offset stands out.
 * @return 1 when @p out_offset was written.
 */
static int
search_best_offset(const float* samples, int lead, int max_shift, const uint8_t* spans, int span_count,
                   const char* window, int* out_offset) {
    double best_score = 0.0;
    double worst_score = 0.0;
    int best_offset = -1;

    for (int off = 0; off <= max_shift; off++) {
        const double score = score_template_at_offset(samples, lead, off, spans, span_count, window);
        if (best_offset < 0 || score > best_score) {
            best_score = score;
            best_offset = off;
        }
        if (off == 0 || score < worst_score) {
            worst_score = score;
        }
    }

    /* A silent or flat trace (a symbol-file replay leaves the sample ring at zero,
       since those inputs never hand the grid samples) has no argmax worth printing. */
    if (best_offset < 0 || best_score <= 0.0 || !(best_score > worst_score)) {
        return 0;
    }

    *out_offset = best_offset;
    return 1;
}

int
dsd_symbol_timing_measure_sync_offset(const float* samples, int sample_count, const uint8_t* spans, int span_count,
                                      const char* window, int* out_offset) {
    if (samples == NULL || spans == NULL || window == NULL || out_offset == NULL || span_count <= 0) {
        return 0;
    }

    int template_samples = 0;
    int max_span = 0;
    if (!template_geometry(spans, span_count, window, &template_samples, &max_span)) {
        return 0;
    }

    /* One sample per symbol is the CQPSK and RTL symbol-rate grid: there is no
       sub-symbol structure left to localise, so decline rather than invent one. */
    if (max_span < 2) {
        return 0;
    }

    const int lead = sample_count - template_samples;
    int max_shift = max_span - 1;
    if (lead < max_shift) {
        max_shift = lead;
    }
    if (max_shift < 1) {
        return 0;
    }

    return search_best_offset(samples, lead, max_shift, spans, span_count, window, out_offset);
}

/**
 * @brief Copy the newest @p want samples out of the ring, oldest first.
 * @return Samples copied, which may be fewer than requested.
 */
static int
trace_collect_samples(const dsd_symbol_timing_trace* trace, float* out, int want) {
    int have = trace->sample_count;
    if (have > want) {
        have = want;
    }
    int index = trace->sample_head - have;
    while (index < 0) {
        index += TRACE_CAPACITY;
    }
    for (int i = 0; i < have; i++) {
        out[i] = trace->samples[index];
        index = (index + 1) % TRACE_CAPACITY;
    }
    return have;
}

/**
 * @brief Copy the newest @p want spans out of the ring, oldest first.
 * @return Spans copied, which may be fewer than requested.
 */
static int
trace_collect_spans(const dsd_symbol_timing_trace* trace, uint8_t* out, int want) {
    int have = trace->span_count;
    if (have > want) {
        have = want;
    }
    int index = trace->span_head - have;
    while (index < 0) {
        index += DSD_SYMBOL_TIMING_TEMPLATE_SYMS;
    }
    for (int i = 0; i < have; i++) {
        out[i] = trace->spans[index];
        index = (index + 1) % DSD_SYMBOL_TIMING_TEMPLATE_SYMS;
    }
    return have;
}

/**
 * @brief Measure the offset for the trailing template, if the trace supports one.
 * @return 1 when @p out_offset was written.
 */
static int
trace_measure(const dsd_symbol_timing_trace* trace, const char* window, int* out_offset) {
    if (trace->samples == NULL || trace->spans == NULL || window == NULL) {
        return 0;
    }

    uint8_t spans[DSD_SYMBOL_TIMING_TEMPLATE_SYMS];
    const int span_count = trace_collect_spans(trace, spans, DSD_SYMBOL_TIMING_TEMPLATE_SYMS);
    if (span_count < DSD_SYMBOL_TIMING_TEMPLATE_SYMS) {
        return 0;
    }

    /* The window's last span_count characters are the symbols the spans describe. */
    const size_t window_len = strlen(window);
    if (window_len < (size_t)span_count) {
        return 0;
    }
    const char* window_tail = window + (window_len - (size_t)span_count);

    float samples[TRACE_CAPACITY];
    const int sample_count = trace_collect_samples(trace, samples, TRACE_CAPACITY);
    return dsd_symbol_timing_measure_sync_offset(samples, sample_count, spans, span_count, window_tail, out_offset);
}

void
dsd_symbol_timing_report_sync(const dsd_state* state, int synctype, const char* window) {
    if (state == NULL || dsd_symbol_timing_debug_level() < DSD_NEO_SYMBOL_TIMING_SYNC_LINE) {
        return;
    }

    int offset = -1;
    const int measured = trace_measure(&state->timing_trace, window, &offset);

    /* The sync type is numeric and the matched symbols are printed beside it: naming the
       protocol here would mean calling into core, and core links this library. */
    DSD_FPRINTF(stderr, "SYMTIMING: sync=%i win=%s sps=%i jitter=%i off=", synctype, window ? window : "?",
                state->samplesPerSymbol, state->jitter);
    if (measured) {
        DSD_FPRINTF(stderr, "%i", offset);
    } else {
        DSD_FPRINTF(stderr, "-");
    }
    DSD_FPRINTF(stderr, " accum=%i\n", state->rtl_fsk_sps_accum);
}
