// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdint.h>
#include "dmr_tiii_site.h"

static void
test_model_default_split(void) {
    assert(dmr_tiii_model_default_split_n(0U) == 3U);
    assert(dmr_tiii_model_default_split_n(1U) == 5U);
    assert(dmr_tiii_model_default_split_n(2U) == 8U);
    assert(dmr_tiii_model_default_split_n(3U) == 10U);
}

static void
test_override_and_clamp(void) {
    assert(dmr_tiii_effective_split_n(5U, 0U, 0U, 5U) == 5U);
    assert(dmr_tiii_effective_split_n(5U, 1U, 0U, 5U) == 0U);
    assert(dmr_tiii_effective_split_n(5U, 1U, 9U, 5U) == 5U);
}

static void
test_display_values_are_one_based_when_split(void) {
    uint16_t n = 5U;
    uint16_t site = 27U;
    uint16_t mask = dmr_tiii_subsite_mask(n);

    assert(mask == 0x001FU);
    assert(dmr_tiii_display_net(2U, n) == 3U);
    assert(dmr_tiii_display_site(site, n) == 1U);
    assert(dmr_tiii_display_subsite(site, mask, n) == 28U);
}

static void
test_display_values_remain_raw_without_split(void) {
    assert(dmr_tiii_display_net(2U, 0U) == 2U);
    assert(dmr_tiii_display_site(27U, 0U) == 27U);
    assert(dmr_tiii_display_subsite(27U, 0U, 0U) == 0U);
}

int
main(void) {
    test_model_default_split();
    test_override_and_clamp();
    test_display_values_are_one_based_when_split();
    test_display_values_remain_raw_without_split();
    return 0;
}
