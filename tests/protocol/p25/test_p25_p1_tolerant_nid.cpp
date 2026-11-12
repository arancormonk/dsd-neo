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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include "p25_nid_generator.hpp"
extern "C" {
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
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

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

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
    frame_sync_p25p1_anchor_note_sync(state, DSD_SYNC_P25P1_POS, anchor_nac, anchor_nac >= 0 ? 3 : 0);
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
        CHECK(dsd_frame_sync_p25p1_tolerant_nac(state, NULL) == -1);
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
    CHECK(dsd_frame_sync_p25p1_tolerant_nac(state, NULL) == -1);
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

int
main(void) {
    test_clean_nid_decodes();
    test_strict_nid_drops();
    test_validated_frames_anchor_at_their_length();
    test_unvalidated_frames_anchor_nothing();
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
    (void)state;
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

/* The real P25p1 sync anchor names a C4FM sync word to the symbol grid and arms its recording; these frames are read on
   no grid. */
void
dsd_symbol_note_p25p1_sync_anchor(const dsd_state* state, const char* sync_dibits, int count) {
    (void)state;
    (void)sync_dibits;
    (void)count;
}

void
dsd_symbol_arm_hunt_sync_anchor(dsd_state* state) {
    (void)state;
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

void
p25_status_accum_reset(dsd_state* state) {
    (void)state;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    (void)dibit_value;
}

void
p25_status_accum_classify(dsd_state* state) {
    (void)state;
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
