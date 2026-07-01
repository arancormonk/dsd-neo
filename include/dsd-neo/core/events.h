// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Core event logging helpers.
 *
 * Declares event-history helpers implemented in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_EVENTS_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_EVENTS_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void init_event_history(Event_History_I* event_struct, uint8_t start, uint8_t stop);
void push_event_history(Event_History_I* event_struct);

static inline void
dsd_event_history_item_set_metadata(Event_History* item, dsd_event_severity severity, dsd_event_category category) {
    if (item == NULL) {
        return;
    }
    item->severity = (uint8_t)severity;
    item->category = (uint8_t)category;
}

static inline void
dsd_event_history_item_copy_text(char* dst, size_t cap, const char* src) {
    if (dst == NULL || cap == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0U;
    while (i + 1U < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static inline void
dsd_event_history_item_set_message(Event_History* item, dsd_event_severity severity, dsd_event_category category,
                                   const char* summary, const char* detail) {
    dsd_event_history_item_set_metadata(item, severity, category);
    if (item == NULL) {
        return;
    }
    if (summary != NULL) {
        dsd_event_history_item_copy_text(item->event_string, sizeof item->event_string, summary);
    }
    if (detail != NULL) {
        dsd_event_history_item_copy_text(item->internal_str, sizeof item->internal_str, detail);
    }
}

void write_event_to_log_file(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite, char* event_string);
void watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot);
void watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot);
void watchdog_event_status(dsd_state* state, const char* status_string, uint8_t slot);
void watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string,
                             uint8_t slot);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_EVENTS_H_H */
