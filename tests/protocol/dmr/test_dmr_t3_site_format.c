// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Tier III site numbers as dsd-neo shows them (issue #799): -D n is the DMRLA of TS 102 361-4 §6.3.2.2.3 (the top n
 * SITE bits are the location area, Figure 6.19), clamped to the model's SITE width; no -D or -D 0 shows the site
 * whole. Every number shown is the raw value + 1.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "dmr_tiii_site.h"

static void
test_model_site_bits(void) {
    assert(dmr_tiii_site_bits(0U) == 3U);
    assert(dmr_tiii_site_bits(1U) == 5U);
    assert(dmr_tiii_site_bits(2U) == 8U);
    assert(dmr_tiii_site_bits(3U) == 10U);
}

static void
test_dmrla_only_when_set(void) {
    assert(dmr_tiii_dmrla(0U, 7U) == 0U);
    assert(dmr_tiii_dmrla(1U, 0U) == 0U);
    assert(dmr_tiii_dmrla(1U, 2U) == 2U);
}

/* Small site 27 = 11011b: DMRLA 2 takes the top two bits (11) as the area and leaves 011 as the site in it. */
static void
test_dmrla_top_bits(void) {
    assert(dmr_tiii_is_split(5U, 2U));
    assert(dmr_tiii_area(27U, 5U, 2U) == 3U);
    assert(dmr_tiii_site_in_area(27U, 5U, 2U) == 3U);
    assert(dmr_tiii_area(27U, 5U, 4U) == 13U);
    assert(dmr_tiii_site_in_area(27U, 5U, 4U) == 1U);
    /* Huge site 1000 = 1111101000b with DMRLA 7: area 125, site 0 in it. */
    assert(dmr_tiii_area(1000U, 10U, 7U) == 125U);
    assert(dmr_tiii_site_in_area(1000U, 10U, 7U) == 0U);
}

static void
test_no_split_and_clamp(void) {
    assert(!dmr_tiii_is_split(5U, 0U));
    assert(!dmr_tiii_is_split(5U, 5U));
    assert(!dmr_tiii_is_split(5U, 9U));
    /* A DMRLA past the SITE width is the whole field: the area is the site, nothing is left inside it. */
    assert(dmr_tiii_area(27U, 5U, 9U) == 27U);
    assert(dmr_tiii_site_in_area(27U, 5U, 9U) == 0U);
    assert(dmr_tiii_site_in_area(27U, 5U, 0U) == 27U);
}

static void
test_site_label_is_one_based(void) {
    char label[16];
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 0U);
    assert(strcmp(label, "28") == 0);
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 2U);
    assert(strcmp(label, "4.4") == 0);
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 5U);
    assert(strcmp(label, "28") == 0);
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 9U);
    assert(strcmp(label, "28") == 0);
    dmr_tiii_site_label(label, sizeof(label), 1023U, 10U, 3U);
    assert(strcmp(label, "8.128") == 0);
    assert(dmr_tiii_display_net(2U) == 3U);
}

int
main(void) {
    test_model_site_bits();
    test_dmrla_only_when_set();
    test_dmrla_top_bits();
    test_no_split_and_clamp();
    test_site_label_is_one_based();
    return 0;
}
