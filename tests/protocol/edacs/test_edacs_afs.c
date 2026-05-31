// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for EDACS AFS string formatting.
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
init_state(dsd_state* state, int a_bits, int f_bits, int s_bits) {
    DSD_MEMSET(state, 0, sizeof(*state));
    state->edacs_a_bits = a_bits;
    state->edacs_f_bits = f_bits;
    state->edacs_s_bits = s_bits;
}

static void
test_default_afs_format(void) {
    static dsd_state state;
    init_state(&state, 4, 4, 3);
    char buffer[7] = {0};

    assert(getAfsStringLength(&state) == 6);
    assert(getAfsString(&state, buffer, 3, 12, 5) == 6);
    assert(strcmp(buffer, "03-125") == 0);
}

static void
test_custom_afs_format_uses_decimal_fields(void) {
    static dsd_state state;
    init_state(&state, 3, 5, 3);
    char buffer[7] = {0};

    assert(getAfsStringLength(&state) == 6);
    assert(getAfsString(&state, buffer, 5, 17, 6) == 6);
    assert(strcmp(buffer, "5:17:6") == 0);
}

static void
test_custom_afs_format_expands_for_large_agency_field(void) {
    static dsd_state state;
    init_state(&state, 8, 1, 2);
    char buffer[8] = {0};

    assert(getAfsStringLength(&state) == 7);
    assert(getAfsString(&state, buffer, 123, 1, 2) == 7);
    assert(strcmp(buffer, "123:1:2") == 0);
}

static void
test_custom_afs_format_expands_for_large_subfleet_field(void) {
    static dsd_state state;
    init_state(&state, 1, 1, 9);
    char buffer[8] = {0};

    assert(getAfsStringLength(&state) == 7);
    assert(getAfsString(&state, buffer, 1, 1, 123) == 7);
    assert(strcmp(buffer, "1:1:123") == 0);
}

int
main(void) {
    test_default_afs_format();
    test_custom_afs_format_uses_decimal_fields();
    test_custom_afs_format_expands_for_large_agency_field();
    test_custom_afs_format_expands_for_large_subfleet_field();
    printf("EDACS_AFS: OK\n");
    return 0;
}
