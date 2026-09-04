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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tune the configured control backend without trunking bookkeeping.
 * @return 0 on success, 1 when an RTL request is deferred, or a negative error/timeout code. An RTL timeout leaves an
 *         accepted request active and retains its target in opts->rtlsdr_center_freq.
 */
int io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq);
void resumeScan(dsd_opts* opts, dsd_state* state);
void openSerial(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_CONTROL_H_H */
