// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <stdlib.h>

#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

static int s_cleanup_calls = 0;

/* Reserved IDs never move (state_ext.h): the analog receiver's slot (issue #522) is pinned
 * here so a renumbering fails the build, not a frontend. */
_Static_assert(DSD_STATE_EXT_DSP_ANALOG_RX == 9, "DSP analog receive owns state_ext slot 9");
_Static_assert((int)DSD_STATE_EXT_DSP_ANALOG_RX < (int)DSD_STATE_EXT_MAX, "slot 9 is inside the table");

static void
test_cleanup_free(void* ptr) {
    s_cleanup_calls++;
    free(ptr);
}

int
main(void) {
    // dsd_state is a multi-megabyte struct; avoid Windows' default ~1MB stack.
    dsd_state* state = calloc(1, sizeof(*state));
    if (!state) {
        return 100;
    }

    void* p1 = malloc(1);
    if (!p1) {
        free(state);
        return 1;
    }

    if (dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_START_MS, p1, test_cleanup_free) != 0) {
        free(p1);
        free(state);
        return 2;
    }
    if (dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) != p1) {
        dsd_state_ext_free_all(state);
        free(state);
        return 3;
    }
    if (s_cleanup_calls != 0) {
        dsd_state_ext_free_all(state);
        free(state);
        return 4;
    }

    void* p2 = malloc(1);
    if (!p2) {
        dsd_state_ext_free_all(state);
        free(state);
        return 5;
    }

    if (dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_START_MS, p2, test_cleanup_free) != 0) {
        free(p2);
        dsd_state_ext_free_all(state);
        free(state);
        return 6;
    }
    if (dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) != p2) {
        dsd_state_ext_free_all(state);
        free(state);
        return 7;
    }
    if (dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_START_MS) != p2) {
        dsd_state_ext_free_all(state);
        free(state);
        return 71;
    }
    if (s_cleanup_calls != 1) {
        dsd_state_ext_free_all(state);
        free(state);
        return 8;
    }

    dsd_state_ext_free_all(state);
    if (s_cleanup_calls != 2) {
        free(state);
        return 9;
    }
    if (dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) != NULL) {
        free(state);
        return 10;
    }
    if (dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_START_MS) != NULL) {
        free(state);
        return 101;
    }

    void* p3 = malloc(1);
    if (!p3) {
        free(state);
        return 11;
    }
    if (dsd_state_ext_set(state, (dsd_state_ext_id)DSD_STATE_EXT_MAX, p3, test_cleanup_free) != -1) {
        free(p3);
        free(state);
        return 12;
    }
    free(p3);

    /* The analog slot behaves like every other slot: owned, replaced and freed with the rest. */
    s_cleanup_calls = 0;
    void* analog = malloc(1);
    if (!analog) {
        free(state);
        return 13;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_DSP_ANALOG_RX, analog, test_cleanup_free) != 0) {
        free(analog);
        free(state);
        return 14;
    }
    if (dsd_state_ext_get_const(state, DSD_STATE_EXT_DSP_ANALOG_RX) != analog
        || dsd_state_ext_get(state, DSD_STATE_EXT_CORE_SOURCE_ALIAS) != NULL) {
        dsd_state_ext_free_all(state);
        free(state);
        return 15;
    }
    dsd_state_ext_free_all(state);
    if (s_cleanup_calls != 1 || dsd_state_ext_get(state, DSD_STATE_EXT_DSP_ANALOG_RX) != NULL) {
        free(state);
        return 16;
    }

    dsd_state_ext_free_all(NULL);

    free(state);
    return 0;
}
