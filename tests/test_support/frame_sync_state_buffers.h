// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Shared dibit/payload/soft/symbol-history buffer setup for the frame-sync DSP tests.
 *
 * free_state_buffers() nulls every pointer it releases. Every current caller
 * zeroes its reused static dsd_state before the next init_state_buffers(), so
 * no double free is reachable today; the nulling is hygiene for a helper that is
 * shared across test files and also runs from inside init_state_buffers() on a
 * partial allocation failure, where the caller sees a half-built state.
 */

#pragma once

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <stdlib.h>

static inline void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_soft_buf);
    free(state->symbol_history);
    state->dibit_buf = NULL;
    state->dmr_payload_buf = NULL;
    state->dmr_soft_buf = NULL;
    state->symbol_history = NULL;
}

static inline int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000000U, sizeof(dsd_dibit_soft_t));
    state->symbol_history = (float*)calloc(DSD_SYMBOL_HISTORY_SIZE, sizeof(float));
    if (!state->dibit_buf || !state->dmr_payload_buf || !state->dmr_soft_buf || !state->symbol_history) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    state->symbol_history_size = DSD_SYMBOL_HISTORY_SIZE;
    state->symbol_history_head = 0;
    state->symbol_history_count = 0;
    return 1;
}
