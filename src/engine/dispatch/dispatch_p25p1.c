// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/runtime/colors.h>
#include <mbelib-neo/mbelib.h>
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

static int
p25p1_valid_decoded_nac(int nac) {
    return nac != 0 && nac != 0xFFF;
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
    int valid_nac = p25p1_valid_decoded_nac(new_nac);

    if (new_nac != state->nac) {
        if (valid_nac) {
            state->nac = new_nac;
        }
        if (state->p2_hardset == 0 && valid_nac) {
            state->p2_cc = (unsigned long long)new_nac;
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

    p25p1_apply_nac_update(state, decoded->nac);
    p25p1_apply_duid_update(state, duid, decoded->duid);
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

/*
 * Mark the static helper roots used by the public dispatch entrypoint. CodeQL's
 * manual C/C++ database can miss this local call chain and report all children.
 */
static uint8_t DSD_ATTR_USED
p25p1_decode_nid_and_duid(dsd_opts* opts, dsd_state* state) {
    char bch_code[63];
    uint8_t bch_reliab[63];
    unsigned char parity;
    uint8_t parity_reliab;
    int observed_nac;
    uint8_t duid = P25P1_DUID_INVALID;

    p25p1_read_nid_fields(opts, state, &duid, bch_code, bch_reliab, &parity, &parity_reliab);

    observed_nac = p25p1_observed_nac(state);
    struct p25p1_nid_result decoded = p25p1_nid_decode(bch_code, bch_reliab, observed_nac, parity, parity_reliab);
    if (decoded.status > 0) {
        p25p1_handle_nid_decode_success(opts, state, &duid, &decoded);
    } else {
        p25p1_handle_nid_decode_failure(opts, state, &duid, decoded.status);
    }
    return duid;
}

static void
p25p1_close_mbe_out_if_open(dsd_opts* opts, dsd_state* state) {
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
}

static void
p25p1_close_mbe_out_pair_if_open(dsd_opts* opts, dsd_state* state) {
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }
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
    if (opts->mbe_out_dir[0] != 0) {
        p25p1_close_mbe_out_if_open(opts, state);
    }

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
    if (opts->mbe_out_dir[0] != 0) {
        p25p1_close_mbe_out_if_open(opts, state);
    }

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
    if (opts->mbe_out_dir[0] != 0) {
        p25p1_close_mbe_out_if_open(opts, state);
    }

    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    state->lasttg = 0;
    state->lastsrc = 0;
    state->lastp25type = 0;
    state->err_str[0] = 0;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " TDU          ");
    processTDU(opts, state);
}

static void
p25p1_handle_tsbk(dsd_opts* opts, dsd_state* state) {
    state->dmrburstL = 29;

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " TSBK");
    }
    if (opts->mbe_out_dir[0] != 0) {
        p25p1_close_mbe_out_pair_if_open(opts, state);
    }
    if (opts->resume > 0) {
        resumeScan(opts, state);
    }

    state->lasttg = 0;
    state->lastsrc = 0;
    state->lastp25type = 3;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " TSBK         ");
    processTSBK(opts, state);
}

static void
p25p1_handle_mpdu(dsd_opts* opts, dsd_state* state) {
    state->dmrburstL = 29;

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " MPDU\n");
    }
    if (opts->mbe_out_dir[0] != 0) {
        p25p1_close_mbe_out_pair_if_open(opts, state);
    }
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

static void DSD_ATTR_USED
p25p1_dispatch_by_duid(dsd_opts* opts, dsd_state* state, uint8_t duid) {
    p25p1_clear_pdu_summary_display(state);

    switch (duid) {
        case P25P1_DUID_HDU: p25p1_handle_hdu(opts, state); break;
        case P25P1_DUID_LDU1: p25p1_handle_ldu1(opts, state); break;
        case P25P1_DUID_LDU2: p25p1_handle_ldu2(opts, state); break;
        case P25P1_DUID_TDULC: p25p1_handle_tdulc(opts, state); break;
        case P25P1_DUID_TDU: p25p1_handle_tdu(opts, state); break;
        case P25P1_DUID_TSBK: p25p1_handle_tsbk(opts, state); break;
        case P25P1_DUID_MPDU: p25p1_handle_mpdu(opts, state); break;
        default: p25p1_handle_unknown_duid(opts, state, duid); break;
    }
}

int
dsd_dispatch_matches_p25p1(int synctype) {
    return DSD_SYNC_IS_P25P1(synctype);
}

void
dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state) {
    p25_status_accum_reset(state);
    uint8_t duid = p25p1_decode_nid_and_duid(opts, state);
    p25p1_dispatch_by_duid(opts, state, duid);
}
