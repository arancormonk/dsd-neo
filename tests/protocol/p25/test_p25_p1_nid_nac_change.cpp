// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A P25 Phase 1 NID naming another NAC than the carrier's NIDs established is taken at once only when it needed at most
 * four corrections; a costlier one is held until the next NID confirms or refutes it. On a simulcast control channel a
 * corrupted TSDU's NID decoded as "LDU1, NAC 00E" with 10-14 corrections: dsd-neo decoded the TSDU as voice and moved
 * the received NAC, state->nac and the Phase 2 seed to the bogus NAC.
 *
 * The NIDs here go through the real dispatcher and the real BCH/soft NID decoder: each is a codeword from the
 * independent generator, with bit errors put in, read dibit by dibit as the dispatcher reads it off the air. The frame
 * handlers past the NID are stand-ins that count what was dispatched.
 *
 * This file is C++ because the codewords come from the test-only generator header.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/protocol/p25/p25p1_nid_verdict.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <mbelib-neo/mbelib.h>
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
    NID_DUID_TDU = 0x3,
    NID_DUID_LDU1 = 0x5,
    NID_DUID_TSDU = 0x7,
    NID_BITS = 64, /* 63 BCH bits and the final parity bit */
    NID_PARITY_BIT = 63,
    NID_DIBITS = 33, /* 32 NID dibits with the status symbol after the 11th */
    NID_STATUS_DIBIT = 11,
    REL_CONFIDENT = 200,
    REL_DOUBTFUL = 10, /* well under the soft search's erasure threshold (64) */
};

namespace {

/* A NID as received: its 64 bits and each bit's reliability. */
struct nid_air {
    char bits[NID_BITS];
    uint8_t rel[NID_BITS];
};

} // namespace

/* The NID the next dispatch reads, as dibits, with the dibit (if any) at which an input interruption cuts it. */
static int g_dibits[NID_DIBITS];
static uint8_t g_dibit_rel[NID_DIBITS][2];
static int g_dibit_reads;
static int g_interrupt_at_dibit = -1;

/* Data units the dispatcher handed on past the NID. */
static int g_ldu1_frames;
static int g_tsdu_frames;
static int g_other_frames;

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
    out->bits[NID_PARITY_BIT] = (char)((duid == NID_DUID_LDU1 || duid == 0xA) ? 1 : 0);
    out->rel[NID_PARITY_BIT] = REL_CONFIDENT;
}

/* Bit errors spread over the BCH word, outside the parity bit. */
static const int k_error_bits[] = {3, 17, 29, 41, 55, 8, 22, 35, 48, 60, 12, 26, 50, 38};
/* Another set, for a second costly NID of the same NAC that is not the same corrupted word. */
static const int k_other_error_bits[] = {1, 14, 27, 39, 52, 6, 19, 33, 45, 58, 10};

static void
put_errors(nid_air* nid, const int* bits, int count) {
    for (int i = 0; i < count; i++) {
        nid->bits[bits[i]] ^= 1;
    }
}

static nid_air
nid_with_errors(int nac, int duid, const int* bits, int count) {
    nid_air nid;
    make_nid(nac, duid, &nid);
    put_errors(&nid, bits, count);
    return nid;
}

/* The final parity bit disagrees with the DUID. */
static void
damage_parity(nid_air* nid) {
    nid->bits[NID_PARITY_BIT] ^= 1;
}

/* Queue @p nid in the order p25p1_read_nid_fields() reads it: NAC, DUID and the first 6 BCH bits, the status symbol,
   the next 40 BCH bits, then the last BCH bit with the parity bit. */
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

/* What the real decoder makes of @p nid on its own, with @p observed_nac for the retry. */
static struct p25p1_nid_result
decode_alone(const nid_air* nid, int observed_nac) {
    return p25p1_nid_decode(nid->bits, nid->rel, observed_nac, (unsigned char)nid->bits[NID_PARITY_BIT],
                            nid->rel[NID_PARITY_BIT]);
}

static void
reset_counts(void) {
    g_ldu1_frames = 0;
    g_tsdu_frames = 0;
    g_other_frames = 0;
    g_interrupt_at_dibit = -1;
}

static dsd_frame_verdict
dispatch_nid(dsd_opts* opts, dsd_state* state, const nid_air* nid) {
    load_nid(nid);
    state->synctype = DSD_SYNC_P25P1_POS;
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

static dsd_state*
new_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (state == NULL) {
        abort();
    }
    reset_counts();
    return state;
}

/* The control channel's NID establishes its NAC: a clean TSDU. */
static void
establish(dsd_opts* opts, dsd_state* state, int nac) {
    nid_air nid;
    make_nid(nac, NID_DUID_TSDU, &nid);
    const int before = g_tsdu_frames;
    (void)dispatch_nid(opts, state, &nid);
    CHECK(g_tsdu_frames == before + 1);
    CHECK(p25p1_carrier_nac_established(state) == nac);
}

/* Everything a NID's NAC writes still names @p nac, with the seed and its proof as given. */
static void
expect_nac_untouched(const dsd_state* state, int nac, unsigned long long p2_cc, uint8_t p2_verified) {
    CHECK(state->nac == nac);
    CHECK(state->rx_id.valid == DSD_RXID_ACCESS && state->rx_id.access == (uint16_t)nac);
    CHECK(state->p25_frame_nac_valid == 0U);
    CHECK(state->p25_frame_nac == (uint16_t)nac);
    CHECK(state->p2_cc == p2_cc);
    CHECK(state->p2_cc_verified == p2_verified);
}

/* Everything a NID's NAC writes names @p nac. */
static void
expect_nac_taken(const dsd_state* state, int nac) {
    CHECK(state->nac == nac);
    CHECK(state->rx_id.valid == DSD_RXID_ACCESS && state->rx_id.access == (uint16_t)nac);
    CHECK(state->p25_frame_nac_valid == 1U && state->p25_frame_nac == (uint16_t)nac);
    CHECK(state->p2_cc == (unsigned long long)nac);
    CHECK(state->p2_cc_verified == 0U);
    CHECK(p25p1_carrier_nac_established(state) == nac);
}

/* A carrier on NAC 0x1A6 whose seed (0x1A6) a Phase 2 burst proved. */
static void
establish_proven(dsd_opts* opts, dsd_state* state) {
    establish(opts, state, 0x1A6);
    state->p2_cc_verified = 1U;
    state->p2_proven_nac = 0x1A6;
}

/* A NID naming another NAC with at most four corrections is a NAC change taken at once, a parity disagreement or not
   (OP25's rx_sync NID limit). */
static void
test_cheap_nac_change_taken_at_once(void) {
    static const int counts[] = {0, 1, 4};
    for (int c = 0; c < 3; c++) {
        for (int parity_damage = 0; parity_damage < 2; parity_damage++) {
            dsd_opts* opts = new_opts();
            dsd_state* state = new_state();
            establish_proven(opts, state);

            nid_air nid = nid_with_errors(0x0E1, NID_DUID_LDU1, k_error_bits, counts[c]);
            if (parity_damage) {
                damage_parity(&nid);
            }
            const struct p25p1_nid_result alone = decode_alone(&nid, 0x1A6);
            CHECK(alone.status == (parity_damage ? NID_PARITY_OVERRIDE : NID_OK));
            CHECK(alone.nac == 0x0E1 && alone.error_count == counts[c] && alone.nac_borrowed == 0);

            CHECK(dispatch_nid(opts, state, &nid) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
            CHECK(g_ldu1_frames == 1);
            expect_nac_taken(state, 0x0E1);
            CHECK(state->nid_failures_total == 0U);
            free(state);
            free(opts);
        }
    }
}

/* A NID naming another NAC with 5 or 11 corrections is held: the LDU1 is not decoded, nothing takes its NAC (the
   received identity, the frame NAC, state->nac, the seed and its proof), and it counts as a failed NID. The next NID
   naming the same NAC confirms it, however far behind (a 1800-dibit PDU between them, or more). */
static void
test_costly_nac_change_held_then_confirmed(void) {
    static const int counts[] = {5, 11};
    for (int c = 0; c < 2; c++) {
        for (int parity_damage = 0; parity_damage < 2; parity_damage++) {
            dsd_opts* opts = new_opts();
            dsd_state* state = new_state();
            establish_proven(opts, state);

            nid_air nid = nid_with_errors(0x0E1, NID_DUID_LDU1, k_error_bits, counts[c]);
            if (parity_damage) {
                damage_parity(&nid);
            }
            const struct p25p1_nid_result alone = decode_alone(&nid, 0x1A6);
            CHECK(alone.status > 0 && alone.nac == 0x0E1 && alone.duid == NID_DUID_LDU1);
            CHECK(alone.error_count == counts[c] && alone.nac_borrowed == 0);

            (void)dispatch_nid(opts, state, &nid);
            CHECK(g_ldu1_frames == 0 && g_other_frames == 0);
            expect_nac_untouched(state, 0x1A6, 0x1A6ULL, 1U);
            CHECK(state->p2_proven_nac == 0x1A6);
            CHECK(state->nid_failures_total == 1U);
            CHECK(p25p1_carrier_nac_established(state) == 0x1A6);

            /* The next NID names the same NAC, after a long gap and itself costly: the change is confirmed. */
            const uint32_t gap_symbols = 1800U * 40U;
            state->symbol_clock += gap_symbols;
            state->symbolcnt += gap_symbols;
            nid_air again = nid_with_errors(0x0E1, NID_DUID_LDU1, k_other_error_bits, counts[c]);
            (void)dispatch_nid(opts, state, &again);
            CHECK(g_ldu1_frames == 1);
            expect_nac_taken(state, 0x0E1);
            CHECK(state->nid_failures_total == 1U);
            free(state);
            free(opts);
        }
    }
}

/* A NID naming the established NAC after a held one discards the held NAC: the next costly NID naming it is held
   again, not taken as its confirmation. */
static void
test_held_nac_discarded_by_established_nac(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    establish_proven(opts, state);

    nid_air bogus = nid_with_errors(0x00E, NID_DUID_LDU1, k_error_bits, 11);
    (void)dispatch_nid(opts, state, &bogus);
    CHECK(g_ldu1_frames == 0);

    nid_air own;
    make_nid(0x1A6, NID_DUID_TSDU, &own);
    put_errors(&own, k_other_error_bits, 7); /* a costly repeat of the carrier's own NAC is no change */
    (void)dispatch_nid(opts, state, &own);
    CHECK(g_tsdu_frames == 2);
    CHECK(state->nac == 0x1A6 && state->p25_frame_nac_valid == 1U && state->p25_frame_nac == 0x1A6U);
    CHECK(state->p2_cc == 0x1A6ULL && state->p2_cc_verified == 1U);

    nid_air bogus_again = nid_with_errors(0x00E, NID_DUID_LDU1, k_other_error_bits, 11);
    (void)dispatch_nid(opts, state, &bogus_again);
    CHECK(g_ldu1_frames == 0);
    expect_nac_untouched(state, 0x1A6, 0x1A6ULL, 1U);
    CHECK(state->nid_failures_total == 2U);
    free(state);
    free(opts);
}

/* A soft decode past the BCH radius (11 BCH corrections plus the soft search's flips) naming another NAC is held. */
static void
test_soft_decode_past_bch_radius_held(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    establish_proven(opts, state);

    nid_air nid = nid_with_errors(0x00E, NID_DUID_LDU1, k_error_bits, 13);
    /* The receiver doubted two of the wrong bits: the soft search flips them back, and the BCH corrects the rest. */
    nid.rel[k_error_bits[11]] = REL_DOUBTFUL;
    nid.rel[k_error_bits[12]] = REL_DOUBTFUL;
    const struct p25p1_nid_result alone = decode_alone(&nid, 0x1A6);
    CHECK(alone.status > 0 && alone.nac == 0x00E && alone.duid == NID_DUID_LDU1);
    CHECK(alone.error_count == 13 && alone.nac_borrowed == 0);

    (void)dispatch_nid(opts, state, &nid);
    CHECK(g_ldu1_frames == 0);
    expect_nac_untouched(state, 0x1A6, 0x1A6ULL, 1U);
    CHECK(state->nid_failures_total == 1U);
    free(state);
    free(opts);
}

/* @p mutate moves the stream on from the established NAC's: the next costly NID naming another NAC is the first on
   its stream, taken at once. */
static void
expect_costly_nac_taken_after(void (*mutate)(dsd_state* state)) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    establish_proven(opts, state);
    mutate(state);
    CHECK(p25p1_carrier_nac_established(state) == -1);

    nid_air nid = nid_with_errors(0x0E1, NID_DUID_LDU1, k_error_bits, 11);
    (void)dispatch_nid(opts, state, &nid);
    CHECK(g_ldu1_frames == 1);
    expect_nac_taken(state, 0x0E1);
    free(state);
    free(opts);
}

static void
no_change(dsd_state* state) {
    (void)state;
}

/* A control channel return: no carrier boundary, but the tune drops the sync histories and, once complete, moves the
   accepted tuning generation. */
static void
cc_return(dsd_state* state) {
    dsd_state_forget_sync_join_histories(state);
    dsd_trunk_tuning_generation_advance();
}

static void
tune_generation_moves(dsd_state* state) {
    (void)state;
    dsd_trunk_tuning_generation_advance();
}

static void
sync_histories_dropped(dsd_state* state) {
    dsd_state_forget_sync_join_histories(state);
}

static void
carrier_boundary(dsd_state* state) {
    state->carrier_seq++;
}

static void
carrier_nac_forgotten(dsd_state* state) {
    dsd_state_forget_p25p1_carrier_nac(state);
}

/* The first NID on a carrier names its NAC at any correction count, as before: a fresh state, and every boundary of
   the stream the established NAC was named on. */
static void
test_first_nid_on_a_stream_taken(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    CHECK(p25p1_carrier_nac_established(state) == -1);
    nid_air nid = nid_with_errors(0x0E1, NID_DUID_LDU1, k_error_bits, 11);
    (void)dispatch_nid(opts, state, &nid);
    CHECK(g_ldu1_frames == 1);
    CHECK(state->nac == 0x0E1 && p25p1_carrier_nac_established(state) == 0x0E1);
    free(state);
    free(opts);

    expect_costly_nac_taken_after(cc_return);
    expect_costly_nac_taken_after(tune_generation_moves);
    expect_costly_nac_taken_after(sync_histories_dropped);
    expect_costly_nac_taken_after(carrier_boundary);
    expect_costly_nac_taken_after(carrier_nac_forgotten);

    /* And the control: on the same stream the same NID is held. */
    opts = new_opts();
    state = new_state();
    establish_proven(opts, state);
    no_change(state);
    (void)dispatch_nid(opts, state, &nid);
    CHECK(g_ldu1_frames == 0 && state->nac == 0x1A6);
    free(state);
    free(opts);
}

/* A NID an input interruption cut forgets the established NAC: what follows the cut is the first on its stream. */
static void
test_cut_nid_forgets_the_established_nac(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    establish_proven(opts, state);

    nid_air own;
    make_nid(0x1A6, NID_DUID_TSDU, &own);
    g_interrupt_at_dibit = 15;
    CHECK(dispatch_nid(opts, state, &own) == DSD_FRAME_VERDICT_WITHHELD);
    CHECK(p25p1_carrier_nac_established(state) == -1);
    g_interrupt_at_dibit = -1;
    state->input_interrupted = 0;

    nid_air nid = nid_with_errors(0x0E1, NID_DUID_LDU1, k_error_bits, 11);
    (void)dispatch_nid(opts, state, &nid);
    CHECK(g_ldu1_frames == 1 && state->nac == 0x0E1);
    free(state);
    free(opts);
}

/* A NID that decoded only once the observed NAC (here a hand-set seed's) stood in for its received NAC field names
   no NAC, as before: its DUID stands, the established NAC and the hand-set seed stay. */
static void
test_borrowed_nac_and_hand_set_seed_unchanged(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    state->p2_cc = 0x293ULL;
    state->p2_hardset = 1;
    establish(opts, state, 0x1A6);
    CHECK(state->p2_cc == 0x293ULL);

    /* The seed's NAC with its NAC field and seven other bits wrong: only the retry with the seed's NAC put in decodes
       it. */
    static const int nac_field_errors[] = {0, 2, 4, 6, 8, 10};
    static const int other_errors[] = {14, 19, 27, 33, 39, 45, 52};
    nid_air nid = nid_with_errors(0x293, NID_DUID_TSDU, nac_field_errors, 6);
    put_errors(&nid, other_errors, 7);
    const struct p25p1_nid_result alone = decode_alone(&nid, 0x293);
    CHECK(alone.status > 0 && alone.nac == 0x293 && alone.nac_borrowed == 1 && alone.duid == NID_DUID_TSDU);
    CHECK(alone.error_count > P25P1_NID_NAC_CHANGE_MAX_CORRECTIONS);

    (void)dispatch_nid(opts, state, &nid);
    CHECK(g_tsdu_frames == 2);
    CHECK(state->nac == 0x1A6 && state->rx_id.access == 0x1A6U && state->p25_frame_nac_valid == 0U);
    CHECK(state->p2_cc == 0x293ULL && state->p2_hardset == 1);
    CHECK(p25p1_carrier_nac_established(state) == 0x1A6);

    /* A costly NID naming another NAC is held under a hand-set seed too, which it leaves alone. */
    nid_air other = nid_with_errors(0x0E1, NID_DUID_LDU1, k_error_bits, 6);
    (void)dispatch_nid(opts, state, &other);
    CHECK(g_ldu1_frames == 0 && state->nac == 0x1A6 && state->p2_cc == 0x293ULL);
    free(state);
    free(opts);
}

/* Issue #804: a control channel whose NAC (0x293) differs from the Phase 2 colour code (0x111) a CRC-passed network
   status set. Repeats of the control channel's NAC, costly or not, keep the colour code and its proof; a costly NID
   naming another NAC is held and touches neither. */
static void
test_distinct_colour_code_kept(void) {
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    establish(opts, state, 0x293);
    state->p2_cc = 0x111ULL;
    state->p2_cc_verified = 1U;

    nid_air repeat = nid_with_errors(0x293, NID_DUID_TSDU, k_error_bits, 7);
    (void)dispatch_nid(opts, state, &repeat);
    CHECK(g_tsdu_frames == 2);
    CHECK(state->nac == 0x293 && state->p2_cc == 0x111ULL && state->p2_cc_verified == 1U);

    nid_air other = nid_with_errors(0x0E1, NID_DUID_LDU1, k_error_bits, 9);
    (void)dispatch_nid(opts, state, &other);
    CHECK(g_ldu1_frames == 0);
    CHECK(state->nac == 0x293 && state->p2_cc == 0x111ULL && state->p2_cc_verified == 1U);
    CHECK(state->rx_id.access == 0x293U);
    free(state);
    free(opts);
}

int
main(void) {
    test_cheap_nac_change_taken_at_once();
    test_costly_nac_change_held_then_confirmed();
    test_held_nac_discarded_by_established_nac();
    test_soft_decode_past_bch_radius_held();
    test_first_nid_on_a_stream_taken();
    test_cut_nid_forgets_the_established_nac();
    test_borrowed_nac_and_hand_set_seed_unchanged();
    test_distinct_colour_code_kept();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "P25 P1 NID NAC change: OK\n");
    return 0;
}

/* Stand-ins for what the dispatcher calls past the NID. */
extern "C" {

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    const int d = g_dibit_reads < NID_DIBITS ? g_dibit_reads : NID_DIBITS - 1;
    if (g_dibit_reads++ == g_interrupt_at_dibit && state != NULL) {
        state->input_interrupted = 1;
    }
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

void
processHDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_other_frames++;
}

void
processLDU1(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_ldu1_frames++;
}

void
processLDU2(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_other_frames++;
}

void
processTDULC(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_other_frames++;
}

void
processTDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_other_frames++;
}

void
processTSBK(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_tsdu_frames++;
}

void
processMPDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_other_frames++;
}

} // extern "C"
