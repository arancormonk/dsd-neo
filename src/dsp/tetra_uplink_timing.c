// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Burst timing for a TETRA uplink session, from each burst's training sequence (tetra_uplink_timing.h).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/dsp/tetra_uplink_timing.h>

/* Burst layouts, in symbols (EN 300 392-2 tables 9.3 and 9.5): the index of the training's last symbol, and the
   burst's length. The normal training sequence 1 sits two symbols before the end of the extended one (clause
   9.4.4.3.3). */
enum {
    UL_CB_TRAINING_LAST = 58,
    UL_CB_SYMBOLS = 103,
    UL_NUB_TRAINING_LAST = 120,
    UL_NUB_SYMBOLS = 231,
    UL_NTS_IN_ETS = 2,
    UL_ETS_LEN = 15,
    UL_NTS_LEN = 11,
    /* The strobe moves onto a burst's grid this many symbols before its first symbol. */
    UL_STROBE_LEAD = 2,
};

/* The agreement a control burst's four outer extended-training symbols need (ul_ets_outer_agreement()): within about 30
   degrees on average. */
#define UL_CB_OUTER_AGREEMENT 0.85f

_Static_assert(DSD_TETRA_UL_LOOKAHEAD_SYMBOLS >= UL_NUB_TRAINING_LAST + UL_STROBE_LEAD + UL_NTS_IN_ETS + 4,
               "the look-ahead covers a normal burst's training, its decision and the strobe lead");
_Static_assert(DSD_TETRA_UL_RING > DSD_TETRA_UL_LOOKAHEAD_SYMBOLS * DSD_TETRA_UL_MAX_SPS,
               "the ring holds the look-ahead at the largest sps");
_Static_assert(DSD_TETRA_UL_DIFF_RING > (UL_ETS_LEN - 1) * DSD_TETRA_UL_MAX_SPS,
               "the phasor ring holds the extended training sequence at the largest sps");
_Static_assert(UL_ETS_LEN <= DSD_TETRA_UL_TRAINING_MAX && UL_NTS_LEN <= DSD_TETRA_UL_TRAINING_MAX,
               "the training tables hold both sequences");
_Static_assert((DSD_TETRA_UL_RING & (DSD_TETRA_UL_RING - 1)) == 0, "a power of two");
_Static_assert((DSD_TETRA_UL_DIFF_RING & (DSD_TETRA_UL_DIFF_RING - 1)) == 0, "a power of two");

/* A training sequence as unit phase steps, in the order sent. Dibit 0, 1, 2, 3 steps the phase by +pi/4, +3pi/4,
   -pi/4, -3pi/4 (clause 5.5.2.2 with the decoder's dibit numbering, sync_patterns.h). */
static void
ul_training_from(dsd_tetra_ul_training* tr, const char* dibits, int len) {
    static const float k = 0.70710678118654752f;
    static const float step_re[4] = {k, -k, k, -k};
    static const float step_im[4] = {k, k, -k, -k};
    tr->len = len;
    for (int i = 0; i < len; i++) {
        const int dibit = (dibits[i] - '0') & 3;
        tr->re[i] = step_re[dibit];
        tr->im[i] = step_im[dibit];
    }
}

void
dsd_tetra_ul_timing_reset(dsd_tetra_ul_timing* t, int sps) {
    if (!t) {
        return;
    }
    DSD_MEMSET(t, 0, sizeof(*t));
    if (sps < DSD_TETRA_UL_MIN_SPS || sps > DSD_TETRA_UL_MAX_SPS) {
        return;
    }
    t->sps = sps;
    t->delay = DSD_TETRA_UL_LOOKAHEAD_SYMBOLS * sps;
    ul_training_from(&t->ets_seq, TETRA_CB_SYNC, UL_ETS_LEN);
    ul_training_from(&t->nts_seq, TETRA_NDB_NTS_SYNC, UL_NTS_LEN);
}

/* The score of the training ending on the newest phasor: |sum d[n - k sps] conj(step)|, the larger of the sequence and
   its conjugate, over sqrt(K sum |d|^2), its bound (Cauchy-Schwarz). It reaches 1 only for phasors of one magnitude all
   turned alike, so a window that spans a burst's edge, where a few strong phasors outweigh the noise ones, scores
   low. 0 before the phasors span it. */
static float
ul_training_score(const dsd_tetra_ul_timing* t, const dsd_tetra_ul_training* tr, uint64_t newest, float* phase,
                  int* conj) {
    *phase = 0.0f;
    *conj = 0;
    const int sps = t->sps;
    if (newest < (uint64_t)(tr->len * sps)) {
        return 0.0f;
    }
    float p_re = 0.0f;
    float p_im = 0.0f;
    float q_re = 0.0f;
    float q_im = 0.0f;
    float power = 0.0f;
    for (int k = 0; k < tr->len; k++) {
        const size_t at = (size_t)((newest - (uint64_t)(k * sps)) & (DSD_TETRA_UL_DIFF_RING - 1));
        const float d_re = t->diff[at * 2];
        const float d_im = t->diff[at * 2 + 1];
        const int i = tr->len - 1 - k;
        const float s_re = tr->re[i];
        const float s_im = tr->im[i];
        /* d conj(s), and d s for the conjugated sequence. */
        p_re += d_re * s_re + d_im * s_im;
        p_im += d_im * s_re - d_re * s_im;
        q_re += d_re * s_re - d_im * s_im;
        q_im += d_im * s_re + d_re * s_im;
        power += t->diff_pow[at];
    }
    const float bound = (float)tr->len * power;
    if (!(bound > 1e-24f)) {
        return 0.0f;
    }
    const float p = p_re * p_re + p_im * p_im;
    const float q = q_re * q_re + q_im * q_im;
    *phase = p > q ? atan2f(p_im, p_re) : atan2f(q_im, q_re);
    *conj = p > q ? 0 : 1;
    return sqrtf((p > q ? p : q) / bound);
}

/* How well the extended training sequence's first two and last two symbols, the four around the normal training
   sequence 1 inside it, agree with the rest of it: the magnitude-weighted mean of cos(their error) once the phasors are
   turned back by @p phase (conjugated when the sequence matched conjugated, @p conj). Near 1 for a control burst; for a
   normal burst whose neighbouring payload happens to resemble them, one symbol a quarter turn off gives at most 0.75. */
static float
ul_ets_outer_agreement(const dsd_tetra_ul_timing* t, uint64_t newest, float phase, int conj) {
    static const int outer[4] = {0, 1, UL_ETS_LEN - 2, UL_ETS_LEN - 1};
    const dsd_tetra_ul_training* tr = &t->ets_seq;
    const float c = cosf(phase);
    const float s = sinf(phase);
    float along = 0.0f;
    float mag = 0.0f;
    for (int j = 0; j < 4; j++) {
        const int i = outer[j];
        const size_t at = (size_t)((newest - (uint64_t)((UL_ETS_LEN - 1 - i) * t->sps)) & (DSD_TETRA_UL_DIFF_RING - 1));
        const float d_re = t->diff[at * 2];
        const float d_im = t->diff[at * 2 + 1];
        /* The step the symbol should have taken, conjugated for a conjugated match, turned by the measured phase. */
        const float r_re = tr->re[i];
        const float r_im = conj ? -tr->im[i] : tr->im[i];
        const float e_re = r_re * c - r_im * s;
        const float e_im = r_re * s + r_im * c;
        along += d_re * e_re + d_im * e_im;
        mag += sqrtf(t->diff_pow[at]);
    }
    return mag > 1e-12f ? along / mag : 0.0f;
}

static int
ul_spans_overlap(const dsd_tetra_ul_burst* a, const dsd_tetra_ul_burst* b) {
    return a->start < b->end && b->start < a->end;
}

/* Keep @p b unless a burst still ahead of the strobe overlaps it with a training that scored as high; it replaces those
   ahead of the strobe it overlaps. A burst the strobe passed blocks nothing: it was a peak on noise, or the burst before
   this one, and a training that did not move the strobe yet is all that can still decide. With every slot taken it
   replaces the burst that starts first. */
static void
ul_burst_found(dsd_tetra_ul_timing* t, const dsd_tetra_ul_burst* b) {
    for (int i = 0; i < DSD_TETRA_UL_BURSTS; i++) {
        const dsd_tetra_ul_burst* o = &t->bursts[i];
        if (o->end != 0 && o->pending && ul_spans_overlap(o, b) && o->score >= b->score) {
            return;
        }
    }
    for (int i = 0; i < DSD_TETRA_UL_BURSTS; i++) {
        dsd_tetra_ul_burst* o = &t->bursts[i];
        if (o->end != 0 && o->pending && ul_spans_overlap(o, b)) {
            DSD_MEMSET(o, 0, sizeof(*o));
        }
    }
    int slot = -1;
    for (int i = 0; i < DSD_TETRA_UL_BURSTS && slot < 0; i++) {
        if (t->bursts[i].end == 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        /* Every slot taken: the burst that starts first, one the strobe passed before one still ahead of it. */
        slot = 0;
        for (int i = 1; i < DSD_TETRA_UL_BURSTS; i++) {
            const dsd_tetra_ul_burst* o = &t->bursts[i];
            const dsd_tetra_ul_burst* k = &t->bursts[slot];
            if (o->pending < k->pending || (o->pending == k->pending && o->start < k->start)) {
                slot = i;
            }
        }
    }
    t->bursts[slot] = *b;
}

/* The peak's fractional offset from its best sample: the vertex of the parabola through it and its neighbours. */
static float
ul_peak_fraction(const dsd_tetra_ul_peak* pk) {
    const float denom = pk->before - 2.0f * pk->best + pk->after;
    if (!(denom < -1e-6f)) {
        return 0.0f;
    }
    float frac = 0.5f * (pk->before - pk->after) / denom;
    if (frac > 0.5f) {
        frac = 0.5f;
    } else if (frac < -0.5f) {
        frac = -0.5f;
    }
    return frac;
}

/* The burst a training peak at @p anchor (a ring sample index) + @p frac places, in Gardner input samples. */
static dsd_tetra_ul_burst
ul_burst_at(const dsd_tetra_ul_timing* t, const dsd_tetra_ul_peak* pk, int training_last, int symbols) {
    dsd_tetra_ul_burst b;
    const uint64_t anchor = pk->best_n;
    const float frac = ul_peak_fraction(pk);
    const float score = pk->best;
    /* The rotation the differential phasors carry, as the carrier loop's phase: 90 degree steps are the frame sync's
       (tetra_training.h), so within a quarter turn either way. */
    const float quarter = 1.57079632679489662f;
    b.carrier = pk->best_phase - quarter * floorf(pk->best_phase / quarter + 0.5f);
    const uint64_t sps = (uint64_t)t->sps;
    b.anchor = anchor + (uint64_t)t->delay;
    b.frac = frac;
    b.start = b.anchor - (uint64_t)training_last * sps;
    b.end = b.start + (uint64_t)symbols * sps;
    b.score = score;
    b.pending = 1;
    return b;
}

/* Follow a training's score on sample @p n. Returns 1 when a peak finished: the best sample and its neighbours are in
   @p pk. A peak finishes half a symbol after its best sample. */
static int
ul_peak_step(dsd_tetra_ul_peak* pk, float score, float phase, int conj, uint64_t n, int sps) {
    int finished = 0;
    const uint64_t settle = (uint64_t)(sps / 2 > 1 ? sps / 2 : 1);
    if (pk->active) {
        if (score > pk->best) {
            pk->before = pk->prev;
            pk->best = score;
            pk->best_phase = phase;
            pk->best_conj = conj;
            pk->best_n = n;
        } else if (n == pk->best_n + 1) {
            pk->after = score;
        }
        if (n >= pk->best_n + settle) {
            pk->active = 0;
            finished = 1;
        }
    } else if (score >= DSD_TETRA_UL_TRAINING_SCORE) {
        pk->active = 1;
        pk->before = pk->prev;
        pk->best = score;
        pk->best_phase = phase;
        pk->best_conj = conj;
        pk->best_n = n;
        pk->after = 0.0f;
    }
    pk->prev = score;
    return finished;
}

static void
ul_search(dsd_tetra_ul_timing* t, uint64_t n) {
    const int sps = t->sps;
    float ets_phase = 0.0f;
    float nts_phase = 0.0f;
    int ets_conj = 0;
    int nts_conj = 0;
    const float ets_score = ul_training_score(t, &t->ets_seq, n, &ets_phase, &ets_conj);
    const float nts_score = ul_training_score(t, &t->nts_seq, n, &nts_phase, &nts_conj);

    if (ul_peak_step(&t->nts, nts_score, nts_phase, nts_conj, n, sps)) {
        const dsd_tetra_ul_peak* pk = &t->nts;
        const dsd_tetra_ul_burst nub = ul_burst_at(t, pk, UL_NUB_TRAINING_LAST, UL_NUB_SYMBOLS);
        /* Two peaks within the decision's few symbols cannot both be trainings: the better one waits. */
        if (!t->nub_held || nub.score > t->nub.score) {
            t->nub = nub;
            t->nub_held = 1;
            /* An extended training sequence ending two symbols later finishes its peak by then. */
            t->nub_decide = pk->best_n + (uint64_t)((UL_NTS_IN_ETS + 1) * sps);
        }
    }
    if (ul_peak_step(&t->ets, ets_score, ets_phase, ets_conj, n, sps)) {
        const dsd_tetra_ul_peak* pk = &t->ets;
        const dsd_tetra_ul_burst cb = ul_burst_at(t, pk, UL_CB_TRAINING_LAST, UL_CB_SYMBOLS);
        /* The normal training sequence inside it, two symbols before its end, scored too. It is a control burst when
           the four symbols around that agree with the rest; otherwise payload next to a normal burst's training
           resembled them, and the normal burst stands. */
        const uint64_t nts_in_ets = cb.anchor - (uint64_t)(UL_NTS_IN_ETS * sps);
        const int inside = t->nub_held && t->nub.anchor + 1 >= nts_in_ets && t->nub.anchor <= nts_in_ets + 1;
        if (ul_ets_outer_agreement(t, pk->best_n, pk->best_phase, pk->best_conj) >= UL_CB_OUTER_AGREEMENT) {
            if (inside) {
                t->nub_held = 0;
            }
            ul_burst_found(t, &cb);
        }
    }
    if (t->nub_held && n >= t->nub_decide) {
        t->nub_held = 0;
        ul_burst_found(t, &t->nub);
    }
}

void
dsd_tetra_ul_timing_push(dsd_tetra_ul_timing* t, float in_r, float in_j, float* out_r, float* out_j) {
    if (!t || t->sps < DSD_TETRA_UL_MIN_SPS) {
        *out_r = in_r;
        *out_j = in_j;
        return;
    }
    const uint64_t n = t->pushed;
    const size_t at = (size_t)(n & (DSD_TETRA_UL_RING - 1));
    /* delay samples back; a slot not yet written holds 0. */
    const size_t back = (size_t)((n + DSD_TETRA_UL_RING - (uint64_t)t->delay) & (DSD_TETRA_UL_RING - 1));
    *out_r = t->ring[back * 2];
    *out_j = t->ring[back * 2 + 1];
    t->ring[at * 2] = in_r;
    t->ring[at * 2 + 1] = in_j;
    /* A squelched block pushes exact zeros; anything else holds signal until the look-ahead returns it. */
    if (fabsf(in_r) + fabsf(in_j) > 1e-30f) {
        t->held_until = n + 1U + (uint64_t)t->delay;
    }

    const int sps = t->sps;
    const size_t dat = (size_t)(n & (DSD_TETRA_UL_DIFF_RING - 1));
    float d_re = 0.0f;
    float d_im = 0.0f;
    if (n >= (uint64_t)sps) {
        const size_t pat = (size_t)((n - (uint64_t)sps) & (DSD_TETRA_UL_RING - 1));
        const float p_re = t->ring[pat * 2];
        const float p_im = t->ring[pat * 2 + 1];
        d_re = in_r * p_re + in_j * p_im;
        d_im = in_j * p_re - in_r * p_im;
    }
    t->diff[dat * 2] = d_re;
    t->diff[dat * 2 + 1] = d_im;
    t->diff_pow[dat] = d_re * d_re + d_im * d_im;
    ul_search(t, n);
    t->pushed = n + 1;
}

int
dsd_tetra_ul_timing_align(dsd_tetra_ul_timing* t, double next_strobe, float omega, float* move) {
    *move = 0.0f;
    if (!t || t->sps < DSD_TETRA_UL_MIN_SPS || !(omega > 0.5f)) {
        return 0;
    }
    const double strobe = (double)t->pushed + next_strobe;
    const double lead = (double)(UL_STROBE_LEAD * t->sps);
    const dsd_tetra_ul_burst* chosen = NULL;
    for (int i = 0; i < DSD_TETRA_UL_BURSTS; i++) {
        dsd_tetra_ul_burst* b = &t->bursts[i];
        if (b->end == 0 || !b->pending || strobe < (double)b->start - lead) {
            continue;
        }
        b->pending = 0;
        /* Past its training the strobe stays: the Gardner has read the burst up to there already. */
        if (strobe <= (double)b->anchor + (double)b->frac && (!chosen || b->start > chosen->start)) {
            chosen = b;
        }
    }
    if (!chosen) {
        return 0;
    }
    const double period = (double)omega;
    const double diff = (double)chosen->anchor + (double)chosen->frac - strobe;
    *move = (float)(diff - period * floor(diff / period + 0.5));
    /* The carrier loop takes the burst's rotation from the symbol after the one the Gardner is emitting. */
    t->carrier_pending = 1;
    t->carrier_at = t->symbols_out + 1;
    t->carrier_phase = chosen->carrier;
    return 1;
}

int
dsd_tetra_ul_timing_carrier(dsd_tetra_ul_timing* t, float* phase) {
    const uint64_t at = t->carrier_seen++;
    if (!t->carrier_pending || at != t->carrier_at) {
        return 0;
    }
    t->carrier_pending = 0;
    *phase = t->carrier_phase;
    return 1;
}

size_t
dsd_tetra_ul_timing_held(const dsd_tetra_ul_timing* t) {
    if (!t || t->sps < DSD_TETRA_UL_MIN_SPS || t->held_until <= t->pushed) {
        return 0U;
    }
    return (size_t)(t->held_until - t->pushed);
}
