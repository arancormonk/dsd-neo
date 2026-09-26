// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Mapping from sync-type IDs to `dsd_vocoder_type_t`.
 *
 * `dsd_vocoder_from_synctype()` is the single authoritative place where a
 * sync-type value is translated into the codec family required to decode the
 * accompanying voice frame.  All other dispatch paths in the vocoder HAL
 * (processMbeFrame / soft_mbe) follow the same precedence order.
 */

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>

dsd_vocoder_type_t
dsd_vocoder_from_synctype(int synctype) {
    /* P25 Phase 1 — IMBE 7200x4400 */
    if (DSD_SYNC_IS_P25P1(synctype)) {
        return DSD_VOCODER_IMBE_7200;
    }

    /* ProVoice / EDACS — IMBE 7100x4400 */
    if (DSD_SYNC_IS_PROVOICE(synctype)) {
        return DSD_VOCODER_IMBE_7100;
    }

    /* D-STAR voice — AMBE 3600x2400 */
    if (DSD_SYNC_IS_DSTAR_VOICE(synctype)) {
        return DSD_VOCODER_AMBE_3600;
    }

    /* M17 stream / LSF / packet — Codec2 */
    if (DSD_SYNC_IS_M17(synctype)) {
        return DSD_VOCODER_CODEC2;
    }

    /* TETRA Normal Downlink Burst — ACELP via subprocess */
    if (synctype == DSD_SYNC_TETRA_NDB_POS || synctype == DSD_SYNC_TETRA_NDB_NEG) {
        return DSD_VOCODER_ACELP;
    }

    /* DMR (BS + MS), P25 Phase 2, NXDN, YSF, X2-TDMA, dPMR — AMBE+2 EHR */
    if (DSD_SYNC_IS_DMR(synctype) || DSD_SYNC_IS_P25P2(synctype) || DSD_SYNC_IS_NXDN(synctype)
        || DSD_SYNC_IS_YSF(synctype) || DSD_SYNC_IS_X2TDMA(synctype) || DSD_SYNC_IS_DPMR(synctype)) {
        return DSD_VOCODER_AMBE2_EHR;
    }

    /* All other sync types are data, control, or unknown — no voice codec */
    return DSD_VOCODER_NONE;
}
