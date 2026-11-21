// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The Capacity Plus and Connect Plus branding a CSBK writes (issue #903) changes how later bursts are read: "Cap+ "
 * reads data headers and preamble CSBKs with a 16-bit target and source, and any brand replaces an "XPT " one, which
 * reads standard LCs with a 16-bit target. So only a CSBK whose own CRC passed sets it: one accepted under relaxed RAS
 * handling (-F) leaves the branding as it was. A checked one still brands.
 */

#include <assert.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_state*
new_state(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->currentslot = 0;
    return state;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

/* A CSBK with opcode @p csbk_o and FID @p fid (third octet @p b2) on slot 0, its CRC passing or accepted by RAS. */
static void
run_csbk(dsd_opts* opts, dsd_state* state, uint8_t csbk_o, uint8_t fid, uint8_t b2, int ras_relaxed) {
    uint8_t pdu[12];
    uint8_t pdu_bits[96];
    DSD_MEMSET(pdu, 0, sizeof(pdu));
    pdu[0] = csbk_o;
    pdu[1] = fid;
    pdu[2] = b2;
    for (size_t i = 0; i < sizeof(pdu_bits); i++) {
        pdu_bits[i] = (uint8_t)((pdu[i / 8U] >> (7U - (i % 8U))) & 1U);
    }
    state->event_crc_invalid[0] = ras_relaxed ? DSD_CRC_RAS_UNVERIFIED : 0U;
    dmr_cspdu(opts, state, pdu_bits, pdu, 1U, 0U);
    state->event_crc_invalid[0] = 0U;
}

typedef struct {
    const char* name;
    uint8_t csbk_o;
    uint8_t fid;
    uint8_t b2;
    const char* brand;
} branding_case;

static const branding_case k_cases[] = {
    // Capacity Plus channel status 0x3E, a single block (first and last fragment).
    {"Cap+ channel status", 0x3EU, 0x10U, 0xC0U, "Cap+ "},   {"Con+ adjacent sites", 0x01U, 0x06U, 0x00U, "Con+ "},
    {"Con+ voice grant", 0x03U, 0x06U, 0x00U, "Con+ "},      {"Con+ data grant", 0x06U, 0x06U, 0x00U, "Con+ "},
    {"Con+ slot termination", 0x0CU, 0x06U, 0x00U, "Con+ "},
};

static void
ras_relaxed_csbk_does_not_brand(const dsd_opts* proto, const branding_case* c) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    DSD_MEMCPY(opts, proto, sizeof(*opts));

    dsd_state* state = new_state();
    run_csbk(opts, state, c->csbk_o, c->fid, c->b2, 1);
    if (strcmp(state->dmr_branding_sub, c->brand) == 0) {
        DSD_FPRINTF(stderr, "%s: a RAS-accepted CSBK branded the site\n", c->name);
        assert(0);
    }
    free_state(state);

    // A RAS-accepted CSBK leaves a checked XPT branding (and its 16-bit LC layout) standing.
    state = new_state();
    run_csbk(opts, state, 0x0BU, 0x68U, 0x00U, 0);
    assert(strcmp(state->dmr_branding_sub, "XPT ") == 0);
    run_csbk(opts, state, c->csbk_o, c->fid, c->b2, 1);
    assert(strcmp(state->dmr_branding_sub, "XPT ") == 0);
    free_state(state);

    // A checked one still brands.
    state = new_state();
    run_csbk(opts, state, c->csbk_o, c->fid, c->b2, 0);
    if (strcmp(state->dmr_branding_sub, c->brand) != 0) {
        DSD_FPRINTF(stderr, "%s: a checked CSBK did not brand the site\n", c->name);
        assert(0);
    }
    free_state(state);
    free(opts);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        ras_relaxed_csbk_does_not_brand(opts, &k_cases[i]);
    }
    free(opts);
    printf("DMR CSBK branding tests passed.\n");
    return 0;
}
