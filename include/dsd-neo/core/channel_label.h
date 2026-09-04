// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Human-readable name for the channel currently being listened to.
 *
 * Resolves the one label a frontend should show while scanning: the active
 * --trunk-scan target, or the name of the -Y scan-list row the receiver is
 * parked on.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_CHANNEL_LABEL_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_CHANNEL_LABEL_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Which scanner is naming the channel currently tuned. */
typedef enum {
    DSD_CHANNEL_LABEL_SOURCE_NONE = 0,       /**< No scanner is naming a channel. */
    DSD_CHANNEL_LABEL_SOURCE_TRUNK_SCAN = 1, /**< The active `--trunk-scan` target's id. */
    DSD_CHANNEL_LABEL_SOURCE_SCAN_LIST = 2,  /**< The name of the `-Y` scan-list row on air. */
} dsd_channel_label_source;

/**
 * Say which scanner would name the channel currently tuned.
 *
 * Same precedence as dsd_channel_label_current(): the active `--trunk-scan` target
 * outranks the `-Y` scan list. A frontend uses the answer to word the label -- a
 * trunk-scan target is a whole system and reads as "Target", a scan-list row as
 * "Channel" -- and to keep the two from naming the same screen differently.
 *
 * @return DSD_CHANNEL_LABEL_SOURCE_NONE when dsd_channel_label_current() would
 *         report no label, else the scanner whose name it reports.
 */
dsd_channel_label_source dsd_channel_label_current_source(const dsd_opts* opts, const dsd_state* state);

/**
 * Resolve the label for the channel currently tuned.
 *
 * Trunk scan wins over the conventional scanner: with `--trunk-scan` enabled the
 * label is the active target's id, and only when no target is selected does a
 * `-Y` scan list get a say, contributing the name of the row it is parked on.
 * An unnamed row, an unnamed target and an idle receiver all report no label.
 *
 * @param opts   Decoder options; NULL reports no label.
 * @param state  Decoder state; NULL reports no label.
 * @param out    Destination buffer, cleared on every call and truncated to fit.
 *               May be NULL when the caller only wants the yes/no answer.
 * @param out_sz Capacity of @p out; 0 leaves @p out untouched.
 * @return 1 when a label was resolved, 0 when there is none.
 */
int dsd_channel_label_current(const dsd_opts* opts, const dsd_state* state, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CHANNEL_LABEL_H_H */
