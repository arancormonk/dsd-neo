// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief P25 Phase 2 retune token: whether the receiver left the channel a superframe was collected on (issue #651).
 *
 * Its own object, so the MAC handlers that check it (xcch, VPDU) do not pull in the frame decoder.
 */

#include <dsd-neo/runtime/trunk_tuning_hooks.h>

#include <stdint.h>

#include "p25p2_frame_internal.h"

/* Counts p25_p2_frame_reset() calls. */
static uint32_t s_frame_reset_generation = 0U;

void
p25p2_retune_note_frame_reset(void) {
    s_frame_reset_generation++;
}

p25p2_retune_token
p25p2_retune_token_now(void) {
    p25p2_retune_token token = {
        .tune_generation = dsd_trunk_tuning_generation(),
        .pending_request = dsd_trunk_tuning_pending_request(),
        .frame_reset_generation = s_frame_reset_generation,
    };
    return token;
}

int
p25p2_retune_token_changed(const p25p2_retune_token* since) {
    if (!since) {
        return 0;
    }
    const p25p2_retune_token now = p25p2_retune_token_now();
    return now.tune_generation != since->tune_generation || now.pending_request != since->pending_request
           || now.frame_reset_generation != since->frame_reset_generation;
}

/* Set while a superframe whose position is unproven is dispatched. */
static int s_window_slot_unproven = 0;

void
p25p2_window_set_slot_unproven(int unproven) {
    s_window_slot_unproven = unproven ? 1 : 0;
}

int
p25p2_window_slot_unproven(void) {
    return s_window_slot_unproven;
}
