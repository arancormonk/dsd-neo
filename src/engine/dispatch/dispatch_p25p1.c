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
#include <mbelib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

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

static void
p25p1_read_duid_bits(dsd_opts* opts, dsd_state* state, char duid[3], char bch_code[63], uint8_t bch_reliab[63],
                     int* index_bch_code) {
    int i;
    dsd_dibit_soft_t soft;

    for (i = 0; i < 2; i++) {
        int dibit = getDibitSoft(opts, state, &soft);
        duid[i] = (char)(dibit + '0');
        p25p1_append_bch_bits(bch_code, bch_reliab, index_bch_code, dibit, &soft);
    }
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
p25p1_read_nid_fields(dsd_opts* opts, dsd_state* state, char duid[3], char bch_code[63], uint8_t bch_reliab[63],
                      unsigned char* parity, uint8_t* parity_reliab) {
    int index_bch_code = 0;
    int dibit;
    uint8_t rel;
    dsd_dibit_soft_t soft;

    p25p1_read_nac_bits(opts, state, bch_code, bch_reliab, &index_bch_code);
    p25p1_read_duid_bits(opts, state, duid, bch_code, bch_reliab, &index_bch_code);
    p25p1_read_bch_soft_dibits(opts, state, bch_code, bch_reliab, &index_bch_code, 3);

    dibit = getDibitWithReliability(opts, state, &rel);
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
p25p1_apply_duid_update(dsd_state* state, char duid[3], const char new_duid[3]) {
    if (strcmp(new_duid, duid) != 0) {
        duid[0] = new_duid[0];
        duid[1] = new_duid[1];
        state->debug_header_errors++;
    }
}

static void
p25p1_handle_nid_decode_success(const dsd_opts* opts, dsd_state* state, char duid[3], int check_result, int error_count,
                                int new_nac, const char new_duid[3]) {
    if (error_count > 0) {
        state->nid_corrections_total += (unsigned int)error_count;
    }
    if (check_result == NID_PARITY_OVERRIDE) {
        state->nid_parity_overrides++;
        if (opts->verbose > 1) {
            DSD_FPRINTF(stderr, " [NID parity override, %d corrections]", error_count);
        }
    }

    p25p1_apply_nac_update(state, new_nac);
    p25p1_apply_duid_update(state, duid, new_duid);
}

static void
p25p1_handle_nid_decode_failure(const dsd_opts* opts, dsd_state* state, char duid[3], int check_result) {
    state->nid_failures_total++;

    if (check_result == NID_PARITY_MISMATCH && opts->verbose > 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " NID PARITY MISMATCH ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    duid[0] = 'E';
    duid[1] = 'E';
    state->debug_header_critical_errors++;
}

/*
 * Mark the static helper roots used by the public dispatch entrypoint. CodeQL's
 * manual C/C++ database can miss this local call chain and report all children.
 */
static void DSD_ATTR_USED
p25p1_decode_nid_and_duid(dsd_opts* opts, dsd_state* state, char duid[3]) {
    char bch_code[63];
    uint8_t bch_reliab[63];
    unsigned char parity;
    uint8_t parity_reliab;
    int new_nac;
    char new_duid[3];
    int check_result;
    int error_count = 0;
    int observed_nac;

    duid[2] = 0;
    p25p1_read_nid_fields(opts, state, duid, bch_code, bch_reliab, &parity, &parity_reliab);

    observed_nac = p25p1_observed_nac(state);
    check_result = check_NID_with_observed_nac_soft(bch_code, bch_reliab, observed_nac, &new_nac, new_duid, parity,
                                                    parity_reliab, &error_count);
    if (check_result > 0) {
        p25p1_handle_nid_decode_success(opts, state, duid, check_result, error_count, new_nac, new_duid);
    } else {
        p25p1_handle_nid_decode_failure(opts, state, duid, check_result);
    }
}

static void
p25p1_open_mbe_out_if_needed(dsd_opts* opts, dsd_state* state) {
    if (opts->mbe_out_f == NULL) {
        openMbeOutFile(opts, state);
    }
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
        p25p1_open_mbe_out_if_needed(opts, state);
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
    if (opts->mbe_out_dir[0] != 0) {
        p25p1_open_mbe_out_if_needed(opts, state);
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
    if (opts->mbe_out_dir[0] != 0) {
        p25p1_open_mbe_out_if_needed(opts, state);
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
p25p1_handle_unknown_duid(dsd_opts* opts, dsd_state* state, const char duid[3]) {
    state->lastp25type = 0;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), "              ");

    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
        DSD_FPRINTF(stderr, " duid:%s \n", duid);
    }

    p25_status_accum_reset(state);
    p25_status_accum_classify(state, opts);
}

static void DSD_ATTR_USED
p25p1_dispatch_by_duid(dsd_opts* opts, dsd_state* state, const char duid[3]) {
    p25p1_clear_pdu_summary_display(state);

    if (strcmp(duid, "00") == 0) {
        p25p1_handle_hdu(opts, state);
    } else if (strcmp(duid, "11") == 0) {
        p25p1_handle_ldu1(opts, state);
    } else if (strcmp(duid, "22") == 0) {
        p25p1_handle_ldu2(opts, state);
    } else if (strcmp(duid, "33") == 0) {
        p25p1_handle_tdulc(opts, state);
    } else if (strcmp(duid, "03") == 0) {
        p25p1_handle_tdu(opts, state);
    } else if (strcmp(duid, "13") == 0) {
        p25p1_handle_tsbk(opts, state);
    } else if (strcmp(duid, "30") == 0) {
        p25p1_handle_mpdu(opts, state);
    } else {
        p25p1_handle_unknown_duid(opts, state, duid);
    }
}

int
dsd_dispatch_matches_p25p1(int synctype) {
    return DSD_SYNC_IS_P25P1(synctype);
}

void
dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state) {
    char duid[3];

    p25_status_accum_reset(state);
    p25p1_decode_nid_and_duid(opts, state, duid);
    p25p1_dispatch_by_duid(opts, state, duid);
}
