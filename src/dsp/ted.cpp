// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief State lifecycle for the active OP25-compatible Gardner timing recovery.
 */

#include <dsd-neo/dsp/ted.h>

void
ted_init_state(ted_state_t* state) {
    if (!state) {
        return;
    }
    state->mu = 0.0f;
    state->omega = 0.0f;
    state->omega_mid = 0.0f;
    state->omega_min = 0.0f;
    state->omega_max = 0.0f;
    state->omega_rel = 0.0f;
    state->last_r = 0.0f;
    state->last_j = 0.0f;
    state->e_ema = 0.0f;
    state->lock_accum = 0.0f;
    state->lock_count = 0;
    state->event_count = 0;
    for (int i = 0; i < TED_DL_SIZE * 2 * 2; i++) {
        state->dl[i] = 0.0f;
    }
    state->dl_index = 0;
    state->twice_sps = 0;
    state->sps = 0;
}

void
ted_soft_reset(ted_state_t* state) {
    if (!state) {
        return;
    }
    state->last_r = 0.0f;
    state->last_j = 0.0f;
    state->e_ema = 0.0f;
    state->lock_accum = 0.0f;
    state->lock_count = 0;
    state->event_count = 0;
}
