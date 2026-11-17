// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Plan helpers the replay planner (iq_replay.c) shares with the general rate plan (iq_rate_plan_general.c).
 */

#ifndef DSD_NEO_IO_IQ_IQ_RATE_PLAN_INTERNAL_H
#define DSD_NEO_IO_IQ_IQ_RATE_PLAN_INTERNAL_H

#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/rate_plan.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fill @p plan from scratch: the capture's rate, the DSP input rate, base and post, the target (0 when @p kept, the
 * recorded chain as captured), the kept and resample flags. Origin, chain, complex rate and forced are the caller's. */
void iq_rate_plan_fill(dsd_iq_replay_rate_plan* plan, uint32_t capture_hz, uint32_t dsp_in_hz, uint32_t base,
                       uint32_t post, uint32_t target_hz, int kept);

/* The rate planner's chain from @p nominal_hz over exactly @p passes to @p target_hz for @p backend, asked as
 * @p request_hz: a request other than the nominal rate plans as forced, which a REPLAY_KEEP plan steps. 1 when it plans
 * those passes. */
int iq_rate_plan_chain(dsd_rate_plan_backend backend, uint32_t request_hz, uint32_t nominal_hz, uint32_t readback_hz,
                       uint32_t target_hz, int passes, dsd_rate_plan* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_IO_IQ_IQ_RATE_PLAN_INTERNAL_H */
