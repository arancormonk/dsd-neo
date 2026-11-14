// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Receive-side speech level AGC and peak limiter (<dsd-neo/dsp/speech_agc.h>).
 *
 * Every constant here was measured on speech, vocoded speech with real decoder masks, analog captures and synthetic
 * noise, tones and vowels. The arithmetic is float per sample and per frame, in a fixed order (the autocorrelation sums
 * every lag in one pass in sample order, which vectorises without reassociation), so the source keeps IEEE semantics
 * under fast-math (src/dsp/CMakeLists.txt).
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/speech_agc.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* A sample at or above this magnitude (or not finite) is dropped: its square would overflow a frame's float sums. */
#define AGC_INPUT_LIMIT          1e15f
/* Fixed gains are capped here so a fixed gain times the largest input stays finite. */
#define AGC_FIXED_GAIN_MAX       1e6f
#define AGC_FULL_SCALE_SQ        (32768.0f * 32768.0f)

/* Frame eligibility: at least 9 in 10 of the frame's samples learn. */
#define AGC_ELIGIBLE_NUM         9
#define AGC_ELIGIBLE_DEN         10
/* Frames under the absolute floor (this far under the level the start gain takes to the target) never count. */
#define AGC_ABS_FLOOR_RANGE_DB   60.0f

/* Cue 1: the frame stands this far over the noise floor (as it was before this frame). */
#define AGC_CUE1_DB              9.0f
/* Cue 2: the best interior autocorrelation peak over 2.5-15 ms reaches this; the first peak is the shortest interior
   maximum within this fraction of it. */
#define AGC_PERIODIC_R           0.5f
#define AGC_FIRST_PEAK_FRACTION  0.9f
/* The coloured-noise veto (analog instances, dsd_speech_agc_set_noise_veto()): a frame that passes cue 2 also needs a
   strict interior peak this high in a first-order pre-emphasised copy of the decimated frame, y[n] = x[n] - c x[n-1].
   Voice keeps its pitch peaks through the pre-emphasis; hiss after heavy de-emphasis loses them. */
#define AGC_VETO_R               0.4f
#define AGC_VETO_PREEMPH         0.9f
#define AGC_LAG_MIN_S            0.0025f
#define AGC_LAG_MAX_S            0.015f
#define AGC_DECIM_RATE_HZ        8000.0f
#define AGC_FRAME_S              0.020f

/* Noise floor: it rises at most 6 dB/s for the first 2 s (100 eligible frames) after a (re)start, then 1 dB/s. It
   restarts when a frame falls this far under it (the noise it sat on has gone), or when it has gone stale: this many
   voiced frames within the window over it fail only cue 1, while their pitch spans and keeps moving by more than the
   relative tolerance. */
#define AGC_FLOOR_FAST_FRAMES    100
#define AGC_FLOOR_FAST_RISE      (6.0f * 0.02f)
#define AGC_FLOOR_SLOW_RISE      (1.0f * 0.02f)
#define AGC_FLOOR_DROP_DB        10.0f
#define AGC_STALE_FRAMES         12
#define AGC_STALE_WINDOW_DB      3.0f
#define AGC_LAG_REL              0.003f

/* Steady-tone test over the run's last DSD_SPEECH_AGC_STATIONARY_FRAMES: a level range under this and a lag range
   within AGC_LAG_REL of the period is a tone, which lifts the floor to this far under the run's quietest frame. */
#define AGC_STATIONARY_LEVEL_DB  1.5f
#define AGC_REJECT_MARGIN_DB     3.0f

/* The level estimate: the plain mean of the first commits, then a one-pole power average, faster on a jump up. */
#define AGC_EST_CUMULATIVE       20
#define AGC_EST_JUMP_DB          6.0f
#define AGC_EST_FAST_DIV         5.0f
#define AGC_EST_SLOW_DIV         100.0f

/* The gain law: steps of up to 2 dB for the first 20 steps after a reset, then +0.12 / -0.20 dB. */
#define AGC_START_STEPS          20
#define AGC_START_STEP_DB        2.0f
#define AGC_STEP_UP_DB           0.12f
#define AGC_STEP_DOWN_DB         0.20f

/* The attenuator: down 6 dB/s while nothing committed for 1 s and the frame would play over -32 dBFS; back to 0 on a
   frame passing both cues, or one this far under the level that drove it down (the carrier quieting). The floor is deep
   enough that hot hiss after a quiet talker, with the gain near the top of its range, still eases to -32 dBFS; on
   ordinary hiss the -32 dBFS level stops the easing long before it. */
#define AGC_ATT_HOLD_FRAMES      50
#define AGC_ATT_ACTIVE_DBFS      (-32.0f)
#define AGC_ATT_STEP_DB          (6.0f * 0.02f)
#define AGC_ATT_FLOOR_DB         (-60.0f)
#define AGC_ATT_QUIETING_DB      6.0f
#define AGC_ATT_NOISE_ALPHA      0.1f

#define AGC_LIM_RELEASE_DB_PER_S 50.0f
#define AGC_SINCE_COMMIT_MAX     (1 << 20)
/* Counters saturate here; a run length wraps by a multiple of the run ring, which keeps every test on it. */
#define AGC_COUNT_MAX            (1 << 30)
#define AGC_RUN_WRAP             (DSD_SPEECH_AGC_STATIONARY_FRAMES * (1 << 24))
#define AGC_RUN_WRAP_STEP        (DSD_SPEECH_AGC_STATIONARY_FRAMES * (1 << 23))

static float
agc_db2lin(float db) {
    return powf(10.0f, db / 20.0f);
}

static inline float
agc_clamp(float x, float lo, float hi) {
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

/* The ceiling in int16 units, rounded down to a whole unit so it never sits over the requested level; 0 when the
   level is not a usable ceiling (not finite, over 0 dBFS, or under one unit). */
static float
agc_ceiling(float ceiling_dbfs) {
    if (!isfinite(ceiling_dbfs) || ceiling_dbfs > 0.0f) {
        return 0.0f;
    }
    const float c = floorf(32768.0f * agc_db2lin(ceiling_dbfs));
    return c >= 1.0f ? c : 0.0f;
}

static int
agc_start_gain_ok(float db) {
    return isfinite(db) && fabsf(db) <= DSD_SPEECH_AGC_START_GAIN_MAX_DB;
}

static int
agc_ready(const dsd_speech_agc* agc) {
    return agc != NULL && agc->frame_len > 0;
}

/* Instant attack and release at the per-sample factor. Rounding can leave a limited sample an ulp past the ceiling: the
   clamp holds it there, and within int16 (a 0 dBFS ceiling is 32768). */
static inline float
agc_limit(float* g_lim, float release, float ceiling, float v) {
    float gl = *g_lim * release;
    gl = gl > 1.0f ? 1.0f : gl;
    const float av = fabsf(v);
    if (av * gl > ceiling) {
        gl = ceiling / av;
    }
    *g_lim = gl;
    const float y = v * gl;
    const float hi = ceiling < 32767.0f ? ceiling : 32767.0f;
    return agc_clamp(y, -ceiling, hi);
}

static void
agc_cp_push(dsd_speech_agc* agc) {
    agc->checkpoints[agc->cp_head].learned = agc->learned;
    agc->checkpoints[agc->cp_head].frames = agc->frames;
    agc->cp_head = (agc->cp_head + 1) % DSD_SPEECH_AGC_CHECKPOINTS;
    if (agc->cp_count < DSD_SPEECH_AGC_CHECKPOINTS) {
        agc->cp_count++;
    }
}

static void
agc_cp_restart(dsd_speech_agc* agc) {
    agc->cp_head = 0;
    agc->cp_count = 0;
    agc_cp_push(agc);
}

/* The signal state a reset or a gate close starts over: the next reception starts from silence, at the learned gain. */
static void
agc_clear_signal(dsd_speech_agc* agc) {
    agc->frame_pos = 0;
    agc->frame_learn = 0;
    agc->frame_sumsq = 0.0f;
    agc->decim_phase = 0;
    agc->decim_pos = 0;
    DSD_MEMSET(agc->history, 0, sizeof agc->history);
    agc->g_target = agc_db2lin(agc->learned.gain_db + agc->learned.atten_db);
    agc->g_cur = agc->g_target;
    agc->g_step = 0.0f;
    agc->ramp_left = 0;
    agc->gain_dirty = 0;
    agc->g_lim = 1.0f;
}

int
dsd_speech_agc_init(dsd_speech_agc* agc, int rate_hz, float start_gain_db, float target_dbfs, float ceiling_dbfs) {
    const float ceiling = agc_ceiling(ceiling_dbfs);
    if (!agc || rate_hz < DSD_SPEECH_AGC_MIN_RATE_HZ || rate_hz > DSD_SPEECH_AGC_MAX_RATE_HZ
        || !agc_start_gain_ok(start_gain_db) || !isfinite(target_dbfs) || !(ceiling > 0.0f)) {
        return -1;
    }
    const int frame_len = (int)lroundf(AGC_FRAME_S * (float)rate_hz);
    int decim = (int)lroundf((float)rate_hz / AGC_DECIM_RATE_HZ);
    decim = decim < 1 ? 1 : decim;
    const int decim_len = (frame_len + decim - 1) / decim;
    const float decim_rate = (float)rate_hz / (float)decim;
    const int lag_lo = (int)lroundf(AGC_LAG_MIN_S * decim_rate);
    const int lag_hi = (int)lroundf(AGC_LAG_MAX_S * decim_rate);
    /* Holds for every supported rate (the header's sizes); the check keeps the arrays safe if a constant changes. */
    if (decim_len + lag_hi + 1 > DSD_SPEECH_AGC_HISTORY_LEN || lag_hi - lag_lo + 1 > DSD_SPEECH_AGC_MAX_LAGS
        || lag_lo < 1 || lag_hi - lag_lo < 2) {
        return -1;
    }
    DSD_MEMSET(agc, 0, sizeof *agc);
    agc->rate_hz = rate_hz;
    agc->frame_len = frame_len;
    agc->decim = decim;
    agc->lag_lo = lag_lo;
    agc->lag_hi = lag_hi;
    agc->target_dbfs = target_dbfs;
    agc->target_db = DSD_SPEECH_AGC_FRAME_TARGET_DBFS + (target_dbfs - DSD_SPEECH_AGC_TARGET_DBFS);
    agc->next_start_gain_db = start_gain_db;
    agc->ceiling = ceiling;
    agc->lim_release = powf(10.0f, AGC_LIM_RELEASE_DB_PER_S / (20.0f * (float)rate_hz));
    dsd_speech_agc_reset(agc);
    return 0;
}

void
dsd_speech_agc_reset(dsd_speech_agc* agc) {
    if (!agc_ready(agc)) {
        return;
    }
    agc->start_gain_db = agc->next_start_gain_db;
    agc->abs_floor_db = (agc->target_dbfs - agc->start_gain_db) - AGC_ABS_FLOOR_RANGE_DB;
    agc->tone_cap = agc_db2lin(agc->start_gain_db);
    DSD_MEMSET(&agc->learned, 0, sizeof agc->learned);
    agc->learned.gain_db = agc->start_gain_db;
    agc->learned.since_commit = AGC_SINCE_COMMIT_MAX;
    agc->learned.noise_floor_db = agc->abs_floor_db;
    agc_clear_signal(agc);
    agc->decodable_prev = 1;
    agc->tone_prev = 0;
    agc->tone_ramp_left = 0;
    agc->g_last = agc->g_cur;
    agc->learn_prev = -1;
    /* A new reception starts at the start gain: auto taking over after the reset does not start from the fixed gain
       the reception before it played. */
    agc->fixed_seed = 0;
    agc_cp_restart(agc); /* the opening checkpoint */
}

void
dsd_speech_agc_set_start_gain_db(dsd_speech_agc* agc, float start_gain_db) {
    if (agc_ready(agc) && agc_start_gain_ok(start_gain_db)) {
        agc->next_start_gain_db = start_gain_db;
    }
}

void
dsd_speech_agc_set_noise_veto(dsd_speech_agc* agc, int on) {
    if (agc_ready(agc)) {
        agc->noise_veto = on != 0;
    }
}

void
dsd_speech_agc_set_fixed(dsd_speech_agc* agc, float gain) {
    if (!agc_ready(agc)) {
        return;
    }
    if (gain > 0.0f && isfinite(gain)) {
        agc->fixed = 1;
        agc->fixed_gain = gain < AGC_FIXED_GAIN_MAX ? gain : AGC_FIXED_GAIN_MAX;
        agc->fixed_seed = 1;
        return;
    }
    if (!agc->fixed) {
        return;
    }
    agc->fixed = 0;
    if (!agc->fixed_seed) {
        return; /* reset since the fixed gain was set: the reception stays at its start gain */
    }
    /* Back to automatic: the estimate stays, the gain starts where the fixed one was (within range), E at 0. */
    const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
    const float g_db = 20.0f * log10f(agc->fixed_gain);
    agc->learned.gain_db = agc_clamp(g_db, lo, hi);
    agc->learned.atten_db = 0.0f;
    agc->g_target = agc_db2lin(agc->learned.gain_db);
    agc->g_cur = agc->g_target;
    agc->g_step = 0.0f;
    agc->ramp_left = 0;
    agc->gain_dirty = 0;
    /* The switch is the new opening checkpoint: a close never restores a gain from before it. */
    agc_cp_restart(agc);
}

/* Normalised autocorrelation of the decimated frame over every lag, each lag summed in sample order. @p r gets one
   value per lag (0 where the correlation is not positive) and @p acc the raw lag products. Returns 0 for a silent
   frame (@p acc then unset). */
static int
agc_correlate(const dsd_speech_agc* agc, float* r, float* acc) {
    const float* x = agc->history + agc->lag_hi;
    const int n = agc->decim_pos;
    const int nl = agc->lag_hi - agc->lag_lo + 1;
    float e0 = 0.0f;
    for (int i = 0; i < n; i++) {
        e0 += x[i] * x[i];
    }
    if (!(e0 > 0.0f)) {
        return 0;
    }
    float et = 0.0f;
    for (int i = 0; i < n; i++) {
        const float v = x[i - agc->lag_lo];
        et += v * v;
    }
    for (int j = 0; j < nl; j++) {
        acc[j] = 0.0f;
    }
    for (int i = 0; i < n; i++) {
        const float xi = x[i];
        const float* y = x + i - agc->lag_lo;
        for (int j = 0; j < nl; j++) {
            acc[j] += xi * y[-j];
        }
    }
    for (int j = 0; j < nl; j++) {
        const int tau = agc->lag_lo + j;
        const float ac = acc[j];
        r[j] = (ac > 0.0f && et > 0.0f) ? ac / sqrtf(e0 * et) : 0.0f;
        if (j == nl - 1) {
            break;
        }
        /* The lagged window's energy for the next lag: one sample enters at the far end, one leaves. */
        const float o = x[n - 1 - tau];
        const float ni = x[-tau - 1];
        et += ni * ni - o * o;
        if (et < 0.0f) {
            et = 0.0f;
        }
    }
    return 1;
}

static int
agc_interior_max(const float* r, int j) {
    return r[j] > r[j - 1] && r[j] > r[j + 1];
}

/* Cue 2's peak: the best strict interior local maximum over the lag range (both ends excluded, so a correlation still
   rising at 15 ms, or low-frequency rumble, never counts). @p lag_ms gets the first peak (the shortest interior
   maximum within AGC_FIRST_PEAK_FRACTION of the best), refined by a parabola, or -1. @p acc gets the raw lag products
   for the veto. */
static float
agc_pitch(const dsd_speech_agc* agc, float* lag_ms, float* acc) {
    float r[DSD_SPEECH_AGC_MAX_LAGS];
    *lag_ms = -1.0f;
    if (!agc_correlate(agc, r, acc)) {
        return 0.0f;
    }
    const int nl = agc->lag_hi - agc->lag_lo + 1;
    float best = 0.0f;
    for (int j = 1; j + 1 < nl; j++) {
        if (agc_interior_max(r, j) && r[j] > best) {
            best = r[j];
        }
    }
    if (!(best > 0.0f)) {
        return best;
    }
    for (int j = 1; j + 1 < nl; j++) {
        if (agc_interior_max(r, j) && r[j] >= AGC_FIRST_PEAK_FRACTION * best) {
            const float y0 = r[j - 1];
            const float y1 = r[j];
            const float y2 = r[j + 1];
            const float den = y0 - 2.0f * y1 + y2;
            float dl = den < 0.0f ? 0.5f * (y0 - y2) / den : 0.0f;
            dl = agc_clamp(dl, -0.5f, 0.5f);
            *lag_ms = 1000.0f * ((float)(agc->lag_lo + j) + dl) * (float)agc->decim / (float)agc->rate_hz;
            break;
        }
    }
    return best;
}

/* Sample @p k of the pre-emphasised copy of the decimated frame @p x, whose history starts at -@p lag_hi (the sample
   before it counts as 0). */
static inline float
agc_preemph(const float* x, int k, int lag_hi) {
    return x[k] - AGC_VETO_PREEMPH * ((k - 1 >= -lag_hi) ? x[k - 1] : 0.0f);
}

/* The pre-emphasised copy's lag products over the lag range, from the raw ones in @p acc: with w[i] = x[i] - c x[i-1],
   Aw(t) = (1 + c^2) A(t) - c A(t+1) - c A(t-1) plus the terms at the window's edges. Needs A(lo-1) and A(hi+1) too. */
static void
agc_preemph_products(const dsd_speech_agc* agc, const float* acc, float* aw, int nl) {
    const float* x = agc->history + agc->lag_hi;
    const int n = agc->decim_pos;
    const int lo = agc->lag_lo;
    const int hi = agc->lag_hi;
    const float c = AGC_VETO_PREEMPH;
    float alo = 0.0f;
    float ahi = 0.0f;
    for (int i = 0; i < n; i++) {
        alo += x[i] * x[i - lo + 1];
    }
    for (int i = 1; i < n; i++) {
        ahi += x[i] * x[i - hi - 1];
    }
    for (int j = 0; j < nl; j++) {
        const int t = lo + j;
        const float at = acc[j];
        const float ap = j + 1 < nl ? acc[j + 1] : ahi;
        const float am = j > 0 ? acc[j - 1] : alo;
        const float xm1t1 = j + 1 < nl ? x[-1] * x[-1 - t] : 0.0f;
        aw[j] = at - c * ap - c * (am + x[-1] * x[-t] - x[n - 1] * x[n - t])
                + c * c * (at + xm1t1 - x[n - 1] * x[n - 1 - t]);
    }
}

/* The coloured-noise veto's test for a frame that passed cue 2 (@p acc: its raw lag products): whether its
   pre-emphasised copy has a strict interior autocorrelation peak of at least AGC_VETO_R. It compares Aw^2 / Ew(t)
   against AGC_VETO_R^2 E0, which orders the lags as the normalised r does where Aw > 0, without a square root. */
static int
agc_survives_preemphasis(const dsd_speech_agc* agc, const float* acc) {
    const float* x = agc->history + agc->lag_hi;
    const int n = agc->decim_pos;
    const int lo = agc->lag_lo;
    const int hi = agc->lag_hi;
    const int nl = hi - lo + 1;
    float q[DSD_SPEECH_AGC_MAX_LAGS];
    agc_preemph_products(agc, acc, q, nl);
    float e0 = 0.0f;
    for (int i = 0; i < n; i++) {
        const float v = agc_preemph(x, i, hi);
        e0 += v * v;
    }
    if (!(e0 > 0.0f)) {
        return 0;
    }
    float et = 0.0f;
    for (int i = 0; i < n; i++) {
        const float v = agc_preemph(x, i - lo, hi);
        et += v * v;
    }
    for (int j = 0; j < nl; j++) {
        const int t = lo + j;
        const float ac = q[j];
        q[j] = (ac > 0.0f && et > 0.0f) ? ac * ac / et : 0.0f;
        if (j + 1 == nl) {
            break;
        }
        /* The lagged window's energy for the next lag, as in agc_correlate(). */
        const float o = agc_preemph(x, n - 1 - t, hi);
        const float ni = agc_preemph(x, -t - 1, hi);
        et += ni * ni - o * o;
        if (et < 0.0f) {
            et = 0.0f;
        }
    }
    const float need = AGC_VETO_R * AGC_VETO_R * e0;
    for (int j = 1; j + 1 < nl; j++) {
        if (agc_interior_max(q, j) && q[j] >= need) {
            return 1;
        }
    }
    return 0;
}

/* One committed frame level into the estimate. */
static void
agc_estimate(dsd_speech_agc* agc, float level_db) {
    dsd_speech_agc_learned* d = &agc->learned;
    if (d->commits < AGC_COUNT_MAX) {
        d->commits++;
    }
    const float p = powf(10.0f, level_db / 10.0f);
    if (d->commits <= AGC_EST_CUMULATIVE) {
        d->power += (p - d->power) / (float)d->commits;
    } else if (level_db > d->level_db + AGC_EST_JUMP_DB) {
        d->power += (p - d->power) / AGC_EST_FAST_DIV;
    } else {
        d->power += (p - d->power) / AGC_EST_SLOW_DIV;
    }
    d->level_db = 10.0f * log10f(d->power + 1e-30f);
}

/* One gain step per committing frame, toward the target within the controller's range. */
static void
agc_gain_step(dsd_speech_agc* agc) {
    dsd_speech_agc_learned* d = &agc->learned;
    float want = agc->target_db - d->level_db;
    const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
    want = agc_clamp(want, lo, hi);
    const int start = d->steps < AGC_START_STEPS;
    if (d->steps < AGC_COUNT_MAX) {
        d->steps++;
    }
    const float up = start ? AGC_START_STEP_DB : AGC_STEP_UP_DB;
    const float dn = start ? AGC_START_STEP_DB : AGC_STEP_DOWN_DB;
    float x = want - d->gain_db;
    x = agc_clamp(x, -dn, up);
    d->gain_db += x;
    agc->gain_dirty = 1;
}

static int
agc_run_slot(int run_len, int back) {
    return (run_len - 1 - back) % DSD_SPEECH_AGC_STATIONARY_FRAMES;
}

/* The run's last DSD_SPEECH_AGC_STATIONARY_FRAMES are one steady sound: a tone, not speech. */
static int
agc_run_stationary(const dsd_speech_agc_learned* d) {
    if (d->run_len < DSD_SPEECH_AGC_STATIONARY_FRAMES) {
        return 0;
    }
    float lmin = 1e30f;
    float lmax = -1e30f;
    float fmin = 1e30f;
    float fmax = -1e30f;
    for (int i = 0; i < DSD_SPEECH_AGC_STATIONARY_FRAMES; i++) {
        const int k = agc_run_slot(d->run_len, i);
        lmin = d->run_level_db[k] < lmin ? d->run_level_db[k] : lmin;
        lmax = d->run_level_db[k] > lmax ? d->run_level_db[k] : lmax;
        fmin = d->run_lag_ms[k] < fmin ? d->run_lag_ms[k] : fmin;
        fmax = d->run_lag_ms[k] > fmax ? d->run_lag_ms[k] : fmax;
    }
    if (!(lmax - lmin < AGC_STATIONARY_LEVEL_DB)) {
        return 0;
    }
    return fmin >= 0.0f && fmax - fmin <= AGC_LAG_REL * 0.5f * (fmax + fmin);
}

/* A steady run is a floor observation: the floor rises to just under it and the run starts over. */
static void
agc_run_reject(dsd_speech_agc_learned* d) {
    float lmin = 1e30f;
    for (int i = 0; i < DSD_SPEECH_AGC_STATIONARY_FRAMES; i++) {
        const int k = agc_run_slot(d->run_len, i);
        lmin = d->run_level_db[k] < lmin ? d->run_level_db[k] : lmin;
    }
    if (lmin - AGC_REJECT_MARGIN_DB > d->noise_floor_db) {
        d->noise_floor_db = lmin - AGC_REJECT_MARGIN_DB;
    }
    d->run_len = 0;
    d->rejections++;
}

static void
agc_floor_restart(dsd_speech_agc* agc) {
    agc->learned.noise_floor_db = agc->abs_floor_db;
    agc->learned.floor_frames = -1; /* the caller's count makes it 0: the fast window starts over */
    agc->learned.restarts++;
}

/* The stale-floor watch for a voiced frame within AGC_STALE_WINDOW_DB over the floor that failed only cue 1. Voice
   moves its pitch on most frames; tone sequences (DTMF) move only at digit changes. Returns 1 when the floor restarts. */
static int
agc_stale_probe(dsd_speech_agc* agc, float lag_ms) {
    dsd_speech_agc_learned* d = &agc->learned;
    if (d->stale_frames == 0) {
        d->stale_lag_min_ms = lag_ms;
        d->stale_lag_max_ms = lag_ms;
        d->stale_moves = 0;
    } else if (fabsf(lag_ms - d->stale_lag_last_ms) > AGC_LAG_REL * lag_ms && d->stale_moves < AGC_COUNT_MAX) {
        d->stale_moves++;
    }
    d->stale_lag_last_ms = lag_ms;
    if (d->stale_frames < AGC_COUNT_MAX) {
        d->stale_frames++; /* a steady sound at the floor's level can probe for months */
    }
    d->stale_lag_min_ms = lag_ms < d->stale_lag_min_ms ? lag_ms : d->stale_lag_min_ms;
    d->stale_lag_max_ms = lag_ms > d->stale_lag_max_ms ? lag_ms : d->stale_lag_max_ms;
    const float tol = AGC_LAG_REL * 0.5f * (d->stale_lag_max_ms + d->stale_lag_min_ms);
    if (d->stale_frames >= AGC_STALE_FRAMES && d->stale_lag_max_ms - d->stale_lag_min_ms > tol
        && (int64_t)d->stale_moves * 3 >= ((int64_t)d->stale_frames - 1) * 2) {
        agc_floor_restart(agc);
        d->stale_frames = 0;
        return 1;
    }
    return 0;
}

static void
agc_count_floor_frame(dsd_speech_agc_learned* d) {
    if (d->floor_frames < AGC_COUNT_MAX) {
        d->floor_frames++;
    }
}

/* Cue 1 and the noise floor for one eligible frame at or above the absolute floor. Cue 1 reads the floor as it was
   before this frame. The pitch cue runs only where it can matter: when cue 1 passes, or for the stale-floor watch; with
   the veto on, a frame that passes it without surviving the pre-emphasis counts as not periodic (r 0, no lag).
   Returns cue 1; @p r and @p lag_ms get the pitch cue (-1 when it did not run). */
static int
agc_floor_and_cue1(dsd_speech_agc* agc, float level_db, float* r, float* lag_ms) {
    dsd_speech_agc_learned* d = &agc->learned;
    const float n_prev = d->noise_floor_db;
    const int cue1 = level_db >= n_prev + AGC_CUE1_DB;
    const int probe = !cue1 && level_db >= n_prev - AGC_STALE_WINDOW_DB;
    if (cue1 || probe) {
        float acc[DSD_SPEECH_AGC_MAX_LAGS];
        *r = agc_pitch(agc, lag_ms, acc);
        if (agc->noise_veto && *r >= AGC_PERIODIC_R && !agc_survives_preemphasis(agc, acc)) {
            *r = 0.0f;
            *lag_ms = -1.0f;
        }
    }
    if (probe && *r >= AGC_PERIODIC_R && *lag_ms >= 0.0f) {
        if (agc_stale_probe(agc, *lag_ms)) {
            agc_count_floor_frame(d);
            return cue1;
        }
    } else {
        d->stale_frames = 0;
    }
    const float rise = d->floor_frames < AGC_FLOOR_FAST_FRAMES ? AGC_FLOOR_FAST_RISE : AGC_FLOOR_SLOW_RISE;
    if (level_db < d->noise_floor_db - AGC_FLOOR_DROP_DB) {
        agc_floor_restart(agc); /* the noise the floor sat on has gone (carrier quieting) */
    } else if (level_db < d->noise_floor_db) {
        d->noise_floor_db = level_db;
    } else {
        const float x = level_db - d->noise_floor_db;
        d->noise_floor_db += x < rise ? x : rise;
    }
    agc_count_floor_frame(d);
    return cue1;
}

/* A speech frame extends the run: the sixth commits the whole run (one gain step), each later one commits itself
   unless the run has turned out to be a steady tone. Returns the frames committed. */
static int
agc_run_add(dsd_speech_agc* agc, float level_db, float lag_ms) {
    dsd_speech_agc_learned* d = &agc->learned;
    d->run_len++;
    if (d->run_len >= AGC_RUN_WRAP) {
        d->run_len -= AGC_RUN_WRAP_STEP;
    }
    const int slot = agc_run_slot(d->run_len, 0);
    d->run_level_db[slot] = level_db;
    d->run_lag_ms[slot] = lag_ms;
    if (d->run_len == DSD_SPEECH_AGC_RUN_FRAMES) {
        for (int i = 0; i < DSD_SPEECH_AGC_RUN_FRAMES; i++) {
            agc_estimate(agc, d->run_level_db[i]);
        }
        return DSD_SPEECH_AGC_RUN_FRAMES;
    }
    if (d->run_len > DSD_SPEECH_AGC_RUN_FRAMES) {
        if (agc_run_stationary(d)) {
            agc_run_reject(d);
            return 0;
        }
        agc_estimate(agc, level_db);
        return 1;
    }
    return 0;
}

/* The attenuator for one eligible frame. */
static void
agc_attenuate(dsd_speech_agc* agc, float level_db, int speech) {
    dsd_speech_agc_learned* d = &agc->learned;
    const int quieted = d->atten_db < 0.0f && level_db < d->noise_level_db - AGC_ATT_QUIETING_DB;
    if (quieted || speech) {
        d->atten_db = 0.0f;
        agc->gain_dirty = 1;
    } else if (d->since_commit >= AGC_ATT_HOLD_FRAMES && level_db + d->gain_db + d->atten_db > AGC_ATT_ACTIVE_DBFS) {
        d->noise_level_db =
            d->atten_db < 0.0f ? d->noise_level_db + (level_db - d->noise_level_db) * AGC_ATT_NOISE_ALPHA : level_db;
        d->atten_db -= AGC_ATT_STEP_DB;
        if (d->atten_db < AGC_ATT_FLOOR_DB) {
            d->atten_db = AGC_ATT_FLOOR_DB;
        }
        agc->gain_dirty = 1;
    }
}

/* The analysis frame is complete: classify it, learn from it, and set the gain ramp for the next frame. */
static void
agc_frame_done(dsd_speech_agc* agc) {
    dsd_speech_agc_learned* d = &agc->learned;
    const int elig = agc->frame_learn * AGC_ELIGIBLE_DEN >= agc->frame_len * AGC_ELIGIBLE_NUM;
    /* The level only matters for an eligible frame, which has learning samples. */
    const float level_db =
        elig ? 10.0f * log10f(agc->frame_sumsq / (float)agc->frame_learn / AGC_FULL_SCALE_SQ + 1e-30f) : 0.0f;
    int speech = 0;
    if (elig && level_db >= agc->abs_floor_db) {
        float r = -1.0f;
        float lag_ms = -1.0f;
        const int cue1 = agc_floor_and_cue1(agc, level_db, &r, &lag_ms);
        speech = cue1 && r >= AGC_PERIODIC_R;
        if (speech && agc_run_add(agc, level_db, lag_ms) > 0) {
            agc_gain_step(agc);
            d->since_commit = -1; /* the count below makes it 0 */
        }
    } else {
        d->stale_frames = 0; /* silence or closed audio breaks a stale-floor run */
    }
    if (!speech) {
        d->run_len = 0;
    }
    if (d->since_commit < AGC_SINCE_COMMIT_MAX) {
        d->since_commit++;
    }
    if (elig) {
        agc_attenuate(agc, level_db, speech);
    }
    if (agc->gain_dirty) {
        agc->g_target = agc_db2lin(d->gain_db + d->atten_db);
        agc->gain_dirty = 0;
    }
    agc->g_step = (agc->g_target - agc->g_cur) / (float)agc->frame_len;
    agc->ramp_left = agc->frame_len;
    DSD_MEMMOVE(agc->history, agc->history + agc->decim_pos, sizeof(float) * (size_t)agc->lag_hi);
    agc->decim_pos = 0;
    agc->frame_pos = 0;
    agc->frame_learn = 0;
    agc->frame_sumsq = 0.0f;
    agc->decim_phase = 0;
    agc->frames++;
    agc_cp_push(agc);
}

/* The controller gain for one sample: fixed, capped on a tone frame, ramping back up after one, or the controller's. */
static inline float
agc_sample_gain(dsd_speech_agc* agc, int tone) {
    if (agc->fixed) {
        return agc->fixed_gain;
    }
    if (tone) {
        agc->tone_prev = 1;
        return agc->g_cur < agc->tone_cap ? agc->g_cur : agc->tone_cap;
    }
    if (agc->tone_prev) {
        agc->tone_prev = 0;
        agc->tone_end_gain = agc->g_last;
        agc->tone_ramp_left = agc->g_cur > agc->g_last ? agc->frame_len : 0;
    }
    if (agc->tone_ramp_left > 0) {
        agc->tone_ramp_left--;
        const float w = (float)(agc->frame_len - agc->tone_ramp_left) / (float)agc->frame_len;
        return agc->tone_end_gain + (agc->g_cur - agc->tone_end_gain) * w;
    }
    return agc->g_cur;
}

static inline float
agc_sample(dsd_speech_agc* agc, float x, int learning, int tone) {
    if (!(fabsf(x) < AGC_INPUT_LIMIT)) {
        return 0.0f;
    }
    if (learning) {
        agc->frame_sumsq += x * x;
        agc->frame_learn++;
    }
    if (agc->decim_phase == 0) {
        agc->history[agc->lag_hi + agc->decim_pos] = x;
        agc->decim_pos++;
    }
    if (++agc->decim_phase >= agc->decim) {
        agc->decim_phase = 0;
    }
    if (agc->ramp_left > 0) {
        agc->ramp_left--;
        agc->g_cur = agc->ramp_left > 0 ? agc->g_cur + agc->g_step : agc->g_target;
    }
    const float g = agc_sample_gain(agc, tone);
    agc->g_last = g;
    const float y = agc_limit(&agc->g_lim, agc->lim_release, agc->ceiling, x * g);
    if (++agc->frame_pos >= agc->frame_len) {
        agc_frame_done(agc);
    }
    return y;
}

void
dsd_speech_agc_process(dsd_speech_agc* agc, float* buf, size_t n, int learn, int decodable, int tone) {
    if (!agc_ready(agc) || !buf) {
        return;
    }
    decodable = decodable != 0;
    tone = tone != 0;
    const int learning = learn && decodable && !tone && !agc->fixed;
    if (decodable && !agc->decodable_prev) {
        /* Decodability returned: the limiter and the pitch history saw audio that never learned; the estimate stays. */
        agc->g_lim = 1.0f;
        DSD_MEMSET(agc->history, 0, sizeof agc->history);
    }
    agc->decodable_prev = decodable;
    for (size_t i = 0; i < n; i++) {
        buf[i] = agc_sample(agc, buf[i], learning, tone);
    }
}

static int
agc_learn_at(const uint8_t* learn, size_t i) {
    return learn != NULL && learn[i] != 0U;
}

void
dsd_speech_agc_process_runs(dsd_speech_agc* agc, float* buf, size_t n, const uint8_t* learn) {
    if (!agc_ready(agc) || !buf) {
        return;
    }
    size_t start = 0;
    while (start < n) {
        const int cur = agc_learn_at(learn, start);
        size_t end = start + 1U;
        while (end < n && agc_learn_at(learn, end) == cur) {
            end++;
        }
        if (agc->learn_prev == 1 && cur == 0) {
            dsd_speech_agc_gate_closed(agc);
        } else if (agc->learn_prev == 0 && cur == 1) {
            dsd_speech_agc_gate_opened(agc);
        }
        agc->learn_prev = cur;
        dsd_speech_agc_process(agc, buf + start, end - start, cur, 1, 0);
        start = end;
    }
}

void
dsd_speech_agc_gate_closed(dsd_speech_agc* agc) {
    if (!agc_ready(agc)) {
        return;
    }
    if (agc->cp_count > 0) {
        const int oldest = (agc->cp_head - agc->cp_count + DSD_SPEECH_AGC_CHECKPOINTS) % DSD_SPEECH_AGC_CHECKPOINTS;
        const dsd_speech_agc_checkpoint* cp = &agc->checkpoints[oldest];
        agc->learned = cp->learned;
        /* since_commit is a clock: it advances by the frames that passed since the checkpoint. */
        const uint64_t sc = (uint64_t)agc->learned.since_commit + (agc->frames - cp->frames);
        agc->learned.since_commit = sc > (uint64_t)AGC_SINCE_COMMIT_MAX ? AGC_SINCE_COMMIT_MAX : (int)sc;
    }
    agc_clear_signal(agc);
    /* A run never continues across a close: the next reception qualifies on its own frames. */
    agc->learned.run_len = 0;
    agc_cp_restart(agc);
}

void
dsd_speech_agc_gate_opened(dsd_speech_agc* agc) {
    if (agc_ready(agc)) {
        agc_cp_push(agc);
    }
}

void
dsd_speech_agc_open_at(dsd_speech_agc* agc, float gain_db, int ramp) {
    if (!agc_ready(agc) || !isfinite(gain_db) || agc->learned.commits != 0) {
        return;
    }
    const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
    agc->learned.gain_db = agc_clamp(gain_db, lo, hi);
    agc->g_target = agc_db2lin(agc->learned.gain_db + agc->learned.atten_db);
    if (ramp) {
        agc->g_step = (agc->g_target - agc->g_cur) / (float)agc->frame_len;
        agc->ramp_left = agc->frame_len;
    } else {
        agc->g_cur = agc->g_target;
        agc->g_step = 0.0f;
        agc->ramp_left = 0;
        agc->g_last = agc->g_cur;
    }
    agc->gain_dirty = 0;
    /* As leaving fixed mode: a close never restores the gain from before it. */
    agc_cp_restart(agc);
}

int
dsd_speech_agc_settled_gain_db(const dsd_speech_agc* agc, float* gain_db) {
    if (!agc_ready(agc) || gain_db == NULL || agc->learned.commits < AGC_EST_CUMULATIVE) {
        return 0;
    }
    const float lo = agc->start_gain_db - DSD_SPEECH_AGC_MAX_CUT_DB;
    const float hi = agc->start_gain_db + DSD_SPEECH_AGC_MAX_BOOST_DB;
    *gain_db = agc_clamp(agc->target_db - agc->learned.level_db, lo, hi);
    return 1;
}

float
dsd_speech_agc_gain_db(const dsd_speech_agc* agc) {
    if (!agc_ready(agc)) {
        return 0.0f;
    }
    const float g = agc->fixed ? agc->fixed_gain : agc->g_last;
    return 20.0f * log10f(g * agc->g_lim);
}

int
dsd_peak_limiter_init(dsd_peak_limiter* l, int rate_hz, float ceiling_dbfs) {
    const float ceiling = agc_ceiling(ceiling_dbfs);
    if (!l || rate_hz < 1 || !(ceiling > 0.0f)) {
        return -1;
    }
    l->ceiling = ceiling;
    l->release = powf(10.0f, AGC_LIM_RELEASE_DB_PER_S / (20.0f * (float)rate_hz));
    l->gain = 1.0f;
    return 0;
}

void
dsd_peak_limiter_process(dsd_peak_limiter* l, float* buf, size_t n) {
    if (!l || !buf || !(l->ceiling > 0.0f)) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const float x = buf[i];
        buf[i] = fabsf(x) < AGC_INPUT_LIMIT ? agc_limit(&l->gain, l->release, l->ceiling, x) : 0.0f;
    }
}

void
dsd_peak_limiter_reset(dsd_peak_limiter* l) {
    if (l) {
        l->gain = 1.0f;
    }
}
