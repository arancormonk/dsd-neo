// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression: Motorola P25 systems transmit the talker alias during hangtime,
 * after MAC_END_PTT has already ended the call epoch. The alias attach gate
 * must accept the retained ended epoch when the FQ-SUID source matches, or
 * every hangtime alias is deferred and lost (observed live: 21/21 CRC-valid
 * aliases dropped with "current call source mismatch").
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_fail;

static void
expect_true(const char* label, int got) {
    if (!got) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_fail = 1;
    }
}

static void
set_bits(uint8_t* bits, int start, int len, uint32_t value) {
    for (int i = 0; i < len; i++) {
        bits[start + i] = (uint8_t)((value >> (len - 1 - i)) & 1U);
    }
}

/* Build the post-CRC input handed to apx_embedded_alias_dump: the FQ-SUID
 * occupies bits 72..127 (WACN 20, SYSID 12, RID 24). */
static void
build_alias_input(uint8_t* input, size_t input_bits, uint32_t wacn, uint32_t sys, uint32_t rid) {
    DSD_MEMSET(input, 0, input_bits);
    set_bits(input, 72, 20, wacn);
    set_bits(input, 92, 12, sys);
    set_bits(input, 104, 24, rid);
}

/* Decoded alias characters live in the odd bytes of the decoded buffer. */
static uint16_t
build_alias_decoded(uint8_t* decoded, size_t decoded_sz, const char* text) {
    DSD_MEMSET(decoded, 0, decoded_sz);
    size_t len = strlen(text);
    for (size_t i = 0; i < len && (i * 2 + 1) < decoded_sz; i++) {
        decoded[(i * 2) + 1] = (uint8_t)text[i];
    }
    return (uint16_t)(len * 2);
}

static int
observe_call(dsd_state* state, uint64_t tg, uint64_t src, double observed_m, dsd_call_boundary boundary) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = tg,
        .policy_target_id = tg,
        .ota_source_id = src,
        .service_options = 0U,
        .has_service_metadata = 1U,
        .observed_m = observed_m,
    };
    return dsd_call_state_observe(state, &observation, boundary);
}

/* Harris Phase 1 alias block LCW: opcode (0x32..0x35) in bits 0..7, seven
 * payload characters starting at bit 16. */
static void
build_l3h_block(uint8_t* lcw, size_t lcw_bits, uint8_t opcode, const char payload[7]) {
    DSD_MEMSET(lcw, 0, lcw_bits);
    set_bits(lcw, 0, 8, opcode);
    for (int i = 0; i < 7; i++) {
        set_bits(lcw, 16 + (i * 8), 8, (uint8_t)payload[i]);
    }
}

static int
history_has_alias(const dsd_state* state, const char* needle) {
    for (int i = 0; i < 8; i++) {
        if (strstr(state->event_history_s[0].Event_History_Items[i].alias, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    Event_History_I* history = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    if (!opts || !state || !history) {
        DSD_FPRINTF(stderr, "FAIL: allocation\n");
        free(history);
        free(state);
        free(opts);
        return 1;
    }
    state->event_history_s = history;
    init_event_history(&state->event_history_s[0], 0, 255);
    init_event_history(&state->event_history_s[1], 0, 255);

    const uint32_t wacn = 0xBEE00;
    const uint32_t sys = 0x4C9;
    const uint32_t rid = 10976;
    const uint64_t tg = 5204;

    uint8_t input[3200];
    uint8_t decoded[200];

    /* Sanity: alias during the active call attaches. */
    expect_true("observe begins epoch", observe_call(state, tg, rid, 1.0, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    build_alias_input(input, sizeof(input), wacn, sys, rid);
    uint16_t num_bytes = build_alias_decoded(decoded, sizeof(decoded), "ACTIVE1");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("active-call alias attaches", strstr(state->generic_talker_alias[0], "ACTIVE1") != NULL);

    /* Regression: Motorola hangtime alias arrives after the epoch ended. */
    expect_true("end call", dsd_call_state_end_ex(state, 0U, 2.0, DSD_CALL_END_TERMINATOR) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    state->generic_talker_alias[0][0] = '\0';
    num_bytes = build_alias_decoded(decoded, sizeof(decoded), "HANGTIME");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("hangtime alias attaches to retained ended call",
                strstr(state->generic_talker_alias[0], "HANGTIME") != NULL);
    expect_true("hangtime alias reaches event history", history_has_alias(state, "HANGTIME"));

    /* A hangtime alias for a different radio must not attach. */
    state->generic_talker_alias[0][0] = '\0';
    build_alias_input(input, sizeof(input), wacn, sys, rid + 1U);
    num_bytes = build_alias_decoded(decoded, sizeof(decoded), "WRONGRID");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("mismatched source is still deferred", state->generic_talker_alias[0][0] == '\0');
    expect_true("mismatched source never reaches history", !history_has_alias(state, "WRONGRID"));

    /* Once a new call begins with a different source, the stale alias must not attach. */
    expect_true("next call begins", observe_call(state, tg, rid + 2U, 3.0, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    state->generic_talker_alias[0][0] = '\0';
    build_alias_input(input, sizeof(input), wacn, sys, rid);
    num_bytes = build_alias_decoded(decoded, sizeof(decoded), "STALE999");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("stale alias does not attach to the next call", state->generic_talker_alias[0][0] == '\0');

    /* Harris Phase 1: alias blocks that straddle the end of the transmission
     * must finish assembling against the retained ended epoch. */
    const uint64_t l3h_src = 810001;
    uint8_t lcw[72];
    expect_true("harris call begins", observe_call(state, tg, l3h_src, 4.0, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    state->generic_talker_alias[0][0] = '\0';
    build_l3h_block(lcw, sizeof(lcw), 0x32U, "ENGINE ");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    build_l3h_block(lcw, sizeof(lcw), 0x33U, "51     ");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    expect_true("harris partial alias attaches while active",
                strstr(state->generic_talker_alias[0], "ENGINE51") != NULL);

    expect_true("harris call ends", dsd_call_state_end_ex(state, 0U, 5.0, DSD_CALL_END_TERMINATOR) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    build_l3h_block(lcw, sizeof(lcw), 0x34U, "STATION");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    build_l3h_block(lcw, sizeof(lcw), 0x35U, "9      ");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    expect_true("harris alias completes during hangtime",
                strstr(state->generic_talker_alias[0], "ENGINE51STATION9") != NULL);
    expect_true("harris hangtime completion reaches event history", history_has_alias(state, "ENGINE51STATION9"));

    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    free(opts);
    return g_fail;
}
