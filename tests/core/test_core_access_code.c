// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The access code a call history row records (issue #575): which kind each protocol carries, and when the live
 * value the decoder holds is a code at all rather than a sentinel.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
    state->nxdn_last_ran = (unsigned int)-1;
    state->dpmr_color_code = -1;
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

static int
test_p25_phase2_reads_p2_cc_only(dsd_state* state) {
    int rc = 0;
    reset_sources(state);
    state->p2_cc = 0x293ULL;
    rc |= expect_code("P25p2 p2_cc 293", state, DSD_SYNC_P25P2_POS, 0U, 0U, 1, DSD_ACCESS_CODE_NAC, 0x293U);
    /* The Phase 1 nac is not a Phase 2 call's code. */
    state->p2_cc = 0ULL;
    state->nac = 0x293;
    rc |= expect_none("P25p2 p2_cc 0 ignores nac", state, DSD_SYNC_P25P2_NEG, 0U, 0U);
    state->p2_cc = 0xFFFULL;
    rc |= expect_none("P25p2 p2_cc FFF", state, DSD_SYNC_P25P2_POS, 0U, 0U);
    state->p2_cc = 0x1000ULL;
    rc |= expect_none("P25p2 p2_cc past twelve bits", state, DSD_SYNC_P25P2_POS, 0U, 0U);
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

/* Protocols with no access code report none even with every source holding a valid value. */
static int
test_codeless_protocols(dsd_state* state) {
    int rc = 0;
    state->dmr_color_code = 3U;
    state->nac = 0x293;
    state->p2_cc = 0x293ULL;
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
    rc |= test_p25_phase2_reads_p2_cc_only(state);
    rc |= test_nxdn_ran(state);
    rc |= test_dpmr_colour_code(state);
    rc |= test_m17_can(state);
    rc |= test_codeless_protocols(state);
    rc |= test_null_inputs();
    dsd_state_ext_free_all(state);
    free(state);
    if (rc == 0) {
        printf("CORE_ACCESS_CODE: OK\n");
    }
    return rc;
}
