// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The floor-relative ("auto") squelch's classifier, floor and gate (issue #518 follow-up); see squelch_floor.h. */

#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Lag rule: the smallest lag at which the channel taps' autocorrelation falls below this share of its peak. */
static const double k_lag_rho_limit = 0.1;

/* Classification windows per second (40 ms) and power sub-windows per second (20 ms, two per window). */
enum { SQF_WINDOWS_PER_S = 25, SQF_SUBS_PER_S = 50 };

/* Normalised thresholds (tools/squelch_model.py "norm/40ms/cdb"). */
static const double k_carrier_x = -7.0;
static const double k_carrier_y = 3.3;
static const double k_noise_x = -3.8;
static const double k_noise_y = 2.1;
/* The gate closes this far below the opening threshold: -3 dB. */
static const double k_close_ratio = 0.50118723362727224;
/* Floor tracking per 40 ms NOISE window: fall with an 80 ms time constant, rise at most 1 dB/s (0.04 dB). */
static const double k_fall_alpha = 0.39346934028736658; /* 1 - exp(-40/80) */
static const double k_rise_step = 1.0092528860766845;   /* 10^(0.04/10) */
/* A floor sharing a seed agrees with NOISE windows within 6 dB; a sustained shift is more than 6 dB above it. */
static const double k_six_db = 3.9810717055349722;
/* A NOISE window this far above the floor (+1.5 dB, beyond any 40 ms window's scatter) means the floor sits low. */
static const double k_hold_min_ratio = 1.4125375446227544;
/* A window whose two sub-windows differ by this much (3 dB) holds a carrier starting or ending: noise in one half
   only. Noise scatters far less (about 10 % per sub-window at the narrowest plans). */
static const double k_unsteady_ratio = 2.0;

/* NOISE windows that confirm a seeded floor; NOISE windows (5 s) a sustained shift needs; a collapse is -20 dB. */
enum { SQF_SEED_AGREE = 3, SQF_SHIFT_WINDOWS = 125 };

static const double k_collapse_ratio = 0.01;
/* Cache: entries older than 30 minutes of sample time are stale; a neighbour is within 5 MHz. */
static const double k_cache_stale_s = 1800.0;
static const int64_t k_cache_neighbour_hz = 5000000;
/* Relative tolerance for plans that classify the same way. */
static const double k_plan_same = 1e-12;
/* A window at or below this mean power holds no signal to classify (exact zeros, a muted capture). */
static const double k_min_power = 1e-30;

static int
sqf_clamp_margin(int margin_db) {
    if (margin_db < 3) {
        return 3;
    }
    if (margin_db > 30) {
        return 30;
    }
    return margin_db;
}

/* R_h(m) for real taps h of length len (0 beyond them). */
static double
sqf_autocorr(const float* h, int len, int m) {
    if (m < 0) {
        m = -m;
    }
    double sum = 0.0;
    for (int k = 0; k + m < len; k++) {
        sum += (double)h[k] * (double)h[k + m];
    }
    return sum;
}

/* 2F1(1/2, 1/2; 2; x) for 0 <= x < 1, by its series. */
static double
sqf_hyp2f1_half_half_2(double x) {
    double term = 1.0;
    double sum = 1.0;
    for (int k = 0; k < 200; k++) {
        const double kk = (double)k;
        term *= ((kk + 0.5) * (kk + 0.5)) / ((kk + 2.0) * (kk + 1.0)) * x;
        sum += term;
        if (fabs(term) < 1e-16 * sum) {
            break;
        }
    }
    return sum;
}

/* The noise autocorrelation r_n(m): white complex noise at twice the rate through the half-band stage (its
   autocorrelation at even lags), then the channel taps. */
static double
sqf_noise_autocorr(const float* taps, int taps_len, const float* hb, int hb_len, int m) {
    if (!hb || hb_len <= 0) {
        return sqf_autocorr(taps, taps_len, m);
    }
    const int hb_span = (hb_len - 1) / 2;
    double sum = 0.0;
    for (int j = -hb_span; j <= hb_span; j++) {
        const double r_in = sqf_autocorr(hb, hb_len, 2 * j);
        sum += sqf_autocorr(taps, taps_len, m - j) * r_in;
    }
    return sum;
}

int
dsd_squelch_floor_plan_design(dsd_squelch_floor_plan* out, const float* taps, int taps_len, const float* hb_taps,
                              int hb_len, int rate_hz) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    /* At least two samples in every 20 ms sub-window. */
    if (rate_hz < 2 * SQF_SUBS_PER_S) {
        return -1;
    }
    static const float k_unit_tap[1] = {1.0f};
    if (!taps || taps_len <= 0) {
        taps = k_unit_tap;
        taps_len = 1;
    }
    const double r0 = sqf_autocorr(taps, taps_len, 0);
    if (!(r0 > 0.0)) {
        return -1;
    }
    int lag = taps_len;
    for (int m = 1; m < taps_len; m++) {
        if (fabs(sqf_autocorr(taps, taps_len, m)) / r0 < k_lag_rho_limit) {
            lag = m;
            break;
        }
    }
    if (taps_len == 1) {
        lag = 1;
    }
    if (lag < 1 || lag > DSD_SQUELCH_FLOOR_MAX_LAG) {
        return -1;
    }
    const int span = (taps_len - 1) + ((hb_taps && hb_len > 0) ? (hb_len - 1) / 2 : 0);
    const double rn0 = sqf_noise_autocorr(taps, taps_len, hb_taps, hb_len, 0);
    if (!(rn0 > 0.0)) {
        return -1;
    }
    double sum_rho2 = 1.0;
    for (int m = 1; m <= span; m++) {
        const double rho = sqf_noise_autocorr(taps, taps_len, hb_taps, hb_len, m) / rn0;
        sum_rho2 += 2.0 * rho * rho;
    }
    const double rho_lag = sqf_noise_autocorr(taps, taps_len, hb_taps, hb_len, lag) / rn0;
    const double window = (double)rate_hz / (double)SQF_WINDOWS_PER_S;
    out->rate_hz = rate_hz;
    out->lag = lag;
    out->rho_lag = rho_lag;
    out->beta = (M_PI / 4.0) * rho_lag * sqf_hyp2f1_half_half_2(rho_lag * rho_lag);
    out->neff = window / sum_rho2;
    out->sqrt_neff = sqrt(out->neff);
    out->noise_gain = rn0;
    out->valid = 1;
    return 0;
}

static int
sqf_same(double a, double b) {
    return fabs(a - b) <= k_plan_same * fmax(1.0, fmax(fabs(a), fabs(b)));
}

int
dsd_squelch_floor_plan_equal(const dsd_squelch_floor_plan* a, const dsd_squelch_floor_plan* b) {
    if (!a || !b) {
        return 0;
    }
    if (a->valid != b->valid) {
        return 0;
    }
    if (!a->valid) {
        return 1;
    }
    return a->rate_hz == b->rate_hz && a->lag == b->lag && sqf_same(a->beta, b->beta)
           && sqf_same(a->sqrt_neff, b->sqrt_neff) && sqf_same(a->noise_gain, b->noise_gain);
}

/* The sample count at which power sub-window @p index (from 0) ends. */
static uint64_t
sqf_sub_end(const dsd_squelch_floor* t, uint64_t index) {
    return ((index + 1U) * (uint64_t)t->plan.rate_hz) / (uint64_t)SQF_SUBS_PER_S;
}

/* Empty the windows and the lag history and start the boundaries over (the floor and the gate stay). */
static void
sqf_restart_windows(dsd_squelch_floor* t) {
    t->hist_pos = 0;
    t->hist_fill = 0;
    t->samples = 0U;
    t->sub_index = 0U;
    t->sub_end = t->plan.valid ? sqf_sub_end(t, 0U) : 0U;
    t->w_n = t->w_sa = t->w_sa2 = t->w_su_re = t->w_su_im = 0.0;
    t->s_n = t->s_sa = 0.0;
    t->sub_power = 0.0;
    t->first_sub_power = 0.0;
}

static void
sqf_clear_learning(dsd_squelch_floor* t) {
    DSD_MEMSET(t->learn_class, 0, sizeof(t->learn_class));
    DSD_MEMSET(t->learn_power, 0, sizeof(t->learn_power));
    t->learn_pos = 0;
    t->learn_count = 0;
}

/* LEARNING again: no floor, gate closed until a CARRIER window. */
static void
sqf_relearn(dsd_squelch_floor* t) {
    t->state = DSD_SQUELCH_FLOOR_LEARNING;
    t->floor_power = 0.0;
    t->agree = 0;
    t->shift_windows = 0;
    t->gate_open = 0;
    t->noise_hold = 0;
    sqf_clear_learning(t);
}

void
dsd_squelch_floor_reset(dsd_squelch_floor* t) {
    if (!t) {
        return;
    }
    sqf_restart_windows(t);
    sqf_relearn(t);
    t->last_class = DSD_SQUELCH_WINDOW_UNDECIDED;
    t->window_power = 0.0;
    t->windows = 0U;
}

void
dsd_squelch_floor_restart_windows(dsd_squelch_floor* t) {
    if (t) {
        sqf_restart_windows(t);
    }
}

void
dsd_squelch_floor_set_margin(dsd_squelch_floor* t, int margin_db) {
    if (!t) {
        return;
    }
    t->margin_db = sqf_clamp_margin(margin_db);
    t->margin_lin = pow(10.0, (double)t->margin_db / 10.0);
}

void
dsd_squelch_floor_set_plan(dsd_squelch_floor* t, const dsd_squelch_floor_plan* plan) {
    if (!t || !plan || dsd_squelch_floor_plan_equal(&t->plan, plan)) {
        return;
    }
    const dsd_squelch_floor_plan old = t->plan;
    t->plan = *plan;
    if (t->margin_lin <= 0.0) {
        dsd_squelch_floor_set_margin(t, 10);
    }
    sqf_restart_windows(t);
    sqf_clear_learning(t);
    if (t->state != DSD_SQUELCH_FLOOR_LEARNING && old.valid && plan->valid && old.noise_gain > 0.0) {
        t->floor_power *= plan->noise_gain / old.noise_gain;
    } else if (t->state != DSD_SQUELCH_FLOOR_LEARNING) {
        sqf_relearn(t);
    }
}

void
dsd_squelch_floor_seed(dsd_squelch_floor* t, double floor_power, int provisional) {
    if (!t || !(floor_power > 0.0)) {
        return;
    }
    sqf_relearn(t);
    t->state = provisional ? DSD_SQUELCH_FLOOR_PROVISIONAL : DSD_SQUELCH_FLOOR_SEEDED;
    t->floor_power = floor_power;
}

int
dsd_squelch_floor_classify(const dsd_squelch_floor_plan* plan, double n, double sum_a, double sum_a2, double su_re,
                           double su_im) {
    if (!plan || !plan->valid || n < 2.0) {
        return DSD_SQUELCH_WINDOW_UNDECIDED;
    }
    const double p = sum_a / n;
    if (!(p > k_min_power)) {
        return DSD_SQUELCH_WINDOW_UNDECIDED;
    }
    const double cv2 = (sum_a2 / n) / (p * p) - 1.0;
    const double x = (cv2 - 1.0) * plan->sqrt_neff;
    const double y = hypot(su_re / n - plan->beta, su_im / n) * plan->sqrt_neff;
    if (x <= k_carrier_x || y >= k_carrier_y) {
        return DSD_SQUELCH_WINDOW_CARRIER;
    }
    if (x >= k_noise_x && y <= k_noise_y) {
        return DSD_SQUELCH_WINDOW_NOISE;
    }
    return DSD_SQUELCH_WINDOW_UNDECIDED;
}

/* With a floor, the power gate on the last 20 ms sub-window. */
static void
sqf_power_gate(dsd_squelch_floor* t) {
    if (t->state == DSD_SQUELCH_FLOOR_LEARNING) {
        return;
    }
    const double p = t->sub_power;
    const double open_at = t->floor_power * t->margin_lin;
    if (!t->noise_hold && p >= open_at) {
        t->gate_open = 1;
    } else if (t->gate_open && p < open_at * k_close_ratio) {
        t->gate_open = 0;
    }
}

/* While LEARNING: remember the window, open for CARRIER only, and take a floor from enough NOISE windows. */
static void
sqf_learning_window(dsd_squelch_floor* t, int cls, double p) {
    t->learn_class[t->learn_pos] = (uint8_t)cls;
    t->learn_power[t->learn_pos] = p;
    t->learn_pos = (t->learn_pos + 1) % DSD_SQUELCH_FLOOR_LEARN_WINDOWS;
    if (t->learn_count < DSD_SQUELCH_FLOOR_LEARN_WINDOWS) {
        t->learn_count++;
    }
    t->gate_open = cls == DSD_SQUELCH_WINDOW_CARRIER;
    int noise = 0;
    double sum = 0.0;
    for (int i = 0; i < t->learn_count; i++) {
        if (t->learn_class[i] == DSD_SQUELCH_WINDOW_NOISE) {
            noise++;
            sum += t->learn_power[i];
        }
    }
    if (noise >= DSD_SQUELCH_FLOOR_LEARN_NEED && sum > 0.0) {
        t->state = DSD_SQUELCH_FLOOR_KNOWN;
        t->floor_power = sum / (double)noise;
        t->agree = 0;
        t->shift_windows = 0;
        t->noise_hold = 0;
        sqf_clear_learning(t);
    }
}

/* With a floor: a NOISE window tracks it (and confirms a seed), and closes the gate. Returns 0 when it started
   LEARNING again. */
static int
sqf_noise_window(dsd_squelch_floor* t, double p) {
    const double floor = t->floor_power;
    if (p < floor * k_collapse_ratio) {
        sqf_relearn(t);
        return 0;
    }
    if (t->state == DSD_SQUELCH_FLOOR_SEEDED || t->state == DSD_SQUELCH_FLOOR_PROVISIONAL) {
        if (p <= floor * k_six_db && p * k_six_db >= floor) {
            if (++t->agree >= SQF_SEED_AGREE) {
                t->state = DSD_SQUELCH_FLOOR_KNOWN;
            }
        } else {
            sqf_relearn(t);
            return 0;
        }
    }
    if (p > floor * k_six_db) {
        if (++t->shift_windows >= SQF_SHIFT_WINDOWS) {
            sqf_relearn(t);
            return 0;
        }
    } else {
        t->shift_windows = 0;
    }
    if (p < floor) {
        t->floor_power = floor + ((p - floor) * k_fall_alpha);
    } else {
        t->floor_power = fmin(p, floor * k_rise_step);
    }
    /* Noise: closed whatever its power. Noise that reads within 3 dB of the opening level, and clearly above the
       floor, means the floor has yet to rise to it: its sub-windows would keep reopening the gate, so hold it shut
       until a CARRIER window. */
    t->gate_open = 0;
    t->noise_hold = p >= t->floor_power * fmax(t->margin_lin * k_close_ratio, k_hold_min_ratio);
    return 1;
}

/* A 40 ms classification window closed. */
static void
sqf_window_end(dsd_squelch_floor* t) {
    int cls = dsd_squelch_floor_classify(&t->plan, t->w_n, t->w_sa, t->w_sa2, t->w_su_re, t->w_su_im);
    const double p = t->w_n > 0.0 ? t->w_sa / t->w_n : 0.0;
    /* A carrier starting or ending inside the window: its noise half says nothing about the floor or the gate. */
    if (cls == DSD_SQUELCH_WINDOW_NOISE
        && (t->sub_power >= t->first_sub_power * k_unsteady_ratio
            || t->first_sub_power >= t->sub_power * k_unsteady_ratio)) {
        cls = DSD_SQUELCH_WINDOW_UNDECIDED;
    }
    t->last_class = cls;
    t->window_power = p;
    t->windows++;
    if (t->state == DSD_SQUELCH_FLOOR_LEARNING) {
        sqf_learning_window(t, cls, p);
    } else if (cls == DSD_SQUELCH_WINDOW_NOISE) {
        (void)sqf_noise_window(t, p);
    } else if (cls == DSD_SQUELCH_WINDOW_CARRIER) {
        /* A carrier releases a hold, and the sub-window that ended with this window decides now. */
        t->noise_hold = 0;
        sqf_power_gate(t);
    }
    t->w_n = t->w_sa = t->w_sa2 = t->w_su_re = t->w_su_im = 0.0;
}

void
dsd_squelch_floor_process(dsd_squelch_floor* t, const float* iq, int count, uint8_t* flags) {
    if (!t || !iq || count <= 0) {
        return;
    }
    if (!t->plan.valid) {
        if (flags) {
            DSD_MEMSET(flags, 0, (size_t)count);
        }
        return;
    }
    const int lag = t->plan.lag;
    for (int i = 0; i < count; i++) {
        if (flags) {
            flags[i] = t->gate_open ? 0U : (uint8_t)DSD_SQUELCH_FLAG_CLOSED;
        }
        const size_t at = (size_t)i * 2U;
        const double re = (double)iq[at];
        const double im = (double)iq[at + 1U];
        const double a = (re * re) + (im * im);
        t->w_n += 1.0;
        t->w_sa += a;
        t->w_sa2 += a * a;
        t->s_n += 1.0;
        t->s_sa += a;
        if (t->hist_fill >= lag) {
            const double pr = (double)t->hist_re[t->hist_pos];
            const double pi = (double)t->hist_im[t->hist_pos];
            const double mag = sqrt(a * ((pr * pr) + (pi * pi)));
            if (mag > 0.0) {
                t->w_su_re += ((re * pr) + (im * pi)) / mag;
                t->w_su_im += ((im * pr) - (re * pi)) / mag;
            }
        }
        t->hist_re[t->hist_pos] = iq[at];
        t->hist_im[t->hist_pos] = iq[at + 1U];
        t->hist_pos = (t->hist_pos + 1) % lag;
        if (t->hist_fill < lag) {
            t->hist_fill++;
        }
        t->samples++;
        if (t->samples >= t->sub_end) {
            t->sub_power = t->s_sa / t->s_n;
            if ((t->sub_index % 2U) == 0U) {
                t->first_sub_power = t->sub_power;
            }
            sqf_power_gate(t);
            t->s_n = t->s_sa = 0.0;
            t->sub_index++;
            if ((t->sub_index % 2U) == 0U) {
                sqf_window_end(t);
            }
            t->sub_end = sqf_sub_end(t, t->sub_index);
        }
    }
}

void
dsd_squelch_floor_get_status(const dsd_squelch_floor* t, dsd_squelch_floor_status* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!t) {
        return;
    }
    out->state = t->state;
    out->gate_open = t->plan.valid ? t->gate_open : 1;
    out->last_class = t->last_class;
    out->floor_power = t->state == DSD_SQUELCH_FLOOR_LEARNING ? 0.0 : t->floor_power;
    out->window_power = t->window_power;
    out->windows = t->windows;
}

int
dsd_squelch_floor_key_equal(const dsd_squelch_floor_key* a, const dsd_squelch_floor_key* b) {
    return a && b && a->freq_hz == b->freq_hz && a->gain == b->gain && a->tuner_agc == b->tuner_agc
           && a->bias == b->bias && a->device == b->device && a->rate_hz == b->rate_hz && a->chain == b->chain;
}

/* Everything but the frequency matches. */
static int
sqf_key_neighbour(const dsd_squelch_floor_key* a, const dsd_squelch_floor_key* b) {
    return a->gain == b->gain && a->tuner_agc == b->tuner_agc && a->bias == b->bias && a->device == b->device
           && a->rate_hz == b->rate_hz && a->chain == b->chain;
}

void
dsd_squelch_floor_cache_advance(dsd_squelch_floor_cache* c, double seconds) {
    if (c && seconds > 0.0) {
        c->clock_s += seconds;
    }
}

static int
sqf_cache_fresh(const dsd_squelch_floor_cache* c, const dsd_squelch_floor_cache_entry* e) {
    return e->used != 0U && (c->clock_s - e->stamp_s) <= k_cache_stale_s;
}

void
dsd_squelch_floor_cache_store(dsd_squelch_floor_cache* c, const dsd_squelch_floor_key* key, double density) {
    if (!c || !key || !(density > 0.0)) {
        return;
    }
    int slot = -1;
    for (int i = 0; i < DSD_SQUELCH_FLOOR_CACHE_SIZE; i++) {
        if (c->entries[i].used != 0U && dsd_squelch_floor_key_equal(&c->entries[i].key, key)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < DSD_SQUELCH_FLOOR_CACHE_SIZE; i++) {
            if (c->entries[i].used == 0U) {
                slot = i;
                break;
            }
            if (c->entries[i].used < oldest) {
                oldest = c->entries[i].used;
                slot = i;
            }
        }
    }
    dsd_squelch_floor_cache_entry* e = &c->entries[slot];
    e->key = *key;
    e->density = density;
    e->stamp_s = c->clock_s;
    e->used = ++c->use_clock;
}

int
dsd_squelch_floor_cache_find(dsd_squelch_floor_cache* c, const dsd_squelch_floor_key* key, double* density) {
    if (!c || !key) {
        return 0;
    }
    int near = -1;
    int64_t near_dist = 0;
    for (int i = 0; i < DSD_SQUELCH_FLOOR_CACHE_SIZE; i++) {
        dsd_squelch_floor_cache_entry* e = &c->entries[i];
        if (!sqf_cache_fresh(c, e) || !sqf_key_neighbour(&e->key, key)) {
            continue;
        }
        const int64_t dist =
            e->key.freq_hz > key->freq_hz ? e->key.freq_hz - key->freq_hz : key->freq_hz - e->key.freq_hz;
        if (dist == 0) {
            e->used = ++c->use_clock;
            if (density) {
                *density = e->density;
            }
            return 1;
        }
        if (dist <= k_cache_neighbour_hz && (near < 0 || dist < near_dist)) {
            near = i;
            near_dist = dist;
        }
    }
    if (near < 0) {
        return 0;
    }
    c->entries[near].used = ++c->use_clock;
    if (density) {
        *density = c->entries[near].density;
    }
    return 2;
}

void
dsd_squelch_floor_change_context(dsd_squelch_floor* t, dsd_squelch_floor_cache* c, const dsd_squelch_floor_key* prev,
                                 const dsd_squelch_floor_key* next) {
    if (!t || (prev && next && dsd_squelch_floor_key_equal(prev, next))) {
        return;
    }
    if (c && prev && t->state == DSD_SQUELCH_FLOOR_KNOWN && t->plan.valid && t->plan.noise_gain > 0.0) {
        dsd_squelch_floor_cache_store(c, prev, t->floor_power / t->plan.noise_gain);
    }
    sqf_restart_windows(t);
    double density = 0.0;
    const int found = (c && next) ? dsd_squelch_floor_cache_find(c, next, &density) : 0;
    if (found != 0 && t->plan.valid && t->plan.noise_gain > 0.0) {
        dsd_squelch_floor_seed(t, density * t->plan.noise_gain, found == 2);
    } else {
        sqf_relearn(t);
    }
}
