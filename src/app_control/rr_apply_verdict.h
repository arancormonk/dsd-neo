// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The session side of the RadioReference import verdict (issue #803), shared by the decoder thread's preflight
 *        (app_command_queue.c) and the frontends' pre-check (dsd_app_rr_apply_block_reason()).
 *
 * Module-private: the public rr_import_apply.h may not take dsd_opts or dsd_state (ARCH_RULES). Both run on whatever
 * options and state they are given, live or a published snapshot pair, and touch no stream, so the decoder and the
 * frontends cannot drift apart.
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_RR_APPLY_VERDICT_H_
#define DSD_NEO_SRC_APP_CONTROL_RR_APPLY_VERDICT_H_

#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill @p out from @p opts and the import @p p: the input as the stream start classifies it, the committed DSP
 *        bandwidth, --tetra-uplink, a running trunk scan, and the outputs the import leaves (@p has_group and
 *        @p has_chan, which a frontend takes from the plan before any file exists).
 *
 * @param actual_rate_hz The running stream's committed DSP rate, or 0 when unknown (no stream; always on a frontend).
 */
void rr_apply_receiver_facts_fill(const dsd_opts* opts, const dsd_app_rr_apply_payload* p, int has_group, int has_chan,
                                  uint32_t actual_rate_hz, dsd_app_rr_receiver_facts* out);

/**
 * @brief Whether the save the import would leave breaks a rule a trunk scan waiting for the next start needs (issue
 *        #671), through the same app_check_next_start() every other change asks: the tuner owner it sets and the
 *        channel map it loads or clears.
 *
 * @param chan_path The channel map the import loads; a frontend asking before the file exists may pass NULL or "",
 *                  which stands for "a channel map" when @p facts has_chan is set.
 * @param why       Receives the broken rule (with its advice after a ';'), or ""; may be NULL.
 * @return 0 when the next start stays startable, -1 otherwise (and when memory runs out).
 */
int rr_apply_next_start_refusal(const dsd_opts* opts, const dsd_state* state, const dsd_app_rr_receiver_facts* facts,
                                const char* chan_path, char* why, size_t why_size);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_SRC_APP_CONTROL_RR_APPLY_VERDICT_H_ */
