// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief M17 protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#ifdef DSD_NEO_TEST_HOOKS
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void processM17STR(dsd_opts* opts, dsd_state* state);
void processM17PKT(dsd_opts* opts, dsd_state* state);
void processM17LSF(dsd_opts* opts, dsd_state* state);
void processM17BRT(dsd_opts* opts, dsd_state* state);
void processM17IPF(dsd_opts* opts, dsd_state* state);
void encodeM17STR(dsd_opts* opts, dsd_state* state);
void encodeM17BRT(dsd_opts* opts, dsd_state* state);
void encodeM17PKT(dsd_opts* opts, dsd_state* state);

#ifdef DSD_NEO_TEST_HOOKS
struct m17_lsf_result;

enum dsd_neo_m17_test_stream_result {
    DSD_NEO_M17_TEST_STREAM_INVALID = -1,
    DSD_NEO_M17_TEST_STREAM_CAN_FILTERED = 0,
    DSD_NEO_M17_TEST_STREAM_CLEAR_DISPATCHED = 1,
    DSD_NEO_M17_TEST_STREAM_ENCRYPTED_LOCKED = 2,
    DSD_NEO_M17_TEST_STREAM_ENCRYPTED_DISPATCHED = 3,
    DSD_NEO_M17_TEST_STREAM_SIGNATURE_CONSUMED = 4,
};

int dsd_neo_m17_test_apply_lsf_result(dsd_state* state, const struct m17_lsf_result* res);
int dsd_neo_m17_test_dispatch_stream_payload(const dsd_opts* opts, dsd_state* state, const uint8_t payload[128],
                                             uint16_t frame_number, uint8_t processed_payload[128]);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H */
