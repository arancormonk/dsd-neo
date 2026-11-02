// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Tier III site numbers as dsd-neo shows them: -D n is the DMRLA of TS 102 361-4 §6.3.2.2.3 (the top n SITE bits are
 * the location area, Figure 6.19); no -D or -D 0 shows the site whole, and -D of the SITE width or more makes the whole
 * SITE the area with site 0 in it. A standard network shows every number as the raw value + 1; a recognised Capacity
 * Max network shows the network and the whole site raw, whatever -D says.
 */

#include <assert.h>
#include <dsd-neo/core/received_identity.h>
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

/* MODEL is the top two bits of the 14-bit code, then NET and SITE at the widths the model sets. */
static void
test_decode_syscode(void) {
    dmr_tiii_code code = dmr_tiii_decode_syscode(0x2101U);
    assert(code.model == 2U && code.net == 1U && code.site == 1U && code.site_bits == 8U);
    assert(strcmp(code.model_name, "Large") == 0);
    code = dmr_tiii_decode_syscode(0x2104U);
    assert(code.model == 2U && code.net == 1U && code.site == 4U);
    code = dmr_tiii_decode_syscode(0x105BU);
    assert(code.model == 1U && code.net == 2U && code.site == 27U && code.site_bits == 5U);
    assert(strcmp(code.model_name, "Small") == 0);
    code = dmr_tiii_decode_syscode(0x0A3FU);
    assert(code.model == 0U && code.net == 327U && code.site == 7U && code.site_bits == 3U);
    assert(strcmp(code.model_name, "Tiny") == 0);
    code = dmr_tiii_decode_syscode(0x3FFFU);
    assert(code.model == 3U && code.net == 3U && code.site == 1023U && code.site_bits == 10U);
    assert(strcmp(code.model_name, "Huge") == 0);
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
    assert(dmr_tiii_is_split(2U));
    assert(dmr_tiii_area(27U, 5U, 2U) == 3U);
    assert(dmr_tiii_site_in_area(27U, 5U, 2U) == 3U);
    assert(dmr_tiii_area(27U, 5U, 4U) == 13U);
    assert(dmr_tiii_site_in_area(27U, 5U, 4U) == 1U);
    /* Huge site 1000 = 1111101000b with DMRLA 7: area 125, site 0 in it. */
    assert(dmr_tiii_area(1000U, 10U, 7U) == 125U);
    assert(dmr_tiii_site_in_area(1000U, 10U, 7U) == 0U);
}

/* No DMRLA leaves the site whole; one of the SITE width or more (clamped to it) makes the whole SITE the area. */
static void
test_no_split_and_full_width(void) {
    assert(!dmr_tiii_is_split(0U));
    assert(dmr_tiii_is_split(5U));
    assert(dmr_tiii_is_split(9U));
    assert(dmr_tiii_area(27U, 5U, 5U) == 27U);
    assert(dmr_tiii_site_in_area(27U, 5U, 5U) == 0U);
    assert(dmr_tiii_area(27U, 5U, 9U) == 27U);
    assert(dmr_tiii_site_in_area(27U, 5U, 9U) == 0U);
    assert(dmr_tiii_site_in_area(27U, 5U, 0U) == 27U);
}

static void
test_site_label_is_one_based(void) {
    char label[16];
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 0U, 0);
    assert(strcmp(label, "28") == 0);
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 2U, 0);
    assert(strcmp(label, "4.4") == 0);
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 4U, 0);
    assert(strcmp(label, "14.2") == 0);
    dmr_tiii_site_label(label, sizeof(label), 1023U, 10U, 3U, 0);
    assert(strcmp(label, "8.128") == 0);
    /* The SITE width or more: the whole SITE is the area, site 0 in it (Hytera CPS: Tiny, LA 3, raw SITE 1 -> 2.1). */
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 5U, 0);
    assert(strcmp(label, "28.1") == 0);
    dmr_tiii_site_label(label, sizeof(label), 27U, 5U, 9U, 0);
    assert(strcmp(label, "28.1") == 0);
    dmr_tiii_site_label(label, sizeof(label), 1U, 3U, 3U, 0);
    assert(strcmp(label, "2.1") == 0);
    assert(dmr_tiii_display_net(2U, 0) == 3U);
}

static void
test_capmax_label_is_raw_and_whole(void) {
    char label[16];
    for (uint16_t n = 0U; n <= 9U; ++n) {
        dmr_tiii_site_label(label, sizeof(label), 27U, 5U, n, 1);
        assert(strcmp(label, "27") == 0);
    }
    dmr_tiii_site_label(label, sizeof(label), 0U, 8U, 3U, 1);
    assert(strcmp(label, "0") == 0);
    assert(dmr_tiii_display_net(2U, 1) == 2U);
    assert(dmr_tiii_display_net(0U, 1) == 0U);
}

static void
test_site_text(void) {
    char text[64];
    dmr_tiii_site_text(text, sizeof(text), 0x2101U, 0U, 0);
    assert(strcmp(text, "TIII Large \xC2\xB7 Net 2 \xC2\xB7 Site 2 \xC2\xB7 SYS 2101") == 0);
    dmr_tiii_site_text(text, sizeof(text), 0x2101U, 3U, 1);
    assert(strcmp(text,
                  "TIII Large \xC2\xB7 " DSD_RXID_T3_CAPMAX_TOKEN " \xC2\xB7 Net 1 \xC2\xB7 Site 1 \xC2\xB7 SYS 2101")
           == 0);
    assert(strcmp(text, "TIII Large \xC2\xB7 Capacity Max \xC2\xB7 Net 1 \xC2\xB7 Site 1 \xC2\xB7 SYS 2101") == 0);
    dmr_tiii_site_text(text, sizeof(text), 0x105BU, 2U, 0);
    assert(strcmp(text, "TIII Small \xC2\xB7 Net 3 \xC2\xB7 Site 4.4 \xC2\xB7 SYS 105B") == 0);
    /* The longest texts still fit the record's field whole. */
    dmr_tiii_site_text(text, sizeof(text), 0x1FFFU, 0U, 1);
    assert(strcmp(text, "TIII Small \xC2\xB7 Capacity Max \xC2\xB7 Net 127 \xC2\xB7 Site 31 \xC2\xB7 SYS 1FFF") == 0);
    dmr_tiii_site_text(text, sizeof(text), 0x3FFFU, 9U, 0);
    assert(strcmp(text, "TIII Huge \xC2\xB7 Net 4 \xC2\xB7 Site 512.2 \xC2\xB7 SYS 3FFF") == 0);
}

int
main(void) {
    test_model_site_bits();
    test_decode_syscode();
    test_dmrla_only_when_set();
    test_dmrla_top_bits();
    test_no_split_and_full_width();
    test_site_label_is_one_based();
    test_capmax_label_is_raw_and_whole();
    test_site_text();
    return 0;
}
