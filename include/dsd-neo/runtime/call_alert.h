// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Call-alert event selection helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_CALL_ALERT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_CALL_ALERT_H_H

#include <dsd-neo/platform/platform.h>

#include <stdint.h>

typedef enum DSD_ATTR_PACKED {
    DSD_CALL_ALERT_EVENT_VOICE_START = 1u << 0,
    DSD_CALL_ALERT_EVENT_VOICE_END = 1u << 1,
    DSD_CALL_ALERT_EVENT_DATA = 1u << 2,
    DSD_CALL_ALERT_EVENT_ALL =
        DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_VOICE_END | DSD_CALL_ALERT_EVENT_DATA
} dsd_call_alert_event_t;

static inline uint8_t
dsd_call_alert_mask_events(uint8_t events) {
    return (uint8_t)(events & DSD_CALL_ALERT_EVENT_ALL);
}

/** Preserve the historical zero-mask representation for "all events." */
static inline uint8_t
dsd_call_alert_normalize_events(uint8_t events) {
    uint8_t normalized = dsd_call_alert_mask_events(events);
    return normalized ? normalized : (uint8_t)DSD_CALL_ALERT_EVENT_ALL;
}

static inline uint8_t
dsd_call_alert_effective_events(int enabled, uint8_t events) {
    if (!enabled) {
        return 0;
    }
    return dsd_call_alert_normalize_events(events);
}

static inline int
dsd_call_alert_event_enabled(int enabled, uint8_t configured_events, uint8_t event) {
    uint8_t events = dsd_call_alert_effective_events(enabled, configured_events);
    return (events & event) != 0;
}
#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_CALL_ALERT_H_H */
