// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * What the P25 Phase 1 dispatcher does with the NID of a frame whose sync word the hunt accepted within its tolerance
 * at the anchor's expected position, and what it tells the frame sync about each frame (dsd_p25p1_sync_anchor).
 *
 * Behind a tolerant word the frame decodes only when its NID decoded cleanly (parity agreeing, at most four
 * corrections), unborrowed, naming the anchor's NAC and a defined DUID; anything else drops it before anything is read
 * from it: no NAC, no counter, no evidence for the demodulator or the profile. Any validated frame (an exact word's too)
 * anchors the next data unit's sync where it ends, at its length on air.
 *
 * The NIDs go through the real dispatcher, the real BCH/soft NID decoder and the real anchor; each is a codeword from
 * the independent generator with bit errors put in, read dibit by dibit as the dispatcher reads it. The frame handlers
 * past the NID are stand-ins that count what was dispatched and report a TSDU's or a PDU's length as its decoder would.
 *
 * C++ for the NID codewords, which come from the test-only generator header.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include "p25_nid_generator.hpp"
extern "C" {
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/protocol/p25/p25p1_nid_verdict.h>
#include <mbelib-neo/mbelib.h>
#include "frame_sync_internal.h"
}

extern "C" dsd_frame_verdict dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state);

static int g_failures;
/* The symbols the last dropped C4FM candidate handed the hunt (frame_sync_history_carry_symbols()), -1 for none. */
static int g_carried_symbols = -1;
/* The pass position it handed with them. */
static int g_carried_pass = -1;
/* The advisory AFC gate every NID read is expected to find published (auto-PPM reads it on the radio thread), and
   whether one found another. */
static uint8_t g_gate_valid_seen;
static uint8_t g_gate_allow_seen;
static int g_gate_moved_in_read;
/* The tolerant C4FM words the frame sync marked in the seam's history at their accept, and those it measured. */
static int g_grid_marks;
static int g_grid_measures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* The NAC an anchored frame's NID must name, -1 behind any other sync. */
static int
anchored_nac(const dsd_state* state, unsigned int* out_errors) {
    int nac = -1;
    return dsd_frame_sync_p25p1_sync_kind(state, &nac, out_errors) == DSD_P25P1_SYNC_ANCHORED ? nac : -1;
}

enum {
    DUID_HDU = 0x0,
    DUID_TDU = 0x3,
    DUID_LDU1 = 0x5,
    DUID_TSDU = 0x7,
    DUID_LDU2 = 0xA,
    DUID_PDU = 0xC,
    DUID_TDULC = 0xF,
    NID_BITS = 64, /* 63 BCH bits and the final parity bit */
    NID_PARITY_BIT = 63,
    NID_DIBITS = 33, /* 32 NID dibits with the status symbol after the 11th */
    NID_STATUS_DIBIT = 11,
    REL_CONFIDENT = 200,
    REL_DOUBTFUL = 10,
    NAC = 0x1A6,
    SYNC_AT = 5000, /* the frame's sync word starts here on the symbol clock */
};

namespace {

struct nid_air {
    char bits[NID_BITS];
    uint8_t rel[NID_BITS];
};

} // namespace

static int g_dibits[NID_DIBITS];
static uint8_t g_dibit_rel[NID_DIBITS][2];
static int g_dibit_reads;

/* Data units the dispatcher handed on past the NID, and the length the TSDU's or PDU's stand-in reports (0 unknown). */
static int g_frames;
static int g_ldu1_frames;
static uint32_t g_reported_unit;
static int g_interrupt_in_handler;
/* The NID read raises an input interruption (a queued command's yield) at this read, -1 for none. */
static int g_interrupt_at_nid_read = -1;

static void
make_nid(int nac, int duid, nid_air* out) {
    char info[16];
    char codeword[63];
    for (int i = 0; i < 12; i++) {
        info[i] = (char)((nac >> (11 - i)) & 1);
    }
    for (int i = 0; i < 4; i++) {
        info[12 + i] = (char)((duid >> (3 - i)) & 1);
    }
    p25_test_generate_nid_codeword(info, codeword);
    for (int i = 0; i < 63; i++) {
        out->bits[i] = codeword[i];
        out->rel[i] = REL_CONFIDENT;
    }
    out->bits[NID_PARITY_BIT] = (char)((duid == DUID_LDU1 || duid == DUID_LDU2) ? 1 : 0);
    out->rel[NID_PARITY_BIT] = REL_CONFIDENT;
}

static const int k_error_bits[] = {3, 17, 29, 41, 55, 8, 22, 35, 48, 60, 12, 26, 50, 38};

static nid_air
nid_with_errors(int nac, int duid, int count) {
    nid_air nid;
    make_nid(nac, duid, &nid);
    for (int i = 0; i < count; i++) {
        nid.bits[k_error_bits[i]] ^= 1;
    }
    return nid;
}

static void
load_nid(const nid_air* nid) {
    int bit = 0;
    for (int d = 0; d < NID_DIBITS; d++) {
        if (d == NID_STATUS_DIBIT) {
            g_dibits[d] = 0;
            g_dibit_rel[d][0] = g_dibit_rel[d][1] = REL_CONFIDENT;
            continue;
        }
        g_dibits[d] = (nid->bits[bit] << 1) | nid->bits[bit + 1];
        g_dibit_rel[d][0] = nid->rel[bit];
        g_dibit_rel[d][1] = nid->rel[bit + 1];
        bit += 2;
    }
    g_dibit_reads = 0;
}

static struct p25p1_nid_result
decode_alone(const nid_air* nid, int observed_nac) {
    return p25p1_nid_decode(nid->bits, nid->rel, observed_nac, (unsigned char)nid->bits[NID_PARITY_BIT],
                            nid->rel[NID_PARITY_BIT]);
}

/* The frame sync accepted the frame's sync word at SYNC_AT (exact when @p anchor_nac is -1, else within the tolerance
   of the anchor naming it), and the dispatcher reads its NID. The stand-in reader does not move the symbol clock: the
   frame reads as having consumed its sync and NID. */
static dsd_frame_verdict
dispatch_frame(dsd_opts* opts, dsd_state* state, const nid_air* nid, int anchor_nac) {
    state->symbol_clock = SYNC_AT + 24;
    frame_sync_p25p1_anchor_note_sync(state, DSD_SYNC_P25P1_POS,
                                      anchor_nac >= 0 ? DSD_P25P1_SYNC_ANCHORED : DSD_P25P1_SYNC_EXACT, anchor_nac,
                                      anchor_nac >= 0 ? 3 : 0);
    state->symbol_clock = SYNC_AT + 24 + NID_DIBITS;
    load_nid(nid);
    state->synctype = DSD_SYNC_P25P1_POS;
    g_frames = 0;
    g_ldu1_frames = 0;
    return dsd_dispatch_handle_p25p1(opts, state);
}

static dsd_opts*
new_opts(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    if (opts == NULL) {
        abort();
    }
    return opts;
}

/* A carrier whose NIDs established NAC 0x1A6, its seed proven, NID evidence fresh, as two-slot TDMA. */
static dsd_state*
new_state(dsd_opts* opts) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (state == NULL) {
        abort();
    }
    state->p25_p1_validated_rf_mod = -1;
    nid_air own;
    make_nid(NAC, DUID_TSDU, &own);
    g_reported_unit = 0U;
    (void)dispatch_frame(opts, state, &own, -1);
    CHECK(g_frames == 1);
    CHECK(p25p1_carrier_nac_established(state) == NAC);
    state->p2_cc_verified = 1U;
    state->p2_proven_nac = NAC;
    state->p25_p1_validated_rf_mod = -1;
    state->p25_p1_nid_evidence = 1;
    state->p25_p1_nid_evidence_symbolcnt = 0U;
    state->tdma_carrier = 1U;
    return state;
}

/* Behind a tolerant word, a clean NID of the anchor's NAC decodes the frame as an exact word's would, at up to four
   corrections, and the frame anchors the next one. */
static void
test_clean_nid_decodes(void) {
    static const int counts[] = {0, 1, 4};
    for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        dsd_opts* opts = new_opts();
        dsd_state* state = new_state(opts);
        const nid_air nid = nid_with_errors(NAC, DUID_LDU1, counts[c]);
        const struct p25p1_nid_result alone = decode_alone(&nid, NAC);
        CHECK(alone.status == NID_OK && alone.error_count == counts[c] && alone.nac_borrowed == 0);
        CHECK(dispatch_frame(opts, state, &nid, NAC) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
        CHECK(g_ldu1_frames == 1);
        CHECK(state->p25_p1_sync_anchor.tolerant_frames == 1U);
        CHECK(state->p25_p1_nid_evidence_symbolcnt == state->symbolcnt);
        CHECK(state->p25_p1_sync_anchor.valid == 1U);
        CHECK(state->p25_p1_sync_anchor.expected_clock == (uint64_t)SYNC_AT + P25P1_LDU_DIBITS);
        CHECK(anchored_nac(state, NULL) == -1);
        free(state);
        free(opts);
    }
}

/* What a dropped frame leaves: everything as it was. */
static void
expect_dropped(dsd_opts* opts, dsd_state* state, const nid_air* nid) {
    const unsigned int failures = state->nid_failures_total;
    const unsigned int critical = state->debug_header_critical_errors;
    const unsigned int corrections = state->nid_corrections_total;
    const unsigned int overrides = state->nid_parity_overrides;
    const unsigned int header_errors = state->debug_header_errors;
    const int lastp25type = state->lastp25type;
    state->lastp25type = 3;
    /* Inside the window of the NID that decoded before: an exact sync's failed NID would still prove the profile. */
    state->symbolcnt = 100U;
    CHECK(dispatch_frame(opts, state, nid, NAC) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    CHECK(g_frames == 0);
    CHECK(state->nid_failures_total == failures);
    CHECK(state->debug_header_critical_errors == critical);
    CHECK(state->nid_corrections_total == corrections);
    CHECK(state->nid_parity_overrides == overrides);
    CHECK(state->debug_header_errors == header_errors);
    /* The previous frame's NAC is not this one's (frame-local, as behind any sync): nothing names it. */
    CHECK(state->p25_frame_nac_valid == 0U);
    CHECK(state->lastp25type == 3);
    state->lastp25type = lastp25type;
    CHECK(state->nac == NAC);
    CHECK(state->rx_id.valid == DSD_RXID_ACCESS && state->rx_id.access == (uint16_t)NAC);
    CHECK(state->p2_cc == (unsigned long long)NAC && state->p2_cc_verified == 1U && state->p2_proven_nac == NAC);
    CHECK(p25p1_carrier_nac_established(state) == NAC);
    CHECK(state->p25_p1_carrier_nac.candidate_valid == 0U);
    CHECK(state->p25_p1_validated_rf_mod == -1);
    CHECK(state->p25_p1_nid_evidence_symbolcnt == 0U);
    CHECK(state->tdma_carrier == 1U);
    CHECK(state->p25_p1_sync_anchor.valid == 0U);
    CHECK(state->p25_p1_sync_anchor.tolerant_frames == 0U);
    CHECK(anchored_nac(state, NULL) == -1);
}

/* Behind a tolerant word anything short of a clean NID of the anchor's NAC drops the frame: the parity bit disagreeing,
   five corrections, a borrowed NAC, another NAC (cheaply, which an exact word's frame would take at once), 0x000. The
   same NIDs behind an exact word decode (or are held) as before. */
static void
test_strict_nid_drops(void) {
    struct {
        const char* name;
        nid_air nid;
        int exact_decodes;
    } cases[6];

    cases[0].name = "parity disagreeing";
    cases[0].nid = nid_with_errors(NAC, DUID_LDU1, 0);
    cases[0].nid.bits[NID_PARITY_BIT] ^= 1;
    cases[0].exact_decodes = 1;
    cases[1].name = "five corrections";
    cases[1].nid = nid_with_errors(NAC, DUID_LDU1, 5);
    cases[1].exact_decodes = 1;
    cases[2].name = "another NAC";
    cases[2].nid = nid_with_errors(0x0E1, DUID_LDU1, 0);
    cases[2].exact_decodes = 1;
    cases[3].name = "NAC 0x000";
    cases[3].nid = nid_with_errors(0x000, DUID_LDU1, 0);
    cases[3].exact_decodes = 1;
    cases[4].name = "NAC 0xFFF";
    cases[4].nid = nid_with_errors(0xFFF, DUID_LDU1, 2);
    cases[4].exact_decodes = 1;
    /* The anchor's NAC with its NAC field and seven other bits wrong: only the retry with the observed NAC decodes it. */
    static const int nac_field_errors[] = {0, 2, 4, 6, 8, 10};
    static const int other_errors[] = {14, 19, 27, 33, 39, 45, 52};
    cases[5].name = "borrowed";
    make_nid(NAC, DUID_LDU1, &cases[5].nid);
    for (int i = 0; i < 6; i++) {
        cases[5].nid.bits[nac_field_errors[i]] ^= 1;
    }
    for (int i = 0; i < 7; i++) {
        cases[5].nid.bits[other_errors[i]] ^= 1;
    }
    cases[5].exact_decodes = 1;
    const struct p25p1_nid_result borrowed = decode_alone(&cases[5].nid, NAC);
    CHECK(borrowed.status > 0 && borrowed.nac == NAC && borrowed.nac_borrowed == 1);
    const struct p25p1_nid_result parity = decode_alone(&cases[0].nid, NAC);
    CHECK(parity.status == NID_PARITY_OVERRIDE && parity.error_count == 0);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_opts* opts = new_opts();
        dsd_state* state = new_state(opts);
        const int failures_before = g_failures;
        expect_dropped(opts, state, &cases[i].nid);
        /* Behind an exact word the frame decodes, as it did before. */
        (void)dispatch_frame(opts, state, &cases[i].nid, -1);
        CHECK(g_ldu1_frames == cases[i].exact_decodes);
        if (g_failures != failures_before) {
            DSD_FPRINTF(stderr, "  (case: %s)\n", cases[i].name);
        }
        free(state);
        free(opts);
    }
}

/* A frame that validates anchors the next data unit's sync at its length on air: by its DUID, or for a TSDU and a PDU
   by what its decoder read (the CRC-checked last block, the header's block count). */
static void
test_validated_frames_anchor_at_their_length(void) {
    static const struct {
        int duid;
        uint32_t reported;
        uint32_t expected;
    } cases[] = {
        {DUID_HDU, 0U, 396U},
        {DUID_LDU1, 0U, 864U},
        {DUID_LDU2, 0U, 864U},
        {DUID_TDU, 0U, 72U},
        {DUID_TDULC, 0U, 216U},
        {DUID_TSDU, 180U, 180U},
        {DUID_TSDU, 288U, 288U},
        {DUID_TSDU, 360U, 360U},
        {DUID_PDU, 1800U, 1800U},
        /* A TSDU or PDU whose length its decoder could not read anchors nothing. */
        {DUID_TSDU, 0U, 0U},
        {DUID_PDU, 0U, 0U},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_opts* opts = new_opts();
        dsd_state* state = new_state(opts);
        nid_air nid;
        make_nid(NAC, cases[i].duid, &nid);
        g_reported_unit = cases[i].reported;
        (void)dispatch_frame(opts, state, &nid, -1);
        CHECK(g_frames == 1);
        if (cases[i].expected == 0U) {
            CHECK(state->p25_p1_sync_anchor.valid == 0U);
        } else {
            CHECK(state->p25_p1_sync_anchor.valid == 1U);
            CHECK(state->p25_p1_sync_anchor.nac == NAC);
            CHECK(state->p25_p1_sync_anchor.expected_clock == (uint64_t)SYNC_AT + cases[i].expected);
        }
        CHECK(state->p25_p1_sync_anchor.frame_valid == 0U);
        g_reported_unit = 0U;
        free(state);
        free(opts);
    }
    /* The block-count lengths themselves (TIA-102.BAAA-A 5.1.2, 8.2, and 8.2.4 for a PDU): 1-3 block TSDUs, and a
       header with 16 data blocks. */
    CHECK(p25p1_block_unit_dibits(0) == 0U);
    CHECK(p25p1_block_unit_dibits(1) == 180U);
    CHECK(p25p1_block_unit_dibits(2) == 288U);
    CHECK(p25p1_block_unit_dibits(3) == 360U);
    CHECK(p25p1_block_unit_dibits(17) == 1800U);
}

/* A frame that does not validate anchors nothing: NAC 0x000 or 0xFFF (decoded, but no NAC), a NID that failed, one past
   the BCH radius, a borrowed NAC, a NAC change held for corroboration, one an input interruption cut in its payload. */
static void
test_unvalidated_frames_anchor_nothing(void) {
    nid_air dead;
    make_nid(0x000, DUID_TSDU, &dead);
    nid_air stuck;
    make_nid(0xFFF, DUID_TSDU, &stuck);
    nid_air empty;
    make_nid(NAC, DUID_TSDU, &empty);
    for (int i = 0; i < 63; i++) {
        empty.bits[i] = (char)((i * 7 + 3) % 5 == 0);
    }
    nid_air past_radius = nid_with_errors(NAC, DUID_TSDU, 13);
    past_radius.rel[k_error_bits[11]] = REL_DOUBTFUL;
    past_radius.rel[k_error_bits[12]] = REL_DOUBTFUL;
    nid_air held = nid_with_errors(0x0E1, DUID_TSDU, 7);
    const struct p25p1_nid_result radius = decode_alone(&past_radius, NAC);
    CHECK(radius.status > 0 && radius.nac == NAC && radius.error_count > P25P1_NID_BCH_RADIUS);
    CHECK(decode_alone(&empty, NAC).status <= 0);

    const nid_air* cases[] = {&dead, &stuck, &empty, &past_radius, &held};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_opts* opts = new_opts();
        dsd_state* state = new_state(opts);
        g_reported_unit = 180U;
        (void)dispatch_frame(opts, state, cases[i], -1);
        CHECK(state->p25_p1_sync_anchor.valid == 0U);
        CHECK(state->p25_p1_sync_anchor.frame_valid == 0U);
        g_reported_unit = 0U;
        free(state);
        free(opts);
    }

    dsd_opts* opts = new_opts();
    dsd_state* state = new_state(opts);
    nid_air own;
    make_nid(NAC, DUID_TSDU, &own);
    g_reported_unit = 180U;
    g_interrupt_in_handler = 1;
    (void)dispatch_frame(opts, state, &own, -1);
    CHECK(g_frames == 1);
    CHECK(state->p25_p1_sync_anchor.valid == 0U);
    g_interrupt_in_handler = 0;
    g_reported_unit = 0U;
    free(state);
    free(opts);
}

/* The frame sync accepted the frame's sync word at SYNC_AT within a tolerance on the C4FM chain (@p kind, the anchor's
   NAC @p anchor_nac for an anchored one), holding what its accept then wrote (frame_sync_p25p1_candidate_hold()): here
   the lock and lastsynctype over a carrier the hunt had lost. The dispatcher reads its NID. */
static dsd_frame_verdict
dispatch_held_candidate(dsd_opts* opts, dsd_state* state, const nid_air* nid, int kind, int anchor_nac) {
    g_grid_marks = 0;
    g_grid_measures = 0;
    state->carrier = 0;
    state->lastsynctype = DSD_SYNC_NONE;
    state->symbol_clock = SYNC_AT + 24;
    frame_sync_p25p1_candidate_hold(state, 700);
    /* What frame_sync_accept_p25p1() writes, and processFrame() after it. */
    state->carrier = 1;
    state->offset = 23;
    state->max = 2.5f;
    state->min = -2.5f;
    state->center = 0.0f;
    state->umid = 1.5f;
    state->lmid = -1.5f;
    state->maxref = 2.0f;
    state->minref = -2.0f;
    state->jitter = -1;
    state->dmrburstR = 17U;
    state->payload_algidR = 0;
    state->dmr_stereo = 1;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "%s", "P25 Phase 1");
    state->p25_p1_c4fm_frame_valid = state->rf_mod == 1 ? 0U : 1U;
    state->p25_p1_c4fm_frame_symbolcnt = state->symbolcnt;
    state->p25_p1_c4fm_sync_levels = state->rf_mod == 1 ? 0U : 1U;
    state->p25_cqpsk_dibit_map_idx = 0U;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    frame_sync_p25p1_anchor_note_sync(state, DSD_SYNC_P25P1_POS, kind, anchor_nac, 2);
    state->symbol_clock = SYNC_AT + 24 + NID_DIBITS;
    load_nid(nid);
    state->synctype = DSD_SYNC_P25P1_POS;
    g_frames = 0;
    g_ldu1_frames = 0;
    g_carried_symbols = -1;
    g_carried_pass = -1;
    state->sps_hunt_counter = 900;
    return dsd_dispatch_handle_p25p1(opts, state);
}

/* A candidate the strict check drops puts back what its accept wrote, and hands the hunt the word's and the NID's
   symbols to go on from; one whose NID passes keeps it all and hands nothing. */
static void
test_dropped_candidate_is_put_back(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state(opts);
    /* A DMR carrier's identity stands (as under -fa): a candidate that is no P25 frame replaces none of it. */
    dsd_rxid_note_sync(state, DSD_SYNC_DMR_BS_DATA_POS);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 3U);
    const nid_air other = nid_with_errors(0x0E1, DUID_LDU1, 0);
    CHECK(dispatch_held_candidate(opts, state, &other, DSD_P25P1_SYNC_ANCHORED, NAC) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    CHECK(g_frames == 0);
    CHECK(state->rx_id.family == (uint8_t)DSD_RXID_FAMILY_DMR && state->rx_id.valid == DSD_RXID_ACCESS);
    CHECK(dsd_state_p25p1_frame_dropped(state));
    CHECK(state->carrier == 0 && state->lastsynctype == DSD_SYNC_NONE);
    CHECK(g_carried_symbols == 24 + NID_DIBITS);
    /* The word's last symbol (which the accept returned on) and the NID's are the hunt's: charged to its dwell, its pass
       going on past them. */
    CHECK(g_carried_pass == 701 + NID_DIBITS && state->sps_hunt_counter == 901 + NID_DIBITS);
    CHECK(state->p25_p1_sync_candidate.valid == 0U);
    /* Marked at the accept, never measured: the grid stays where the hunt left it. */
    CHECK(g_grid_marks == 1 && g_grid_measures == 0);
    /* On the CQPSK chain the accept is put back too; the carry is the C4FM hunt's sign history, so nothing is carried
       and nothing charged. */
    state->rf_mod = 1;
    CHECK(dispatch_held_candidate(opts, state, &other, DSD_P25P1_SYNC_ANCHORED, NAC) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    CHECK(state->carrier == 0 && state->lastsynctype == DSD_SYNC_NONE);
    CHECK(g_carried_symbols == -1 && state->sps_hunt_counter == 900);
    state->rf_mod = 0;

    const nid_air own = nid_with_errors(NAC, DUID_LDU1, 2);
    CHECK(dispatch_held_candidate(opts, state, &own, DSD_P25P1_SYNC_ANCHORED, NAC) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    CHECK(g_ldu1_frames == 1);
    /* Past the strict check the frame is P25's: the dispatcher replaces the identity, and the NID names its NAC. */
    CHECK(state->rx_id.family == (uint8_t)DSD_RXID_FAMILY_P25 && state->rx_id.access == (uint16_t)NAC);
    CHECK(!dsd_state_p25p1_frame_dropped(state));
    CHECK(state->carrier == 1 && state->lastsynctype == DSD_SYNC_P25P1_POS);
    CHECK(g_carried_symbols == -1 && state->sps_hunt_counter == 900);
    CHECK(state->p25_p1_sync_candidate.valid == 0U);
    /* The word is measured against the grid once the NID passed, and the mark spent with the frame's report. */
    CHECK(g_grid_marks == 1 && g_grid_measures == 1);
    CHECK(state->p25_p1_sync_anchor.frame_word.valid == 0U);
    /* On the CQPSK chain the grid takes a sample a symbol: nothing is marked. */
    state->rf_mod = 1;
    CHECK(dispatch_held_candidate(opts, state, &own, DSD_P25P1_SYNC_ANCHORED, NAC) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    CHECK(g_grid_marks == 0 && g_grid_measures == 0);
    free(state);
    free(opts);
}

/* Behind a word accepted hunt-wide on C4FM (no anchor) a strict NID decodes the frame whatever usable NAC it names,
   another than the carrier's included, at up to four corrections; it is counted, and anchors the next unit. Anything
   short of a strict NID drops the frame as behind an anchored word: parity disagreeing, five corrections, a borrowed
   NAC, 0x000 or 0xFFF, a soft decode past the BCH radius. */
static void
test_acquisition_needs_a_strict_nid(void) {
    static const int counts[] = {0, 4};
    for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        dsd_opts* opts = new_opts();
        dsd_state* state = new_state(opts);
        const nid_air other = nid_with_errors(0x0E1, DUID_LDU1, counts[c]);
        CHECK(dispatch_held_candidate(opts, state, &other, DSD_P25P1_SYNC_ACQUISITION, -1)
              == DSD_FRAME_VERDICT_PROFILE_PROVEN);
        CHECK(g_ldu1_frames == 1);
        CHECK(state->nac == 0x0E1);
        CHECK(state->p25_p1_sync_anchor.acquisition_frames == 1U);
        CHECK(state->p25_p1_sync_anchor.tolerant_frames == 0U);
        CHECK(state->p25_p1_sync_anchor.valid == 1U);
        CHECK(g_carried_symbols == -1);
        CHECK(g_grid_marks == 1 && g_grid_measures == 1);
        free(state);
        free(opts);
    }

    nid_air cases[6];
    cases[0] = nid_with_errors(NAC, DUID_LDU1, 0);
    cases[0].bits[NID_PARITY_BIT] ^= 1;
    cases[1] = nid_with_errors(NAC, DUID_LDU1, 5);
    cases[2] = nid_with_errors(0x000, DUID_LDU1, 0);
    cases[3] = nid_with_errors(0xFFF, DUID_LDU1, 2);
    static const int nac_field_errors[] = {0, 2, 4, 6, 8, 10};
    static const int other_errors[] = {14, 19, 27, 33, 39, 45, 52};
    make_nid(NAC, DUID_LDU1, &cases[4]);
    for (int i = 0; i < 6; i++) {
        cases[4].bits[nac_field_errors[i]] ^= 1;
    }
    for (int i = 0; i < 7; i++) {
        cases[4].bits[other_errors[i]] ^= 1;
    }
    cases[5] = nid_with_errors(NAC, DUID_LDU1, 13);
    cases[5].rel[k_error_bits[11]] = REL_DOUBTFUL;
    cases[5].rel[k_error_bits[12]] = REL_DOUBTFUL;
    CHECK(decode_alone(&cases[4], NAC).nac_borrowed == 1);
    CHECK(decode_alone(&cases[5], NAC).error_count > P25P1_NID_BCH_RADIUS);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_opts* opts = new_opts();
        dsd_state* state = new_state(opts);
        const unsigned int failures = state->nid_failures_total;
        CHECK(dispatch_held_candidate(opts, state, &cases[i], DSD_P25P1_SYNC_ACQUISITION, -1)
              == DSD_FRAME_VERDICT_UNPRODUCTIVE);
        CHECK(g_frames == 0);
        CHECK(state->nid_failures_total == failures);
        CHECK(state->nac == NAC && p25p1_carrier_nac_established(state) == NAC);
        CHECK(state->p25_p1_sync_anchor.acquisition_frames == 0U);
        CHECK(state->carrier == 0 && state->lastsynctype == DSD_SYNC_NONE);
        CHECK(g_carried_symbols == 24 + NID_DIBITS);
        CHECK(g_grid_marks == 1 && g_grid_measures == 0);
        if (g_failures != 0) {
            DSD_FPRINTF(stderr, "  (acquisition case %zu)\n", i);
        }
        free(state);
        free(opts);
    }
}

/* What a tolerant accept and the NID read behind it may write before the strict check speaks, as one value: the lock,
   the slicer levels, the frame label and slot 2 fields, the sync type the engine records, the frame span, the map, the
   frame NAC, the identity and the carrier's NAC, the NID counters, and the status accumulator with the advisory AFC
   gate it publishes. */
namespace {

struct provisional_view {
    float levels[7];
    int carrier, offset, jitter, lastsynctype, synctype, payload_algidR, nac;
    unsigned int dmrburstR, nid_failures, nid_corrections, header_errors;
    short int dmr_stereo;
    char ftype[16];
    uint32_t c4fm_frame_symbolcnt;
    uint8_t c4fm_frame_valid, map_idx, frame_nac_valid, sync_levels, rx_family;
    uint16_t rx_access;
    uint8_t ss_buf[P25_STATUS_ACCUM_MAX];
    uint8_t ss_count, ss_active, ss_class, gate_allow, gate_valid;
    unsigned int afc_allowed, afc_suppressed;
};

} // namespace

static provisional_view
provisional_view_of(const dsd_state* state) {
    provisional_view v;
    std::memset(&v, 0, sizeof(v));
    const float levels[7] = {
        state->max, state->min, state->center, state->umid, state->lmid, state->maxref, state->minref,
    };
    std::memcpy(v.levels, levels, sizeof(levels));
    v.carrier = state->carrier;
    v.offset = state->offset;
    v.jitter = state->jitter;
    v.lastsynctype = state->lastsynctype;
    v.synctype = state->synctype;
    v.payload_algidR = state->payload_algidR;
    v.nac = state->nac;
    v.dmrburstR = state->dmrburstR;
    v.nid_failures = state->nid_failures_total;
    v.nid_corrections = state->nid_corrections_total;
    v.header_errors = state->debug_header_errors;
    v.dmr_stereo = state->dmr_stereo;
    std::memcpy(v.ftype, state->ftype, sizeof(v.ftype));
    v.c4fm_frame_symbolcnt = state->p25_p1_c4fm_frame_symbolcnt;
    v.c4fm_frame_valid = state->p25_p1_c4fm_frame_valid;
    v.map_idx = state->p25_cqpsk_dibit_map_idx;
    v.frame_nac_valid = state->p25_frame_nac_valid;
    v.sync_levels = state->p25_p1_c4fm_sync_levels;
    v.rx_family = state->rx_id.family;
    v.rx_access = state->rx_id.access;
    std::memcpy(v.ss_buf, state->p25_ss_buf, sizeof(v.ss_buf));
    v.ss_count = state->p25_ss_count;
    v.ss_active = state->p25_ss_frame_active;
    v.ss_class = state->p25_ss_classification;
    v.gate_allow = state->p25_afc_gate_allow;
    v.gate_valid = state->p25_afc_gate_valid;
    v.afc_allowed = state->p25_afc_allowed_count;
    v.afc_suppressed = state->p25_afc_suppressed_count;
    return v;
}

/* Whether two views are the same: the levels within a tolerance, everything else exactly. */
static int
provisional_view_equal(const provisional_view* a, const provisional_view* b) {
    for (int i = 0; i < 7; i++) {
        if (std::fabs(a->levels[i] - b->levels[i]) > 1e-6f) {
            return 0;
        }
    }
    return a->carrier == b->carrier && a->offset == b->offset && a->jitter == b->jitter
           && a->lastsynctype == b->lastsynctype && a->synctype == b->synctype && a->payload_algidR == b->payload_algidR
           && a->nac == b->nac && a->dmrburstR == b->dmrburstR && a->nid_failures == b->nid_failures
           && a->nid_corrections == b->nid_corrections && a->header_errors == b->header_errors
           && a->dmr_stereo == b->dmr_stereo && std::memcmp(a->ftype, b->ftype, sizeof(a->ftype)) == 0
           && a->c4fm_frame_symbolcnt == b->c4fm_frame_symbolcnt && a->c4fm_frame_valid == b->c4fm_frame_valid
           && a->map_idx == b->map_idx && a->frame_nac_valid == b->frame_nac_valid && a->sync_levels == b->sync_levels
           && a->rx_family == b->rx_family && a->rx_access == b->rx_access
           && std::memcmp(a->ss_buf, b->ss_buf, sizeof(a->ss_buf)) == 0 && a->ss_count == b->ss_count
           && a->ss_active == b->ss_active && a->ss_class == b->ss_class && a->gate_allow == b->gate_allow
           && a->gate_valid == b->gate_valid && a->afc_allowed == b->afc_allowed
           && a->afc_suppressed == b->afc_suppressed;
}

/* The decoder as a subscriber's data unit left it (opt-in AFC gate: valid, not allowing), with its own levels and
   frame fields, set apart from what an accept writes. */
static void
mark_published_state(dsd_state* state) {
    state->max = 7.0f;
    state->min = -7.0f;
    state->center = 0.25f;
    state->umid = 4.0f;
    state->lmid = -4.0f;
    state->maxref = 5.6f;
    state->minref = -5.6f;
    state->offset = 77;
    state->jitter = 3;
    state->dmrburstR = 9U;
    state->payload_algidR = 0x84;
    state->dmr_stereo = 0;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "%s", "DMR");
    /* The engine's last sync, which it overwrites with the candidate's (dispatch_held_candidate()). */
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->p25_p1_c4fm_frame_valid = 1;
    state->p25_p1_c4fm_frame_symbolcnt = 4242U;
    state->p25_cqpsk_dibit_map_idx = 2U;
    state->p25_frame_nac_valid = 1U;
    state->p25_p1_c4fm_sync_levels = 0U;
    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0);
    p25_status_accum_add(state, 2);
    p25_status_accum_add(state, 0);
    p25_status_accum_classify(state);
    CHECK(state->p25_afc_gate_valid == 1U && state->p25_afc_gate_allow == 0U);
    CHECK(state->p25_ss_classification == (uint8_t)P25_SS_CLASS_SUBSCRIBER);
    g_gate_valid_seen = state->p25_afc_gate_valid;
    g_gate_allow_seen = state->p25_afc_gate_allow;
    g_gate_moved_in_read = 0;
}

/* Whatever chain and kind, a candidate whose NID fails leaves the decoder exactly as it found it, the advisory AFC gate
   included: the gate the last data unit published stands through the NID read too (auto-PPM must not train on the
   subscriber the gate excludes while the NID is provisional). A candidate whose NID passes withdraws the gate as an
   exact data unit's start does, once it passed; behind an exact word the read finds it withdrawn, as before. */
static void
test_rejected_candidate_leaves_the_state_as_it_was(void) {
    for (int rf_mod = 0; rf_mod <= 1; rf_mod++) {
        for (int kind = 0; kind < 2; kind++) {
            dsd_opts* opts = new_opts();
            dsd_state* state = new_state(opts);
            state->rf_mod = rf_mod;
            const int sync_kind = kind == 0 ? DSD_P25P1_SYNC_ANCHORED : DSD_P25P1_SYNC_ACQUISITION;
            const int anchor_nac = kind == 0 ? NAC : -1;
            mark_published_state(state);
            state->carrier = 0;
            state->lastsynctype = DSD_SYNC_NONE;
            const provisional_view before = provisional_view_of(state);
            const nid_air noise = nid_with_errors(NAC, DUID_LDU1, 13);
            CHECK(dispatch_held_candidate(opts, state, &noise, sync_kind, anchor_nac)
                  == DSD_FRAME_VERDICT_UNPRODUCTIVE);
            CHECK(g_frames == 0);
            CHECK(g_gate_moved_in_read == 0);
            const provisional_view after = provisional_view_of(state);
            if (!provisional_view_equal(&before, &after)) {
                DSD_FPRINTF(stderr, "  (rf_mod %d, kind %d: the state moved)\n", rf_mod, sync_kind);
            }
            CHECK(provisional_view_equal(&before, &after));

            mark_published_state(state);
            const nid_air clean = nid_with_errors(NAC, DUID_LDU1, 0);
            CHECK(dispatch_held_candidate(opts, state, &clean, sync_kind, anchor_nac)
                  == DSD_FRAME_VERDICT_PROFILE_PROVEN);
            CHECK(g_gate_moved_in_read == 0);
            CHECK(state->p25_afc_gate_valid == 0U && state->p25_afc_gate_allow == 0U);
            free(state);
            free(opts);
        }
    }
}

/* A queued command's yield cuts the NID behind a tolerant word, of either kind on either chain: the strict check never
   spoke, so the frame synced nothing (the voice gate starts no qualify window on it), its verdict is withheld, what its
   accept changed goes back, and the hunt does not go on from the NID's symbols. */
static void
test_cut_candidate_leaves_the_state_as_it_was(void) {
    for (int rf_mod = 0; rf_mod <= 1; rf_mod++) {
        for (int kind = 0; kind < 2; kind++) {
            dsd_opts* opts = new_opts();
            dsd_state* state = new_state(opts);
            state->rf_mod = rf_mod;
            const int sync_kind = kind == 0 ? DSD_P25P1_SYNC_ANCHORED : DSD_P25P1_SYNC_ACQUISITION;
            mark_published_state(state);
            state->carrier = 0;
            state->lastsynctype = DSD_SYNC_NONE;
            const provisional_view before = provisional_view_of(state);
            const nid_air clean = nid_with_errors(NAC, DUID_LDU1, 0);
            g_interrupt_at_nid_read = 12;
            CHECK(dispatch_held_candidate(opts, state, &clean, sync_kind, kind == 0 ? NAC : -1)
                  == DSD_FRAME_VERDICT_WITHHELD);
            g_interrupt_at_nid_read = -1;
            /* The read after the cut handed out stand-ins: the hunt does not go on from them. */
            CHECK(g_frames == 0 && g_carried_symbols == -1 && state->sps_hunt_counter == 900);
            CHECK(dsd_state_p25p1_frame_dropped(state));
            const provisional_view after = provisional_view_of(state);
            if (!provisional_view_equal(&before, &after)) {
                DSD_FPRINTF(stderr, "  (rf_mod %d, kind %d: the cut left the state moved)\n", rf_mod, sync_kind);
            }
            CHECK(provisional_view_equal(&before, &after));
            CHECK(state->p25_p1_sync_candidate.valid == 0U);
            state->input_interrupted = 0;
            state->input_interrupt_reason = DSD_INTERRUPT_NONE;
            free(state);
            free(opts);
        }
    }
}

int
main(void) {
    test_dropped_candidate_is_put_back();
    test_acquisition_needs_a_strict_nid();
    test_clean_nid_decodes();
    test_strict_nid_drops();
    test_validated_frames_anchor_at_their_length();
    test_unvalidated_frames_anchor_nothing();
    test_rejected_candidate_leaves_the_state_as_it_was();
    test_cut_candidate_leaves_the_state_as_it_was();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "P25 P1 tolerant NID: OK\n");
    return 0;
}

/* Stand-ins for what the dispatcher calls past the NID. */
extern "C" {

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    if (state->p25_afc_gate_valid != g_gate_valid_seen || state->p25_afc_gate_allow != g_gate_allow_seen) {
        g_gate_moved_in_read = 1;
    }
    if (g_dibit_reads == g_interrupt_at_nid_read) {
        state->input_interrupted = 1;
        state->input_interrupt_reason = DSD_INTERRUPT_YIELD;
    }
    const int d = g_dibit_reads < NID_DIBITS ? g_dibit_reads : NID_DIBITS - 1;
    g_dibit_reads++;
    if (out_soft != NULL) {
        out_soft->reliability = REL_CONFIDENT;
        out_soft->llr[0] = (int16_t)(((g_dibits[d] >> 1) & 1) ? g_dibit_rel[d][0] : -(int)g_dibit_rel[d][0]);
        out_soft->llr[1] = (int16_t)((g_dibits[d] & 1) ? g_dibit_rel[d][1] : -(int)g_dibit_rel[d][1]);
    }
    return g_dibits[d];
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dsd_frame_sync_p25p1_mod_compare_publish(const dsd_state* state) {
    (void)state;
}

int
dsd_frame_sync_off_thread_tune_pending(void) {
    return 0;
}

/* The real P25p1 sync anchor names a C4FM sync word to the symbol grid (a tolerant one once its NID passed) and arms
   its recording; these frames are read on no grid. */
void
dsd_symbol_note_p25p1_sync_anchor(dsd_state* state, const char* sync_dibits, int count) {
    (void)state;
    (void)sync_dibits;
    (void)count;
}

/* A tolerant word is marked where it lies in the seam's history at its accept, and measured once its NID passed: what
   was asked, and of which word. */
void
dsd_symbol_mark_p25p1_sync_anchor(const dsd_state* state, int count, dsd_matched_filter_seam_mark* out) {
    (void)state;
    CHECK(count == 23);
    g_grid_marks++;
    out->valid = 1U;
}

int
dsd_symbol_note_p25p1_marked_sync_anchor(dsd_state* state, const dsd_matched_filter_seam_mark* mark,
                                         const char* sync_dibits, int count) {
    (void)state;
    CHECK(mark->valid == 1U && count == 23 && std::strncmp(sync_dibits, &P25P1_SYNC[1], 23) == 0);
    g_grid_measures++;
    return 1;
}

void
dsd_symbol_arm_hunt_sync_anchor(dsd_state* state) {
    (void)state;
}

/* A dropped C4FM candidate hands the hunt the symbols it took (the real one is the frame sync's): what it was asked. */
void
frame_sync_history_carry_symbols(dsd_state* state, int symbols, int pass_symbols) {
    (void)state;
    g_carried_symbols = symbols;
    g_carried_pass = pass_symbols;
}

void
dsd_mbe_out_close_protocol(dsd_opts* opts, dsd_state* state, int slot_mask, int synctype) {
    (void)opts;
    (void)state;
    (void)slot_mask;
    (void)synctype;
}

void
resumeScan(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
mbe_initMbeParms(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    (void)cur_mp;
    (void)prev_mp;
    (void)prev_mp_enhanced;
}

/* Every data unit's stand-in: counted, and an interruption raised in it when asked. */
static void
handled(dsd_state* state) {
    g_frames++;
    if (g_interrupt_in_handler) {
        state->input_interrupted = 1;
    }
}

void
processHDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    handled(state);
}

void
processLDU1(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_ldu1_frames++;
    handled(state);
}

void
processLDU2(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    handled(state);
}

void
processTDULC(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    handled(state);
}

void
processTDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    handled(state);
}

int
processTSBK(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->p25_p1_unit_dibits = g_reported_unit;
    handled(state);
    return 1;
}

void
processMPDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->p25_p1_unit_dibits = g_reported_unit;
    handled(state);
}

} // extern "C"
