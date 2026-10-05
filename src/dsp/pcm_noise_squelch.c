// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The PCM noise squelch's band, learned reference and gate (issue #628); see pcm_noise_squelch.h and
   tools/pcm_noise_squelch_model.py, whose run_learner() this follows step for step. */

#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/pcm_noise_squelch.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "noise_squelch_bank.h"

/* The band (tools/pcm_noise_squelch_model.py): from just above voice to 6.5 kHz, under 0.45 of the native rate. */
static const double k_band_lo_hz = 3800.0;
static const double k_band_top_hz = 6500.0;
static const double k_nyquist_fraction = 0.45;
static const double k_min_band_hz = 1200.0;
static const double k_voice_lo_hz = 400.0;
static const double k_voice_hi_hz = 2600.0;
/* Q = max(Q_sum, Q_max - 6 dB); the gate closes 3 dB under N, and never under 1.5 dB. */
static const double k_guard_db = 6.0;
static const double k_hysteresis_db = 3.0;
static const double k_close_floor_db = 1.5;
/* The learner. */
static const double k_steady_db = 1.5;   /* a run's windows within this of their median */
static const double k_step_db = 4.0;     /* a new level */
static const double k_gain_tol_db = 1.5; /* both bands moved by the same amount: a gain step */
/* A louder stretch under the reference is noise at another gain only with noise's voice-to-band ratio, within this (a gain
   step keeps the ratio), besides the gain steps that put it there. */
static const double k_shape_tol_db = 3.0;
static const double k_ratio_max_db = 30.0; /* a sub-band this far under the voice band per Hz carries nothing */
static const double k_gain_steady_fraction = 0.75;
static const double k_stale_steady_fraction = 0.9;
static const double k_track = 1.0 / 50.0; /* a 1 s time constant at a window every 20 ms */

enum {
    PNSQ_HALVES_PER_S = 50,
    PNSQ_NO_BAND_WINDOWS = 50, /* 1 s */
    PNSQ_CONFIRM_WINDOWS = 3,
    PNSQ_GAIN_HOLD_WINDOWS = 10,
    PNSQ_STALE_WINDOWS = 250, /* 5 s */
};

static const double k_min_power = DSD_NOISE_SQUELCH_BANK_MIN_POWER;
/* Filter state this small is flushed to zero at each half-window (digital silence would otherwise go denormal). */
static const double k_state_flush = 1e-150;
/* Relative tolerance for plans that decide the same way. */
static const double k_plan_same = 1e-12;

/* ---------------------------------------------------------------------------------------------- the plan */

int
dsd_pcm_noise_squelch_plan_design(dsd_pcm_noise_squelch_plan* out, int rate_hz, int native_rate_hz) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (rate_hz < 2 * PNSQ_HALVES_PER_S || native_rate_hz <= 0 || native_rate_hz > rate_hz) {
        return -1;
    }
    double hi = k_nyquist_fraction * (double)native_rate_hz;
    if (hi > k_band_top_hz) {
        hi = k_band_top_hz;
    }
    const double width = hi - k_band_lo_hz;
    if (width < k_min_band_hz) {
        return -1;
    }
    int k_count = (int)floor(width / (double)DSD_PCM_NOISE_SQUELCH_SUB_BAND_HZ);
    if (k_count < 1) {
        k_count = 1;
    } else if (k_count > DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS) {
        k_count = DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS;
    }
    const double fs = (double)rate_hz;
    const double step = width / (double)k_count;
    out->rate_hz = rate_hz;
    out->native_rate_hz = native_rate_hz;
    out->lo_hz = k_band_lo_hz;
    out->hi_hz = hi;
    out->step_hz = step;
    out->sub_bands = k_count;
    out->bands = k_count + ((DSD_PCM_NOISE_SQUELCH_SETS - 1) * (k_count - 1));
    out->band_bw_db = 10.0 * log10(step);
    out->voice_bw_db = 10.0 * log10(k_voice_hi_hz - k_voice_lo_hz);
    for (int k = 0; k < k_count; k++) {
        const double f1 = k_band_lo_hz + (step * (double)k);
        dsd_noise_squelch_bank_design_band_pass(f1, f1 + step, fs, out->section[k]);
    }
    /* The staggered sets: set j shifted by j / SETS of a sub-band, so a line on a boundary of one set sits inside a
       band-pass of another. */
    for (int j = 1; j < DSD_PCM_NOISE_SQUELCH_SETS; j++) {
        for (int k = 0; k + 1 < k_count; k++) {
            const double f1 = k_band_lo_hz + (step * ((double)k + ((double)j / (double)DSD_PCM_NOISE_SQUELCH_SETS)));
            dsd_noise_squelch_bank_design_band_pass(f1, f1 + step, fs,
                                                    out->section[k_count + ((j - 1) * (k_count - 1)) + k]);
        }
    }
    dsd_noise_squelch_bank_design_band_pass(k_voice_lo_hz, k_voice_hi_hz, fs, out->voice);
    out->valid = 1;
    return 0;
}

static int
pnsq_same(double a, double b) {
    const double scale = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return fabs(a - b) <= k_plan_same * (scale > 0.0 ? scale : 1.0);
}

int
dsd_pcm_noise_squelch_plan_equal(const dsd_pcm_noise_squelch_plan* a, const dsd_pcm_noise_squelch_plan* b) {
    if (!a || !b || a->valid != b->valid) {
        return 0;
    }
    if (!a->valid) {
        return 1;
    }
    return a->rate_hz == b->rate_hz && a->native_rate_hz == b->native_rate_hz && a->sub_bands == b->sub_bands
           && a->bands == b->bands && pnsq_same(a->lo_hz, b->lo_hz) && pnsq_same(a->hi_hz, b->hi_hz);
}

/* ---------------------------------------------------------------------------------------------- helpers */

static double
pnsq_db(double power) {
    return 10.0 * log10(power > k_min_power ? power : k_min_power);
}

/* The above-band power in dB: the sub-bands' sum. */
static double
pnsq_total_db(const dsd_pcm_noise_squelch_plan* plan, const double* p) {
    double sum = 0.0;
    for (int k = 0; k < plan->sub_bands; k++) {
        sum += p[k];
    }
    return pnsq_db(sum);
}

/* The median of RUN_WINDOWS values. */
static double
pnsq_median(const double* x) {
    double v[DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS];
    for (int i = 0; i < DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS; i++) {
        v[i] = x[i];
    }
    for (int i = 1; i < DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS; i++) {
        const double t = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > t) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = t;
    }
    const int n = DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS;
    return (n % 2) ? v[n / 2] : 0.5 * (v[(n / 2) - 1] + v[n / 2]);
}

/* Whether RUN_WINDOWS values all sit within k_steady_db of their median. */
static int
pnsq_steady(const double* x) {
    const double med = pnsq_median(x);
    for (int i = 0; i < DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS; i++) {
        if (fabs(x[i] - med) > k_steady_db) {
            return 0;
        }
    }
    return 1;
}

/* Which band-passes carry discriminator noise in powers p against the voice band at v_db: a sub-band whose power per
   Hz sits more than k_ratio_max_db under the voice band's holds only a stopband or a floor; a staggered band-pass
   takes part when both sub-bands it straddles do. Returns the band taking part, Hz. */
static double
pnsq_participating(const dsd_pcm_noise_squelch_plan* plan, const double* p, double v_db, unsigned char* part) {
    const int k_count = plan->sub_bands;
    int taking = 0;
    for (int k = 0; k < k_count; k++) {
        const double per_hz = pnsq_db(p[k]) - plan->band_bw_db;
        part[k] = ((v_db - plan->voice_bw_db) - per_hz <= k_ratio_max_db) ? 1U : 0U;
        taking += part[k];
    }
    for (int j = 1; j < DSD_PCM_NOISE_SQUELCH_SETS; j++) {
        for (int k = 0; k + 1 < k_count; k++) {
            part[k_count + ((j - 1) * (k_count - 1)) + k] = (part[k] && part[k + 1]) ? 1U : 0U;
        }
    }
    return (double)taking * plan->step_hz;
}

static void
pnsq_take_reference(dsd_pcm_noise_squelch* t, const double* p, double v_db) {
    for (int k = 0; k < t->plan.bands; k++) {
        t->ref[k] = p[k];
    }
    t->v_ref_db = v_db;
    t->usable_hz = pnsq_participating(&t->plan, t->ref, t->v_ref_db, t->part);
    t->carrier_gain_db = 0.0;
    t->carrier_gain_seen = 0;
}

/* A gain-like step between two carrier stretches (both bands moved alike, the voice band stationary): the source's
   volume moved during a transmission, which is where its noise comes back. */
static void
pnsq_note_carrier_gain(dsd_pcm_noise_squelch* t, double da) {
    t->carrier_gain_db += da;
    t->carrier_gain_seen = 1;
}

static void
pnsq_flush_state(dsd_pcm_noise_squelch* t) {
    for (int k = 0; k < t->plan.bands; k++) {
        for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
            if (fabs(t->s1[k][s]) < k_state_flush) {
                t->s1[k][s] = 0.0;
            }
            if (fabs(t->s2[k][s]) < k_state_flush) {
                t->s2[k][s] = 0.0;
            }
        }
    }
    for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
        if (fabs(t->v1[s]) < k_state_flush) {
            t->v1[s] = 0.0;
        }
        if (fabs(t->v2[s]) < k_state_flush) {
            t->v2[s] = 0.0;
        }
    }
}

static int
pnsq_has_reference(const dsd_pcm_noise_squelch* t) {
    return t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL || t->state == DSD_PCM_NOISE_SQUELCH_KNOWN;
}

static int
pnsq_available(const dsd_pcm_noise_squelch* t) {
    return t->plan.valid
           && (t->state == DSD_PCM_NOISE_SQUELCH_LEARNING || t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL
               || t->state == DSD_PCM_NOISE_SQUELCH_KNOWN);
}

/* ---------------------------------------------------------------------------------------------- set-up */

/* The sample count at which half-window @p index (from 0) ends. */
static uint64_t
pnsq_half_end(const dsd_pcm_noise_squelch* t, uint64_t index) {
    return ((index + 1U) * (uint64_t)t->plan.rate_hz) / (uint64_t)PNSQ_HALVES_PER_S;
}

void
dsd_pcm_noise_squelch_restart(dsd_pcm_noise_squelch* t) {
    if (!t) {
        return;
    }
    DSD_MEMSET(t->s1, 0, sizeof(t->s1));
    DSD_MEMSET(t->s2, 0, sizeof(t->s2));
    DSD_MEMSET(t->v1, 0, sizeof(t->v1));
    DSD_MEMSET(t->v2, 0, sizeof(t->v2));
    DSD_MEMSET(t->half_e, 0, sizeof(t->half_e));
    DSD_MEMSET(t->prev_e, 0, sizeof(t->prev_e));
    t->half_v = t->prev_v = 0.0;
    t->half_x = t->prev_x = 0.0;
    t->half_n = t->prev_n = 0.0;
    t->have_prev = 0;
    t->samples = 0U;
    t->half_index = 0U;
    t->half_end = t->plan.valid ? pnsq_half_end(t, 0U) : 0U;
    t->run_pos = 0;
    t->run_len = 0;
    t->have_stretch = 0;
    t->st_pending = 0;
    t->have_cand = 0;
    t->cand_n = 0;
    t->nb = 0;
    t->exit_n = 0;
    t->gate_open = 0;
    t->quieting_valid = 0;
    t->quieting_db = 0.0;
    t->windows = 0U;
}

void
dsd_pcm_noise_squelch_forget(dsd_pcm_noise_squelch* t) {
    if (!t) {
        return;
    }
    t->state = t->plan.valid ? DSD_PCM_NOISE_SQUELCH_LEARNING : DSD_PCM_NOISE_SQUELCH_NO_ROOM;
    DSD_MEMSET(t->ref, 0, sizeof(t->ref));
    DSD_MEMSET(t->part, 0, sizeof(t->part));
    t->carrier_gain_db = 0.0;
    t->carrier_gain_seen = 0;
    t->v_ref_db = 0.0;
    t->usable_hz = 0.0;
    DSD_MEMSET(t->cache, 0, sizeof(t->cache));
    t->cache_clock = 0U;
    dsd_pcm_noise_squelch_restart(t);
}

void
dsd_pcm_noise_squelch_set_plan(dsd_pcm_noise_squelch* t, const dsd_pcm_noise_squelch_plan* plan) {
    if (!t || !plan) {
        return;
    }
    if (dsd_pcm_noise_squelch_plan_equal(&t->plan, plan)
        && (t->plan.valid || t->state == DSD_PCM_NOISE_SQUELCH_NO_ROOM)) {
        return;
    }
    t->plan = *plan;
    dsd_pcm_noise_squelch_forget(t);
}

void
dsd_pcm_noise_squelch_set_threshold(dsd_pcm_noise_squelch* t, int threshold_db) {
    if (!t) {
        return;
    }
    if (threshold_db < DSD_SQUELCH_MARGIN_MIN_DB) {
        threshold_db = DSD_SQUELCH_MARGIN_MIN_DB;
    } else if (threshold_db > DSD_SQUELCH_MARGIN_MAX_DB) {
        threshold_db = DSD_SQUELCH_MARGIN_MAX_DB;
    }
    t->threshold_db = threshold_db;
    t->open_db = (double)threshold_db;
    const double close = (double)threshold_db - k_hysteresis_db;
    t->close_db = close > k_close_floor_db ? close : k_close_floor_db;
}

/* The cache entry for @p passband_hz, or NULL. */
static dsd_pcm_noise_squelch_cache_entry*
pnsq_cache_find(dsd_pcm_noise_squelch* t, int32_t passband_hz) {
    for (int i = 0; i < DSD_PCM_NOISE_SQUELCH_CACHE_SIZE; i++) {
        if (t->cache[i].used && t->cache[i].passband_hz == passband_hz) {
            return &t->cache[i];
        }
    }
    return NULL;
}

static void
pnsq_cache_store(dsd_pcm_noise_squelch* t, int32_t passband_hz) {
    if (passband_hz == DSD_PCM_NOISE_SQUELCH_PASSBAND_UNKNOWN || t->state == DSD_PCM_NOISE_SQUELCH_LEARNING
        || !t->plan.valid) {
        return;
    }
    dsd_pcm_noise_squelch_cache_entry* e = pnsq_cache_find(t, passband_hz);
    if (!e) {
        e = &t->cache[0];
        for (int i = 0; i < DSD_PCM_NOISE_SQUELCH_CACHE_SIZE; i++) {
            if (!t->cache[i].used) {
                e = &t->cache[i];
                break;
            }
            if (t->cache[i].stamp < e->stamp) {
                e = &t->cache[i];
            }
        }
    }
    e->used = 1;
    e->passband_hz = passband_hz;
    e->state = t->state;
    DSD_MEMCPY(e->ref, t->ref, sizeof(e->ref));
    e->v_ref_db = t->v_ref_db;
    e->stamp = ++t->cache_clock;
}

void
dsd_pcm_noise_squelch_set_key(dsd_pcm_noise_squelch* t, const dsd_pcm_noise_squelch_key* key) {
    if (!t || !key) {
        return;
    }
    if (!t->have_key) {
        t->key = *key;
        t->have_key = 1;
        return;
    }
    if (t->key.source != key->source || t->key.native_rate_hz != key->native_rate_hz || t->key.volume != key->volume) {
        t->key = *key;
        dsd_pcm_noise_squelch_forget(t);
        return;
    }
    if (t->key.passband_hz == key->passband_hz) {
        return;
    }
    pnsq_cache_store(t, t->key.passband_hz);
    t->key = *key;
    dsd_pcm_noise_squelch_cache_entry* e =
        key->passband_hz == DSD_PCM_NOISE_SQUELCH_PASSBAND_UNKNOWN ? NULL : pnsq_cache_find(t, key->passband_hz);
    if (e && t->plan.valid) {
        t->state = e->state;
        e->stamp = ++t->cache_clock;
        if (pnsq_has_reference(t)) {
            pnsq_take_reference(t, e->ref, e->v_ref_db);
        }
    } else if (t->plan.valid) {
        t->state = DSD_PCM_NOISE_SQUELCH_LEARNING;
        DSD_MEMSET(t->ref, 0, sizeof(t->ref));
        DSD_MEMSET(t->part, 0, sizeof(t->part));
        t->carrier_gain_db = 0.0;
        t->carrier_gain_seen = 0;
        t->usable_hz = 0.0;
    }
    dsd_pcm_noise_squelch_restart(t);
}

/* ---------------------------------------------------------------------------------------------- the learner */

static void
pnsq_new_stretch(dsd_pcm_noise_squelch* t, double sa, int pending, double pend_db) {
    t->have_stretch = 1;
    t->st_a_db = sa;
    t->st_v_sum = 0.0;
    t->st_n = 0;
    t->st_vs = 0;
    t->st_at_ref = pnsq_has_reference(t) && sa >= pnsq_total_db(&t->plan, t->ref) - k_step_db;
    t->st_pending = pending;
    t->st_pend_db = pend_db;
    t->st_pn = 0;
    t->st_ps = 0;
    t->st_on = 0;
    t->st_os = 0;
}

/* A transition to the stretch whose run means are sp, sa and sv (the run's voice band stationary or not). */
static void
pnsq_transition(dsd_pcm_noise_squelch* t, const double* sp, double sa, double sv, int v_steady) {
    int pending = 0;
    double pend_db = 0.0;
    t->have_cand = 0;
    t->cand_n = 0;
    if (t->state == DSD_PCM_NOISE_SQUELCH_LEARNING) {
        pnsq_take_reference(t, sp, sv);
        t->state = DSD_PCM_NOISE_SQUELCH_PROVISIONAL;
    } else if (t->state == DSD_PCM_NOISE_SQUELCH_NO_BAND) {
        /* Left only through the no-band exit. */
    } else if (!t->have_stretch) {
        /* A first stretch after a restart louder than the reference is noise. */
        if (sa > pnsq_total_db(&t->plan, t->ref) + k_step_db) {
            pnsq_take_reference(t, sp, sv);
        }
    } else {
        const double da = sa - t->st_a_db;
        const double dv = sv - pnsq_db(t->st_v_sum / (double)t->st_n);
        const int gain_like = fabs(da - dv) <= k_gain_tol_db;
        const double ra = pnsq_total_db(&t->plan, t->ref);
        /* Louder and up at the reference: noise. Louder but well under it is noise only where the gain steps seen under
           a carrier put the reference, with noise's voice-to-band ratio: the source turned down during a
           transmission. Without that evidence it is the carrier's modulation or level changing (speech after a pause,
           a fading carrier), and the reference stays. */
        const int lowered = t->carrier_gain_seen && fabs(sa - (ra + t->carrier_gain_db)) <= k_step_db
                            && fabs((sv - sa) - (t->v_ref_db - ra)) <= k_shape_tol_db;
        const int noise_like = sa >= ra - k_step_db || lowered;
        if (da > 0.0 && noise_like) {
            /* The band got louder: that side is noise. A shape change (a carrier dropped) confirms the reference. */
            pnsq_take_reference(t, sp, sv);
            if (!gain_like && t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL) {
                t->state = DSD_PCM_NOISE_SQUELCH_KNOWN;
            }
        } else if (da > 0.0) {
            if (gain_like && v_steady) {
                pnsq_note_carrier_gain(t, da);
            }
        } else if (gain_like && v_steady && t->st_at_ref && t->st_vs * 2 >= t->st_n) {
            /* Quieter, from noise, both bands by the same amount: a gain step, once it holds. */
            pending = 1;
            pend_db = da;
        } else {
            if (gain_like && v_steady && !t->st_at_ref) {
                pnsq_note_carrier_gain(t, da);
            }
            if (t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL) {
                /* A carrier keyed: the louder side, the reference, was noise. */
                t->state = DSD_PCM_NOISE_SQUELCH_KNOWN;
            }
        }
    }
    pnsq_new_stretch(t, sa, pending, pend_db);
}

/* The steady run continues the stretch: its pending gain step, stale quieting and slow tracking. */
static void
pnsq_continue(dsd_pcm_noise_squelch* t, const double* sp, double sa, double sv, int v_steady) {
    t->st_v_sum += pow(10.0, sv / 10.0);
    t->st_n++;
    t->st_vs += v_steady ? 1 : 0;
    if (t->st_pending) {
        t->st_pn++;
        t->st_ps += v_steady ? 1 : 0;
        if (t->st_pn >= PNSQ_GAIN_HOLD_WINDOWS) {
            if ((double)t->st_ps >= k_gain_steady_fraction * (double)t->st_pn) {
                const double g = pow(10.0, t->st_pend_db / 10.0);
                for (int k = 0; k < t->plan.bands; k++) {
                    t->ref[k] *= g;
                }
                t->v_ref_db += t->st_pend_db;
                t->usable_hz = pnsq_participating(&t->plan, t->ref, t->v_ref_db, t->part);
                t->st_at_ref = 1;
            } else if (t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL) {
                t->state = DSD_PCM_NOISE_SQUELCH_KNOWN;
            }
            t->st_pending = 0;
        }
    }
    /* Stale quieting: open on this stretch for 5 s with the voice band stationary throughout. */
    if (t->gate_open && pnsq_has_reference(t)) {
        t->st_on++;
        t->st_os += v_steady ? 1 : 0;
        if (t->st_on >= PNSQ_STALE_WINDOWS && (double)t->st_os >= k_stale_steady_fraction * (double)t->st_on) {
            pnsq_take_reference(t, sp, sv);
            t->st_on = 0;
            t->st_os = 0;
            t->st_at_ref = 1;
        }
    }
    if (pnsq_has_reference(t) && !t->st_pending && sa >= pnsq_total_db(&t->plan, t->ref) - k_steady_db) {
        for (int k = 0; k < t->plan.bands; k++) {
            t->ref[k] += k_track * (sp[k] - t->ref[k]);
        }
        t->v_ref_db += k_track * (sv - t->v_ref_db);
        t->st_a_db += k_track * (sa - t->st_a_db);
        t->usable_hz = pnsq_participating(&t->plan, t->ref, t->v_ref_db, t->part);
    }
}

/* One window's band powers @p p, voice power @p v and input mean square @p e. */
static void
pnsq_window(dsd_pcm_noise_squelch* t, const double* p, double v, double e) {
    const dsd_pcm_noise_squelch_plan* plan = &t->plan;
    t->windows++;
    if (!(e > k_min_power)) {
        /* Digital silence: closed, and nothing learned. A gap ends the stretch, as a restart does, so the next
           transmission inherits no open time or pending gain step from the one before it. */
        t->run_len = 0;
        t->nb = 0;
        t->gate_open = 0;
        t->cand_n = 0;
        t->exit_n = 0;
        t->have_stretch = 0;
        t->st_pending = 0;
        return;
    }
    const int slot = t->run_pos;
    for (int k = 0; k < plan->bands; k++) {
        t->run_p[slot][k] = p[k];
    }
    t->run_v[slot] = v;
    t->run_a_db[slot] = pnsq_total_db(plan, p);
    t->run_v_db[slot] = pnsq_db(v);
    t->run_pos = (t->run_pos + 1) % DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS;
    t->run_len++;
    if (t->run_len >= DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS) {
        double sp[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
        double v_sum = 0.0;
        for (int k = 0; k < plan->bands; k++) {
            double acc = 0.0;
            for (int i = 0; i < DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS; i++) {
                acc += t->run_p[i][k];
            }
            sp[k] = acc / (double)DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS;
        }
        for (int i = 0; i < DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS; i++) {
            v_sum += t->run_v[i];
        }
        const double sa = pnsq_total_db(plan, sp);
        const double sv = pnsq_db(v_sum / (double)DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS);
        const int v_steady = pnsq_steady(t->run_v_db);
        /* No-band evidence: at the reference (or before one), the voice band stationary and too little of the band
           carrying anything within k_ratio_max_db of it per Hz. */
        unsigned char run_part[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
        const int thin = pnsq_participating(plan, sp, sv, run_part) < k_min_band_hz;
        const int at_ceiling = !pnsq_has_reference(t) || sa >= pnsq_total_db(plan, t->ref) - k_step_db;
        t->nb = (v_steady && thin && at_ceiling) ? t->nb + 1 : 0;
        if (t->nb >= PNSQ_NO_BAND_WINDOWS) {
            t->state = DSD_PCM_NOISE_SQUELCH_NO_BAND;
        }
        const int steady = pnsq_steady(t->run_a_db);
        /* Leaving NO_BAND: a band, and a stationary voice band, for 1 s in a row. */
        t->exit_n = (t->state == DSD_PCM_NOISE_SQUELCH_NO_BAND && steady && v_steady && !thin) ? t->exit_n + 1 : 0;
        if (t->exit_n >= PNSQ_NO_BAND_WINDOWS) {
            pnsq_take_reference(t, sp, sv);
            t->state = DSD_PCM_NOISE_SQUELCH_PROVISIONAL;
            t->have_stretch = 0;
            t->have_cand = 1;
            t->cand_a_db = sa;
            t->cand_n = PNSQ_CONFIRM_WINDOWS;
            t->exit_n = 0;
        }
        const int is_new = steady && (!t->have_stretch || fabs(sa - t->st_a_db) >= k_step_db);
        if (is_new) {
            if (t->have_cand && fabs(sa - t->cand_a_db) < k_step_db) {
                t->cand_n++;
            } else {
                t->have_cand = 1;
                t->cand_a_db = sa;
                t->cand_n = 1;
            }
        } else {
            t->have_cand = 0;
            t->cand_n = 0;
        }
        if (steady && !(is_new && t->cand_n < PNSQ_CONFIRM_WINDOWS)) {
            if (is_new) {
                pnsq_transition(t, sp, sa, sv, v_steady);
            }
            pnsq_continue(t, sp, sa, sv, v_steady);
        }
    }
    if (pnsq_has_reference(t) && t->usable_hz < k_min_band_hz) {
        /* A reference with too little band taking part does not gate: shut until the no-band evidence arrives. */
        t->gate_open = 0;
    } else if (pnsq_has_reference(t)) {
        double ref_sum = 0.0;
        for (int k = 0; k < plan->sub_bands; k++) {
            if (t->part[k]) {
                ref_sum += t->ref[k];
            }
        }
        const double q =
            dsd_noise_squelch_bank_quieting_db(t->ref, ref_sum, p, t->part, plan->sub_bands, plan->bands, k_guard_db);
        t->quieting_db = q;
        t->quieting_valid = 1;
        t->gate_open = t->gate_open ? (q >= t->close_db) : (q >= t->open_db);
    } else {
        t->gate_open = t->state == DSD_PCM_NOISE_SQUELCH_NO_BAND ? 1 : 0;
    }
}

/* A half-window ends: with the one before it, it makes a window. */
static void
pnsq_close_half(dsd_pcm_noise_squelch* t) {
    if (t->have_prev) {
        const double n = t->prev_n + t->half_n;
        double p[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
        for (int k = 0; k < t->plan.bands; k++) {
            p[k] = (t->prev_e[k] + t->half_e[k]) / n;
        }
        pnsq_window(t, p, (t->prev_v + t->half_v) / n, (t->prev_x + t->half_x) / n);
    }
    DSD_MEMCPY(t->prev_e, t->half_e, sizeof(t->prev_e));
    DSD_MEMSET(t->half_e, 0, sizeof(t->half_e));
    t->prev_v = t->half_v;
    t->prev_x = t->half_x;
    t->prev_n = t->half_n;
    t->half_v = 0.0;
    t->half_x = 0.0;
    t->half_n = 0.0;
    t->have_prev = 1;
    pnsq_flush_state(t);
}

void
dsd_pcm_noise_squelch_process(dsd_pcm_noise_squelch* t, const float* pcm, int count, uint8_t* flags) {
    if (!t || !pcm || count <= 0) {
        return;
    }
    if (!t->plan.valid) {
        if (flags) {
            DSD_MEMSET(flags, 0, (size_t)count);
        }
        return;
    }
    for (int i = 0; i < count; i++) {
        if (flags) {
            flags[i] = (pnsq_available(t) && !t->gate_open) ? (uint8_t)DSD_SQUELCH_FLAG_CLOSED : 0U;
        }
        const double x = (double)pcm[i];
        for (int k = 0; k < t->plan.bands; k++) {
            const double y = dsd_noise_squelch_bank_run(t->plan.section[k], t->s1[k], t->s2[k], x);
            t->half_e[k] += y * y;
        }
        const double vy = dsd_noise_squelch_bank_run(t->plan.voice, t->v1, t->v2, x);
        t->half_v += vy * vy;
        t->half_x += x * x;
        t->half_n += 1.0;
        t->samples++;
        if (t->samples >= t->half_end) {
            pnsq_close_half(t);
            t->half_index++;
            t->half_end = pnsq_half_end(t, t->half_index);
        }
    }
}

void
dsd_pcm_noise_squelch_get_status(const dsd_pcm_noise_squelch* t, dsd_pcm_noise_squelch_status* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!t || !t->plan.valid) {
        out->state = DSD_PCM_NOISE_SQUELCH_NO_ROOM;
        out->gate_open = 1;
        return;
    }
    out->state = t->state;
    out->available = pnsq_available(t);
    out->gate_open = out->available ? t->gate_open : 1;
    out->quieting_valid = t->quieting_valid;
    out->quieting_db = t->quieting_db;
    out->usable_hz = pnsq_has_reference(t) ? t->usable_hz : 0.0;
    out->windows = t->windows;
}
