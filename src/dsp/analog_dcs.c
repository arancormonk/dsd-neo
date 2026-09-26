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
 * of equal bits sag toward zero. A carrier off frequency adds a DC level that steps in with the
 * carrier, and an input with no DC block ahead of the front end (a DC-coupled PCM source) keeps
 * all of it. So, per decimated sample:
 *
 *   1. Re-pole: the front end's DC blocker is known exactly, so it is undone twice over. The
 *      re-poled stream puts a 0.5 Hz pole in its place, which barely sags a run but still
 *      removes a DC offset, slowly (about 0.3 s per 1/e); the balance stream puts a 0.01 Hz
 *      pole there, which leaves an offset all but constant over the two words a lock is read
 *      from.
 *   2. Integrate each stream over each bit (a boxcar of one bit), the matched filter for NRZ,
 *      read at the end of each bit.
 *   3. Bit clock: the difference of two consecutive bit integrals of the re-poled stream is a
 *      triangle that peaks at the end of a bit a transition started, whatever the sag. Its
 *      energy, averaged over about eight bits, has a component at the bit rate whose angle says
 *      where in the bit those peaks fall (square-law timing recovery); each bit end is steered
 *      halfway onto it. Being an average over every phase of the bit, it has no false lock half a
 *      bit off, which an early/late gate on the triangle has.
 *   4. Slice with decision feedback, once per droop hypothesis. A one-pole high-pass of time
 *      constant tau takes the level of each bit toward zero by d = e^(-T/tau) per bit, and the
 *      level it has taken away follows the bits decided so far. Each slicer predicts that
 *      baseline under its own d (none, and three sags that cover the demodulator's DC block at 8
 *      to 78 kHz and a sound card's 10 Hz coupling) and adds it back before deciding. A plain
 *      slicer loses over 1% of bits at 3 dB in-band under the demodulator's DC block; the
 *      matching hypothesis about 0.5%. These slicers read the re-poled stream, so a DC step
 *      larger than the code holds every one of them on one polarity until the step has decayed
 *      below the code's level. The balance slicer, fifth, reads the balance stream: every
 *      supported code's word carries 11 or 12 ones, so its level averages to within 1/23 of zero
 *      over any 23 bits, and the slicer decides each of its two 23-bit windows against that
 *      window's own mean of bit integrals, which carries any offset that holds over a word and
 *      next to nothing of the code.
 *   5. Acquire: a slicer that reads a supported code's word twice in a row -- its newest 23
 *      decisions and the 23 before them are one rotation of that word, in either polarity,
 *      exactly in one window and within one bit in the other -- locks the code's class under its
 *      canonical name (dsd_dcs_match()). Asking for both windows exactly would lock only once
 *      46 bits in a row come through clean: over 100,000 starts at each rate and de-emphasis the
 *      3 dB p95 rose from 377-416 ms to 436-666 ms, past the 450 ms target in 7 rows of 8, 8
 *      starts passed the 1,500 ms ceiling, and at 10 dB 2 of 800,000 passed the 520 ms bound (the
 *      slowest 553 ms, against 422 ms with the slack). Noise reads that way about once in
 *      6 x 10^8 bits per droop slicer, some 50 days at 134.4 bit/s, and about 2.3 times as often
 *      through the balance slicer, whose windows are balanced like a code's (once in
 *      2.7 x 10^8 bits, some 23 days); with all five, at most once in 10^8 bits, some 8 days.
 *   6. Hold: every bit the expected window rotates by one; the lock holds while some slicer reads
 *      it within one bit, the balance slicer exactly (a slip of one bit either way is followed;
 *      noise the balance slicer reads within a bit of the expected word held a stopped code past
 *      450 ms about twice as often as without it), or reads the locked class exactly at another
 *      place in the word, which it then follows: the same code starting over elsewhere in its
 *      word, as a radio that re-keys inside the carrier hangover or another transmitter behind a
 *      repeater does. It is lost after 32 bits without a hold, or at the first bit without once
 *      the 134.4 Hz turn-off tone has carried over a third of the band's power for two bits. A
 *      bit integral over exactly one period of 134.4 Hz is zero, so the tone reads as nothing to
 *      the slicers; a correlator over the newest six bits finds it. The tone ends a lock only
 *      once the code has gone too: a steady component near 134.4 Hz under a code the slicers
 *      still read (an interferer, a voice that holds the pitch) leaves the lock alone. A window
 *      that holds a bit read with the carrier closed can hold no word, so the 32 bits count only
 *      windows read wholly with the carrier open, and a lock that has not held for 64 bits
 *      (476 ms), with the carrier open or closed, is lost: enough for the same code to come back
 *      at another place after a dropout up to the hangover (the gap, a word to clear the windows
 *      of it and the bit clock's settling), and a bound on how long a stopped code stays shown
 *      under a carrier that keeps dropping out, whose windows are never read wholly open.
 *
 * A carrier with no code reads ACQUIRING until 500 ms of it have been evaluated, then NONE,
 * like the CTCSS detector. Samples inside the carrier hangover keep the bit clock and the
 * windows moving but change no verdict on what they read; only the 64-bit span, which is
 * carrier time, can run out on them.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <math.h>
#include <stdint.h>
#include "analog_rx_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
    DCS_WORD_BITS = DSD_DCS_WORD_BITS,
    DCS_RING_MASK = DSD_ANALOG_DCS_RING - 1,
    /* The balance slicer's place in the slicer array, after the droop slicers. */
    DCS_BALANCE = DSD_ANALOG_DCS_HYPOTHESES,
};

_Static_assert((DSD_ANALOG_DCS_RING & (DSD_ANALOG_DCS_RING - 1)) == 0, "the sample ring is a power of two");
_Static_assert(DSD_ANALOG_DCS_HISTORY_BITS == 2 * DSD_DCS_WORD_BITS, "each slicer keeps exactly two words");
_Static_assert(DSD_ANALOG_DCS_SLICERS == DSD_ANALOG_DCS_HYPOTHESES + 1, "the balance slicer follows the droop slicers");

/* The longest bit integral the ring must serve: every decimated rate is below twice the target
   rate (fs / floor(fs / 2400) < 4800 Hz), where a bit at 134.4 bit/s is under 36 samples. */
enum { DCS_MAX_BOX_LEN = ((2 * DSD_ANALOG_RX_TARGET_RATE_HZ * 10) + 1343) / 1344 };

_Static_assert(DSD_ANALOG_DCS_RING >= (2 * DCS_MAX_BOX_LEN) + 2,
               "the ring holds two bit integrals and a sample for the interpolation");

#define DCS_WORD_MASK ((1U << DCS_WORD_BITS) - 1U)

/* Per-bit sag of each droop hypothesis: none; the demodulator's DC block (2048 samples) at
   48 kHz (0.84) and at 78.125 kHz (0.75, near 0.72); a 10 Hz coupling (0.63, near 0.55). The
   same block at 8 kHz sags by 0.97 per bit, which the first slicer reads as well as a
   matching one. */
static const double k_droop[] = {1.0, 0.84, 0.72, 0.55};

_Static_assert(sizeof(k_droop) / sizeof(k_droop[0]) == DSD_ANALOG_DCS_HYPOTHESES,
               "one per-bit sag for each droop slicer");

/* The bit clock: its edge-energy average spans about this many bits, and each bit end moves
   this share of the way onto the phase it reads. */
static const double k_clock_bits = 8.0;
static const double k_clock_gain = 0.5;

/* Weight of a new bit in each slicer's level estimate. */
static const double k_amp_alpha = 1.0 / 16.0;

/* The turn-off tone counts once it carries this share of the band's power over the newest
   TURNOFF_BITS bits: about 1 for the tone in the clear and about 0.5 at 3 dB in-band, a few
   percent for any DCS word, whose NRZ spectrum is null at the bit rate. Anything else that
   reaches it (a steady component near 134.4 Hz, a voice) ends no lock while the code is still
   read (dcs_judge()), which is what lets it sit below the 3 dB level. */
static const double k_turnoff_share = 0.35;

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
    for (int j = 0; j < DSD_ANALOG_DCS_SLICERS; j++) {
        dsd_analog_dcs_slicer* s = &det->slicer[j];
        s->baseline = 0.0;
        s->amp = 0.0;
        s->bits = 0U;
        s->count = 0;
    }
    DSD_MEMSET(det->balance_integrals, 0, sizeof(det->balance_integrals));
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
    det->v1 = 0.0;
    DSD_MEMSET(det->v, 0, sizeof(det->v));
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
    det->since_held = 0;
    det->since_frozen = 0;
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
    if (det->box_len > DCS_MAX_BOX_LEN) {
        /* A rate the front end never delivers: the ring could not hold two bits of it. */
        det->rate_hz = 0.0;
        det->samples_per_bit = 0.0;
        det->box_len = 1;
    }
    det->clock_alpha = det->samples_per_bit > 0.0 ? 1.0 / (k_clock_bits * det->samples_per_bit) : 0.0;
    if (det->rate_hz > 0.0) {
        det->fe_pole = exp(-2.0 * M_PI * DSD_ANALOG_RX_DC_CORNER_HZ / det->rate_hz);
        det->slow_pole = exp(-2.0 * M_PI * DSD_ANALOG_DCS_DC_CORNER_HZ / det->rate_hz);
        det->balance_pole = exp(-2.0 * M_PI * DSD_ANALOG_DCS_BALANCE_CORNER_HZ / det->rate_hz);
        const double w = 2.0 * M_PI * DSD_ANALOG_DCS_BAUD / det->rate_hz;
        det->step_re = cos(w);
        det->step_im = -sin(w);
    }
    for (int j = 0; j < DSD_ANALOG_DCS_HYPOTHESES; j++) {
        const double d = k_droop[j];
        det->slicer[j].droop = d;
        det->slicer[j].gain = (d < 1.0) ? (1.0 - d) / -log(d) : 1.0;
    }
    det->slicer[DCS_BALANCE].droop = 1.0;
    det->slicer[DCS_BALANCE].gain = 1.0;
    dcs_reset(det);
}

/* The bit integral of the stream in @p ring (det->u or det->v) ending at sample index @p k: its
   newest box_len samples. */
static double
dcs_box_at(const dsd_analog_dcs* det, const double* ring, int64_t k) {
    double sum = 0.0;
    for (int i = 0; i < det->box_len; i++) {
        sum += ring[(uint64_t)(k - i) & (uint64_t)DCS_RING_MASK];
    }
    return sum;
}

/* The bit integral of the stream in @p ring ending at fractional sample time @p t. */
static double
dcs_box(const dsd_analog_dcs* det, const double* ring, double t) {
    const double k = floor(t);
    const double f = t - k;
    const int64_t i = (int64_t)k;
    return ((1.0 - f) * dcs_box_at(det, ring, i)) + (f * dcs_box_at(det, ring, i + 1));
}

/* The difference of the re-poled bit integrals ending at sample index @p k and a bit before
   it: a triangle that peaks where a transition began the later of the two. */
static double
dcs_edge_at(const dsd_analog_dcs* det, int64_t k) {
    return dcs_box_at(det, det->u, k) - dcs_box_at(det, det->u, k - det->box_len);
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

/* The balance slicer's decisions once a new bit integral of the balance stream arrives (see
   step 4): each of its two words is sliced against that word's own mean, which carries any DC
   offset that holds over the word. */
static void
dcs_slice_balance(dsd_analog_dcs* det, double integral) {
    DSD_MEMMOVE(det->balance_integrals, &det->balance_integrals[1],
                sizeof(det->balance_integrals) - sizeof(det->balance_integrals[0]));
    det->balance_integrals[DSD_ANALOG_DCS_HISTORY_BITS - 1] = integral;
    uint64_t bits = 0U;
    for (int first = 0; first < DSD_ANALOG_DCS_HISTORY_BITS; first += DCS_WORD_BITS) {
        double mean = 0.0;
        for (int i = first; i < first + DCS_WORD_BITS; i++) {
            mean += det->balance_integrals[i];
        }
        mean /= (double)DCS_WORD_BITS;
        for (int i = first; i < first + DCS_WORD_BITS; i++) {
            if (det->balance_integrals[i] > mean) {
                bits |= (uint64_t)1 << i;
            }
        }
    }
    dsd_analog_dcs_slicer* s = &det->slicer[DCS_BALANCE];
    s->bits = bits;
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
    det->since_held = 0;
}

static void
dcs_unlock(dsd_analog_dcs* det) {
    det->state = DSD_ANALOG_TONE_STATE_NONE;
    det->code = -1;
    det->inverted = 0;
    det->expected = 0U;
    det->fail_run = 0;
    det->since_held = 0;
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
    for (int j = 0; j < DSD_ANALOG_DCS_SLICERS; j++) {
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
   hold distance (the balance slicer exactly), and follows a slip; or reads the locked class
   exactly at another place in the word, and follows it there. The same code can start over
   anywhere in its word under a held lock: a radio that re-keys inside the carrier hangover, or
   another transmitter behind a repeater whose carrier stays up. The balance slicer's windows
   are balanced like a code's even in noise, and with a bit of slack the noise after a stop
   held a stopped code about twice as often (see step 6). */
static int
dcs_hold(dsd_analog_dcs* det) {
    static const int k_slips[] = {0, 1, -1};
    for (int si = 0; si < (int)(sizeof(k_slips) / sizeof(k_slips[0])); si++) {
        const uint32_t expected = dcs_rotr(det->expected, k_slips[si]);
        for (int j = 0; j < DSD_ANALOG_DCS_SLICERS; j++) {
            const dsd_analog_dcs_slicer* s = &det->slicer[j];
            if (s->count < DCS_WORD_BITS) {
                continue;
            }
            const int distance = (j == DCS_BALANCE) ? 0 : DSD_ANALOG_DCS_HOLD_DISTANCE;
            if (dcs_popcount23(dcs_window(s) ^ expected) <= distance) {
                det->expected = expected;
                return 1;
            }
        }
    }
    for (int j = 0; j < DSD_ANALOG_DCS_SLICERS; j++) {
        const dsd_analog_dcs_slicer* s = &det->slicer[j];
        if (s->count < DCS_WORD_BITS) {
            continue;
        }
        const uint32_t window = dcs_window(s);
        int code = -1;
        int inverted = 0;
        if (dsd_dcs_match(window, &code, &inverted) && code == det->code && inverted == det->inverted) {
            det->expected = window;
            return 1;
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
        if (dcs_acquire(det) >= 0) {
            return;
        }
        if (dcs_hold(det)) {
            det->fail_run = 0;
            det->since_held = 0;
            return;
        }
        if (turnoff) {
            dcs_unlock(det);
            /* What the slicers hold is the transmission that just ended. */
            dcs_clear_slicers(det);
            return;
        }
        /* A window that holds a bit read with the carrier closed can hold no word, so only
           windows read wholly with it open count; the span bounds the rest (see step 6). */
        if (det->since_frozen >= DCS_WORD_BITS) {
            det->fail_run++;
        }
        if (det->fail_run >= DSD_ANALOG_DCS_LOSE_BITS || det->since_held >= DSD_ANALOG_DCS_SPAN_BITS) {
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
    const double integral = dcs_box(det, det->u, det->next_bit);
    for (int j = 0; j < DSD_ANALOG_DCS_HYPOTHESES; j++) {
        dcs_slice(&det->slicer[j], integral);
    }
    dcs_slice_balance(det, dcs_box(det, det->v, det->next_bit));
    det->bits_decided++;
    if (!open) {
        det->since_frozen = 0;
    } else if (det->since_frozen < DCS_WORD_BITS) {
        det->since_frozen++;
    }
    if (det->state == DSD_ANALOG_TONE_STATE_LOCKED) {
        det->expected = dcs_rotr(det->expected, 1);
        if (det->since_held < DSD_ANALOG_DCS_SPAN_BITS) {
            det->since_held++;
        }
    }
    const double share = dcs_turnoff_share(det);
    det->turnoff_run = (open && share >= k_turnoff_share) ? det->turnoff_run + 1 : 0;
    det->next_bit += det->samples_per_bit;
    dcs_steer(det);
    if (open) {
        dcs_judge(det, det->turnoff_run >= DSD_ANALOG_DCS_TURNOFF_RUN);
    } else if (det->state == DSD_ANALOG_TONE_STATE_LOCKED && det->since_held >= DSD_ANALOG_DCS_SPAN_BITS) {
        /* The span is carrier time, not a reading: it runs out inside a dropout as well. */
        dcs_unlock(det);
    }
}

static void
dcs_push_sample(dsd_analog_dcs* det, double x, int freeze) {
    const double u = (det->slow_pole * det->u1) + x - (det->fe_pole * det->x1);
    const double v = (det->balance_pole * det->v1) + x - (det->fe_pole * det->x1);
    det->x1 = x;
    det->u1 = u;
    det->v1 = v;
    det->u[(uint64_t)det->n & (uint64_t)DCS_RING_MASK] = u;
    det->v[(uint64_t)det->n & (uint64_t)DCS_RING_MASK] = v;
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
    if (det->rate_hz <= 0.0) {
        /* Designed for no usable rate: it hears nothing, so the core need not wait for it. */
        out->state = DSD_ANALOG_TONE_STATE_NONE;
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
