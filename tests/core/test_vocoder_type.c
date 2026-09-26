// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for dsd_vocoder_from_synctype().
 *
 * Each test verifies that the canonical protocol → vocoder mapping is
 * correct.  The return values must be stable because callers depend on them
 * to select codec libraries at runtime.
 */

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>

/* Return non-zero (test name encoded as exit code) on failure. */

static int
test_p25p1(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_P25P1_POS) != DSD_VOCODER_IMBE_7200) return 1;
    if (dsd_vocoder_from_synctype(DSD_SYNC_P25P1_NEG) != DSD_VOCODER_IMBE_7200) return 2;
    return 0;
}

static int
test_provoice(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_PROVOICE_POS) != DSD_VOCODER_IMBE_7100) return 10;
    if (dsd_vocoder_from_synctype(DSD_SYNC_PROVOICE_NEG) != DSD_VOCODER_IMBE_7100) return 11;
    return 0;
}

static int
test_dstar_voice(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_DSTAR_VOICE_POS) != DSD_VOCODER_AMBE_3600) return 20;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DSTAR_VOICE_NEG) != DSD_VOCODER_AMBE_3600) return 21;
    /* D-STAR header frames carry no voice — expect NONE */
    if (dsd_vocoder_from_synctype(DSD_SYNC_DSTAR_HD_POS) != DSD_VOCODER_NONE) return 22;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DSTAR_HD_NEG) != DSD_VOCODER_NONE) return 23;
    return 0;
}

static int
test_dmr(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_DMR_BS_VOICE_POS) != DSD_VOCODER_AMBE2_EHR) return 30;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DMR_BS_VOICE_NEG) != DSD_VOCODER_AMBE2_EHR) return 31;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DMR_BS_DATA_POS)  != DSD_VOCODER_AMBE2_EHR) return 32;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DMR_BS_DATA_NEG)  != DSD_VOCODER_AMBE2_EHR) return 33;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DMR_MS_VOICE)     != DSD_VOCODER_AMBE2_EHR) return 34;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DMR_MS_DATA)      != DSD_VOCODER_AMBE2_EHR) return 35;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DMR_RC_DATA)      != DSD_VOCODER_AMBE2_EHR) return 36;
    return 0;
}

static int
test_p25p2(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_P25P2_POS) != DSD_VOCODER_AMBE2_EHR) return 40;
    if (dsd_vocoder_from_synctype(DSD_SYNC_P25P2_NEG) != DSD_VOCODER_AMBE2_EHR) return 41;
    return 0;
}

static int
test_nxdn(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_NXDN_POS) != DSD_VOCODER_AMBE2_EHR) return 50;
    if (dsd_vocoder_from_synctype(DSD_SYNC_NXDN_NEG) != DSD_VOCODER_AMBE2_EHR) return 51;
    return 0;
}

static int
test_ysf(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_YSF_POS) != DSD_VOCODER_AMBE2_EHR) return 60;
    if (dsd_vocoder_from_synctype(DSD_SYNC_YSF_NEG) != DSD_VOCODER_AMBE2_EHR) return 61;
    return 0;
}

static int
test_x2tdma(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_X2TDMA_VOICE_POS) != DSD_VOCODER_AMBE2_EHR) return 70;
    if (dsd_vocoder_from_synctype(DSD_SYNC_X2TDMA_VOICE_NEG) != DSD_VOCODER_AMBE2_EHR) return 71;
    if (dsd_vocoder_from_synctype(DSD_SYNC_X2TDMA_DATA_POS)  != DSD_VOCODER_AMBE2_EHR) return 72;
    if (dsd_vocoder_from_synctype(DSD_SYNC_X2TDMA_DATA_NEG)  != DSD_VOCODER_AMBE2_EHR) return 73;
    return 0;
}

static int
test_dpmr(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_DPMR_FS1_POS) != DSD_VOCODER_AMBE2_EHR) return 80;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DPMR_FS4_NEG) != DSD_VOCODER_AMBE2_EHR) return 81;
    return 0;
}

static int
test_m17(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_M17_STR_POS)  != DSD_VOCODER_CODEC2) return 90;
    if (dsd_vocoder_from_synctype(DSD_SYNC_M17_STR_NEG)  != DSD_VOCODER_CODEC2) return 91;
    if (dsd_vocoder_from_synctype(DSD_SYNC_M17_LSF_POS)  != DSD_VOCODER_CODEC2) return 92;
    if (dsd_vocoder_from_synctype(DSD_SYNC_M17_LSF_NEG)  != DSD_VOCODER_CODEC2) return 93;
    if (dsd_vocoder_from_synctype(DSD_SYNC_M17_PKT_POS)  != DSD_VOCODER_CODEC2) return 94;
    if (dsd_vocoder_from_synctype(DSD_SYNC_M17_PKT_NEG)  != DSD_VOCODER_CODEC2) return 95;
    if (dsd_vocoder_from_synctype(DSD_SYNC_M17_PRE_POS)  != DSD_VOCODER_CODEC2) return 96;
    if (dsd_vocoder_from_synctype(DSD_SYNC_M17_PRE_NEG)  != DSD_VOCODER_CODEC2) return 97;
    return 0;
}

static int
test_tetra(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_TETRA_NDB_POS) != DSD_VOCODER_ACELP) return 100;
    if (dsd_vocoder_from_synctype(DSD_SYNC_TETRA_NDB_NEG) != DSD_VOCODER_ACELP) return 101;
    /* TETRA Sync Burst carries no TCH — expect NONE */
    if (dsd_vocoder_from_synctype(DSD_SYNC_TETRA_SB_POS)  != DSD_VOCODER_NONE)  return 102;
    if (dsd_vocoder_from_synctype(DSD_SYNC_TETRA_SB_NEG)  != DSD_VOCODER_NONE)  return 103;
    return 0;
}

static int
test_none(void) {
    if (dsd_vocoder_from_synctype(DSD_SYNC_NONE)    != DSD_VOCODER_NONE) return 110;
    if (dsd_vocoder_from_synctype(DSD_SYNC_ANALOG)  != DSD_VOCODER_NONE) return 111;
    if (dsd_vocoder_from_synctype(DSD_SYNC_DIGITAL) != DSD_VOCODER_NONE) return 112;
    /* Completely unknown values must also return NONE */
    if (dsd_vocoder_from_synctype(999)              != DSD_VOCODER_NONE) return 113;
    return 0;
}

int
main(void) {
    int r;

    r = test_p25p1();     if (r) return r;
    r = test_provoice();  if (r) return r;
    r = test_dstar_voice(); if (r) return r;
    r = test_dmr();       if (r) return r;
    r = test_p25p2();     if (r) return r;
    r = test_nxdn();      if (r) return r;
    r = test_ysf();       if (r) return r;
    r = test_x2tdma();    if (r) return r;
    r = test_dpmr();      if (r) return r;
    r = test_m17();       if (r) return r;
    r = test_tetra();     if (r) return r;
    r = test_none();      if (r) return r;

    return 0;
}
