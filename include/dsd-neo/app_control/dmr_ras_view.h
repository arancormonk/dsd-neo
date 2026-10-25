// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral DMR RAS indicator: whether the carrier uses RAS (Restricted Access to System), and whether
 * relaxed CRC (-F) is in force to decode what it sends.
 *
 * A RAS system computes its control and data checks over a secret as well, so its bursts fail the standard CRC. The
 * DMR decoder confirms RAS on a carrier from two signature bursts (dmr_ras.c) and the received identity record shows
 * it (DSD_RXID_DMR_RAS); relaxed CRC accepts such bursts and marks them [RAS]. This view joins the two, from the
 * record and the Relaxed CRC policy (crc_policy_view.h), so the terminal header and the other frontends say the same.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_DMR_RAS_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_DMR_RAS_VIEW_H_

#include <dsd-neo/app_control/crc_policy_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The DMR RAS indicator as every frontend shows it. */
typedef struct {
    uint8_t detected;      /**< 1 = RAS is confirmed on the carrier being decoded (a DMR record's DSD_RXID_DMR_RAS). */
    uint8_t needs_relaxed; /**< 1 = detected while relaxed CRC is not in force: -F (Relaxed CRC) decodes its content. */
    dsd_app_crc_policy crc; /**< The Relaxed CRC policy (configured, in force, a scan row's override). */
} dsd_app_dmr_ras;

/**
 * @brief Fill @p out from decoder state or a frontend snapshot pair.
 *
 * Zeroes @p out first; NULL @p state detects nothing (the policy is still read from @p opts).
 */
void dsd_app_dmr_ras_view(const dsd_opts* opts, const dsd_state* state, dsd_app_dmr_ras* out);

/**
 * @brief The compact label: "RAS" while relaxed CRC is in force, "RAS (-F decodes)" while it is not, except
 * "RAS (channel sets strict CRC)" while -F is configured on and the scan row on air keeps strict CRC itself; "" when
 * RAS is not detected (or @p view is NULL).
 */
const char* dsd_app_dmr_ras_label(const dsd_app_dmr_ras* view);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_DMR_RAS_VIEW_H_ */
