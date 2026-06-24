// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#ifdef DSD_NEO_TEST_HOOKS
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void nxdn_frame(dsd_opts* opts, dsd_state* state);

#ifdef DSD_NEO_TEST_HOOKS
int dsd_neo_nxdn_test_route_decoded_lich(dsd_opts* opts, dsd_state* state, uint8_t lich, const uint8_t bits[364],
                                         const uint8_t reliab[364]);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_ */
