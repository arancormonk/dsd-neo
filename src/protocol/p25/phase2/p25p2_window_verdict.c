// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A collected P25 Phase 2 window's ISCH evidence and the verdict on a window a tolerant sync opened
   (p25p2_window_verdict.h). */

#include "p25p2_window_verdict.h"

#include <dsd-neo/fec/ez.h>
#include <dsd-neo/fec/p25p2_isch.h>
#include <stdint.h>

enum {
    P25P2_ISCH_BITS = 40,
    /* Bit offset of collected ISCH 0 in the window: after timeslot 0's 320-bit burst; each timeslot is 360 bits. */
    P25P2_ISCH_FIRST_BIT = 320,
    P25P2_TIMESLOT_BITS = 360,
    /* Bits of the window processP2() collects (700 dibits): the fourth ISCH, the next sync, is past them. */
    P25P2_WINDOW_BITS = 1400,
    P25P2_SUPERFRAME_TIMESLOTS = 12,
    P25P2_WINDOW_TIMESLOTS = 4,
};

int
p25p2_timeslot_mod12(int timeslot) {
    return ((timeslot % P25P2_SUPERFRAME_TIMESLOTS) + P25P2_SUPERFRAME_TIMESLOTS) % P25P2_SUPERFRAME_TIMESLOTS;
}

uint64_t
p25p2_window_isch_word(const int* bits, int fc) {
    if (fc < 0 || fc >= P25P2_WINDOW_ISCHS) {
        return 0U;
    }
    uint64_t word = 0U;
    for (int i = 0; i < P25P2_ISCH_BITS; i++) {
        word = (word << 1) | (uint64_t)(bits[P25P2_ISCH_FIRST_BIT + (P25P2_TIMESLOT_BITS * fc) + i] & 1);
    }
    return word;
}

int
p25p2_window_isch_expected(int start, int fc, int* out_chan, int* out_loc) {
    /* Collected ISCH fc follows window timeslot fc, so it is the one ahead of superframe timeslot start + fc + 1. */
    const int ahead_of = p25p2_timeslot_mod12(start + fc + 1);
    *out_chan = ahead_of % 4;
    *out_loc = ahead_of / 4;
    return *out_chan < 2;
}

p25p2_isch_evidence
p25p2_window_isch_check(const int* bits, int start, int fc) {
    if (fc < 0 || fc >= P25P2_WINDOW_ISCHS) {
        return P25P2_ISCH_NONE;
    }
    int chan = 0;
    int loc = 0;
    const int i_isch = p25p2_window_isch_expected(start, fc, &chan, &loc);
    int distance = P25P2_ISCH_BITS;
    const int nearest = dsd_p25p2_isch_nearest(p25p2_window_isch_word(bits, fc), &distance);
    if (nearest == DSD_P25P2_ISCH_NEAREST_S_ISCH) {
        if (!i_isch) {
            return distance <= P25P2_S_ISCH_EVIDENCE_MAX_BITS ? P25P2_ISCH_MATCH : P25P2_ISCH_NONE;
        }
        return distance <= P25P2_ISCH_EVIDENCE_MAX_BITS ? P25P2_ISCH_CONTRA : P25P2_ISCH_NONE;
    }
    if (distance > P25P2_ISCH_EVIDENCE_MAX_BITS) {
        return P25P2_ISCH_NONE;
    }
    /* An I-ISCH within reach: the one predicted (whatever its free bit and UF count), or one the start rules out. */
    const int got_chan = (nearest >> 5) & 3;
    const int got_loc = (nearest >> 3) & 3;
    return (i_isch && got_chan == chan && got_loc == loc) ? P25P2_ISCH_MATCH : P25P2_ISCH_CONTRA;
}

void
p25p2_window_isch_evidence(const int* bits, int start, p25p2_window_evidence* out) {
    out->i_isch_matches = 0;
    out->s_isch_matches = 0;
    out->contradictions = 0;
    for (int fc = 0; fc < P25P2_WINDOW_ISCHS; fc++) {
        int chan = 0;
        int loc = 0;
        const int i_isch = p25p2_window_isch_expected(start, fc, &chan, &loc);
        out->isch[fc] = p25p2_window_isch_check(bits, start, fc);
        if (out->isch[fc] == P25P2_ISCH_MATCH) {
            if (i_isch) {
                out->i_isch_matches++;
            } else {
                out->s_isch_matches++;
            }
        } else if (out->isch[fc] == P25P2_ISCH_CONTRA) {
            out->contradictions++;
        }
    }
}

uint8_t
p25p2_llr_reliability(const int16_t* llr, int abs_bit) {
    if (abs_bit < 0 || abs_bit >= P25P2_WINDOW_BITS) {
        return 0U;
    }
    int v = llr[abs_bit] < 0 ? -(int)llr[abs_bit] : (int)llr[abs_bit];
    return (uint8_t)(v > 255 ? 255 : v);
}

int
p25p2_window_isch_soft_decode(const int* bits, const int16_t* llr, int fc) {
    if (fc < 0 || fc >= P25P2_WINDOW_ISCHS) {
        return -2;
    }
    const uint64_t word = p25p2_window_isch_word(bits, fc);
    if (word == DSD_P25P2_S_ISCH) {
        return -2;
    }
    uint8_t reliab[P25P2_ISCH_BITS];
    for (int i = 0; i < P25P2_ISCH_BITS; i++) {
        reliab[i] = p25p2_llr_reliability(llr, P25P2_ISCH_FIRST_BIT + (P25P2_TIMESLOT_BITS * fc) + i);
    }
    return isch_lookup_soft(word, reliab);
}

int
p25p2_window_isch_offset(int decoded, int fc) {
    if (decoded < 0 || ((decoded >> 5) & 3) != 1) {
        return -1;
    }
    /* Relative position to the only channel 1 I-ISCHs there are: ahead of superframe timeslots 1, 5 and 9. */
    switch ((decoded >> 3) & 3) {
        case 0: return 12 - fc;
        case 1: return 4 - fc;
        case 2: return 8 - fc;
        default: return -1;
    }
}

int
p25p2_window_soft_offset(const int* bits, const int16_t* llr) {
    int offset = -1;
    for (int fc = 0; fc < P25P2_WINDOW_ISCHS; fc++) {
        const int located = p25p2_window_isch_offset(p25p2_window_isch_soft_decode(bits, llr, fc), fc);
        if (located >= 0) {
            offset = located;
        }
    }
    return offset;
}

int
p25p2_window_duid_dispatches(int duid) {
    switch (duid) {
        case 0:  /* 4V */
        case 6:  /* 2V */
        case 3:  /* SACCH, scrambled */
        case 9:  /* FACCH, scrambled */
        case 4:  /* LCCH, scrambled */
        case 12: /* SACCH */
        case 15: /* FACCH */
        case 13: /* LCCH */
        case 10: /* DATCH */ return 1;
        default: return 0;
    }
}

p25p2_window_verdict
p25p2_tolerant_window_verdict(const int* bits, const int16_t* llr, int start, int context_holds, const int duids[4]) {
    if (!context_holds) {
        return P25P2_WINDOW_FAIL_CONTEXT;
    }
    p25p2_window_evidence evidence;
    p25p2_window_isch_evidence(bits, start, &evidence);
    if (evidence.contradictions > 0) {
        return P25P2_WINDOW_FAIL_CONTRADICTED;
    }
    /* The soft read processP2() positions an exact window by reaches further: a start it would locate elsewhere is a
       contradiction too. */
    const int soft = p25p2_window_soft_offset(bits, llr);
    if (soft >= 0 && p25p2_timeslot_mod12(soft) != p25p2_timeslot_mod12(start)) {
        return P25P2_WINDOW_FAIL_CONTRADICTED;
    }
    if (evidence.i_isch_matches + evidence.s_isch_matches == 0) {
        return P25P2_WINDOW_FAIL_UNCORROBORATED;
    }
    int unhandled = 0;
    for (int ts = 0; ts < P25P2_WINDOW_TIMESLOTS; ts++) {
        unhandled += p25p2_window_duid_dispatches(duids[ts]) ? 0 : 1;
    }
    return unhandled > 1 ? P25P2_WINDOW_FAIL_DUIDS : P25P2_WINDOW_PASS;
}
