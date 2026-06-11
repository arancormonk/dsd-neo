// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_UI_TERMINAL_UI_KEY_STATUS_H
#define DSD_NEO_UI_TERMINAL_UI_KEY_STATUS_H

#include <dsd-neo/core/state.h>
#include <stddef.h>
#include "dsd-neo/core/state_fwd.h"

static inline unsigned int
ui_hytera_key_segment_count(const dsd_state* state) {
    if (state == NULL || (state->K1 == 0ULL && state->K2 == 0ULL && state->K3 == 0ULL && state->K4 == 0ULL)) {
        return 0U;
    }
    if (state->hytera_key_segments == 2U || state->hytera_key_segments == 4U) {
        return state->hytera_key_segments;
    }
    if (state->hytera_key_segments == 1U) {
        return 1U;
    }
    if (state->K3 != 0ULL || state->K4 != 0ULL) {
        return 4U;
    }
    if (state->K2 != 0ULL) {
        return 2U;
    }
    return 1U;
}

#endif
