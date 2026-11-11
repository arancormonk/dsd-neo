// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What a collected P25 Phase 2 window's own ISCHs and DUIDs say about where it starts in the superframe, and the
 * verdict on a window a tolerant sync opened (dsd_p25p2_sync_anchor).
 *
 * processP2() collects four timeslots after a 20-dibit sync: their bursts, and the ISCH after each of the first three
 * (the fourth's is the next sync, which it does not read). The ISCH ahead of superframe timeslot t (0-11) is an I-ISCH
 * naming channel t mod 4 and location t / 4 when t mod 4 is 0 or 1, and an S-ISCH (the sync word) when it is 2 or 3.
 * Every function here is pure: it reads the window's bits (the p2bit layout) and their LLRs (p2llr), nothing else.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_WINDOW_VERDICT_H_
#define DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_WINDOW_VERDICT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** The ISCHs a window collects: after its timeslots 0, 1 and 2. */
    P25P2_WINDOW_ISCHS = 3,
    /** Bits an I-ISCH may be off by and still count, for or against a start: 11 under the code's distance of 14. */
    P25P2_ISCH_EVIDENCE_MAX_BITS = 3,
    /** Bits the S-ISCH inside a window may be off by and still corroborate its start (the tolerant sync's own). */
    P25P2_S_ISCH_EVIDENCE_MAX_BITS = 4,
};

/** What one collected ISCH says about a start. */
typedef enum {
    P25P2_ISCH_NONE = 0,   /**< nothing within reach: neither for nor against */
    P25P2_ISCH_MATCH = 1,  /**< the ISCH the start predicts there */
    P25P2_ISCH_CONTRA = 2, /**< within reach of an ISCH the start rules out there */
} p25p2_isch_evidence;

/** The three collected ISCHs against a start. */
typedef struct {
    p25p2_isch_evidence isch[P25P2_WINDOW_ISCHS];
    int i_isch_matches; /**< MATCHes at I-ISCH positions */
    int s_isch_matches; /**< MATCHes at the S-ISCH position */
    int contradictions;
} p25p2_window_evidence;

/** The verdict on a window a tolerant sync opened. */
typedef enum {
    P25P2_WINDOW_PASS = 0,
    P25P2_WINDOW_FAIL_CONTEXT = 1,        /**< the stream moved, or continuity no longer predicts the start */
    P25P2_WINDOW_FAIL_CONTRADICTED = 2,   /**< an ISCH names another start */
    P25P2_WINDOW_FAIL_UNCORROBORATED = 3, /**< no ISCH of its own matches the start */
    P25P2_WINDOW_FAIL_DUIDS = 4,          /**< two or more timeslots carry no DUID the dispatch handles */
} p25p2_window_verdict;

/** The 40 bits of collected ISCH @p fc (0-2), bit 39 first; 0 for any other @p fc (the fourth is never collected). */
uint64_t p25p2_window_isch_word(const int* bits, int fc);

/** Whether collected ISCH @p fc of a window starting at superframe timeslot @p start is an I-ISCH (1), with the channel
    and location it names, or the S-ISCH (0). */
int p25p2_window_isch_expected(int start, int fc, int* out_chan, int* out_loc);

/** What collected ISCH @p fc says about a window start of @p start, read hard (MATCH and CONTRA within
    P25P2_ISCH_EVIDENCE_MAX_BITS of an I-ISCH, MATCH within P25P2_S_ISCH_EVIDENCE_MAX_BITS of the S-ISCH). */
p25p2_isch_evidence p25p2_window_isch_check(const int* bits, int start, int fc);

/** The three collected ISCHs against @p start. */
void p25p2_window_isch_evidence(const int* bits, int start, p25p2_window_evidence* out);

/** Collected ISCH @p fc decoded as processP2() applies it (isch_lookup_soft() over its bits and their LLRs): the
    I-ISCH's information bits, or -2 for the S-ISCH word and for nothing decoded. -2 for @p fc past 2. */
int p25p2_window_isch_soft_decode(const int* bits, const int16_t* llr, int fc);

/** The descrambler offset (12 - fc, 4 - fc or 8 - fc) a channel-1 I-ISCH soft-decoded at @p fc as @p decoded places
    the window at; -1 when @p decoded is no channel-1 I-ISCH of location 0-2. */
int p25p2_window_isch_offset(int decoded, int fc);

/** The descrambler offset the collected ISCHs locate the window at, as processP2() applies them (the last that
    locates it wins); -1 when none does. */
int p25p2_window_soft_offset(const int* bits, const int16_t* llr);

/** Whether the burst dispatch handles DUID @p duid (p25p2_window_duid()'s value): anything else counts toward aborting
    the window. Kept in step with p25p2_duid_dispatch() (p25p2_frame.c). */
int p25p2_window_duid_dispatches(int duid);

/**
 * @brief The verdict on a window a tolerant sync opened, before any of it takes effect.
 *
 * @p start is the superframe timeslot the anchor predicts; @p context_holds says the stream is still the one the anchor
 * measured and continuity predicts the same start; @p duids are the four timeslots' DUIDs as dispatch reads them. It
 * passes only when no collected ISCH contradicts the start (hard, and as processP2()'s soft read would locate it), at
 * least one matches it, and at most one DUID is one the dispatch does not handle (so the window cannot abort midway).
 * The sync that opened the window is no evidence.
 */
p25p2_window_verdict p25p2_tolerant_window_verdict(const int* bits, const int16_t* llr, int start, int context_holds,
                                                   const int duids[4]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_WINDOW_VERDICT_H_ */
