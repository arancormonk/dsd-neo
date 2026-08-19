// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend boundary for applying a finished RadioReference import.
 *
 * Carries the by-value payload the app-command queue copies to the decoder
 * thread, and the pure plan -> payload mapper both frontends share. Including
 * <dsd-neo/runtime/radioreference_import.h> here is legal: the ARCH_RULES
 * public-frontend rule bans only core/opts.h, core/state.h, dsd-neo/io/,
 * dsd-neo/protocol/, curses and Qt from include/dsd-neo/app_control/.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RR_IMPORT_APPLY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RR_IMPORT_APPLY_H_

#include <dsd-neo/runtime/radioreference_import.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Everything DSD_APP_CMD_RR_APPLY_IMPORT needs, by value. */
typedef struct {
    int32_t decode_mode;           /**< dsdneoUserDecodeMode as int32. */
    uint8_t edacs_ea;              /**< state->ea_mode. */
    uint8_t edacs_esk;             /**< state->esk_mask = 0xA0 when set. */
    uint8_t simulcast_qpsk;        /**< force QPSK after the preset. */
    uint8_t p25_prefer_candidates; /**< opts->p25_prefer_candidates (-^). */
    uint8_t trunking;              /**< mutually exclusive with scanner. */
    uint8_t scanner;               /**< opts->scanner_mode (-Y). */
    uint8_t has_chan;              /**< chan_path is meaningful. */
    uint8_t has_group;             /**< group_path is meaningful. */
    uint32_t tune_hz;              /**< 0 = nothing to tune. */
    char chan_path[1024];
    char group_path[1024];
} dsd_app_rr_apply_payload;

/** @brief Account fields DSD_APP_CMD_RR_ACCOUNT_SET mirrors into dsd_opts. */
typedef struct {
    char username[128]; /**< matches dsd_rr_auth::username. */
    char app_key[64];   /**< matches dsd_rr_auth::app_key. */
} dsd_app_rr_account_payload;

/**
 * @brief Map a finished import plan onto the apply payload. Pure; no session.
 *
 * @param plan       Built by dsd_rr_import_plan_build(); must have plan->ok.
 * @param chan_path  Written channel-map path, or NULL/"" when none.
 * @param group_path Written group-list path, or NULL/"" when none.
 * @param out        Filled on success; zeroed first.
 * @return 0 on success, -1 when the plan is not ok, the protocol has no decode
 *         mode, or plan->tune_hz does not fit an unsigned 32-bit Hz value.
 */
int dsd_app_rr_fill_apply_payload(const dsd_rr_import_plan* plan, const char* chan_path, const char* group_path,
                                  dsd_app_rr_apply_payload* out);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RR_IMPORT_APPLY_H_ */
