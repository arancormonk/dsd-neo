// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A dibit's reliability is measured in the coordinates of the slicer that cut it. The fixed CQPSK levels +-1/+-3 about
 * the centre hold only for the RTL CQPSK demodulator's arg * 4/pi symbols, when P25's CQPSK slicer cut them; TETRA's
 * quadrant slicer cuts the same domain about 0; every other symbol (a float symbol file, which arrives scaled by
 * 10000, or PCM audio, both of a QPSK session too) is cut on the adaptive thresholds and measured against them. Read
 * against the fixed levels, a scaled symbol is "more than one level off" and its reliability, and both LLRs with it,
 * read 0: every soft consumer downstream (AMBE error counts, the P25 Phase 2 DUID rescue, the erasure ranking) gets
 * nothing.
 *
 * Driven through the real callers: digitize() for the payload, the frame sync hunt's payload store and the DMR CACH
 * re-read for the hunt. The claim is not numeric equality across coordinate systems: a slicer's reliabilities are
 * nonzero with LLR signs matching the dibit, and they fall as the symbol nears that slicer's decision boundary.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_test_support.h"

enum { BUF_LEN = 1000001, FLOOR = 200 };

/* The datascope's call line, which no case here prints. */
int
dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    (void)state;
    (void)slot;
    (void)out;
    return -1;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} rig;

/* A symbol and the dibit, soft metric and stored dibit its caller gave it. */
typedef struct {
    int dibit;
    dsd_dibit_soft_t soft;
} sliced;

static int
rig_init(rig* r) {
    r->opts = (dsd_opts*)calloc(1, sizeof(*r->opts));
    r->state = (dsd_state*)calloc(1, sizeof(*r->state));
    if (r->opts == NULL || r->state == NULL) {
        return 0;
    }
    r->state->dibit_buf = (int*)calloc(BUF_LEN, sizeof(int));
    r->state->dmr_payload_buf = (int*)calloc(BUF_LEN, sizeof(int));
    r->state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(BUF_LEN, sizeof(dsd_dibit_soft_t));
    return r->state->dibit_buf != NULL && r->state->dmr_payload_buf != NULL && r->state->dmr_soft_buf != NULL;
}

static void
rig_free(rig* r) {
    if (r->state != NULL) {
        free(r->state->dibit_buf);
        free(r->state->dmr_payload_buf);
        free(r->state->dmr_soft_buf);
        free(r->state->symbol_history);
    }
    free(r->state);
    free(r->opts);
    r->state = NULL;
    r->opts = NULL;
}

static void
rig_rewind(rig* r) {
    r->state->dibit_buf_p = r->state->dibit_buf + FLOOR;
    r->state->dmr_payload_p = r->state->dmr_payload_buf + FLOOR;
    r->state->dmr_soft_p = r->state->dmr_soft_buf + FLOOR;
}

/* The adaptive thresholds use_symbol() derives from averaged extremes at +-3 levels of @p scale about @p bias. */
static void
set_adaptive_levels(dsd_state* state, float scale, float bias) {
    state->max = bias + (3.0f * scale);
    state->min = bias - (3.0f * scale);
    state->center = 0.5f * (state->max + state->min);
    state->umid = ((state->max - state->center) * 5.0f / 8.0f) + state->center;
    state->lmid = ((state->min - state->center) * 5.0f / 8.0f) + state->center;
}

static sliced
digitize_one(rig* r, float symbol) {
    rig_rewind(r);
    sliced out;
    out.dibit = digitize(r->opts, r->state, symbol);
    out.soft = r->state->dmr_soft_p[-1];
    return out;
}

static sliced
hunt_one(rig* r, float symbol) {
    rig_rewind(r);
    (void)dsd_frame_sync_test_process_dibit_and_payload(r->opts, r->state, symbol);
    sliced out;
    out.dibit = r->state->dmr_payload_buf[FLOOR];
    out.soft = r->state->dmr_soft_buf[FLOOR];
    return out;
}

typedef sliced (*slice_fn)(rig* r, float symbol);

/* Both LLRs nonzero, each with the sign of its bit of @p s.dibit (positive favours 1). */
static int
check_signs(const char* what, float symbol, sliced s) {
    const int msb = (s.dibit >> 1) & 1;
    const int lsb = s.dibit & 1;
    const int ok = s.soft.llr[0] != 0 && s.soft.llr[1] != 0 && (s.soft.llr[0] > 0) == (msb != 0)
                   && (s.soft.llr[1] > 0) == (lsb != 0) && s.soft.reliability > 0U;
    if (!ok) {
        DSD_FPRINTF(stderr, "%s: symbol %.4f dibit %d llr %d/%d reliability %u\n", what, (double)symbol, s.dibit,
                    s.soft.llr[0], s.soft.llr[1], s.soft.reliability);
    }
    return ok ? 0 : 1;
}

/* Walks @p count symbols from @p start by @p step toward a decision boundary @p want_dibit never leaves: each keeps
   the dibit with LLR signs that match it, and the reliability never rises and ends below where it began. */
static int
check_walk(const char* what, rig* r, slice_fn slice, float start, float step, int count, int want_dibit) {
    int rc = 0;
    int first = -1;
    int prev = 256;
    for (int i = 0; i < count; i++) {
        const float symbol = start + (step * (float)i);
        const sliced s = slice(r, symbol);
        if (s.dibit != want_dibit) {
            DSD_FPRINTF(stderr, "%s: symbol %.4f sliced %d, want %d\n", what, (double)symbol, s.dibit, want_dibit);
            return 1;
        }
        rc |= check_signs(what, symbol, s);
        const int rel = (int)s.soft.reliability;
        if (rel > prev) {
            DSD_FPRINTF(stderr, "%s: reliability rose from %d to %d at symbol %.4f nearing the boundary\n", what, prev,
                        rel, (double)symbol);
            rc = 1;
        }
        if (first < 0) {
            first = rel;
        }
        prev = rel;
    }
    if (prev >= first) {
        DSD_FPRINTF(stderr, "%s: reliability %d at the boundary, %d at the level\n", what, prev, first);
        rc = 1;
    }
    return rc;
}

/* The four levels' walks toward their nearest adaptive boundary: the inner levels (at 15/16 of a level step, the
   ideals between centre and umid/lmid) toward umid and lmid, the outer ones from +-3 inward toward them too. */
static int
check_adaptive_walks(const char* what, rig* r, slice_fn slice, float scale, float bias, int negative) {
    const dsd_state* st = r->state;
    const float inner_up = 0.5f * (st->center + st->umid);
    const float inner_dn = 0.5f * (st->center + st->lmid);
    const float step = 0.15f * scale;
    /* The dibit of each level, as the slicer reads it, positive and negative polarity. */
    const int d_p1 = negative ? 2 : 0;
    const int d_p3 = negative ? 3 : 1;
    const int d_m1 = negative ? 0 : 2;
    const int d_m3 = negative ? 1 : 3;
    int rc = 0;
    rc |= check_walk(what, r, slice, inner_up, step, 5, d_p1);
    rc |= check_walk(what, r, slice, inner_dn, -step, 5, d_m1);
    rc |= check_walk(what, r, slice, bias + (3.0f * scale), -step, 7, d_p3);
    rc |= check_walk(what, r, slice, bias - (3.0f * scale), step, 7, d_m3);
    /* A noisy outer level overshooting the averaged extreme is at least as sure as the level itself. */
    const sliced at = slice(r, bias + (3.0f * scale));
    const sliced past = slice(r, bias + (3.6f * scale));
    rc |= check_signs(what, bias + (3.6f * scale), past);
    if (past.dibit != d_p3 || past.soft.reliability < at.soft.reliability) {
        DSD_FPRINTF(stderr, "%s: overshooting +3 sliced %d at reliability %u, +3 itself %u\n", what, past.dibit,
                    past.soft.reliability, at.soft.reliability);
        rc = 1;
    }
    return rc;
}

/* A float symbol file of a P25 QPSK session (rf_mod 1), and PCM audio: the adaptive slicer cut the dibit, so its
   thresholds measure it, in the input's own units. */
static int
test_digitize_scaled_qpsk_symbols_use_adaptive_thresholds(void) {
    rig r;
    int rc = 0;
    if (!rig_init(&r)) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        rig_free(&r);
        return 1;
    }

    const struct {
        int audio_in_type;
        float scale;
    } inputs[] = {
        {AUDIO_IN_SYMBOL_FLT, 10000.0f}, /* a float symbol file, scaled by 10000 on the way in */
        {AUDIO_IN_WAV, 3000.0f},         /* PCM audio in sample units */
        {AUDIO_IN_WAV, 0.3f},            /* PCM audio in small units, where the fixed levels read nonzero but wrong */
    };

    const int synctypes[] = {DSD_SYNC_P25P2_POS, DSD_SYNC_P25P2_NEG, DSD_SYNC_P25P1_POS, DSD_SYNC_P25P1_NEG};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        r.opts->audio_in_type = inputs[i].audio_in_type;
        r.state->rf_mod = 1;
        for (size_t s = 0; s < sizeof(synctypes) / sizeof(synctypes[0]); s++) {
            r.state->synctype = synctypes[s];
            r.state->lastsynctype = synctypes[s];
            const int negative = synctypes[s] == DSD_SYNC_P25P2_NEG || synctypes[s] == DSD_SYNC_P25P1_NEG;
            for (int b = -1; b <= 1; b++) {
                const float bias = 0.2f * inputs[i].scale * (float)b;
                set_adaptive_levels(r.state, inputs[i].scale, bias);
                rc |= check_adaptive_walks("digitize scaled QPSK", &r, digitize_one, inputs[i].scale, bias, negative);
            }
        }
    }
    rig_free(&r);
    return rc;
}

/* C4FM (rf_mod 0) keeps the adaptive reliability it always had. */
static int
test_digitize_c4fm_unchanged(void) {
    rig r;
    int rc = 0;
    if (!rig_init(&r)) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        rig_free(&r);
        return 1;
    }
    r.opts->audio_in_type = AUDIO_IN_WAV;
    r.state->rf_mod = 0;
    r.state->synctype = DSD_SYNC_P25P1_POS;
    set_adaptive_levels(r.state, 10000.0f, 0.0f);
    rc |= check_adaptive_walks("digitize C4FM", &r, digitize_one, 10000.0f, 0.0f, 0);
    rig_free(&r);
    return rc;
}

/* TETRA's quadrant slicer cuts arg * 4/pi at 0 and +-2 whatever the adaptive centre: its reliability is measured
   about 0 too, so a biased centre leaves a symbol on a fixed level fully reliable. The adaptive levels here put the
   ideals on the fixed levels with the centre 0.4 high, so only the reliability's coordinates can tell. */
static int
test_digitize_tetra_measures_about_zero(void) {
    rig r;
    int rc = 0;
    if (!rig_init(&r)) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        rig_free(&r);
        return 1;
    }
    r.state->rf_mod = 1;
    r.state->synctype = DSD_SYNC_TETRA_SB_POS;
    r.state->center = 0.4f;
    r.state->umid = 1.6f;
    r.state->lmid = -2.4f;
    r.state->max = 3.0f;
    r.state->min = -3.0f;
    const float levels[] = {1.0f, 3.0f, -1.0f, -3.0f};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        const sliced s = digitize_one(&r, levels[i]);
        rc |= check_signs("digitize TETRA level", levels[i], s);
        if (s.soft.reliability < 250U) {
            DSD_FPRINTF(stderr, "digitize TETRA: level %.1f reliability %u with the centre at 0.4\n", (double)levels[i],
                        s.soft.reliability);
            rc = 1;
        }
    }
    /* Toward the quadrant edge +2 from +1, and from -1 toward 0 (which the biased centre sits beyond). */
    rc |= check_walk("digitize TETRA +1", &r, digitize_one, 1.0f, 0.2f, 5, 0);
    rc |= check_walk("digitize TETRA -1", &r, digitize_one, -1.0f, 0.2f, 5, 2);
    rig_free(&r);
    return rc;
}

/* The hunt's payload store: on a float symbol file of a QPSK session it cuts with the adaptive thresholds and measures
   with them. */
static int
test_hunt_scaled_qpsk_symbols_use_adaptive_thresholds(void) {
    rig r;
    int rc = 0;
    if (!rig_init(&r)) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        rig_free(&r);
        return 1;
    }
    r.opts->audio_in_type = AUDIO_IN_SYMBOL_FLT;
    r.opts->frame_p25p1 = 1;
    r.opts->frame_p25p2 = 1;
    r.state->rf_mod = 1;
    r.state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    for (int b = -1; b <= 1; b++) {
        const float bias = 2000.0f * (float)b;
        set_adaptive_levels(r.state, 10000.0f, bias);
        rc |= check_adaptive_walks("hunt float symbol file", &r, hunt_one, 10000.0f, bias, 0);
    }
    rig_free(&r);
    return rc;
}

/* The hunt under TETRA's 18000/4 profile slices about 0 (frame_sync_cqpsk_slice_center()) and measures about 0. */
static int
test_hunt_tetra_measures_about_zero(void) {
    rig r;
    int rc = 0;
    if (!rig_init(&r)) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        rig_free(&r);
        return 1;
    }
    r.opts->frame_tetra = 1;
    r.state->rf_mod = 1;
    r.state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_18000_4;
    r.state->center = 0.4f;
    r.state->umid = 1.6f;
    r.state->lmid = -2.4f;
    r.state->max = 3.0f;
    r.state->min = -3.0f;
    const sliced s = hunt_one(&r, 1.0f);
    rc |= check_signs("hunt TETRA +1", 1.0f, s);
    if (s.soft.reliability < 250U) {
        DSD_FPRINTF(stderr, "hunt TETRA: +1 reliability %u with the centre at 0.4\n", s.soft.reliability);
        rc = 1;
    }
    rc |= check_walk("hunt TETRA +1", &r, hunt_one, 1.0f, 0.2f, 5, 0);
    rc |= check_walk("hunt TETRA -1", &r, hunt_one, -1.0f, 0.2f, 5, 2);
    rc |= check_walk("hunt TETRA +3", &r, hunt_one, 3.0f, -0.2f, 5, 1);
    rig_free(&r);
    return rc;
}

/* A DMR sync re-reads the 66 dibits before it from the symbol history on the adaptive thresholds
   (dmr_resample_cach()); those below the cleared buffer's floor get their soft metrics from the same thresholds, on
   C4FM and on a QPSK session's scaled symbols alike. */
static int
test_dmr_resample_refresh_uses_adaptive_thresholds(void) {
    enum { READBACK = 90, WRITTEN = 24, HISTORY = 128 };

    rig r;
    int rc = 0;
    if (!rig_init(&r)) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        rig_free(&r);
        return 1;
    }
    r.state->symbol_history = (float*)calloc(HISTORY, sizeof(float));
    if (r.state->symbol_history == NULL) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        rig_free(&r);
        return 1;
    }
    r.state->symbol_history_size = HISTORY;
    r.state->symbol_history_count = READBACK;
    r.state->symbol_history_head = READBACK;
    set_adaptive_levels(r.state, 10000.0f, 0.0f);
    const float levels[4] = {10000.0f, 30000.0f, -10000.0f, -30000.0f}; /* dibits 0..3 */
    for (int mod = 0; mod <= 1; mod++) {
        r.state->rf_mod = mod;
        /* READBACK symbols, oldest first; the newest WRITTEN of them were written since the clear. */
        for (int i = 0; i < READBACK; i++) {
            r.state->symbol_history[i] = levels[i % 4] * (1.0f - (0.05f * (float)(i % 5)));
        }
        rig_rewind(&r);
        r.state->dmr_payload_p += WRITTEN;
        r.state->dmr_soft_p += WRITTEN;
        int* dibits = r.state->dmr_payload_p - READBACK;
        dsd_dibit_soft_t* soft = r.state->dmr_soft_p - READBACK;
        for (int i = 0; i < READBACK; i++) {
            dibits[i] = i % 4;
            soft[i] = (dsd_dibit_soft_t){0};
        }
        dsd_frame_sync_test_dmr_refresh_cleared_soft(r.state, READBACK);
        for (int i = 0; i < READBACK - WRITTEN; i++) {
            sliced s = {.dibit = dibits[i], .soft = soft[i]};
            rc |= check_signs(mod ? "DMR re-read, QPSK" : "DMR re-read, C4FM", r.state->symbol_history[i], s);
        }
        /* Nearer the centre, less sure: the +1 symbol at its level (dibit 0) and at 0.8 of it (dibit 4). */
        if (soft[0].reliability <= soft[4].reliability) {
            DSD_FPRINTF(stderr, "DMR re-read rf_mod %d: +1 at 1.0 reliability %u, at 0.8 %u\n", mod,
                        soft[0].reliability, soft[4].reliability);
            rc = 1;
        }
    }
    rig_free(&r);
    return rc;
}

#ifdef USE_RADIO
static int
cqpsk_status_active(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    *out_cqpsk_enable = 1;
    *out_cqpsk_timing_active = 1;
    return 0;
}

/* The RTL CQPSK phase domain, cut by P25's CQPSK slicer: the fixed +-1/+-3 levels about the centre, as before. A
   symbol past an outer level nears the +-4 wrap, where the phase is a half turn and either outer dibit as likely. */
static int
check_cqpsk_phase_walks(const char* what, rig* r, slice_fn slice, float center, int negative) {
    int rc = 0;
    const int d_p1 = negative ? 2 : 0;
    const int d_p3 = negative ? 3 : 1;
    const int d_m1 = negative ? 0 : 2;
    const int d_m3 = negative ? 1 : 3;
    rc |= check_walk(what, r, slice, center + 1.0f, 0.2f, 5, d_p1);
    rc |= check_walk(what, r, slice, center + 1.0f, -0.2f, 5, d_p1);
    rc |= check_walk(what, r, slice, center - 1.0f, -0.2f, 5, d_m1);
    rc |= check_walk(what, r, slice, center + 3.0f, -0.2f, 5, d_p3);
    rc |= check_walk(what, r, slice, center - 3.0f, 0.2f, 5, d_m3);
    rc |= check_walk(what, r, slice, center + 3.0f, 0.2f, 5, d_p3);
    rc |= check_walk(what, r, slice, center - 3.0f, -0.2f, 5, d_m3);
    const sliced wrap = slice(r, center + 3.98f);
    if (wrap.soft.reliability > 10U) {
        DSD_FPRINTF(stderr, "%s: symbol at the +4 wrap reliability %u\n", what, wrap.soft.reliability);
        rc = 1;
    }
    return rc;
}

static int
test_cqpsk_phase_domain_unchanged(void) {
    rig r;
    int rc = 0;
    if (!rig_init(&r)) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        rig_free(&r);
        return 1;
    }
    dsd_rtl_stream_metrics_hooks hooks = {
        .cqpsk_status = cqpsk_status_active,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    r.opts->audio_in_type = AUDIO_IN_RTL;
    r.opts->frame_p25p1 = 1;
    r.opts->frame_p25p2 = 1;
    r.state->rf_mod = 1;
    r.state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    const float centers[] = {0.0f, 0.3f, -0.3f};
    const int synctypes[] = {DSD_SYNC_P25P2_POS, DSD_SYNC_P25P1_NEG};
    for (size_t c = 0; c < sizeof(centers) / sizeof(centers[0]); c++) {
        /* The adaptive levels are nowhere near the phase levels: only the fixed ones can measure these symbols. */
        set_adaptive_levels(r.state, 10000.0f, 0.0f);
        r.state->center = centers[c];
        for (size_t s = 0; s < sizeof(synctypes) / sizeof(synctypes[0]); s++) {
            r.state->synctype = synctypes[s];
            r.state->lastsynctype = synctypes[s];
            const int negative = synctypes[s] == DSD_SYNC_P25P1_NEG;
            r.state->p25_cqpsk_dibit_map_idx = 0U;
            rc |= check_cqpsk_phase_walks("digitize CQPSK phase", &r, digitize_one, centers[c], negative);
            /* A rotated constellation (the dibit map the sync matched under) moves the dibits, not the reliabilities. */
            for (uint8_t map = 1U; map < 5U; map++) {
                r.state->p25_cqpsk_dibit_map_idx = map;
                const sliced s0 = digitize_one(&r, centers[c] + 1.0f);
                const sliced s1 = digitize_one(&r, centers[c] + 1.6f);
                rc |= check_signs("digitize CQPSK rotated", centers[c] + 1.0f, s0);
                rc |= check_signs("digitize CQPSK rotated", centers[c] + 1.6f, s1);
                if (s1.soft.reliability >= s0.soft.reliability) {
                    DSD_FPRINTF(stderr, "digitize CQPSK map %u: reliability %u at +1, %u at +1.6\n", map,
                                s0.soft.reliability, s1.soft.reliability);
                    rc = 1;
                }
            }
        }
        r.state->p25_cqpsk_dibit_map_idx = 0U;
        rc |= check_cqpsk_phase_walks("hunt CQPSK phase", &r, hunt_one, centers[c], 0);
    }
    dsd_rtl_stream_metrics_hooks_set(NULL);
    rig_free(&r);
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    rc |= test_digitize_scaled_qpsk_symbols_use_adaptive_thresholds();
    rc |= test_digitize_c4fm_unchanged();
    rc |= test_digitize_tetra_measures_about_zero();
    rc |= test_hunt_scaled_qpsk_symbols_use_adaptive_thresholds();
    rc |= test_hunt_tetra_measures_about_zero();
    rc |= test_dmr_resample_refresh_uses_adaptive_thresholds();
#ifdef USE_RADIO
    rc |= test_cqpsk_phase_domain_unchanged();
#endif
    return rc;
}
