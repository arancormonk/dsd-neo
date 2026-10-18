// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Sync-anchored correction of the open-loop FSK symbol grid (issue #729).
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <dsd-neo/dsp/symbol_timing_debug.h>
#include "dsd-neo/core/state_fwd.h"
#include "symbol_anchor_internal.h"

/* Samples the grid keeps: the longest sync word at the widest span the decoder produces, plus the shift room either
   side and the symbol of look-ahead. A power of two, so the ring index is a mask. */
#define ANCHOR_RING_SAMPLES 4096U
/* Symbols whose centres the grid keeps: the longest sync word and the look-ahead symbol, rounded up. */
#define ANCHOR_RING_SYMBOLS 64U
/* Samples either side of the centre a symbol's level is read over: the decoder's own C4FM window is about this. */
#define ANCHOR_CENTRE_HALF  1

struct dsd_symbol_anchor {
    float samples[ANCHOR_RING_SAMPLES];
    uint64_t centres[ANCHOR_RING_SYMBOLS];
    uint64_t sample_total; /* Samples pushed since the last reset. */
    uint64_t symbol_total; /* Symbols ended since the last reset. */
    uint64_t pending_centre;
    int pending_centre_set;
    int samples_per_symbol;
    char sync[DSD_SYMBOL_ANCHOR_MAX_SYMS];
    int sync_count;
    uint64_t sync_end_symbol; /* symbol_total when the sync word's last symbol had ended. */
    int sync_pending;
    int slip;          /* +1: take one sample more at the next symbol; -1: one fewer. */
    int armed;         /* recording: a protocol said a sync word is coming (dsd_symbol_arm_sync_anchor()) */
    int armed_symbols; /* symbols recorded since it was armed */
};

/* Symbols an armed anchor records before giving up on a sync word that was not named: a DMR burst's sync field ends
   25 symbols after the arming point, so this is ample slack. */
#define ANCHOR_ARMED_MAX_SYMBOLS 64

static int
anchor_level_for_dibit(char c) {
    static const int levels[4] = {1, 3, -1, -3};
    if (c < '0' || c > '3') {
        return 0;
    }
    return levels[c - '0'];
}

/* The waveform at a fractional sample index, interpolated between its neighbours. */
static double
anchor_sample_at(const float* samples, double at) {
    const int lo = (int)floor(at);
    const double frac = at - (double)lo;
    return (1.0 - frac) * (double)samples[lo] + frac * (double)samples[lo + 1];
}

static double
anchor_score_at(const float* samples, const int* centres, const char* dibits, int count, int shift) {
    double score = 0.0;
    for (int k = 0; k < count; k++) {
        double sum = 0.0;
        for (int d = -ANCHOR_CENTRE_HALF; d <= ANCHOR_CENTRE_HALF; d++) {
            sum += (double)samples[centres[k] + shift + d];
        }
        score += (double)anchor_level_for_dibit(dibits[k]) * sum;
    }
    return score;
}

/* Whether every symbol is a dibit whose centre has @p reach samples (and the read window) either side in the trace. */
static int
anchor_symbols_readable(int sample_count, const int* centres, const char* dibits, int count, int reach) {
    for (int k = 0; k < count; k++) {
        if (anchor_level_for_dibit(dibits[k]) == 0 || centres[k] - reach - ANCHOR_CENTRE_HALF < 0
            || centres[k] + reach + ANCHOR_CENTRE_HALF >= sample_count) {
            return 0;
        }
    }
    return 1;
}

/* The whole-sample shift within +/-@p reach whose correlation with the symbols' levels peaks, the levels' sign taken
   from the data so an inverted signal measures the same. Returns 0 when no shift correlates (a flat trace). */
static int
anchor_coarse_shift(const float* samples, const int* centres, const char* dibits, int count, int reach, int* out_best) {
    const double sign = (anchor_score_at(samples, centres, dibits, count, 0) < 0.0) ? -1.0 : 1.0;
    int best = -reach;
    double best_score = sign * anchor_score_at(samples, centres, dibits, count, -reach);
    for (int shift = -reach + 1; shift <= reach; shift++) {
        const double score = sign * anchor_score_at(samples, centres, dibits, count, shift);
        if (score > best_score) {
            best_score = score;
            best = shift;
        }
    }
    if (!(best_score > 0.0)) {
        return 0;
    }
    *out_best = best;
    return 1;
}

/* The peak is flat at the centre, so its vertex is the noisiest place to read the phase from. The symbols' sign changes
   are the sharpest: midway between two symbols of opposite sign the waveform crosses zero, steeply. Read the samples
   there, at the coarse shift, and fit the one delay that moves every crossing onto them: the grid sits before a
   crossing by its value over its slope. The straight-line crossing holds within about a sample, so the fit is held
   to that. */
static double
anchor_crossing_fit(const float* samples, const int* centres, const char* dibits, int count, int coarse) {
    double fit_num = 0.0;
    double fit_den = 0.0;
    for (int k = 1; k < count; k++) {
        if (anchor_level_for_dibit(dibits[k]) * anchor_level_for_dibit(dibits[k - 1]) >= 0) {
            continue;
        }
        const double mid = (0.5 * (double)(centres[k - 1] + centres[k])) + (double)coarse;
        const double value = anchor_sample_at(samples, mid);
        const double slope = 0.5 * (anchor_sample_at(samples, mid + 1.0) - anchor_sample_at(samples, mid - 1.0));
        fit_num += value * slope;
        fit_den += slope * slope;
    }
    if (!(fit_den > 1e-12)) {
        return 0.0;
    }
    const double fine = -fit_num / fit_den;
    return fmax(-1.0, fmin(1.0, fine));
}

int
dsd_symbol_anchor_measure_offset(const float* samples, int sample_count, const int* centres, const char* dibits,
                                 int count, int samples_per_symbol, double* out_offset) {
    if (samples == NULL || centres == NULL || dibits == NULL || out_offset == NULL || count <= 0
        || count > DSD_SYMBOL_ANCHOR_MAX_SYMS) {
        return 0;
    }
    /* Half a symbol either way, short of the neighbours' centres: further than that the fit is the next symbol's. */
    const int reach = samples_per_symbol / 2 - 1;
    if (reach < 1 || !anchor_symbols_readable(sample_count, centres, dibits, count, reach)) {
        return 0;
    }
    int coarse = 0;
    if (!anchor_coarse_shift(samples, centres, dibits, count, reach, &coarse)) {
        return 0;
    }
    *out_offset = (double)coarse + anchor_crossing_fit(samples, centres, dibits, count, coarse);
    return 1;
}

static void
anchor_free(void* ptr) {
    free(ptr);
}

dsd_symbol_anchor*
dsd_symbol_anchor_get(dsd_state* state) {
    if (state == NULL) {
        return NULL;
    }
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    if (anchor != NULL) {
        return anchor;
    }
    anchor = (dsd_symbol_anchor*)calloc(1, sizeof(*anchor));
    if (anchor == NULL) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR, anchor, anchor_free) != 0) {
        anchor_free(anchor);
        return NULL;
    }
    return anchor;
}

void
dsd_symbol_anchor_reset(dsd_state* state) {
    dsd_symbol_anchor* anchor =
        state ? DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR) : NULL;
    if (anchor == NULL) {
        return;
    }
    state->symbol_anchor_hot = 0U;
    anchor->sample_total = 0;
    anchor->symbol_total = 0;
    anchor->pending_centre_set = 0;
    anchor->sync_pending = 0;
    anchor->slip = 0;
    anchor->armed = 0;
}

int
dsd_symbol_anchor_hot(const dsd_symbol_anchor* anchor) {
    return anchor->armed != 0 || anchor->slip != 0;
}

int
dsd_symbol_anchor_recording(const dsd_symbol_anchor* anchor) {
    return anchor != NULL && anchor->armed != 0;
}

void
dsd_symbol_anchor_push_sample(dsd_symbol_anchor* anchor, float sample, int is_centre) {
    if (is_centre) {
        anchor->pending_centre = anchor->sample_total;
        anchor->pending_centre_set = 1;
    }
    anchor->samples[anchor->sample_total & (ANCHOR_RING_SAMPLES - 1U)] = sample;
    anchor->sample_total++;
}

/* The named sync word's centres, oldest first, and the span they cover. A centre the grid never marked (UINT64_MAX)
   declines. */
static int
anchor_gather_centres(const dsd_symbol_anchor* anchor, int count, uint64_t* abs_centres, uint64_t* first,
                      uint64_t* last) {
    *first = UINT64_MAX;
    *last = 0;
    for (int k = 0; k < count; k++) {
        const uint64_t symbol = anchor->sync_end_symbol - (uint64_t)count + (uint64_t)k;
        const uint64_t centre = anchor->centres[symbol & (ANCHOR_RING_SYMBOLS - 1U)];
        if (centre == UINT64_MAX) {
            return 0;
        }
        abs_centres[k] = centre;
        *first = (centre < *first) ? centre : *first;
        *last = (centre > *last) ? centre : *last;
    }
    return 1;
}

/* The whole samples the grid moves for a measured offset: to the nearest, at most two, none within half a sample. */
static int
anchor_slip_for(double offset) {
    if (offset > 1.5) {
        return 2;
    }
    if (offset > 0.5) {
        return 1;
    }
    if (offset < -1.5) {
        return -2;
    }
    if (offset < -0.5) {
        return -1;
    }
    return 0;
}

static void
anchor_measure_pending(dsd_symbol_anchor* anchor) {
    anchor->sync_pending = 0;
    const int count = anchor->sync_count;
    const int sps = anchor->samples_per_symbol;
    if ((uint64_t)count + 1U > anchor->symbol_total || sps < 2 || sps > DSD_SYMBOL_TIMING_MAX_SPAN) {
        return;
    }

    /* The sync word's centres, and a linear copy of the samples around them, so the measurement reads plain arrays.
       Samples the ring has overwritten since decline. */
    uint64_t abs_centres[DSD_SYMBOL_ANCHOR_MAX_SYMS];
    uint64_t first = 0;
    uint64_t last = 0;
    if (!anchor_gather_centres(anchor, count, abs_centres, &first, &last)) {
        return;
    }
    const uint64_t margin = (uint64_t)sps;
    if (first < margin || last + margin >= anchor->sample_total
        || anchor->sample_total - (first - margin) > ANCHOR_RING_SAMPLES) {
        return;
    }
    const uint64_t base = first - margin;
    const uint64_t span = last + margin - base + 1U;

    float window[ANCHOR_RING_SAMPLES];
    for (uint64_t n = 0; n < span; n++) {
        window[n] = anchor->samples[(base + n) & (ANCHOR_RING_SAMPLES - 1U)];
    }
    int centres[DSD_SYMBOL_ANCHOR_MAX_SYMS];
    for (int k = 0; k < count; k++) {
        centres[k] = (int)(abs_centres[k] - base);
    }

    double offset = 0.0;
    if (dsd_symbol_anchor_measure_offset(window, (int)span, centres, anchor->sync, count, sps, &offset)) {
        anchor->slip = anchor_slip_for(offset);
    }
}

void
dsd_symbol_anchor_end_symbol(dsd_symbol_anchor* anchor, int samples_per_symbol) {
    anchor->centres[anchor->symbol_total & (ANCHOR_RING_SYMBOLS - 1U)] =
        anchor->pending_centre_set ? anchor->pending_centre : UINT64_MAX;
    anchor->pending_centre_set = 0;
    anchor->symbol_total++;
    anchor->samples_per_symbol = samples_per_symbol;
    /* The symbol after the sync word gives the look-ahead the later shifts read; then the recording is done. */
    if (anchor->sync_pending && anchor->symbol_total > anchor->sync_end_symbol) {
        anchor_measure_pending(anchor);
        anchor->armed = 0;
    } else if (++anchor->armed_symbols > ANCHOR_ARMED_MAX_SYMBOLS) {
        anchor->armed = 0;
    }
}

void
dsd_symbol_arm_sync_anchor(dsd_state* state) {
    dsd_symbol_anchor* anchor = dsd_symbol_anchor_get(state);
    if (anchor == NULL) {
        return;
    }
    /* A fresh recording: what an earlier one left is not contiguous with it. A move already queued still applies. */
    anchor->sample_total = 0;
    anchor->symbol_total = 0;
    anchor->pending_centre_set = 0;
    anchor->sync_pending = 0;
    anchor->armed = 1;
    anchor->armed_symbols = 0;
    state->symbol_anchor_hot = 1U;
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_symbol_anchor_test_queue_slip(dsd_state* state, int slip) {
    dsd_symbol_anchor* anchor = dsd_symbol_anchor_get(state);
    if (anchor != NULL) {
        anchor->slip = slip;
        state->symbol_anchor_hot = 1U;
    }
}
#endif

int
dsd_symbol_anchor_take_slip(dsd_symbol_anchor* anchor) {
    if (anchor == NULL) {
        return 0;
    }
    const int slip = anchor->slip;
    anchor->slip = 0;
    return slip;
}

void
dsd_symbol_note_sync_anchor(const dsd_state* state, const char* sync_dibits, int count) {
    if (state == NULL || sync_dibits == NULL || count <= 0 || count > DSD_SYMBOL_ANCHOR_MAX_SYMS) {
        return;
    }
    /* Only a recording armed ahead of the sync word holds its samples. */
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    if (anchor == NULL || !anchor->armed || anchor->symbol_total < (uint64_t)count) {
        return;
    }
    DSD_MEMCPY(anchor->sync, sync_dibits, (size_t)count);
    anchor->sync_count = count;
    anchor->sync_end_symbol = anchor->symbol_total;
    anchor->sync_pending = 1;
}
