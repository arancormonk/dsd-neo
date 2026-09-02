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
