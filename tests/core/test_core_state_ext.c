// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>

#include <stdlib.h>

static int s_cleanup_calls = 0;

static void
test_cleanup_free(void* ptr) {
    s_cleanup_calls++;
    free(ptr);
}

int
main(void) {
    dsd_state state = {0};

    void* p1 = malloc(1);
    if (!p1) {
        return 1;
    }

    if (dsd_state_ext_set(&state, DSD_STATE_EXT_ENGINE_START_MS, p1, test_cleanup_free) != 0) {
        free(p1);
        return 2;
    }
    if (dsd_state_ext_get(&state, DSD_STATE_EXT_ENGINE_START_MS) != p1) {
        dsd_state_ext_free_all(&state);
        return 3;
    }
    if (s_cleanup_calls != 0) {
        dsd_state_ext_free_all(&state);
        return 4;
    }

    void* p2 = malloc(1);
    if (!p2) {
        dsd_state_ext_free_all(&state);
        return 5;
    }

    if (dsd_state_ext_set(&state, DSD_STATE_EXT_ENGINE_START_MS, p2, test_cleanup_free) != 0) {
        free(p2);
        dsd_state_ext_free_all(&state);
        return 6;
    }
    if (dsd_state_ext_get(&state, DSD_STATE_EXT_ENGINE_START_MS) != p2) {
        dsd_state_ext_free_all(&state);
        return 7;
    }
    if (s_cleanup_calls != 1) {
        dsd_state_ext_free_all(&state);
        return 8;
    }

    dsd_state_ext_free_all(&state);
    if (s_cleanup_calls != 2) {
        return 9;
    }
    if (dsd_state_ext_get(&state, DSD_STATE_EXT_ENGINE_START_MS) != NULL) {
        return 10;
    }

    void* p3 = malloc(1);
    if (!p3) {
        return 11;
    }
    if (dsd_state_ext_set(&state, (dsd_state_ext_id)DSD_STATE_EXT_MAX, p3, test_cleanup_free) != -1) {
        free(p3);
        return 12;
    }
    free(p3);

    dsd_state_ext_free_all(NULL);

    return 0;
}
