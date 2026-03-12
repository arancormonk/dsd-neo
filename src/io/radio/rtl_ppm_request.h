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

struct RtlPpmApplyPlan {
    int reconfigure = 0;
    uint32_t freq_hz = 0;
};

/* Reconfigure/reset the active stream only after the backend has accepted the
 * new PPM value. Rejected live PPM requests must leave the current demod
 * chain running undisturbed. */
static inline bool
rtl_ppm_should_reconfigure_after_apply(const RtlPpmApplyPlan& plan, int ppm_apply_rc) {
    return ppm_apply_rc == 0 && plan.reconfigure;
}

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

/* A live PPM correction shifts the RF center without stopping sample flow. If
 * the controller knows the active tuned frequency, force the update through
 * the full reconfigure/reset path so CQPSK/FLL/TED state is realigned before
 * the demod thread consumes post-correction samples. */
static inline RtlPpmApplyPlan
rtl_ppm_plan_apply_to_active_stream(uint32_t last_applied_freq_hz, uint32_t fallback_freq_hz, int applied_ppm,
                                    int requested_ppm) {
    if (requested_ppm == applied_ppm) {
        return {};
    }
    uint32_t active_freq_hz = last_applied_freq_hz ? last_applied_freq_hz : fallback_freq_hz;
    if (active_freq_hz != 0) {
        return {1, active_freq_hz};
    }
    return {};
}

} // namespace radio
} // namespace io
} // namespace dsd
