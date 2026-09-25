// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DCS detector over the decimated sub-audible stream (issue #523).
 *
 * A DCS transmitter repeats one 23-bit Golay word at 134.4 bit/s as NRZ (runtime/analog_tones.h
 * has the words). Before it reaches this detector the waveform has been through the receiver's
 * DC blocking: the demodulator's (a one-pole high-pass, about 3.7 Hz at 48 kHz), a sound card's
 * coupling on PCM input, and the front end's own 10 Hz DC blocker. Each makes the level of a run
 * of equal bits sag toward zero. So, per decimated sample:
 *
 *   1. Re-pole: the front end's DC blocker is known exactly, so it is undone and a 0.5 Hz pole
 *      put in its place, which keeps the removal of any DC offset but not the sag.
 *   2. Integrate over each bit (a boxcar of one bit), the matched filter for NRZ, read at the end
 *      of each bit.
 *   3. Bit clock: the difference of two consecutive bit integrals is a triangle that peaks at
 *      the end of a bit a transition started, whatever the sag. Its energy, averaged over about
 *      eight bits, has a component at the bit rate whose angle says where in the bit those
 *      peaks fall (square-law timing recovery); each bit end is steered halfway onto it. Being
 *      an average over every phase of the bit, it has no false lock half a bit off, which an
 *      early/late gate on the triangle has.
 *   4. Slice with decision feedback, once per droop hypothesis. A one-pole high-pass of time
 *      constant tau takes the level of each bit toward zero by d = e^(-T/tau) per bit, and the
 *      level it has taken away follows the bits decided so far. Each slicer predicts that
 *      baseline under its own d (none, and three sags that cover the demodulator's DC block
 *      at 8 to 78 kHz and a sound card's 10 Hz coupling) and adds it back before deciding. A
 *      plain slicer loses over 1% of bits at 3 dB in-band under the demodulator's DC block;
 *      the matching hypothesis about 0.5%.
 *   5. Acquire: a slicer that reads a supported code's word twice in a row -- its newest 23
 *      decisions and the 23 before them are one rotation of that word, in either polarity,
 *      exactly in one window and within one bit in the other -- locks the code's class under
 *      its canonical name (dsd_dcs_match()). Asking for both windows exactly would lock at 3 dB
 *      only when 46 bits in a row come through clean, which several starts in a hundred do not
 *      manage within 700 ms; one bit of slack in either window makes that rare. Random bits read that way about once in 6 x 10^8 bits per slicer, some 50
 *      days of noise at 134.4 bit/s.
 *   6. Hold: every bit the expected window rotates by one; the lock holds while some slicer
 *      reads it within one bit (a slip of one bit either way is followed), and is lost after
 *      32 consecutive bits without, or at once when the 134.4 Hz turn-off tone dominates the
 *      band for two bits. A bit integral over exactly one period of 134.4 Hz is zero, so the
 *      tone reads as nothing to the slicers; a correlator over the newest six bits finds it.
 *
 * A carrier with no code reads ACQUIRING until 500 ms of it have been evaluated, then NONE,
 * like the CTCSS detector. Samples inside the carrier hangover keep the bit clock and the
 * windows moving but change no verdict.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "analog_rx_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
    DCS_WORD_BITS = DSD_DCS_WORD_BITS,
    DCS_RING_MASK = DSD_ANALOG_DCS_RING - 1,
};

_Static_assert((DSD_ANALOG_DCS_RING & (DSD_ANALOG_DCS_RING - 1)) == 0, "the sample ring is a power of two");
_Static_assert(DSD_ANALOG_DCS_HISTORY_BITS == 2 * DSD_DCS_WORD_BITS, "each slicer keeps exactly two words");

#define DCS_WORD_MASK ((1U << DCS_WORD_BITS) - 1U)

/* Per-bit sag of each droop hypothesis: none; the demodulator's DC block (2048 samples) at
   48 kHz (0.84) and at 78.125 kHz (0.75, near 0.72); a 10 Hz coupling (0.63, near 0.55). The
   same block at 8 kHz sags by 0.97 per bit, which the first slicer reads as well as a
   matching one. */
static const double k_droop[DSD_ANALOG_DCS_HYPOTHESES] = {1.0, 0.84, 0.72, 0.55};

/* The bit clock: its edge-energy average spans about this many bits, and each bit end moves
   this share of the way onto the phase it reads. */
static const double k_clock_bits = 8.0;
static const double k_clock_gain = 0.5;

/* Weight of a new bit in each slicer's level estimate. */
static const double k_amp_alpha = 1.0 / 16.0;

/* The turn-off tone ends a lock when it carries this share of the band's power over the
   newest TURNOFF_BITS bits: about 1 for the tone, a few percent for any DCS word, whose NRZ
   spectrum is null at the bit rate. */
static const double k_turnoff_share = 0.5;

/* Carrier time without a lock after which the verdict is "no code" (as for CTCSS). */
static const double k_no_code_ms = 500.0;

static uint32_t
dcs_rotr(uint32_t word, int k) {
    k %= DCS_WORD_BITS;
    if (k < 0) {
        k += DCS_WORD_BITS;
    }
    if (k == 0) {
        return word & DCS_WORD_MASK;
    }
    return ((word >> k) | (word << (DCS_WORD_BITS - k))) & DCS_WORD_MASK;
}

static int
dcs_popcount23(uint32_t x) {
    x &= DCS_WORD_MASK;
    int count = 0;
    while (x != 0U) {
        x &= x - 1U;
        count++;
    }
    return count;
}

static void
dcs_clear_slicers(dsd_analog_dcs* det) {
    for (int j = 0; j < DSD_ANALOG_DCS_HYPOTHESES; j++) {
        dsd_analog_dcs_slicer* s = &det->slicer[j];
        s->baseline = 0.0;
        s->amp = 0.0;
        s->bits = 0U;
        s->count = 0;
    }
}

static void
dcs_reset(void* ctx) {
    dsd_analog_dcs* det = (dsd_analog_dcs*)ctx;
    if (!det) {
        return;
    }
    det->x1 = 0.0;
    det->u1 = 0.0;
    DSD_MEMSET(det->u, 0, sizeof(det->u));
    det->n = 0;
    det->next_bit = det->samples_per_bit;
    det->clock_re = 0.0;
    det->clock_im = 0.0;
    det->clock_heard = 0;
    dcs_clear_slicers(det);
    det->osc_re = 1.0;
    det->osc_im = 0.0;
    det->bit_re = 0.0;
    det->bit_im = 0.0;
    det->bit_energy = 0.0;
    det->bit_samples = 0;
    det->bit_open = 0;
    DSD_MEMSET(det->ring_re, 0, sizeof(det->ring_re));
    DSD_MEMSET(det->ring_im, 0, sizeof(det->ring_im));
    DSD_MEMSET(det->ring_energy, 0, sizeof(det->ring_energy));
    DSD_MEMSET(det->ring_samples, 0, sizeof(det->ring_samples));
    det->ring_head = 0;
    det->ring_count = 0;
    det->turnoff_run = 0;
    det->state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    det->code = -1;
    det->inverted = 0;
    det->expected = 0U;
    det->fail_run = 0;
    det->open_samples = 0;
    det->bits_decided = 0;
}

static void
dcs_configure(void* ctx, double rate_hz) {
    dsd_analog_dcs* det = (dsd_analog_dcs*)ctx;
    if (!det) {
        return;
    }
    det->rate_hz = rate_hz > 0.0 ? rate_hz : 0.0;
    det->samples_per_bit = det->rate_hz / DSD_ANALOG_DCS_BAUD;
    det->box_len = (int)lround(det->samples_per_bit);
    if (det->box_len < 1) {
        det->box_len = 1;
    }
    det->clock_alpha = det->samples_per_bit > 0.0 ? 1.0 / (k_clock_bits * det->samples_per_bit) : 0.0;
    if (det->rate_hz > 0.0) {
        det->fe_pole = exp(-2.0 * M_PI * DSD_ANALOG_RX_DC_CORNER_HZ / det->rate_hz);
        det->slow_pole = exp(-2.0 * M_PI * DSD_ANALOG_DCS_DC_CORNER_HZ / det->rate_hz);
        const double w = 2.0 * M_PI * DSD_ANALOG_DCS_BAUD / det->rate_hz;
        det->step_re = cos(w);
        det->step_im = -sin(w);
    }
    for (int j = 0; j < DSD_ANALOG_DCS_HYPOTHESES; j++) {
        const double d = k_droop[j];
        det->slicer[j].droop = d;
        det->slicer[j].gain = (d < 1.0) ? (1.0 - d) / -log(d) : 1.0;
    }
    dcs_reset(det);
}

/* The bit integral ending at sample index @p k (the newest box_len re-poled samples). */
static double
dcs_box_at(const dsd_analog_dcs* det, int64_t k) {
    double sum = 0.0;
    for (int i = 0; i < det->box_len; i++) {
        sum += det->u[(uint64_t)(k - i) & (uint64_t)DCS_RING_MASK];
    }
    return sum;
}

/* The bit integral ending at fractional sample time @p t. */
static double
dcs_box(const dsd_analog_dcs* det, double t) {
    const double k = floor(t);
    const double f = t - k;
    const int64_t i = (int64_t)k;
    return ((1.0 - f) * dcs_box_at(det, i)) + (f * dcs_box_at(det, i + 1));
}

/* The difference of the bit integrals ending at sample index @p k and a bit before it: a
   triangle that peaks where a transition began the later of the two. */
static double
dcs_edge_at(const dsd_analog_dcs* det, int64_t k) {
    return dcs_box_at(det, k) - dcs_box_at(det, k - det->box_len);
}

/* Where in the bit the edges fall, as a share of a bit in the phasor's reference (sample n is
   at n / samples_per_bit), or -1 while the clock has heard nothing. */
static double
dcs_clock_phase(const dsd_analog_dcs* det) {
    if (!det->clock_heard) {
        return -1.0;
    }
    double phase = -atan2(det->clock_im, det->clock_re) / (2.0 * M_PI);
    if (phase < 0.0) {
        phase += 1.0;
    }
    return phase;
}

/* Steer the next bit end halfway onto the phase the clock reads (see step 3). */
static void
dcs_steer(dsd_analog_dcs* det) {
    const double target = dcs_clock_phase(det);
    if (target < 0.0) {
        return;
    }
    const double at = fmod(det->next_bit, det->samples_per_bit) / det->samples_per_bit;
    double delta = target - at;
    if (delta >= 0.5) {
        delta -= 1.0;
    } else if (delta < -0.5) {
        delta += 1.0;
    }
    det->next_bit += k_clock_gain * delta * det->samples_per_bit;
}

/* One slicer's decision on a bit integral, and its droop and level updates. */
static void
dcs_slice(dsd_analog_dcs_slicer* s, double integral) {
    const double level = (integral / s->gain) + s->baseline;
    const int bit = level > 0.0 ? 1 : 0;
    const double mag = fabs(level);
    s->amp = (s->amp > 0.0) ? s->amp + (k_amp_alpha * (mag - s->amp)) : mag;
    s->baseline = (s->droop * s->baseline) + ((1.0 - s->droop) * (bit ? s->amp : -s->amp));
    s->bits = (s->bits >> 1) | ((uint64_t)bit << (DSD_ANALOG_DCS_HISTORY_BITS - 1));
    if (s->count < DSD_ANALOG_DCS_HISTORY_BITS) {
        s->count++;
    }
}

/* A slicer's newest 23 decisions, the earliest in bit 0; and the 23 before them. */
static uint32_t
dcs_window(const dsd_analog_dcs_slicer* s) {
    return (uint32_t)(s->bits >> DCS_WORD_BITS) & DCS_WORD_MASK;
}

static uint32_t
dcs_window_before(const dsd_analog_dcs_slicer* s) {
    return (uint32_t)s->bits & DCS_WORD_MASK;
}

static void
dcs_lock(dsd_analog_dcs* det, int code, int inverted, uint32_t window) {
    det->state = DSD_ANALOG_TONE_STATE_LOCKED;
    det->code = code;
    det->inverted = inverted;
    det->expected = window;
    det->fail_run = 0;
}

static void
dcs_unlock(dsd_analog_dcs* det) {
    det->state = DSD_ANALOG_TONE_STATE_NONE;
    det->code = -1;
    det->inverted = 0;
    det->expected = 0U;
    det->fail_run = 0;
}

/* The word a slicer read twice in a row: its newest 23 decisions and the 23 before them are a
   supported code's word, one exactly and the other within DSD_ANALOG_DCS_ACQUIRE_DISTANCE bits.
   Writes the exact one (the signal is periodic, so it is also what the newest window should
   read) and its class. */
static int
dcs_read_twice(const dsd_analog_dcs_slicer* s, uint32_t* word, int* code, int* inverted) {
    if (s->count < DSD_ANALOG_DCS_HISTORY_BITS) {
        return 0;
    }
    const uint32_t newest = dcs_window(s);
    const uint32_t before = dcs_window_before(s);
    if (dcs_popcount23(newest ^ before) > DSD_ANALOG_DCS_ACQUIRE_DISTANCE) {
        return 0;
    }
    if (dsd_dcs_match(newest, code, inverted)) {
        *word = newest;
        return 1;
    }
    if (dsd_dcs_match(before, code, inverted)) {
        *word = before;
        return 1;
    }
    return 0;
}

/* A slicer that read a supported word twice in a row locks it, or -1. A slicer that reads the
   locked class again does not count: the hold is its business. */
static int
dcs_acquire(dsd_analog_dcs* det) {
    for (int j = 0; j < DSD_ANALOG_DCS_HYPOTHESES; j++) {
        uint32_t word = 0U;
        int code = -1;
        int inverted = 0;
        if (!dcs_read_twice(&det->slicer[j], &word, &code, &inverted)) {
            continue;
        }
        if (det->state == DSD_ANALOG_TONE_STATE_LOCKED && code == det->code && inverted == det->inverted) {
            continue;
        }
        dcs_lock(det, code, inverted, word);
        return j;
    }
    return -1;
}

/* Whether some slicer reads the expected window, or one slipped a bit either way, within the
   hold distance; follows a slip. */
static int
dcs_hold(dsd_analog_dcs* det) {
    static const int k_slips[] = {0, 1, -1};
    for (int si = 0; si < 3; si++) {
        const uint32_t expected = dcs_rotr(det->expected, k_slips[si]);
        for (int j = 0; j < DSD_ANALOG_DCS_HYPOTHESES; j++) {
            const dsd_analog_dcs_slicer* s = &det->slicer[j];
            if (s->count < DCS_WORD_BITS) {
                continue;
            }
            if (dcs_popcount23(dcs_window(s) ^ expected) <= DSD_ANALOG_DCS_HOLD_DISTANCE) {
                det->expected = expected;
                return 1;
            }
        }
    }
    return 0;
}

/* Close this bit's turn-off correlation into the ring and measure the tone's share of the band
   over the newest TURNOFF_BITS bits: 1 for a pure 134.4 Hz tone. */
static double
dcs_turnoff_share(dsd_analog_dcs* det) {
    const int slot = det->ring_head;
    det->ring_re[slot] = det->bit_re;
    det->ring_im[slot] = det->bit_im;
    det->ring_energy[slot] = det->bit_energy;
    det->ring_samples[slot] = det->bit_samples;
    det->ring_head = (det->ring_head + 1) % DSD_ANALOG_DCS_TURNOFF_BITS;
    if (det->ring_count < DSD_ANALOG_DCS_TURNOFF_BITS) {
        det->ring_count++;
    }
    det->bit_re = 0.0;
    det->bit_im = 0.0;
    det->bit_energy = 0.0;
    det->bit_samples = 0;
    /* Renormalise the phasor once per bit so rounding never grows its gain. */
    const double mag = sqrt((det->osc_re * det->osc_re) + (det->osc_im * det->osc_im));
    if (mag > 0.0) {
        det->osc_re /= mag;
        det->osc_im /= mag;
    }
    if (det->ring_count < DSD_ANALOG_DCS_TURNOFF_BITS) {
        return 0.0;
    }
    double re = 0.0;
    double im = 0.0;
    double energy = 0.0;
    int samples = 0;
    for (int i = 0; i < DSD_ANALOG_DCS_TURNOFF_BITS; i++) {
        re += det->ring_re[i];
        im += det->ring_im[i];
        energy += det->ring_energy[i];
        samples += det->ring_samples[i];
    }
    /* A tone of amplitude a over N samples correlates to a N / 2 and carries a^2 N / 2. */
    const double denom = energy * (double)samples / 2.0;
    return denom > 0.0 ? ((re * re) + (im * im)) / denom : 0.0;
}

/* The verdict after a bit that arrived with the carrier open. */
static void
dcs_judge(dsd_analog_dcs* det, int turnoff) {
    if (det->state == DSD_ANALOG_TONE_STATE_LOCKED) {
        if (turnoff) {
            dcs_unlock(det);
            /* What the slicers hold is the transmission that just ended. */
            dcs_clear_slicers(det);
            return;
        }
        if (dcs_acquire(det) >= 0) {
            return;
        }
        if (dcs_hold(det)) {
            det->fail_run = 0;
        } else if (++det->fail_run >= DSD_ANALOG_DCS_LOSE_BITS) {
            dcs_unlock(det);
        }
        return;
    }
    if (dcs_acquire(det) >= 0) {
        return;
    }
    const double open_ms = (double)det->open_samples * 1000.0 / det->rate_hz;
    if (det->state == DSD_ANALOG_TONE_STATE_ACQUIRING && open_ms >= k_no_code_ms) {
        det->state = DSD_ANALOG_TONE_STATE_NONE;
    }
}

/* Read the bit that ends at next_bit: slice it, time the next one, and judge. */
static void
dcs_read_bit(dsd_analog_dcs* det) {
    const int open = det->bit_open;
    det->bit_open = 0;
    const double integral = dcs_box(det, det->next_bit);
    for (int j = 0; j < DSD_ANALOG_DCS_HYPOTHESES; j++) {
        dcs_slice(&det->slicer[j], integral);
    }
    det->bits_decided++;
    if (det->state == DSD_ANALOG_TONE_STATE_LOCKED) {
        det->expected = dcs_rotr(det->expected, 1);
    }
    const double share = dcs_turnoff_share(det);
    det->turnoff_run = (open && share >= k_turnoff_share) ? det->turnoff_run + 1 : 0;
    det->next_bit += det->samples_per_bit;
    dcs_steer(det);
    if (open) {
        dcs_judge(det, det->turnoff_run >= DSD_ANALOG_DCS_TURNOFF_RUN);
    }
}

static void
dcs_push_sample(dsd_analog_dcs* det, double x, int freeze) {
    const double u = (det->slow_pole * det->u1) + x - (det->fe_pole * det->x1);
    det->x1 = x;
    det->u1 = u;
    det->u[(uint64_t)det->n & (uint64_t)DCS_RING_MASK] = u;
    if (!freeze) {
        /* The clock hears the edges only while the carrier is open. */
        const double edge = dcs_edge_at(det, det->n);
        const double energy = edge * edge;
        det->clock_re += det->clock_alpha * ((energy * det->osc_re) - det->clock_re);
        det->clock_im += det->clock_alpha * ((energy * det->osc_im) - det->clock_im);
        det->clock_heard = 1;
    }
    det->bit_re += u * det->osc_re;
    det->bit_im += u * det->osc_im;
    det->bit_energy += u * u;
    det->bit_samples++;
    const double ore = det->osc_re;
    det->osc_re = (ore * det->step_re) - (det->osc_im * det->step_im);
    det->osc_im = (ore * det->step_im) + (det->osc_im * det->step_re);
    if (!freeze) {
        det->open_samples++;
        det->bit_open = 1;
    }
    det->n++;
}

static void
dcs_process(void* ctx, const float* band, const float* wide, const float* full, int count, int freeze) {
    (void)wide;
    (void)full;
    dsd_analog_dcs* det = (dsd_analog_dcs*)ctx;
    if (!det || !band || count <= 0 || det->rate_hz <= 0.0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        dcs_push_sample(det, (double)band[i], freeze);
        /* A bit is read once the sample after its end has arrived, for the interpolation. */
        while ((double)(det->n - 1) >= det->next_bit + 1.0) {
            dcs_read_bit(det);
        }
    }
}

static void
dcs_report(const void* ctx, dsd_analog_rx_report* out) {
    const dsd_analog_dcs* det = (const dsd_analog_dcs*)ctx;
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    out->kind = DSD_ANALOG_TONE_KIND_NONE;
    if (!det) {
        return;
    }
    out->state = det->state;
    if (det->state == DSD_ANALOG_TONE_STATE_LOCKED && det->code >= 0) {
        out->kind = DSD_ANALOG_TONE_KIND_DCS;
        out->dcs_code = det->code;
        out->dcs_inverted = det->inverted;
    }
}

const dsd_analog_rx_detector_ops dsd_analog_dcs_ops = {
    "dcs", dcs_configure, dcs_reset, dcs_process, dcs_report,
};
