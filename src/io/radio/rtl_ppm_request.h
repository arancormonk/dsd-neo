// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

namespace dsd {
namespace io {
namespace radio {

struct RtlPpmControllerRequestState {
    int pending = 0;
    int ppm = 0;
    uint32_t request_id = 0;
};

struct RtlPpmControllerRequestsSnapshot {
    RtlPpmControllerRequestState queued_request = {};
    RtlPpmControllerRequestState active_request = {};
};

struct RtlPpmRejectedRequestResolution {
    int requested_ppm = 0;
    uint32_t requested_request_id = 0;
    int rolled_back = 0;
};

static inline bool
rtl_ppm_controller_request_matches(const RtlPpmControllerRequestState& state, int requested_ppm,
                                   uint32_t requested_request_id) {
    return state.pending && state.ppm == requested_ppm && state.request_id == requested_request_id;
}

static inline bool
rtl_ppm_controller_request_has_ppm(const RtlPpmControllerRequestState& state, int requested_ppm) {
    return state.pending && state.ppm == requested_ppm;
}

/* Queue a new controller request only when the desired generation is not
 * already represented by the queued or active controller state. A stale queued
 * request must still be overwritten even if an active apply happens to match
 * the latest desired value. */
static inline bool
rtl_ppm_should_schedule_request(int applied_ppm, int requested_ppm, uint32_t requested_request_id,
                                const RtlPpmControllerRequestState& queued_request,
                                const RtlPpmControllerRequestState& active_request) {
    bool queued_matches = rtl_ppm_controller_request_matches(queued_request, requested_ppm, requested_request_id);
    if (queued_request.pending && !queued_matches) {
        return true;
    }
    if (requested_ppm == applied_ppm) {
        return false;
    }
    return !queued_matches && !rtl_ppm_controller_request_matches(active_request, requested_ppm, requested_request_id);
}

/* The controller thread can learn about a rejected PPM request after the read
 * thread or UI has already published newer desired values. Roll back only when
 * the caller still holds the same logical request generation, so same-value
 * retries are preserved instead of being mistaken for stale state. */
static inline RtlPpmRejectedRequestResolution
rtl_ppm_resolve_rejected_request(int applied_ppm, int requested_ppm, uint32_t requested_request_id,
                                 uint32_t rejected_request_id) {
    if (requested_request_id == rejected_request_id) {
        return {applied_ppm, requested_request_id, 1};
    }
    return {requested_ppm, requested_request_id, 0};
}

} // namespace radio
} // namespace io
} // namespace dsd
