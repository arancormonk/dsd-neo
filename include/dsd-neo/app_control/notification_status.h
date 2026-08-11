// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Compact decoder status for surfaces outside the single-consumer snapshot.
 *
 * The snapshot accessors in @ref snapshot.h are single-consumer: exactly one thread may
 * call them, and on Android that thread is Qt's. The foreground service needs the same
 * facts with no Qt in the picture -- Android can destroy the Activity while the engine
 * keeps decoding, which is precisely when the notification is the only visible surface.
 *
 * So this is a second, far smaller publication: a fixed-size POD copied out under a short
 * mutex, safe to read from any thread and any number of them. It is fed by the telemetry
 * hooks, which run on the decode thread for the whole session (see
 * dsd_app_frontend_runtime_start()), so it keeps updating with no frontend attached.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_NOTIFICATION_STATUS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_NOTIFICATION_STATUS_H_

#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** Longest label dsd_synctype_to_string() returns, plus room and a NUL. */
    DSD_APP_NOTIFICATION_PROTOCOL_SIZE = 24,
};

typedef struct {
    uint64_t revision; /**< Advances on every publish; 0 before the first. */
    /** dsd_synctype_to_string() for the current sync, or "" when NONE/UNKNOWN. */
    char protocol[DSD_APP_NOTIFICATION_PROTOCOL_SIZE];
    int64_t vc_freq_hz;     /**< Voice channel; 0 when none resolves. */
    int64_t cc_freq_hz;     /**< Control channel; 0 when none is known. */
    int64_t center_freq_hz; /**< Tuner centre; 0 unless the input is a radio. */
    uint8_t radio_input;    /**< Non-zero for RTL-family input only. */
    uint8_t trunking;       /**< opts->trunk_enable. */
    uint8_t trunk_tuned;    /**< opts->trunk_is_tuned. */
    dsd_app_slot_call slots[DSD_CALL_STATE_SLOT_COUNT];
} dsd_app_notification_status;

/** @brief Publish the state-derived half. Called on the decode thread. */
void dsd_app_notification_publish_state(const dsd_state* state);

/** @brief Publish the options-derived half. Called on the decode thread. */
void dsd_app_notification_publish_opts(const dsd_opts* opts);

/**
 * @brief Copy the latest status out.
 *
 * Safe from any thread, and from any number of them concurrently -- unlike the snapshot
 * accessors. Zeroes @p out when nothing has been published yet.
 *
 * @return 1 when @p out holds a published status, 0 otherwise.
 */
int dsd_app_notification_get(dsd_app_notification_status* out);

/** @brief The monotonic clock the publisher stamps call ages from; for tests. */
double dsd_app_notification_test_now_m(void);

enum {
    /** Comfortably larger than any record the encoder produces. */
    DSD_APP_NOTIFICATION_RECORD_SIZE = 1024,
};

/**
 * @brief Encode the latest status as one versioned, tab-separated record.
 *
 * One record per call so a reader cannot tear its view across fields. Text fields have
 * control characters replaced with spaces: they arrive from CSV imports and off the air,
 * and an embedded tab would invent a field and desynchronise everything after it.
 *
 * @return Characters written excluding the NUL, or 0 when nothing is published, @p out is
 *         NULL, or @p out_size cannot hold the whole record. Never truncates -- a partial
 *         record is one a reader would happily parse as a shorter one.
 */
size_t dsd_app_notification_encode(char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_NOTIFICATION_STATUS_H_ */
