// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief IO control and tuning helpers.
 *
 * These functions provide low-level tuning backends and radio control helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_CONTROL_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_IO_CONTROL_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tune the configured control backend without trunking bookkeeping, and say which RTL request is pending.
 *
 * Each RTL tune carries a manual-tune ID of its own (RTL_STREAM_TUNE_ID_MANUAL_BIT set, issue #671), under which the
 * controller reports the request's completion to the registered completion callback, the late one of a timed-out
 * request included. The ID owns no controller request: a later tune coalesces into a queued one as into an untagged
 * tune. Rigctl is synchronous and has no pending request.
 *
 * @param freq_hz Target in Hz, carried as the tuner path holds it (a 32-bit long cannot hold one past 2147.483647 MHz).
 * @param out_pending_id May be NULL. Set to the request's ID when an RTL tune timed out (RTL_STREAM_TUNE_TIMEOUT), its
 *                       completion still to come; 0 otherwise.
 * @return 0 on success, 1 when an RTL request is deferred, or a negative error/timeout code (-1 refused: no backend,
 *         no context or 0 Hz). An RTL timeout leaves an accepted request active and retains its target in
 *         opts->rtlsdr_center_freq.
 */
int io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t freq_hz, uint64_t* out_pending_id);

/**
 * @brief io_control_set_freq_tagged() for a caller that does not follow a pending request.
 */
static inline int
io_control_set_freq(dsd_opts* opts, const dsd_state* state, uint32_t freq_hz) {
    return io_control_set_freq_tagged(opts, state, freq_hz, NULL);
}

void resumeScan(dsd_opts* opts, dsd_state* state);
void openSerial(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_CONTROL_H_H */
