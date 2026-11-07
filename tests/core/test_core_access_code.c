// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The access code a call history row records (issue #575): which kind each protocol carries, and when the live
 * value the decoder holds is a code at all rather than a sentinel.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* Pinned: history rows, the control API and the Qt call history store carry these values. */
_Static_assert(DSD_ACCESS_CODE_NONE == 0, "access code kind NONE is pinned");
_Static_assert(DSD_ACCESS_CODE_COLOR_CODE == 1, "access code kind COLOR_CODE is pinned");
_Static_assert(DSD_ACCESS_CODE_NAC == 2, "access code kind NAC is pinned");
_Static_assert(DSD_ACCESS_CODE_RAN == 3, "access code kind RAN is pinned");
_Static_assert(DSD_ACCESS_CODE_CAN == 4, "access code kind CAN is pinned");

static int
expect_code(const char* label, const dsd_state* state, int protocol, uint16_t svc, uint8_t has_svc, int want_rc,
            dsd_access_code_kind want_kind, uint16_t want_value) {
    /* Poisoned, so a reader that leaves an output untouched is caught. */
    uint8_t kind = 0xA5U;
    uint16_t value = 0xBEEFU;
    const int rc = dsd_access_code_current(state, protocol, svc, has_svc, &kind, &value);
    if (rc != want_rc || kind != (uint8_t)want_kind || value != want_value) {
        DSD_FPRINTF(stderr, "%s: got rc=%d kind=%u value=0x%X, want rc=%d kind=%u value=0x%X\n", label, rc,
                    (unsigned)kind, (unsigned)value, want_rc, (unsigned)want_kind, (unsigned)want_value);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_none(const char* label, const dsd_state* state, int protocol, uint16_t svc, uint8_t has_svc) {
    return expect_code(label, state, protocol, svc, has_svc, 0, DSD_ACCESS_CODE_NONE, 0U);
}

static int
expect_kind(const char* label, int synctype, dsd_access_code_kind want) {
    const dsd_access_code_kind got = dsd_access_code_kind_for_protocol(synctype);
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: kind_for_protocol(%d) = %d, want %d\n", label, synctype, (int)got, (int)want);
        return 1;
    }
    return 0;
}

/* Every source at its "nothing decoded" value, as a fresh carrier leaves it. */
static void
reset_sources(dsd_state* state) {
    state->dmr_color_code = 16U;
    state->nac = 0;
    state->p2_cc = 0ULL;
    dsd_state_forget_p2_proof(state);
    state->nxdn_last_ran = (unsigned int)-1;
    state->nxdn_last_ran_stand_in = 0U;
    state->dpmr_color_code = -1;
    dsd_tetra_publication_reset(&state->tetra_pub);
    dsd_rxid_reset(state);
}

static int
test_kind_for_every_family(void) {
    int rc = 0;
    rc |= expect_kind("DMR BS voice", DSD_SYNC_DMR_BS_VOICE_POS, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("DMR BS data", DSD_SYNC_DMR_BS_DATA_NEG, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("DMR MS voice", DSD_SYNC_DMR_MS_VOICE, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("DMR MS data", DSD_SYNC_DMR_MS_DATA, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("DMR RC data", DSD_SYNC_DMR_RC_DATA, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("dPMR FS1", DSD_SYNC_DPMR_FS1_POS, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("dPMR FS4-", DSD_SYNC_DPMR_FS4_NEG, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("P25 Phase 1", DSD_SYNC_P25P1_POS, DSD_ACCESS_CODE_NAC);
    rc |= expect_kind("P25 Phase 1-", DSD_SYNC_P25P1_NEG, DSD_ACCESS_CODE_NAC);
    rc |= expect_kind("P25 Phase 2", DSD_SYNC_P25P2_POS, DSD_ACCESS_CODE_NAC);
    rc |= expect_kind("P25 Phase 2-", DSD_SYNC_P25P2_NEG, DSD_ACCESS_CODE_NAC);
    rc |= expect_kind("NXDN", DSD_SYNC_NXDN_POS, DSD_ACCESS_CODE_RAN);
    rc |= expect_kind("NXDN-", DSD_SYNC_NXDN_NEG, DSD_ACCESS_CODE_RAN);
    rc |= expect_kind("M17 LSF", DSD_SYNC_M17_LSF_POS, DSD_ACCESS_CODE_CAN);
    rc |= expect_kind("M17 stream", DSD_SYNC_M17_STR_NEG, DSD_ACCESS_CODE_CAN);
    rc |= expect_kind("M17 packet", DSD_SYNC_M17_PKT_POS, DSD_ACCESS_CODE_CAN);
    rc |= expect_kind("TETRA NDB", DSD_SYNC_TETRA_NDB_POS, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("TETRA NDB-", DSD_SYNC_TETRA_NDB_NEG, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("TETRA SB", DSD_SYNC_TETRA_SB_POS, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("TETRA SB-", DSD_SYNC_TETRA_SB_NEG, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_kind("D-STAR voice", DSD_SYNC_DSTAR_VOICE_POS, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("D-STAR header", DSD_SYNC_DSTAR_HD_NEG, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("YSF", DSD_SYNC_YSF_POS, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("EDACS", DSD_SYNC_EDACS_POS, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("ProVoice", DSD_SYNC_PROVOICE_NEG, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("X2-TDMA voice", DSD_SYNC_X2TDMA_VOICE_POS, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("X2-TDMA data", DSD_SYNC_X2TDMA_DATA_NEG, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("analog", DSD_SYNC_ANALOG, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("digital", DSD_SYNC_DIGITAL, DSD_ACCESS_CODE_NONE);
    rc |= expect_kind("no sync", DSD_SYNC_NONE, DSD_ACCESS_CODE_NONE);
    return rc;
}

static int
test_dmr_colour_code(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    rc |= expect_none("DMR unlocked (16)", state, DSD_SYNC_DMR_BS_VOICE_POS, 0U, 0U);
    state->dmr_color_code = 0U;
    rc |= expect_code("DMR CC 0", state, DSD_SYNC_DMR_BS_VOICE_POS, 0U, 0U, 1, DSD_ACCESS_CODE_COLOR_CODE, 0U);
    state->dmr_color_code = 15U;
    rc |= expect_code("DMR CC 15", state, DSD_SYNC_DMR_MS_VOICE, 0U, 0U, 1, DSD_ACCESS_CODE_COLOR_CODE, 15U);
    state->dmr_color_code = 200U;
    rc |= expect_none("DMR out of range", state, DSD_SYNC_DMR_BS_DATA_POS, 0U, 0U);
    return rc;
}

static int
test_p25_phase1_reads_nac_only(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    state->nac = 0x293;
    rc |= expect_code("P25p1 NAC 293", state, DSD_SYNC_P25P1_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0x293U);
    /* A Phase 1 NID that decodes 0x000 or 0xFFF leaves nac at 0; a p2_cc left by an earlier Phase 2 carrier (or by
     * this one's own earlier NID) is not this carrier's code. */
    state->nac = 0;
    state->p2_cc = 0x3A1ULL;
    rc |= expect_none("P25p1 nac 0 ignores p2_cc", state, DSD_SYNC_P25P1_NEG, 0U, 0U);
    state->nac = 0xFFF;
    rc |= expect_none("P25p1 NAC FFF", state, DSD_SYNC_P25P1_POS, 0U, 0U);
    state->nac = 0x001;
    rc |= expect_code("P25p1 NAC 001", state, DSD_SYNC_P25P1_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0x001U);
    state->nac = 0xFFE;
    rc |= expect_code("P25p1 NAC FFE", state, DSD_SYNC_P25P1_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0xFFEU);
    state->nac = -1;
    rc |= expect_none("P25p1 negative nac", state, DSD_SYNC_P25P1_POS, 0U, 0U);
    return rc;
}

/* A Phase 2 call reads the NAC the latest checked proof on this carrier named (issue #802), never the descrambler's
 * seed as such, a Phase 1 NAC, or the received identity record. */
static int
test_p25_phase2_reads_the_proven_nac(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    state->p2_cc = 0x293ULL;
    /* A seed no proof has named is no received NAC: -X, or another carrier, may have left it (issue #575). */
    rc |= expect_none("P25p2 unproven p2_cc", state, DSD_SYNC_P25P2_POS, 0U, 0U);
    /* The seed's own proof mark is not read either: only the NAC a proof named is. */
    state->p2_cc_verified = 1U;
    rc |= expect_none("P25p2 seed proof without a proven NAC", state, DSD_SYNC_P25P2_POS, 0U, 0U);
    rc |= expect_int("a proof names the seed's NAC", dsd_state_note_p2_proven_nac(state, state->p2_cc), 1);
    rc |= expect_code("P25p2 proven 293", state, DSD_SYNC_P25P2_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0x293U);
    /* The Phase 1 nac and the received identity record are not a Phase 2 call's code. */
    reset_sources(state);
    state->nac = 0x293;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
    rc |= expect_none("P25p2 ignores nac and rx_id", state, DSD_SYNC_P25P2_NEG, 0U, 0U);
    /* dsd-neo's own rule (a dead or stuck input decodes as NAC 0x000 or 0xFFF): neither, nor anything past twelve bits,
     * is a proof, and the record keeps what it had. */
    reset_sources(state);
    rc |= expect_int("NAC 000 proves nothing", dsd_state_note_p2_proven_nac(state, 0x000ULL), 0);
    rc |= expect_int("NAC FFF proves nothing", dsd_state_note_p2_proven_nac(state, 0xFFFULL), 0);
    rc |= expect_int("past twelve bits proves nothing", dsd_state_note_p2_proven_nac(state, 0x1293ULL), 0);
    rc |= expect_none("P25p2 no proof", state, DSD_SYNC_P25P2_POS, 0U, 0U);
    rc |= expect_int("NAC 001 is a proof", dsd_state_note_p2_proven_nac(state, 0x001ULL), 1);
    rc |= expect_code("P25p2 proven 001", state, DSD_SYNC_P25P2_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0x001U);
    rc |= expect_int("NAC FFF keeps the proof", dsd_state_note_p2_proven_nac(state, 0xFFFULL), 0);
    rc |= expect_int("NAC FFE is a proof", dsd_state_note_p2_proven_nac(state, 0xFFEULL), 1);
    rc |= expect_code("P25p2 proven FFE", state, DSD_SYNC_P25P2_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0xFFEU);
    /* A record holding a value outside the range (written whole by a snapshot, say) reads as none. */
    state->p2_proven_nac = 0xFFF;
    rc |= expect_none("P25p2 record FFF", state, DSD_SYNC_P25P2_POS, 0U, 0U);
    state->p2_proven_nac = 0;
    rc |= expect_none("P25p2 record 000", state, DSD_SYNC_P25P2_POS, 0U, 0U);
    return rc;
}

/* The proof record's life on one carrier (issue #802 B8): the latest proof wins, whatever seed is configured, and the
 * receive policy's NAC verdict follows the same record. */
static int
test_p25_phase2_proof_lifecycle(dsd_state* state) {
    static dsd_opts opts;
    int rc = 0;
    reset_sources(state);
    DSD_MEMSET(&opts, 0, sizeof opts);
    char error[160];
    rc |= expect_int("NAC filter parses",
                     dsd_receive_edit_parse(&opts.receive, DSD_RECEIVE_NAC, 1, "293", "", "", error, sizeof error), 0);

    /* A hand-set seed (-X 123) proves nothing by itself: the policy is still checking. */
    state->p2_hardset = 1;
    state->p2_cc = 0x123ULL;
    rc |= expect_none("hand-set seed alone", state, DSD_SYNC_P25P2_POS, 0U, 0U);
    rc |= expect_int("hand-set seed alone: checking", dsd_receive_status(&opts, state, DSD_SYNC_P25P2_POS, 0),
                     DSD_RECEIVE_CHECKING);
    /* A checked network status names 293 on this carrier: the call reads 293 though the seed stays 123, and the policy
     * admits it. */
    rc |= expect_int("the broadcast's NAC is a new proof", dsd_state_note_p2_proven_nac(state, 0x293ULL), 1);
    rc |= expect_code("hand-set 123, broadcast 293", state, DSD_SYNC_P25P2_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0x293U);
    rc |= expect_int("hand-set 123, broadcast 293: admitted", dsd_receive_status(&opts, state, DSD_SYNC_P25P2_POS, 0),
                     DSD_RECEIVE_ALLOWED);
    rc |= expect_int("the same broadcast again is no new value", dsd_state_note_p2_proven_nac(state, 0x293ULL), 0);
    /* A burst descrambled with the seed proves 123 later: the latest proof wins, and the policy rejects it. */
    rc |= expect_int("the seed's proof is a new value", dsd_state_note_p2_proven_nac(state, 0x123ULL), 1);
    rc |= expect_code("then the seed's proof", state, DSD_SYNC_P25P2_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0x123U);
    rc |= expect_int("then the seed's proof: rejected", dsd_receive_status(&opts, state, DSD_SYNC_P25P2_POS, 0),
                     DSD_RECEIVE_REJECTED);

    /* The carrier goes: nothing is proven, the seed stays, and the policy is checking again. */
    state->p2_cc_verified = 1U;
    dsd_state_forget_p2_proof(state);
    rc |= expect_int("the forget drops the seed's proof", state->p2_cc_verified, 0);
    rc |= expect_int("the forget drops the proven NAC", state->p2_proven_nac, -1);
    rc |= expect_int("the forget keeps the seed", (long long)state->p2_cc, 0x123);
    rc |= expect_none("after the forget", state, DSD_SYNC_P25P2_POS, 0U, 0U);
    rc |= expect_int("after the forget: checking", dsd_receive_status(&opts, state, DSD_SYNC_P25P2_POS, 0),
                     DSD_RECEIVE_CHECKING);
    state->p2_hardset = 0;
    reset_sources(state);
    return rc;
}

static int
test_nxdn_ran(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    rc |= expect_none("NXDN unknown RAN", state, DSD_SYNC_NXDN_POS, 0U, 0U);
    state->nxdn_last_ran = 63U;
    rc |= expect_code("NXDN RAN 63", state, DSD_SYNC_NXDN_NEG, 0U, 0U, 1, DSD_ACCESS_CODE_RAN, 63U);
    state->nxdn_last_ran = 0U;
    rc |= expect_code("NXDN RAN 0", state, DSD_SYNC_NXDN_POS, 0U, 0U, 1, DSD_ACCESS_CODE_RAN, 0U);
    state->nxdn_last_ran = 64U;
    rc |= expect_none("NXDN RAN 64", state, DSD_SYNC_NXDN_POS, 0U, 0U);
    return rc;
}

/* nxdn_last_ran also holds values that are not a RAN, which the terminal shows in its place: an IDAS (Type-D)
 * carrier's SCCH area bit, and the 7 a DCR transmission is given. The protocol marks them with
 * nxdn_last_ran_stand_in, and neither carrier has an access code; a conventional NXDN carrier's RAN still reads. */
static int
test_nxdn_stand_ins_are_not_rans(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    state->nxdn_last_ran = 1U;
    state->nxdn_last_ran_stand_in = 1U;
    rc |= expect_none("IDAS area bit", state, DSD_SYNC_NXDN_POS, 0U, 0U);
    state->nxdn_last_ran = 0U;
    rc |= expect_none("IDAS area bit, negative polarity", state, DSD_SYNC_NXDN_NEG, 0U, 0U);

    reset_sources(state);
    state->nxdn_last_ran = 7U;
    state->nxdn_last_ran_stand_in = 1U;
    rc |= expect_none("DCR stand-in 7", state, DSD_SYNC_NXDN_POS, 0U, 0U);

    reset_sources(state);
    state->nxdn_last_ran = 7U;
    rc |= expect_code("conventional NXDN RAN 7", state, DSD_SYNC_NXDN_POS, 0U, 0U, 1, DSD_ACCESS_CODE_RAN, 7U);
    return rc;
}

static int
test_dpmr_colour_code(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    rc |= expect_none("dPMR none (-1)", state, DSD_SYNC_DPMR_FS2_POS, 0U, 0U);
    state->dpmr_color_code = 0;
    rc |= expect_code("dPMR CC 0", state, DSD_SYNC_DPMR_FS2_POS, 0U, 0U, 1, DSD_ACCESS_CODE_COLOR_CODE, 0U);
    state->dpmr_color_code = 63;
    rc |= expect_code("dPMR CC 63", state, DSD_SYNC_DPMR_FS3_NEG, 0U, 0U, 1, DSD_ACCESS_CODE_COLOR_CODE, 63U);
    state->dpmr_color_code = 64;
    rc |= expect_none("dPMR CC 64", state, DSD_SYNC_DPMR_FS2_POS, 0U, 0U);
    return rc;
}

static int
test_m17_can(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    rc |= expect_none("M17 without service metadata", state, DSD_SYNC_M17_LSF_POS, 0x13U, 0U);
    rc |= expect_code("M17 svc 0x13", state, DSD_SYNC_M17_LSF_POS, 0x13U, 1U, 1, DSD_ACCESS_CODE_CAN, 3U);
    rc |= expect_code("M17 CAN 0", state, DSD_SYNC_M17_STR_POS, 0x00U, 1U, 1, DSD_ACCESS_CODE_CAN, 0U);
    rc |= expect_code("M17 CAN 15", state, DSD_SYNC_M17_STR_POS, 0x0FU, 1U, 1, DSD_ACCESS_CODE_CAN, 15U);
    return rc;
}

/* TETRA's colour code is the one a CRC-checked trunked mode BSCH sent on this carrier (the received identity record).
 * A --tetra-net preset or the network the descrambler retains is configuration: with no BSCH there is no code. A
 * direct mode carrier has none, and a carrier boundary forgets it. */
static int
test_tetra_colour_code(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    rc |= expect_none("TETRA nothing decoded", state, DSD_SYNC_TETRA_NDB_POS, 0U, 0U);
    /* What a --tetra-net preset (or a network kept from another carrier) leaves in the publication. */
    state->tetra_pub.mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    state->tetra_pub.net_known = 1U;
    state->tetra_pub.colour = 25U;
    rc |= expect_none("TETRA preset alone", state, DSD_SYNC_TETRA_NDB_POS, 0U, 0U);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    rc |= expect_code("TETRA CC 25", state, DSD_SYNC_TETRA_SB_NEG, 0U, 0U, 1, DSD_ACCESS_CODE_COLOR_CODE, 25U);
    state->tetra_pub.colour = 7U; /* the publication is not read */
    rc |=
        expect_code("TETRA BSCH not preset", state, DSD_SYNC_TETRA_SB_NEG, 0U, 0U, 1, DSD_ACCESS_CODE_COLOR_CODE, 25U);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 0U);
    rc |= expect_code("TETRA CC 0", state, DSD_SYNC_TETRA_NDB_POS, 0U, 0U, 1, DSD_ACCESS_CODE_COLOR_CODE, 0U);
    dsd_rxid_mark_stale(state); /* a gap keeps this carrier's code */
    rc |= expect_code("TETRA stale", state, DSD_SYNC_TETRA_NDB_POS, 0U, 0U, 1, DSD_ACCESS_CODE_COLOR_CODE, 0U);
    dsd_rxid_note_tetra_direct_mode(state);
    rc |= expect_none("TETRA direct mode", state, DSD_SYNC_TETRA_NDB_POS, 0U, 0U);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    dsd_rxid_trunk_tune(state);
    rc |= expect_none("TETRA trunking retune", state, DSD_SYNC_TETRA_NDB_POS, 0U, 0U);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    dsd_rxid_reset(state);
    rc |= expect_none("TETRA carrier boundary", state, DSD_SYNC_TETRA_NDB_POS, 0U, 0U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 9U);
    rc |= expect_none("TETRA after a DMR code", state, DSD_SYNC_TETRA_NDB_POS, 0U, 0U);
    return rc;
}

/* Protocols with no access code report none even with every source holding a valid value. */
static int
test_codeless_protocols(dsd_state* state) {
    int rc = 0;
    state->dmr_color_code = 3U;
    state->nac = 0x293;
    state->p2_cc = 0x293ULL;
    state->p2_proven_nac = 0x293;
    state->nxdn_last_ran = 5U;
    state->dpmr_color_code = 7;
    rc |= expect_none("YSF", state, DSD_SYNC_YSF_POS, 0x13U, 1U);
    rc |= expect_none("D-STAR", state, DSD_SYNC_DSTAR_VOICE_POS, 0x13U, 1U);
    rc |= expect_none("EDACS", state, DSD_SYNC_EDACS_POS, 0x13U, 1U);
    rc |= expect_none("ProVoice", state, DSD_SYNC_PROVOICE_POS, 0x13U, 1U);
    rc |= expect_none("X2-TDMA", state, DSD_SYNC_X2TDMA_VOICE_NEG, 0x13U, 1U);
    rc |= expect_none("analog", state, DSD_SYNC_ANALOG, 0x13U, 1U);
    rc |= expect_none("no sync", state, DSD_SYNC_NONE, 0x13U, 1U);
    return rc;
}

/* Issue #667 (D5): an analog call records no access code, even on a carrier whose checked messages named one -- a -Y
   list or a trunk scan that moves from a DMR, P25, NXDN, dPMR or M17 row to an NFM or AM row. Its CTCSS/DCS tone is its
   receive identity, never an access code. */
static int
test_analog_after_every_code(dsd_state* state) {
    static const struct {
        dsd_rxid_family family;
        dsd_access_code_kind kind;
        uint16_t value;
    } k_codes[] = {
        {DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 9U}, {DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U},
        {DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U},       {DSD_RXID_FAMILY_DPMR, DSD_ACCESS_CODE_COLOR_CODE, 7U},
        {DSD_RXID_FAMILY_M17, DSD_ACCESS_CODE_CAN, 3U},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof k_codes / sizeof k_codes[0]; i++) {
        reset_sources(state);
        dsd_rxid_note_access(state, k_codes[i].family, (uint8_t)k_codes[i].kind, k_codes[i].value);
        rc |= expect_none("analog after a checked code", state, DSD_SYNC_ANALOG, 0U, 0U);
        rc |= expect_none("analog after a checked code, svc", state, DSD_SYNC_ANALOG, k_codes[i].value, 1U);
    }
    reset_sources(state);
    return rc;
}

static int
test_null_inputs(void) {
    int rc = expect_none("NULL state", NULL, DSD_SYNC_DMR_BS_VOICE_POS, 0U, 0U);
    rc |= expect_none("NULL state M17", NULL, DSD_SYNC_M17_LSF_POS, 0x13U, 1U);
    static dsd_state state;
    state.dmr_color_code = 5U;
    if (dsd_access_code_current(&state, DSD_SYNC_DMR_BS_VOICE_POS, 0U, 0U, NULL, NULL) != 1) {
        DSD_FPRINTF(stderr, "NULL outputs: valid code not reported\n");
        rc = 1;
    }
    return rc;
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        return 1;
    }
    int rc = 0;
    rc |= test_kind_for_every_family();
    rc |= test_dmr_colour_code(state);
    rc |= test_p25_phase1_reads_nac_only(state);
    rc |= test_p25_phase2_reads_the_proven_nac(state);
    rc |= test_p25_phase2_proof_lifecycle(state);
    rc |= test_nxdn_ran(state);
    rc |= test_nxdn_stand_ins_are_not_rans(state);
    rc |= test_dpmr_colour_code(state);
    rc |= test_m17_can(state);
    rc |= test_tetra_colour_code(state);
    rc |= test_codeless_protocols(state);
    rc |= test_analog_after_every_code(state);
    rc |= test_null_inputs();
    dsd_state_ext_free_all(state);
    free(state);
    if (rc == 0) {
        printf("CORE_ACCESS_CODE: OK\n");
    }
    return rc;
}
