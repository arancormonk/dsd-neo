// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The PCM noise squelch's band, learned reference and gate (issue #628); see pcm_noise_squelch.h and
   tools/pcm_noise_squelch_model.py, whose run_learner() this follows step for step. */

#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
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
/* The voice band's parts the learner compares with the reference's: their edges, Hz. */
static const double k_voice_part_hz[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS + 1] = {400.0, 800.0, 1300.0, 1900.0, 2600.0};
/* Q = max(Q_sum, Q_max - 6 dB); the gate closes 3 dB under N, and never under 1.5 dB. */
static const double k_guard_db = 6.0;
static const double k_hysteresis_db = 3.0;
static const double k_close_floor_db = 1.5;
/* The learner. */
static const double k_steady_db = 1.5;   /* a run's windows within this of their median */
static const double k_step_db = 4.0;     /* a new level */
static const double k_gain_tol_db = 1.5; /* both bands moved by the same amount: a gain step */
/* A quieter stretch is noise at a lower gain (the source turned down in noise, during a transmission or across a pause)
   only with noise's spectrum against the reference: its voice-to-band ratio within k_shape_tol_db (noise at another
   gain keeps it within +/-1 dB, a dead carrier reads 6.5-11.5 dB lower, speech and tones swing or sit higher), each
   voice part moved within k_shape_part_tol_db as far as the band (a quieted carrier's noise falls toward the low parts,
   14 dB and more at 400-800 Hz; a tone or speech fills some parts and not others), and its sub-bands under the
   reference tilted by at most k_shape_tilt_db end to end. The band's move is its sub-bands' median and their spread is
   not asked: a spur added after the source's volume stands out of its sub-bands once the noise is turned down. A gain
   step is taken after PNSQ_GAIN_HOLD_WINDOWS,
   any other after PNSQ_LOWER_HOLD_WINDOWS, most of them with that spectrum and the voice band steady. */
static const double k_shape_tol_db = 1.5;
static const double k_shape_part_tol_db = 2.5;
static const double k_shape_tilt_db = 2.5;
static const double k_tilt_trim_db = 3.0; /* the tilt's line leaves out sub-bands this far off the median move */
/* A downward step is refuted by a voice band clearly louder than noise's: by this many times the shape tolerances. */
static const double k_refute_factor = 2.0;
static const double k_ratio_max_db = 30.0; /* a sub-band this far under the voice band per Hz carries nothing */
static const double k_gain_steady_fraction = 0.75;
static const double k_stale_steady_fraction = 0.9;
static const double k_track = 1.0 / 50.0; /* a 1 s time constant at a window every 20 ms */

enum {
    PNSQ_HALVES_PER_S = 50,
    PNSQ_NO_BAND_WINDOWS = 50, /* 1 s */
    PNSQ_CONFIRM_WINDOWS = 3,
    PNSQ_GAIN_HOLD_WINDOWS = 10,
    PNSQ_LOWER_HOLD_WINDOWS = 20, /* 0.4 s */
    PNSQ_STALE_WINDOWS = 250,     /* 5 s */
    PNSQ_REFUTE_RUNS = 10,        /* a step is refuted by PNSQ_REFUTE_MIN of the last PNSQ_REFUTE_RUNS runs at it */
    PNSQ_REFUTE_MIN = 8,
    PNSQ_REFUTED_HOLD_WINDOWS = 30, /* 0.6 s: the evidence a stretch needs to step down again after a refutation */
    PNSQ_BACK_HOLD_WINDOWS = 10,    /* 0.2 s: the evidence noise at a refuted step's level needs to take it back */
};

_Static_assert(PNSQ_REFUTE_RUNS < 32, "the refutation runs fit an unsigned int");

static const double k_min_power = DSD_NOISE_SQUELCH_BANK_MIN_POWER;
/* Filter state this small is flushed to zero at each half-window (digital silence would otherwise go denormal). */
static const double k_state_flush = 1e-150;
/* Relative tolerance for plans that decide the same way. */
static const double k_plan_same = 1e-12;

/* A steady run's levels: the means over its RUN_WINDOWS windows. */
typedef struct {
    double sp[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];    /* band-pass powers */
    double sa;                                     /* the above band, dB */
    double sv;                                     /* the voice band, dB */
    double svp[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS]; /* its parts, dB */
    int v_steady;                                  /* the voice band stationary over the run */
} pnsq_run;

/* A reference a run is measured against: the current one, or a step a refutation undid. */
typedef struct {
    const double* ref;
    const unsigned char* part; /* its participating band-passes */
    double v_ref_db;
    const double* vp_ref_db;
} pnsq_against;

/* A run against the reference. */
typedef struct {
    int shaped;  /* noise's spectrum: what noise at another gain looks like */
    int speech;  /* a voice band clearly louder than noise's: modulation */
    double step; /* the band's move: the participating sub-bands' median, dB */
} pnsq_shape;

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
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        dsd_noise_squelch_bank_design_band_pass(k_voice_part_hz[j], k_voice_part_hz[j + 1], fs, out->voice_part[j]);
    }
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

_Static_assert(DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS <= DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS, "a run's median fits");

/* The median of @p n (1..MAX_SUB_BANDS) values: the middle one, or the mean of the middle two. */
static double
pnsq_median(const double* x, int n) {
    double v[DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS];
    for (int i = 0; i < n; i++) {
        v[i] = x[i];
    }
    for (int i = 1; i < n; i++) {
        const double t = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > t) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = t;
    }
    return 0.5 * (v[(n - 1) / 2] + v[n / 2]);
}

/* Whether RUN_WINDOWS values all sit within k_steady_db of their median. */
static int
pnsq_steady(const double* x) {
    const double med = pnsq_median(x, DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS);
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
pnsq_take_reference(dsd_pcm_noise_squelch* t, const double* p, double v_db, const double* vp_db) {
    for (int k = 0; k < t->plan.bands; k++) {
        t->ref[k] = p[k];
    }
    t->v_ref_db = v_db;
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        t->vp_ref_db[j] = vp_db[j];
    }
    t->usable_hz = pnsq_participating(&t->plan, t->ref, t->v_ref_db, t->part);
    /* A new reference ends any held step, and any refuted one. */
    t->prior.valid = 0;
    t->alt.valid = 0;
}

/* The run @p r becomes the reference. */
static void
pnsq_take_run(dsd_pcm_noise_squelch* t, const pnsq_run* r) {
    pnsq_take_reference(t, r->sp, r->sv, r->svp);
}

/* The current reference, held in @p h. */
static void
pnsq_keep(const dsd_pcm_noise_squelch* t, dsd_pcm_noise_squelch_held* h) {
    DSD_MEMCPY(h->ref, t->ref, sizeof(h->ref));
    h->v_ref_db = t->v_ref_db;
    DSD_MEMCPY(h->vp_ref_db, t->vp_ref_db, sizeof(h->vp_ref_db));
    h->valid = 1;
}

/* Hold the reference as the one before a downward step, unless a step is held already (the reference from before the
   first stands). */
static void
pnsq_hold_prior(dsd_pcm_noise_squelch* t) {
    if (!t->prior.valid) {
        pnsq_keep(t, &t->prior);
    }
}

static void
pnsq_flush_sections(double* s) {
    for (int i = 0; i < DSD_NOISE_SQUELCH_SECTIONS; i++) {
        if (fabs(s[i]) < k_state_flush) {
            s[i] = 0.0;
        }
    }
}

static void
pnsq_flush_state(dsd_pcm_noise_squelch* t) {
    for (int k = 0; k < t->plan.bands; k++) {
        pnsq_flush_sections(t->s1[k]);
        pnsq_flush_sections(t->s2[k]);
    }
    pnsq_flush_sections(t->v1);
    pnsq_flush_sections(t->v2);
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        pnsq_flush_sections(t->vp1[j]);
        pnsq_flush_sections(t->vp2[j]);
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
    DSD_MEMSET(t->vp1, 0, sizeof(t->vp1));
    DSD_MEMSET(t->vp2, 0, sizeof(t->vp2));
    DSD_MEMSET(t->half_e, 0, sizeof(t->half_e));
    DSD_MEMSET(t->prev_e, 0, sizeof(t->prev_e));
    DSD_MEMSET(t->half_vp, 0, sizeof(t->half_vp));
    DSD_MEMSET(t->prev_vp, 0, sizeof(t->prev_vp));
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
    t->v_ref_db = 0.0;
    DSD_MEMSET(t->vp_ref_db, 0, sizeof(t->vp_ref_db));
    t->usable_hz = 0.0;
    t->prior.valid = 0;
    t->alt.valid = 0;
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
    DSD_MEMCPY(e->vp_ref_db, t->vp_ref_db, sizeof(e->vp_ref_db));
    e->prior = t->prior;
    e->alt = t->alt;
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
            pnsq_take_reference(t, e->ref, e->v_ref_db, e->vp_ref_db);
            /* A step the entry held is still held, with the reference from before it, as is a step refuted. */
            t->prior = e->prior;
            t->alt = e->alt;
        }
    } else if (t->plan.valid) {
        t->state = DSD_PCM_NOISE_SQUELCH_LEARNING;
        DSD_MEMSET(t->ref, 0, sizeof(t->ref));
        DSD_MEMSET(t->part, 0, sizeof(t->part));
        t->usable_hz = 0.0;
        t->prior.valid = 0;
        t->alt.valid = 0;
    }
    dsd_pcm_noise_squelch_restart(t);
}

/* ---------------------------------------------------------------------------------------------- the learner */

static void
pnsq_new_stretch(dsd_pcm_noise_squelch* t, double sa, int pending) {
    t->have_stretch = 1;
    t->st_a_db = sa;
    t->st_v_sum = 0.0;
    t->st_n = 0;
    t->st_vs = 0;
    t->st_at_ref = pnsq_has_reference(t) && sa >= pnsq_total_db(&t->plan, t->ref) - k_step_db;
    t->st_pending = pending;
    t->st_pn = 0;
    t->st_ps = 0;
    t->st_psum = 0.0;
    t->st_on = 0;
    t->st_os = 0;
    t->st_acc = 0;
    t->st_ring = 0U;
    t->st_ring_n = 0;
    t->st_refutes = 0;
    t->st_bn = 0;
    t->st_bs = 0;
}

/* The tilt of the moves @p d at sub-bands @p x (n of them, n >= 3) end to end: a least-squares line across those within
   k_tilt_trim_db of the median move @p med (all when fewer than three are), so a spur in one or two sub-bands, at an
   edge as anywhere, is left out while a carrier's smoothly rising noise is not. */
static double
pnsq_tilt(const double* d, const double* x, int n, double med, int k_count) {
    int kept = 0;
    for (int i = 0; i < n; i++) {
        kept += (fabs(d[i] - med) <= k_tilt_trim_db) ? 1 : 0;
    }
    const int trim = kept >= 3;
    double xm = 0.0;
    double dm = 0.0;
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!trim || fabs(d[i] - med) <= k_tilt_trim_db) {
            xm += x[i];
            dm += d[i];
            m++;
        }
    }
    xm /= (double)m;
    dm /= (double)m;
    double sxy = 0.0;
    double sxx = 0.0;
    for (int i = 0; i < n; i++) {
        if (!trim || fabs(d[i] - med) <= k_tilt_trim_db) {
            sxy += (x[i] - xm) * (d[i] - dm);
            sxx += (x[i] - xm) * (x[i] - xm);
        }
    }
    return sxx > 0.0 ? (sxy / sxx) * (double)(k_count - 1) : 0.0;
}

/* The sub-bands of @p sp against the reference @p a, over its participating sub-bands: their median move (dB) in
   @p step, and in @p level whether they tilt by at most k_shape_tilt_db end to end. Returns 0 with fewer than three
   taking part. */
static int
pnsq_band_step(const dsd_pcm_noise_squelch_plan* plan, const pnsq_against* a, const double* sp, double* step,
               int* level) {
    const int k_count = plan->sub_bands;
    double d[DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS];
    double x[DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS];
    int n = 0;
    for (int k = 0; k < k_count; k++) {
        if (a->part[k]) {
            d[n] = pnsq_db(sp[k]) - pnsq_db(a->ref[k]);
            x[n] = (double)k;
            n++;
        }
    }
    if (n < 3) {
        return 0;
    }
    *step = pnsq_median(d, n);
    *level = fabs(pnsq_tilt(d, x, n, *step, k_count)) <= k_shape_tilt_db;
    return 1;
}

/* The run @p r against the reference @p a: noise's spectrum (the voice band and each of its parts moved as far as the
   band, whose sub-bands do not tilt), and a voice band clearly louder than noise's (modulation; a carrier's own noise
   reads lower). */
static void
pnsq_shape_against(const dsd_pcm_noise_squelch_plan* plan, const pnsq_against* a, const pnsq_run* r, pnsq_shape* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    int level = 0;
    if (!pnsq_band_step(plan, a, r->sp, &out->step, &level)) {
        return;
    }
    const double dv = (r->sv - a->v_ref_db) - out->step;
    double dp_abs = 0.0;
    double dp_up = -HUGE_VAL;
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        const double e = (r->svp[j] - a->vp_ref_db[j]) - out->step;
        dp_abs = fabs(e) > dp_abs ? fabs(e) : dp_abs;
        dp_up = e > dp_up ? e : dp_up;
    }
    out->shaped = level && fabs(dv) <= k_shape_tol_db && dp_abs <= k_shape_part_tol_db;
    out->speech = dv > k_refute_factor * k_shape_tol_db || dp_up > k_refute_factor * k_shape_part_tol_db;
}

/* The run @p r against the reference. */
static void
pnsq_shape_of(const dsd_pcm_noise_squelch* t, const pnsq_run* r, pnsq_shape* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (pnsq_has_reference(t)) {
        const pnsq_against a = {t->ref, t->part, t->v_ref_db, t->vp_ref_db};
        pnsq_shape_against(&t->plan, &a, r, out);
    }
}

/* A transition from the current stretch to the run @p r (@p sh against the reference): whether it is a pending gain
   step. */
static int
pnsq_transition_from(dsd_pcm_noise_squelch* t, const pnsq_run* r, const pnsq_shape* sh) {
    const double da = r->sa - t->st_a_db;
    const double dv = r->sv - pnsq_db(t->st_v_sum / (double)t->st_n);
    const int gain_like = fabs(da - dv) <= k_gain_tol_db;
    const int at_ref = r->sa >= pnsq_total_db(&t->plan, t->ref) - k_step_db;
    if (da > 0.0 && at_ref && (!t->prior.valid || sh->shaped)) {
        /* The band got louder, up at the reference: that side is noise. A shape change (a carrier dropped) confirms
           the reference. While a downward step is held, only with noise's spectrum: a carrier there does not replace
           the reference, it refutes the step (pnsq_refute()). */
        pnsq_take_run(t, r);
        if (!gain_like && t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL) {
            t->state = DSD_PCM_NOISE_SQUELCH_KNOWN;
        }
    } else if (da > 0.0) {
        /* Louder but well under the reference: the carrier's modulation or level changed (speech after a pause, a
           fading carrier), and the reference stays, unless the stretch is noise come back at a lower gain
           (pnsq_lowered()). */
    } else if (gain_like && r->v_steady && t->st_at_ref && t->st_vs * 2 >= t->st_n) {
        /* Quieter, from noise, both bands by the same amount: a gain step, once it holds with noise's spectrum. */
        return 1;
    } else if (t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL) {
        /* A carrier keyed: the louder side, the reference, was noise. */
        t->state = DSD_PCM_NOISE_SQUELCH_KNOWN;
    }
    return 0;
}

/* A transition to the run @p r, a new level held for PNSQ_CONFIRM_WINDOWS. */
static void
pnsq_transition(dsd_pcm_noise_squelch* t, const pnsq_run* r, const pnsq_shape* sh) {
    int pending = 0;
    t->have_cand = 0;
    t->cand_n = 0;
    if (t->state == DSD_PCM_NOISE_SQUELCH_LEARNING) {
        pnsq_take_run(t, r);
        t->state = DSD_PCM_NOISE_SQUELCH_PROVISIONAL;
    } else if (t->state == DSD_PCM_NOISE_SQUELCH_NO_BAND) {
        /* Left only through the no-band exit. */
    } else if (!t->have_stretch) {
        /* A first stretch after a restart louder than the reference is noise; while a downward step is held, only with
           noise's spectrum (a carrier there refutes the step). */
        if (r->sa > pnsq_total_db(&t->plan, t->ref) + k_step_db && (!t->prior.valid || sh->shaped)) {
            pnsq_take_run(t, r);
        }
    } else {
        pending = pnsq_transition_from(t, r, sh);
    }
    pnsq_new_stretch(t, r->sa, pending);
}

/* The reference moves by @p step_db, every band alike. */
static void
pnsq_rescale(dsd_pcm_noise_squelch* t, double step_db) {
    const double g = pow(10.0, step_db / 10.0);
    for (int k = 0; k < t->plan.bands; k++) {
        t->ref[k] *= g;
    }
    t->v_ref_db += step_db;
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        t->vp_ref_db[j] += step_db;
    }
    t->usable_hz = pnsq_participating(&t->plan, t->ref, t->v_ref_db, t->part);
}

/* A downward step of @p step_db: the reference it replaces is held (the one from before the first, on a step already
   held), in case a carrier at the new one refutes it; a step refuted before is dropped. */
static void
pnsq_step_down(dsd_pcm_noise_squelch* t, double step_db) {
    pnsq_hold_prior(t);
    t->alt.valid = 0;
    pnsq_rescale(t, step_db);
    t->st_at_ref = 1;
    t->st_ring = 0U;
    t->st_ring_n = 0;
    t->st_acc = 0;
}

static int
pnsq_bits(unsigned int v) {
    int n = 0;
    for (; v != 0U; v &= v - 1U) {
        n++;
    }
    return n;
}

/* A held step is refuted by a carrier under the gate's threshold against the stepped reference (a run less than N dB
   under it: every run the gate keeps shut, and those its hysteresis still holds open, so a carrier fading toward the
   reference -- under a source's AGC, say -- refutes the step before the gate shuts on it): PNSQ_REFUTE_MIN of the last
   PNSQ_REFUTE_RUNS such runs with a voice band clearly louder than noise's (noise turned down keeps noise's; a carrier
   relaying noise that matched it carries speech), or more than k_steady_db above it without noise's spectrum (noise is
   the ceiling: no carrier reads louder than noise at its gain, and noise turned back up keeps its spectrum). A carrier
   N dB under it refutes nothing. The reference from before the first held step comes back, the step is kept in case
   noise at its level takes it back (pnsq_take_back()), and the stretch steps down again only on a longer hold of
   evidence (pnsq_lowered()). */
static void
pnsq_refute(dsd_pcm_noise_squelch* t, const pnsq_run* r, pnsq_shape* sh) {
    if (!t->prior.valid) {
        return;
    }
    const double ra = pnsq_total_db(&t->plan, t->ref);
    if (r->sa <= ra - t->open_db) {
        return;
    }
    const int evidence = sh->speech || (!sh->shaped && r->sa > ra + k_steady_db);
    const unsigned int mask = (1U << PNSQ_REFUTE_RUNS) - 1U;
    t->st_ring = ((t->st_ring << 1) | (evidence ? 1U : 0U)) & mask;
    t->st_ring_n += t->st_ring_n < PNSQ_REFUTE_RUNS ? 1 : 0;
    if (t->st_ring_n < PNSQ_REFUTE_RUNS || pnsq_bits(t->st_ring) < PNSQ_REFUTE_MIN) {
        return;
    }
    dsd_pcm_noise_squelch_held undone;
    pnsq_keep(t, &undone);
    pnsq_take_reference(t, t->prior.ref, t->prior.v_ref_db, t->prior.vp_ref_db);
    t->alt = undone;
    t->st_refutes++;
    t->st_ring = 0U;
    t->st_ring_n = 0;
    t->st_pending = 0;
    t->st_acc = 0;
    t->st_bn = 0;
    t->st_bs = 0;
    t->st_at_ref = r->sa >= pnsq_total_db(&t->plan, t->ref) - k_step_db;
    sh->shaped = 0;
    sh->speech = 1;
}

/* The runs of evidence taking a refuted step back needs: PNSQ_BACK_HOLD_WINDOWS, doubled for each refutation in the
   stretch (three times at most). */
static int
pnsq_back_hold(const dsd_pcm_noise_squelch* t) {
    int hold = PNSQ_BACK_HOLD_WINDOWS;
    for (int i = 0; i < t->st_refutes && i < 3; i++) {
        hold *= 2;
    }
    return hold;
}

/* A refuted step taken back: the refuting carrier was weak traffic over noise genuinely turned down (or speech over a
   source the stale rule rightly took), and noise comes back at the step's level. Runs within k_steady_db of it gather
   evidence against it; once pnsq_back_hold() of them hold most with its spectrum and the voice band steady, it is the
   reference again, and the one it replaces is held as the step's prior. Modulation against it starts the evidence
   over. */
static void
pnsq_take_back(dsd_pcm_noise_squelch* t, const pnsq_run* r, pnsq_shape* sh) {
    if (!t->alt.valid) {
        return;
    }
    unsigned char alt_part[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    (void)pnsq_participating(&t->plan, t->alt.ref, t->alt.v_ref_db, alt_part);
    const pnsq_against a = {t->alt.ref, alt_part, t->alt.v_ref_db, t->alt.vp_ref_db};
    pnsq_shape ash;
    pnsq_shape_against(&t->plan, &a, r, &ash);
    if (ash.speech) {
        t->st_bn = 0;
        t->st_bs = 0;
        return;
    }
    if (fabs(r->sa - pnsq_total_db(&t->plan, t->alt.ref)) > k_steady_db) {
        return;
    }
    t->st_bn++;
    t->st_bs += (ash.shaped && r->v_steady) ? 1 : 0;
    if (t->st_bn < pnsq_back_hold(t) || (double)t->st_bs < k_gain_steady_fraction * (double)t->st_bn) {
        return;
    }
    dsd_pcm_noise_squelch_held restored;
    pnsq_keep(t, &restored);
    pnsq_take_reference(t, t->alt.ref, t->alt.v_ref_db, t->alt.vp_ref_db);
    t->prior = restored;
    t->st_bn = 0;
    t->st_bs = 0;
    t->st_ring = 0U;
    t->st_ring_n = 0;
    t->st_pending = 0;
    t->st_acc = 0;
    t->st_at_ref = 1;
    *sh = ash;
}

/* The runs of evidence a step down needs: PNSQ_LOWER_HOLD_WINDOWS, or after refutations in the stretch
   PNSQ_REFUTED_HOLD_WINDOWS, doubled for each one after the first (three times at most). */
static int
pnsq_lowered_hold(const dsd_pcm_noise_squelch* t) {
    if (t->st_refutes <= 0) {
        return PNSQ_LOWER_HOLD_WINDOWS;
    }
    int hold = PNSQ_REFUTED_HOLD_WINDOWS;
    for (int i = 1; i < t->st_refutes && i <= 3; i++) {
        hold *= 2;
    }
    return hold;
}

/* Noise come back at a lower gain (the source turned down in noise, during a transmission or across a pause): the
   stretch's runs under the reference's tracking range gather evidence at their level, and the step is taken once
   PNSQ_LOWER_HOLD_WINDOWS or more of them hold most with noise's spectrum and the voice band steady, moved k_steady_db
   or more on average. The evidence is kept rather than tried afresh, so a carrier that only sometimes matches noise
   never gathers enough; a level that moves k_steady_db starts over. After a refutation the hold is
   PNSQ_REFUTED_HOLD_WINDOWS, doubled for each further one in the stretch, and modulation starts the evidence over. */
static void
pnsq_lowered(dsd_pcm_noise_squelch* t, const pnsq_run* r, const pnsq_shape* sh) {
    if (t->st_pending || !pnsq_has_reference(t)) {
        return;
    }
    if (t->st_refutes > 0 && sh->speech) {
        t->st_acc = 0;
        return;
    }
    if (r->sa >= pnsq_total_db(&t->plan, t->ref) - k_steady_db) {
        return;
    }
    if (!t->st_acc || fabs(r->sa - t->st_acc_db) > k_steady_db) {
        t->st_acc = 1;
        t->st_acc_db = r->sa;
        t->st_an = 0;
        t->st_as = 0;
        t->st_asum = 0.0;
    }
    t->st_an++;
    if (r->v_steady && sh->shaped) {
        t->st_as++;
        t->st_asum += sh->step;
    }
    const double mean = t->st_as > 0 ? t->st_asum / (double)t->st_as : 0.0;
    if (t->st_an < pnsq_lowered_hold(t) || (double)t->st_as < k_gain_steady_fraction * (double)t->st_an
        || mean > -k_steady_db) {
        return;
    }
    pnsq_step_down(t, mean);
    if (t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL) {
        /* Noise after a carrier, at its lower gain: the carrier keyed is confirmed. */
        t->state = DSD_PCM_NOISE_SQUELCH_KNOWN;
    }
}

/* A gain step straight out of noise, from its transition: taken after PNSQ_GAIN_HOLD_WINDOWS by the mean step of its
   runs if most held noise's spectrum with the voice band steady; otherwise a carrier keyed. */
static void
pnsq_gain_step(dsd_pcm_noise_squelch* t, const pnsq_run* r, const pnsq_shape* sh) {
    if (!t->st_pending) {
        return;
    }
    t->st_pn++;
    if (r->v_steady && sh->shaped) {
        t->st_ps++;
        t->st_psum += sh->step;
    }
    if (t->st_pn < PNSQ_GAIN_HOLD_WINDOWS) {
        return;
    }
    const double mean = t->st_ps > 0 ? t->st_psum / (double)t->st_ps : 0.0;
    if ((double)t->st_ps >= k_gain_steady_fraction * (double)t->st_pn && mean <= -k_steady_db) {
        pnsq_step_down(t, mean);
    } else if (t->state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL) {
        t->state = DSD_PCM_NOISE_SQUELCH_KNOWN;
    }
    t->st_pending = 0;
}

/* Stale quieting: open on this stretch for 5 s with the voice band stationary throughout. */
static void
pnsq_stale(dsd_pcm_noise_squelch* t, const pnsq_run* r) {
    if (!t->gate_open || !pnsq_has_reference(t)) {
        return;
    }
    t->st_on++;
    t->st_os += r->v_steady ? 1 : 0;
    if (t->st_on >= PNSQ_STALE_WINDOWS && (double)t->st_os >= k_stale_steady_fraction * (double)t->st_on) {
        /* The stretch becomes the reference as a held step: a carrier taken so is refuted, and the reference before it
           comes back, once it shows modulation; noise at the stretch's level after that takes it back
           (pnsq_take_back()). */
        pnsq_hold_prior(t);
        const dsd_pcm_noise_squelch_held prior = t->prior;
        pnsq_take_run(t, r);
        t->prior = prior;
        t->st_on = 0;
        t->st_os = 0;
        t->st_at_ref = 1;
        t->st_ring = 0U;
        t->st_ring_n = 0;
    }
}

/* A run at or above the reference less k_steady_db tracks it (slow drift); while a downward step is held, only one with
   noise's spectrum. */
static void
pnsq_track(dsd_pcm_noise_squelch* t, const pnsq_run* r, const pnsq_shape* sh) {
    if (!pnsq_has_reference(t) || t->st_pending || (t->prior.valid && !sh->shaped)
        || r->sa < pnsq_total_db(&t->plan, t->ref) - k_steady_db) {
        return;
    }
    for (int k = 0; k < t->plan.bands; k++) {
        t->ref[k] += k_track * (r->sp[k] - t->ref[k]);
    }
    t->v_ref_db += k_track * (r->sv - t->v_ref_db);
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        t->vp_ref_db[j] += k_track * (r->svp[j] - t->vp_ref_db[j]);
    }
    t->st_a_db += k_track * (r->sa - t->st_a_db);
    t->usable_hz = pnsq_participating(&t->plan, t->ref, t->v_ref_db, t->part);
}

/* The steady run @p r (@p sh against the reference) continues the stretch: a held step's refutation, a refuted step
   taken back, noise at a lower gain, a pending gain step, stale quieting and slow tracking. */
static void
pnsq_continue(dsd_pcm_noise_squelch* t, const pnsq_run* r, pnsq_shape* sh) {
    t->st_v_sum += pow(10.0, r->sv / 10.0);
    t->st_n++;
    t->st_vs += r->v_steady ? 1 : 0;
    pnsq_refute(t, r, sh);
    pnsq_take_back(t, r, sh);
    pnsq_lowered(t, r, sh);
    pnsq_gain_step(t, r, sh);
    pnsq_stale(t, r);
    pnsq_track(t, r, sh);
}

/* Digital silence: closed, and nothing learned. A gap ends the stretch, as a restart does, so the next transmission
   inherits no open time or pending step from the one before it. */
static void
pnsq_silent_window(dsd_pcm_noise_squelch* t) {
    t->run_len = 0;
    t->nb = 0;
    t->gate_open = 0;
    t->cand_n = 0;
    t->exit_n = 0;
    t->have_stretch = 0;
    t->st_pending = 0;
}

/* Add the window (@p p, @p v, @p vp) to the run. Returns 1 with the run's levels in @p r once it spans RUN_WINDOWS
   windows since the last silent window or restart. */
static int
pnsq_run_push(dsd_pcm_noise_squelch* t, const double* p, double v, const double* vp, pnsq_run* r) {
    const dsd_pcm_noise_squelch_plan* plan = &t->plan;
    const int slot = t->run_pos;
    for (int k = 0; k < plan->bands; k++) {
        t->run_p[slot][k] = p[k];
    }
    t->run_v[slot] = v;
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        t->run_vp[slot][j] = vp[j];
    }
    t->run_a_db[slot] = pnsq_total_db(plan, p);
    t->run_v_db[slot] = pnsq_db(v);
    t->run_pos = (t->run_pos + 1) % DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS;
    t->run_len++;
    if (t->run_len < DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS) {
        return 0;
    }
    DSD_MEMSET(r, 0, sizeof(*r));
    double v_sum = 0.0;
    double vp_sum[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS] = {0.0};
    for (int i = 0; i < DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS; i++) {
        for (int k = 0; k < plan->bands; k++) {
            r->sp[k] += t->run_p[i][k];
        }
        v_sum += t->run_v[i];
        for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
            vp_sum[j] += t->run_vp[i][j];
        }
    }
    for (int k = 0; k < plan->bands; k++) {
        r->sp[k] /= (double)DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS;
    }
    r->sa = pnsq_total_db(plan, r->sp);
    r->sv = pnsq_db(v_sum / (double)DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS);
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        r->svp[j] = pnsq_db(vp_sum[j] / (double)DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS);
    }
    r->v_steady = pnsq_steady(t->run_v_db);
    return 1;
}

/* No-band evidence: at the reference (or before one), the voice band stationary and too little of the band carrying
   anything within k_ratio_max_db of it per Hz. NO_BAND is left after a band and a stationary voice band for 1 s in a
   row. */
static void
pnsq_no_band(dsd_pcm_noise_squelch* t, const pnsq_run* r, int steady) {
    const dsd_pcm_noise_squelch_plan* plan = &t->plan;
    unsigned char run_part[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    const int thin = pnsq_participating(plan, r->sp, r->sv, run_part) < k_min_band_hz;
    const int at_ceiling = !pnsq_has_reference(t) || r->sa >= pnsq_total_db(plan, t->ref) - k_step_db;
    t->nb = (r->v_steady && thin && at_ceiling) ? t->nb + 1 : 0;
    if (t->nb >= PNSQ_NO_BAND_WINDOWS) {
        t->state = DSD_PCM_NOISE_SQUELCH_NO_BAND;
    }
    t->exit_n = (t->state == DSD_PCM_NOISE_SQUELCH_NO_BAND && steady && r->v_steady && !thin) ? t->exit_n + 1 : 0;
    if (t->exit_n >= PNSQ_NO_BAND_WINDOWS) {
        pnsq_take_run(t, r);
        t->state = DSD_PCM_NOISE_SQUELCH_PROVISIONAL;
        t->have_stretch = 0;
        t->have_cand = 1;
        t->cand_a_db = r->sa;
        t->cand_n = PNSQ_CONFIRM_WINDOWS;
        t->exit_n = 0;
    }
}

/* The run's level: a new one is a transition once held for PNSQ_CONFIRM_WINDOWS; every steady run that is not a new
   level still being confirmed continues the stretch. */
static void
pnsq_level(dsd_pcm_noise_squelch* t, const pnsq_run* r, int steady) {
    const int is_new = steady && (!t->have_stretch || fabs(r->sa - t->st_a_db) >= k_step_db);
    if (!is_new) {
        t->have_cand = 0;
        t->cand_n = 0;
    } else if (t->have_cand && fabs(r->sa - t->cand_a_db) < k_step_db) {
        t->cand_n++;
    } else {
        t->have_cand = 1;
        t->cand_a_db = r->sa;
        t->cand_n = 1;
    }
    if (!steady || (is_new && t->cand_n < PNSQ_CONFIRM_WINDOWS)) {
        return;
    }
    pnsq_shape sh;
    pnsq_shape_of(t, r, &sh);
    if (is_new) {
        pnsq_transition(t, r, &sh);
    }
    pnsq_continue(t, r, &sh);
}

/* The gate on the window's band powers @p p against the reference. */
static void
pnsq_gate(dsd_pcm_noise_squelch* t, const double* p) {
    const dsd_pcm_noise_squelch_plan* plan = &t->plan;
    if (!pnsq_has_reference(t)) {
        t->gate_open = t->state == DSD_PCM_NOISE_SQUELCH_NO_BAND ? 1 : 0;
        return;
    }
    if (t->usable_hz < k_min_band_hz) {
        /* A reference with too little band taking part does not gate: shut until the no-band evidence arrives. */
        t->gate_open = 0;
        return;
    }
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
}

/* One window's band powers @p p, voice power @p v, voice-part powers @p vp and input mean square @p e. */
static void
pnsq_window(dsd_pcm_noise_squelch* t, const double* p, double v, const double* vp, double e) {
    t->windows++;
    if (!(e > k_min_power)) {
        pnsq_silent_window(t);
        return;
    }
    pnsq_run run;
    if (pnsq_run_push(t, p, v, vp, &run)) {
        const int steady = pnsq_steady(t->run_a_db);
        pnsq_no_band(t, &run, steady);
        pnsq_level(t, &run, steady);
    }
    pnsq_gate(t, p);
}

/* A half-window ends: with the one before it, it makes a window. */
static void
pnsq_close_half(dsd_pcm_noise_squelch* t) {
    if (t->have_prev) {
        const double n = t->prev_n + t->half_n;
        double p[DSD_PCM_NOISE_SQUELCH_MAX_BANDS] = {0.0};
        double vp[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS] = {0.0};
        for (int k = 0; k < t->plan.bands; k++) {
            p[k] = (t->prev_e[k] + t->half_e[k]) / n;
        }
        for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
            vp[j] = (t->prev_vp[j] + t->half_vp[j]) / n;
        }
        pnsq_window(t, p, (t->prev_v + t->half_v) / n, vp, (t->prev_x + t->half_x) / n);
    }
    DSD_MEMCPY(t->prev_e, t->half_e, sizeof(t->prev_e));
    DSD_MEMSET(t->half_e, 0, sizeof(t->half_e));
    DSD_MEMCPY(t->prev_vp, t->half_vp, sizeof(t->prev_vp));
    DSD_MEMSET(t->half_vp, 0, sizeof(t->half_vp));
    t->prev_v = t->half_v;
    t->prev_x = t->half_x;
    t->prev_n = t->half_n;
    t->half_v = 0.0;
    t->half_x = 0.0;
    t->half_n = 0.0;
    t->have_prev = 1;
    pnsq_flush_state(t);
}

/* One sample through the band-passes into the open half-window. */
static void
pnsq_sample(dsd_pcm_noise_squelch* t, double x) {
    for (int k = 0; k < t->plan.bands; k++) {
        const double y = dsd_noise_squelch_bank_run(t->plan.section[k], t->s1[k], t->s2[k], x);
        t->half_e[k] += y * y;
    }
    const double vy = dsd_noise_squelch_bank_run(t->plan.voice, t->v1, t->v2, x);
    t->half_v += vy * vy;
    for (int j = 0; j < DSD_PCM_NOISE_SQUELCH_VOICE_PARTS; j++) {
        const double py = dsd_noise_squelch_bank_run(t->plan.voice_part[j], t->vp1[j], t->vp2[j], x);
        t->half_vp[j] += py * py;
    }
    t->half_x += x * x;
    t->half_n += 1.0;
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
        pnsq_sample(t, (double)pcm[i]);
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
