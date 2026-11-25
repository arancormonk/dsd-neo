// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Motorola Capacity Max voice LCs under FID 0x10 carry bit 0x10 over the standard FLCO: 0x10 is the group voice
 * channel user and 0x13 the unit-to-unit (private) one. A checked FLCO 0x13 LC opens a private call with the standard
 * target and source; FLCO 0x10 stays a group call, and FLCO 0x13 under FID 0 is not read as Capacity Max (it opens
 * no call).
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

static void
write_bits(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

/* A voice LC header on slot 0 with the given FLCO and FID, its check passed; returns 1 and fills @p call when it leaves
   a call on the slot. */
static int
try_voice_header(dsd_opts* opts, uint8_t flco, uint8_t fid, uint32_t target, uint32_t source, dsd_call_snapshot* call) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->currentslot = 0;

    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    write_bits(bits, 2U, flco, 6U);
    write_bits(bits, 8U, fid, 8U);
    write_bits(bits, 24U, target, 24U);
    write_bits(bits, 48U, source, 24U);
    uint32_t irr = 0;
    dmr_flco(opts, state, bits, 1U, &irr, 1U);

    const int have_call = dsd_call_state_get(state, 0U, call) > 0;
    dsd_state_ext_free_all(state);
    free(state);
    return have_call;
}

static dsd_call_snapshot
run_voice_header(dsd_opts* opts, uint8_t flco, uint8_t fid, uint32_t target, uint32_t source) {
    dsd_call_snapshot call;
    assert(try_voice_header(opts, flco, fid, target, source, &call));
    return call;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);

    dsd_call_snapshot call = run_voice_header(opts, 0x13U, 0x10U, 1234U, 5678U);
    assert(call.kind == DSD_CALL_KIND_PRIVATE_VOICE);
    assert(call.ota_target_id == 1234U && call.ota_source_id == 5678U);

    call = run_voice_header(opts, 0x10U, 0x10U, 1234U, 5678U);
    assert(call.kind == DSD_CALL_KIND_GROUP_VOICE);
    assert(call.ota_target_id == 1234U);

    // Under FID 0, FLCO 0x13 is no voice channel user (TS 102 361-2 Table B.1): it opens no call at all.
    assert(!try_voice_header(opts, 0x13U, 0x00U, 1234U, 5678U, &call));

    // The standard private call user is unchanged.
    call = run_voice_header(opts, 0x03U, 0x00U, 1234U, 5678U);
    assert(call.kind == DSD_CALL_KIND_PRIVATE_VOICE);

    free(opts);
    printf("DMR FLCO Capacity Max private call tests passed.\n");
    return 0;
}
