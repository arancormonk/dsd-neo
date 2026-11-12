// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/protocol/p25/p25p1_nid_verdict.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <mbelib-neo/mbelib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

enum {
    P25P1_DUID_HDU = 0x0,
    P25P1_DUID_TDU = 0x3,
    P25P1_DUID_LDU1 = 0x5,
    P25P1_DUID_TSBK = 0x7,
    P25P1_DUID_LDU2 = 0xA,
    P25P1_DUID_MPDU = 0xC,
    P25P1_DUID_TDULC = 0xF,
    P25P1_DUID_INVALID = 0xFF,
};

/* How long a decoded NID vouches for the syncs that follow it, in symbols. Two seconds at
 * 4800 symbols/s: long enough to span the gaps a control channel leaves between the frames
 * it does decode, short enough that a channel which stops decoding gives the profile up
 * within a dwell of the last thing it proved. The same span as
 * DSD_FRAME_SYNC_P25P1_VALIDATED_HOLD_SYMBOLS, deliberately not the same constant -- that
 * one holds a modulation against the vote heuristics, this one holds a symbol profile
 * against the hunt's budget, and neither should move because the other did. */
#define P25P1_NID_EVIDENCE_WINDOW_SYMBOLS 9600U

static int
p25p1_valid_observed_nac(unsigned long long nac) {
    return nac > 0ULL && nac <= 0xFFFULL && nac != 0xFFFULL;
}

static int
p25p1_observed_nac(const dsd_state* state) {
    if (state == NULL) {
        return 0;
    }
    if (state->p2_hardset != 0 && p25p1_valid_observed_nac(state->p2_cc)) {
        return (int)state->p2_cc;
    }
    if (p25p1_valid_observed_nac((unsigned long long)state->nac)) {
        return state->nac;
    }
    if (p25p1_valid_observed_nac(state->p2_cc)) {
        return (int)state->p2_cc;
    }
    return 0;
}

static uint8_t
p25p1_llr_reliability(int16_t llr) {
    int v = llr < 0 ? -(int)llr : (int)llr;
    if (v > 255) {
        v = 255;
    }
    return (uint8_t)v;
}

static void
p25p1_append_bch_bits(char bch_code[63], uint8_t bch_reliab[63], int* index_bch_code, int dibit,
                      const dsd_dibit_soft_t* soft) {
    bch_code[*index_bch_code] = 1 & (dibit >> 1);
    bch_reliab[*index_bch_code] = p25p1_llr_reliability(soft->llr[0]);
    (*index_bch_code)++;

    bch_code[*index_bch_code] = 1 & dibit;
    bch_reliab[*index_bch_code] = p25p1_llr_reliability(soft->llr[1]);
    (*index_bch_code)++;
}

static void
p25p1_read_nac_bits(dsd_opts* opts, dsd_state* state, char bch_code[63], uint8_t bch_reliab[63], int* index_bch_code) {
    int i;
    dsd_dibit_soft_t soft;

    for (i = 0; i < 6; i++) {
        int dibit = getDibitSoft(opts, state, &soft);
        p25p1_append_bch_bits(bch_code, bch_reliab, index_bch_code, dibit, &soft);
    }
}

static uint8_t
p25p1_read_duid_bits(dsd_opts* opts, dsd_state* state, char bch_code[63], uint8_t bch_reliab[63], int* index_bch_code) {
    int i;
    uint8_t duid = 0;
    dsd_dibit_soft_t soft;

    for (i = 0; i < 2; i++) {
        int dibit = getDibitSoft(opts, state, &soft);
        duid = (uint8_t)((duid << 2) | (uint8_t)(dibit & 0x3));
        p25p1_append_bch_bits(bch_code, bch_reliab, index_bch_code, dibit, &soft);
    }
    return duid;
}

static void
p25p1_read_bch_soft_dibits(dsd_opts* opts, dsd_state* state, char bch_code[63], uint8_t bch_reliab[63],
                           int* index_bch_code, int dibit_count) {
    int i;
    dsd_dibit_soft_t soft;

    for (i = 0; i < dibit_count; i++) {
        int dibit = getDibitSoft(opts, state, &soft);
        p25p1_append_bch_bits(bch_code, bch_reliab, index_bch_code, dibit, &soft);
    }
}

static void
p25p1_read_nid_fields(dsd_opts* opts, dsd_state* state, uint8_t* duid, char bch_code[63], uint8_t bch_reliab[63],
                      unsigned char* parity, uint8_t* parity_reliab) {
    int index_bch_code = 0;
    int dibit;
    dsd_dibit_soft_t soft;

    p25p1_read_nac_bits(opts, state, bch_code, bch_reliab, &index_bch_code);
    *duid = p25p1_read_duid_bits(opts, state, bch_code, bch_reliab, &index_bch_code);
    p25p1_read_bch_soft_dibits(opts, state, bch_code, bch_reliab, &index_bch_code, 3);

    dibit = getDibitSoft(opts, state, NULL);
    p25_status_accum_add(state, dibit);

    p25p1_read_bch_soft_dibits(opts, state, bch_code, bch_reliab, &index_bch_code, 20);

    dibit = getDibitSoft(opts, state, &soft);
    bch_code[index_bch_code] = 1 & (dibit >> 1);
    bch_reliab[index_bch_code] = p25p1_llr_reliability(soft.llr[0]);
    *parity = (unsigned char)(1 & dibit);
    *parity_reliab = p25p1_llr_reliability(soft.llr[1]);
}

static void
p25p1_apply_nac_update(dsd_state* state, int new_nac) {
    int valid_nac = p25p1_nid_nac_usable(new_nac);

    /* A NID split by a carrier boundary names no carrier's NAC (issue #804): the BCH may accept the split word, but
       neither the received identity nor state->nac, the seed's NAC or the frame's takes it. The boundary zeroed
       state->nac so the next carrier never reads the old one. */
    if (state->frame_dispatch_active && dsd_state_frame_read_lost(state, state->frame_dispatch_carrier_seq)) {
        return;
    }
    /* The NAC of a NID the BCH accepted is the carrier's received access code, an unchanged repeat included, and the
       NAC of the frame being decoded (issue #804), which a network status in it is paired with. */
    if (valid_nac) {
        dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, (uint16_t)new_nac);
        state->p25_frame_nac = (uint16_t)new_nac;
        state->p25_frame_nac_valid = 1U;
    }
    /* The seed's NAC follows a NID that names a new NAC, and comes back from the next NID once cleared by hand on this
       carrier (issue #804). A repeat of the NAC state->nac holds leaves any other seed NAC alone: on a system whose
       Phase 2 colour code differs from its Phase 1 control channel's NAC, a network status that passed its CRC set it,
       and repeats of the control channel's NAC keep it. A carrier boundary, a trunked voice channel tune included,
       zeroes state->nac, so the first NID after one still rewrites a seed NAC that differs. */
    if (state->p2_hardset == 0 && valid_nac && state->p2_cc != (unsigned long long)new_nac
        && (state->p2_cc == 0ULL || new_nac != state->nac)) {
        state->p2_cc = (unsigned long long)new_nac;
        dsd_state_forget_p2_proof(state); // a Phase 1 NID proves no Phase 2 seed or NAC (issue #575, #802)
    }
    if (new_nac != state->nac) {
        if (valid_nac) {
            state->nac = new_nac;
        }
        state->debug_header_errors++;
    }
}

static void
p25p1_apply_duid_update(dsd_state* state, uint8_t* duid, uint8_t new_duid) {
    if (new_duid != *duid) {
        *duid = new_duid;
        state->debug_header_errors++;
    }
}

static void
p25p1_handle_nid_decode_success(const dsd_opts* opts, dsd_state* state, uint8_t* duid,
                                const struct p25p1_nid_result* decoded) {
    if (decoded->error_count > 0) {
        state->nid_corrections_total += (unsigned int)decoded->error_count;
    }
    if (decoded->status == NID_PARITY_OVERRIDE) {
        state->nid_parity_overrides++;
        if (opts->verbose > 1) {
            DSD_FPRINTF(stderr, " [NID parity override, %d corrections]", decoded->error_count);
        }
    }

    /* A NID that decoded only once the observed NAC stood in for its received NAC field carries the decoder's own NAC
       (a hand-set seed's, the last NID's, or the seed's, which a carrier boundary keeps), not one read off the air
       (issue #804): its DUID stands, but neither the received NAC, the received identity, the seed's NAC nor the
       frame's NAC a network status pairs with takes it. */
    if (!decoded->nac_borrowed) {
        p25p1_apply_nac_update(state, decoded->nac);
    }
    p25p1_apply_duid_update(state, duid, decoded->duid);
}

/* A NID naming another NAC than the carrier's at a costly correction count (p25p1_nid_verdict_classify()): as likely a
   miscorrected NID of the carrier's own frame as a real NAC change, so nothing decodes and nothing takes its NAC until
   the next NID confirms it. It counts as a NID that failed. */
static void
p25p1_handle_nid_nac_held(const dsd_opts* opts, dsd_state* state, uint8_t* duid, const p25p1_nid_verdict* verdict) {
    state->nid_failures_total++;
    if (opts->verbose > 1) {
        DSD_FPRINTF(stderr, " [NID NAC %03X held, %d corrections]", (unsigned int)verdict->nac, verdict->corrections);
    }
    *duid = P25P1_DUID_INVALID;
    state->debug_header_critical_errors++;
}

static void
p25p1_handle_nid_decode_failure(const dsd_opts* opts, dsd_state* state, uint8_t* duid, enum NidResult status) {
    state->nid_failures_total++;

    if (status == NID_PARITY_MISMATCH && opts->verbose > 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " NID PARITY MISMATCH ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    *duid = P25P1_DUID_INVALID;
    state->debug_header_critical_errors++;
}

/* How the frame sync accepted the frame's sync word (dsd_frame_sync_p25p1_tolerant_nac()): @c nac is the anchor's NAC
   when within the tolerance, -1 when exact. */
typedef struct {
    int nac;
    unsigned int bit_errors;
} p25p1_sync_acceptance;

/* The frame behind a sync word the hunt accepted within its tolerance decodes only when its NID passes the strict check
   (p25p1_nid_verdict_passes_tolerant_sync()). One that does not may be no frame at all: it is dropped before anything
   takes its NID's NAC, and counts nowhere, as a sync the exact hunt never accepted would. */
static int
p25p1_tolerant_sync_dropped(const dsd_opts* opts, const p25p1_sync_acceptance* sync, const p25p1_nid_verdict* verdict) {
    if (sync->nac < 0) {
        return 0;
    }
    if (!p25p1_nid_verdict_passes_tolerant_sync(verdict, sync->nac)) {
        if (opts->verbose > 1 && verdict->disposition == P25P1_NID_REJECTED) {
            DSD_FPRINTF(stderr, " [tolerant sync, %u bit errors, dropped: NID failed]\n", sync->bit_errors);
        } else if (opts->verbose > 1) {
            DSD_FPRINTF(stderr, " [tolerant sync, %u bit errors, dropped: NAC %03X, %d corrections, parity %s%s]\n",
                        sync->bit_errors, (unsigned int)verdict->nac, verdict->corrections,
                        verdict->parity_agreed ? "agreed" : "disagreed", verdict->nac_borrowed ? ", borrowed" : "");
        }
        return 1;
    }
    if (opts->verbose > 1) {
        DSD_FPRINTF(stderr, " [tolerant sync, %u bit errors]", sync->bit_errors);
    }
    return 0;
}

/*
 * Mark the static helper roots used by the public dispatch entrypoint. CodeQL's
 * manual C/C++ database can miss this local call chain and report all children.
 *
 * @p sync says how the frame sync accepted the frame's sync word. The NID's verdict goes to @p out_verdict;
 * *@p out_dropped is set when a tolerant sync's frame was dropped.
 */
static uint8_t DSD_ATTR_USED
p25p1_decode_nid_and_duid(dsd_opts* opts, dsd_state* state, const p25p1_sync_acceptance* sync,
                          p25p1_nid_verdict* out_verdict, int* out_dropped) {
    char bch_code[63];
    uint8_t bch_reliab[63];
    unsigned char parity;
    uint8_t parity_reliab;
    int observed_nac;
    uint8_t duid = P25P1_DUID_INVALID;

    /* The previous frame's NAC is not this one's: only this frame's NID, once decoded, names it. */
    state->p25_frame_nac_valid = 0U;
    p25p1_read_nid_fields(opts, state, &duid, bch_code, bch_reliab, &parity, &parity_reliab);
    /* An input interruption cut the NID (a Signal search left the frame): the rest of it is erasures the reader stood
       in, which the BCH can take for the all-zero codeword (NAC 0, an HDU). Nothing of it decodes, and the NAC the
       carrier's NIDs established says nothing of what follows the cut. */
    if (state->input_interrupted) {
        dsd_state_forget_p25p1_carrier_nac(state);
        return P25P1_DUID_INVALID;
    }

    observed_nac = p25p1_observed_nac(state);
    struct p25p1_nid_result decoded = p25p1_nid_decode(bch_code, bch_reliab, observed_nac, parity, parity_reliab);
    /* Read against the NAC the carrier's NIDs established before anything publishes it: a costly NAC change waits for
       the next NID. */
    const p25p1_nid_verdict verdict = p25p1_nid_verdict_classify(state, &decoded);
    *out_verdict = verdict;
    if (p25p1_tolerant_sync_dropped(opts, sync, &verdict)) {
        *out_dropped = 1;
        return P25P1_DUID_INVALID;
    }
    p25p1_nid_verdict_commit(state, &verdict);
    if (verdict.disposition == P25P1_NID_ACCEPTED) {
        p25p1_handle_nid_decode_success(opts, state, &duid, &decoded);
    } else if (verdict.disposition == P25P1_NID_PROVISIONAL) {
        p25p1_handle_nid_nac_held(opts, state, &duid, &verdict);
    } else {
        p25p1_handle_nid_decode_failure(opts, state, &duid, decoded.status);
    }
    return duid;
}

/* A P25 Phase 1 header, terminator or control frame ends the P25 capture segment on both slots: a Phase 2 slot 2
   capture left on the carrier belongs to traffic that is over (issue #661). Another protocol's capture is not P25's to
   close. */
static void
p25p1_close_mbe_out(dsd_opts* opts, dsd_state* state) {
    dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_BOTH, DSD_SYNC_P25P1_POS);
}

static void
p25p1_clear_pdu_summary_display(dsd_state* state) {
    /* P25p1 PDU summaries reuse the DMR LRRP display field in the shared UI. */
    state->dmr_lrrp_gps[0][0] = '\0';
}

static void
p25p1_handle_hdu(dsd_opts* opts, dsd_state* state) {
    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " HDU\n");
    }
    p25p1_close_mbe_out(opts, state);

    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    state->lastp25type = 2;
    state->dmrburstL = 25;
    state->currentslot = 0;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " HDU          ");
    processHDU(opts, state);
}

static void
p25p1_handle_ldu1(dsd_opts* opts, dsd_state* state) {
    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " LDU1  ");
    }
    state->lastp25type = 1;
    state->dmrburstL = 26;
    state->currentslot = 0;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " LDU1         ");
    state->numtdulc = 0;
    processLDU1(opts, state);
}

static void
p25p1_handle_ldu2(dsd_opts* opts, dsd_state* state) {
    state->dmrburstL = 27;
    state->currentslot = 0;

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        if (state->lastp25type != 1) {
            DSD_FPRINTF(stderr, " LDU2 (late entry)  ");
        } else {
            DSD_FPRINTF(stderr, " LDU2  ");
        }
    }
    state->lastp25type = 2;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " LDU2         ");
    state->numtdulc = 0;
    processLDU2(opts, state);
}

static void
p25p1_handle_tdulc(dsd_opts* opts, dsd_state* state) {
    state->dmrburstL = 28;

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " TDULC\n");
    }
    p25p1_close_mbe_out(opts, state);

    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    state->lastp25type = 0;
    state->err_str[0] = 0;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " TDULC        ");
    state->numtdulc++;

    if ((opts->resume > 0) && (state->numtdulc > opts->resume)) {
        resumeScan(opts, state);
    }
    processTDULC(opts, state);
    state->err_str[0] = 0;
}

static void
p25p1_handle_tdu(dsd_opts* opts, dsd_state* state) {
    state->dmrburstL = 28;

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " TDU\n");
    }
    p25p1_close_mbe_out(opts, state);

    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    state->lastp25type = 0;
    state->err_str[0] = 0;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " TDU          ");
    processTDU(opts, state);
}

/* 1 when a TSBK of the TSDU passed its CRC (processTSBK()). */
static int
p25p1_handle_tsbk(dsd_opts* opts, dsd_state* state) {
    state->dmrburstL = 29;

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " TSBK");
    }
    p25p1_close_mbe_out(opts, state);
    if (opts->resume > 0) {
        resumeScan(opts, state);
    }

    state->lastp25type = 3;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " TSBK         ");
    return processTSBK(opts, state);
}

static void
p25p1_handle_mpdu(dsd_opts* opts, dsd_state* state) {
    state->dmrburstL = 29;

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " MPDU\n");
    }
    p25p1_close_mbe_out(opts, state);
    if (opts->resume > 0) {
        resumeScan(opts, state);
    }

    state->lastp25type = 4;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " MPDU         ");
    processMPDU(opts, state);
}

static void
p25p1_handle_unknown_duid(dsd_opts* opts, dsd_state* state, uint8_t duid) {
    state->lastp25type = 0;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), "              ");

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        if (duid <= 0xFU) {
            DSD_FPRINTF(stderr, " duid:%u%u \n", (unsigned int)(duid >> 2), (unsigned int)(duid & 0x3U));
        } else {
            DSD_FPRINTF(stderr, " duid:EE \n");
        }
    }

    p25_status_accum_reset(state);
    p25_status_accum_classify(state);
}

/* Decode the data unit by its DUID: 1 when it was a TSDU with a TSBK that passed its CRC, the one P25p1 content that
   proves the carrier to the CQPSK timing loop (p25p1_note_frame_decoded()), else 0. */
static int DSD_ATTR_USED
p25p1_dispatch_by_duid(dsd_opts* opts, dsd_state* state, uint8_t duid) {
    p25p1_clear_pdu_summary_display(state);

    int tsbk_crc_passed = 0;
    switch (duid) {
        case P25P1_DUID_HDU: p25p1_handle_hdu(opts, state); break;
        case P25P1_DUID_LDU1: p25p1_handle_ldu1(opts, state); break;
        case P25P1_DUID_LDU2: p25p1_handle_ldu2(opts, state); break;
        case P25P1_DUID_TDULC: p25p1_handle_tdulc(opts, state); break;
        case P25P1_DUID_TDU: p25p1_handle_tdu(opts, state); break;
        case P25P1_DUID_TSBK: tsbk_crc_passed = p25p1_handle_tsbk(opts, state); break;
        case P25P1_DUID_MPDU: p25p1_handle_mpdu(opts, state); break;
        default: p25p1_handle_unknown_duid(opts, state, duid); break;
    }
    return tsbk_crc_passed;
}

int
dsd_dispatch_matches_p25p1(int synctype) {
    return DSD_SYNC_IS_P25P1(synctype);
}

/* Each data unit's length on air, the rows the frame sync's anchors read where the next unit's sync starts from
   (dsd_frame_sync_p25p1_frame_decoded()): a fixed length, or 0 for one its decoder reads (dsd_state::p25_p1_unit_dibits:
   a TSDU's from a last block that passed its CRC, none when the trailing-null rule or a failed block ended it; a PDU's
   from a header that passed its CRC). */
static const struct {
    uint8_t duid;
    uint16_t dibits;
} k_p25p1_unit_lengths[] = {
    {P25P1_DUID_HDU, P25P1_HDU_DIBITS},
    {P25P1_DUID_LDU1, P25P1_LDU_DIBITS},
    {P25P1_DUID_LDU2, P25P1_LDU_DIBITS},
    {P25P1_DUID_TDU, P25P1_TDU_DIBITS},
    {P25P1_DUID_TDULC, P25P1_TDULC_DIBITS},
    {P25P1_DUID_TSBK, 0U},
    {P25P1_DUID_MPDU, 0U},
};

/* The length on air of the data unit just decoded, by its DUID, or as its decoder read it (a TSDU's and a PDU's block
   counts); 0 when not known. */
static uint32_t
p25p1_decoded_unit_dibits(const dsd_state* state, uint8_t duid) {
    for (size_t i = 0; i < sizeof(k_p25p1_unit_lengths) / sizeof(k_p25p1_unit_lengths[0]); i++) {
        if (k_p25p1_unit_lengths[i].duid == duid) {
            return k_p25p1_unit_lengths[i].dibits != 0U ? k_p25p1_unit_lengths[i].dibits : state->p25_p1_unit_dibits;
        }
    }
    return 0U;
}

/* Tell the frame sync what the frame was: one that validated (p25p1_nid_verdict_validates()) and was read whole
   anchors the next data unit's sync where it ends (dsd_p25p1_sync_anchor) and, on the C4FM chain, has the symbol
   grid record that sync word to place itself on. When it was also a TSDU with a TSBK that passed its CRC
   (@p tsbk_crc_passed), it proves the carrier P25 to the CQPSK timing loop for @p tune_generation, the tune it was
   synced on (dsd_rtl_stream_metrics_hook_note_p25_proven()), while the stream is still the one its sync was found
   on. Voice and data frames never do: on a simulcast voice dwell the loop's slower post-lock gain lags the timing
   swing of the sites' beat, and a dwell is too short for the symbol slips it saves on a control channel to pay for
   that (docs/code_map.md, the Gardner gain rule). */
static void
p25p1_note_frame_decoded(dsd_state* state, uint8_t duid, const p25p1_nid_verdict* verdict, uint64_t tune_generation,
                         int tsbk_crc_passed) {
    /* A frame an input interruption cut was not read whole; a carrier boundary inside it fails the frame sync's own
       context check. */
    const int validated =
        duid != P25P1_DUID_INVALID && p25p1_nid_verdict_validates(verdict) && !state->input_interrupted;
    /* A carrier boundary inside the TSDU (a replay read adopting a recorded retune) moves no tuning generation, so a
       proof given for a block read before it would look current on the carrier reached; nor does a flush. The proof
       stands only on the stream the frame's sync was accepted on, read before the report spends that context. */
    const int stream_held = dsd_frame_sync_p25p1_frame_context_holds(state);
    (void)dsd_frame_sync_p25p1_frame_decoded(state, verdict->nac,
                                             validated ? p25p1_decoded_unit_dibits(state, duid) : 0U);
    if (validated && tsbk_crc_passed && stream_held) {
        dsd_rtl_stream_metrics_hook_note_p25_proven(tune_generation);
    }
}

dsd_frame_verdict
dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state) {
    /* The tune the frame was synced on: its handler may tune away (a grant) before the frame is reported. */
    const uint64_t tune_generation = dsd_trunk_tuning_generation();
    p25_status_accum_reset(state);
    p25p1_sync_acceptance sync;
    sync.nac = dsd_frame_sync_p25p1_tolerant_nac(state, &sync.bit_errors);
    p25p1_nid_verdict verdict;
    DSD_MEMSET(&verdict, 0, sizeof(verdict));
    int dropped = 0;
    uint8_t duid = p25p1_decode_nid_and_duid(opts, state, &sync, &verdict, &dropped);
    /* A NID an input interruption cut decoded nothing: no data unit follows it, and it is no evidence about the
       demodulator or the profile (processFrame() withholds the verdict). */
    if (state->input_interrupted) {
        (void)dsd_frame_sync_p25p1_frame_decoded(state, -1, 0U);
        return DSD_FRAME_VERDICT_WITHHELD;
    }
    /* A tolerant sync whose NID failed the strict check: no frame, so no evidence about the demodulator or the profile
       either, not even inside a window of NIDs that decoded, and the hunt resumes past the NID it read. */
    if (dropped) {
        (void)dsd_frame_sync_p25p1_frame_decoded(state, -1, 0U);
        return DSD_FRAME_VERDICT_UNPRODUCTIVE;
    }
    /* A decoded NID is the one place that knows which demodulator actually carried a P25p1
     * frame: the sync pattern is the same on C4FM and CQPSK, so nothing earlier can tell them
     * apart. Recording it here is what lets the trunking retunes and the SPS hunt restore a
     * modulation the signal itself has confirmed, and what lets the frame sync stop the
     * modulation heuristics from second-guessing a demodulator that is visibly working. */
    if (duid != P25P1_DUID_INVALID && !opts->mod_cli_lock && (state->rf_mod == 0 || state->rf_mod == 1)) {
        state->p25_p1_validated_rf_mod = state->rf_mod;
        state->p25_p1_validated_symbolcnt = state->symbolcnt;
        dsd_frame_sync_p25p1_mod_compare_publish(state); /* for a tune the P25 watchdog makes */
    }
    /* The same decode, read for what it says about the symbol profile rather than the
     * demodulator: nothing but P25p1 on this profile produces a NID the BCH accepts. No gate
     * here -- a locked modulation and a demodulator the pair above will not speak for still
     * leave that true. */
    if (duid != P25P1_DUID_INVALID) {
        state->p25_p1_nid_evidence = 1;
        state->p25_p1_nid_evidence_symbolcnt = state->symbolcnt;
        /* An FDMA carrier: a trunked system's return from a Phase 2 voice channel to its Phase 1 control channel (or a
           Phase 1 voice channel) is no carrier boundary, so the decoded NID is what says the second slot is gone. */
        state->tdma_carrier = 0U;
    }
    state->p25_p1_unit_dibits = 0U;
    const int tsbk_crc_passed = p25p1_dispatch_by_duid(opts, state, duid);
    p25p1_note_frame_decoded(state, duid, &verdict, tune_generation, tsbk_crc_passed);
    /* The 63-bit BCH over the NID is the one verdict this path has that covers every DUID.
     * When it fails the DUID is invalid, p25p1_handle_unknown_duid() decodes nothing, and
     * the 33 dibits already read validated nothing. Past that the picture is mixed --
     * state->p25_p1_fec_ok counts TSBK and MPDU header successes only, and says nothing
     * about HDU, LDU1/2, TDU or TDULC -- so a decoded NID is taken at its word.
     *
     * A decoded NID proves the profile rather than merely paying for the frame: a one-block
     * TSDU reads 134 symbols of its ~180-symbol slot, so consumption credit alone leaves a
     * control channel losing ground on every frame it decodes (#400).
     *
     * A failure inside the window of one that decoded reports the same. The NID is the first
     * thing after the sync and the smallest thing this path reads, so it fails first and
     * fails often on a signal the modulation votes are flapping under -- but a P25p1 sync
     * arriving where P25p1 was decoding a moment ago is evidence about the profile whatever
     * the BCH made of those 63 bits. Outside the window it goes back to speaking only for
     * itself, and only decoded NIDs ever move the window, so this cannot ratchet: a channel
     * that stops decoding stops being vouched for. */
    if (duid != P25P1_DUID_INVALID) {
        return DSD_FRAME_VERDICT_PROFILE_PROVEN;
    }
    if (state->p25_p1_nid_evidence != 0
        && (uint32_t)(state->symbolcnt - state->p25_p1_nid_evidence_symbolcnt) < P25P1_NID_EVIDENCE_WINDOW_SYMBOLS) {
        return DSD_FRAME_VERDICT_PROFILE_PROVEN;
    }
    return DSD_FRAME_VERDICT_UNPRODUCTIVE;
}
