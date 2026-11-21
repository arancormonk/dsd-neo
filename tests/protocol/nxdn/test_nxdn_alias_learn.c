// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN Kenwood talker alias learning (plan 4.6), through the element dispatch NXDN_Elements_Content_decode() with
 * PROP_FORM (0x3F) fragments: only a complete four-fragment alias whose fragments all arrived under the ACTIVE call of
 * its talker, on one carrier, is learned as that talker's alias of the reception's scope. Partial, spliced, other-call,
 * other-carrier, other-count and hangtime (ended call) assemblies are never learned.
 */

#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int g_failures;

static void
put_bits(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0U; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> (nbits - 1U - i)) & 1U);
    }
}

/* A Kenwood fragment (PROP_FORM 0x3F, MFID 0x68, 0x8204, number and count, four octets) through the element dispatch. */
static void
send_fragment(dsd_opts* opts, dsd_state* state, uint8_t number, uint8_t total, const char* text) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof bits);
    put_bits(bits, 0U, 0x3FU, 8U);
    put_bits(bits, 8U, 0x68U, 8U);
    put_bits(bits, 16U, 0x82U, 8U);
    put_bits(bits, 24U, 0x04U, 8U);
    put_bits(bits, 32U, number, 4U);
    put_bits(bits, 36U, total, 4U);
    for (size_t i = 0U; i < 4U; i++) {
        put_bits(bits, 40U + (i * 8U), (uint8_t)text[i], 8U);
    }
    dsd_test_quiet_stderr cap;
    if (dsd_test_quiet_stderr_begin(&cap) != 0) {
        abort();
    }
    NXDN_Elements_Content_decode(opts, state, bits, sizeof bits);
    if (dsd_test_quiet_stderr_end(&cap) != 0) {
        abort();
    }
}

static void
send_alias(dsd_opts* opts, dsd_state* state, const char* sixteen) {
    for (uint8_t n = 1U; n <= 4U; n++) {
        send_fragment(opts, state, n, 4U, sixteen + ((size_t)(n - 1U) * 4U));
    }
}

static void
open_call(dsd_state* state, uint32_t source) {
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof obs);
    obs.protocol = DSD_SYNC_NXDN_POS;
    obs.slot = 0U;
    obs.kind = DSD_CALL_KIND_GROUP_VOICE;
    obs.ota_target_id = 100U;
    obs.policy_target_id = 100U;
    obs.ota_source_id = source;
    if (dsd_call_state_observe(state, &obs, DSD_CALL_BOUNDARY_BEGIN) < 0) {
        abort();
    }
}

static void
expect_learned(const dsd_state* state, uint32_t rid, const char* want, const char* tag) {
    char mode[8];
    char name[DSD_SOURCE_ALIAS_NAME_MAX];
    const int found = dsd_source_label_lookup_scoped(state, NULL, rid, mode, sizeof mode, name, sizeof name);
    if (want == NULL) {
        if (found) {
            DSD_FPRINTF(stderr, "FAIL: %s: radio %u learned '%s', want nothing\n", tag, (unsigned)rid, name);
            g_failures++;
        }
        return;
    }
    if (!found || strcmp(name, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: radio %u named '%s', want '%s'\n", tag, (unsigned)rid, found ? name : "", want);
        g_failures++;
    }
}

static dsd_state*
new_state(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    if (state == NULL) {
        abort();
    }
    return state;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_complete_learned(dsd_opts* opts) {
    dsd_state* st = new_state();
    open_call(st, 1001U);
    send_fragment(opts, st, 1U, 4U, "DISP");
    send_fragment(opts, st, 2U, 4U, "ATCH");
    send_fragment(opts, st, 3U, 4U, "CH01");
    expect_learned(st, 1001U, NULL, "three of four fragments");
    /* The last two octets of fragment 4 are not text. */
    send_fragment(opts, st, 4U, 4U, " 7AZ");
    expect_learned(st, 1001U, "DISPATCHCH01 7", "complete alias learned, 14 octets");
    free_state(st);
}

static void
test_not_learned(dsd_opts* opts) {
    /* A missing fragment never completes (no splice). */
    dsd_state* st = new_state();
    open_call(st, 2002U);
    send_fragment(opts, st, 1U, 4U, "DISP");
    send_fragment(opts, st, 3U, 4U, "CH01");
    send_fragment(opts, st, 4U, 4U, " 7AZ");
    expect_learned(st, 2002U, NULL, "spliced assembly");
    free_state(st);

    /* Fragments of another call. */
    st = new_state();
    open_call(st, 3003U);
    send_fragment(opts, st, 1U, 4U, "AAAA");
    send_fragment(opts, st, 2U, 4U, "BBBB");
    open_call(st, 3004U);
    send_fragment(opts, st, 3U, 4U, "CCCC");
    send_fragment(opts, st, 4U, 4U, "DD  ");
    expect_learned(st, 3003U, NULL, "other call: first talker");
    expect_learned(st, 3004U, NULL, "other call: second talker");
    free_state(st);

    /* Fragments heard on another carrier. */
    st = new_state();
    open_call(st, 4004U);
    send_fragment(opts, st, 1U, 4U, "AAAA");
    send_fragment(opts, st, 2U, 4U, "BBBB");
    dsd_trunk_tuning_generation_advance();
    send_fragment(opts, st, 3U, 4U, "CCCC");
    send_fragment(opts, st, 4U, 4U, "DD  ");
    expect_learned(st, 4004U, NULL, "other carrier");
    free_state(st);

    /* Another fragment count is printed raw, never learned. */
    st = new_state();
    open_call(st, 5005U);
    send_fragment(opts, st, 1U, 2U, "TEST");
    send_fragment(opts, st, 2U, 2U, "NAME");
    expect_learned(st, 5005U, NULL, "two-fragment count");
    free_state(st);

    /* No call open: the alias shows but names nobody. */
    st = new_state();
    send_alias(opts, st, "NOCALL      XX..");
    expect_learned(st, 0U, NULL, "no call");
    free_state(st);

    /* An alias completed in hangtime binds to the ended call: it may be the next talker's, so it is not learned. */
    st = new_state();
    open_call(st, 6006U);
    if (dsd_call_state_end(st, 0U, 1.0) < 0) {
        abort();
    }
    send_alias(opts, st, "HANGTIME    XX..");
    expect_learned(st, 6006U, NULL, "hangtime alias");
    free_state(st);

    /* Slot 0 keeps another protocol's call whose end never decoded (a DMR or P25 call): an NXDN alias completed before
       its own call opened is no alias of that call's talker. */
    static const int k_other_protocols[] = {DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_P25P1_POS};
    for (size_t i = 0; i < sizeof k_other_protocols / sizeof k_other_protocols[0]; i++) {
        st = new_state();
        dsd_call_observation other;
        DSD_MEMSET(&other, 0, sizeof other);
        other.protocol = k_other_protocols[i];
        other.slot = 0U;
        other.kind = DSD_CALL_KIND_GROUP_VOICE;
        other.ota_target_id = 100U;
        other.policy_target_id = 100U;
        other.ota_source_id = 9009U;
        if (dsd_call_state_observe(st, &other, DSD_CALL_BOUNDARY_BEGIN) < 0) {
            abort();
        }
        send_alias(opts, st, "FOREIGN     XX..");
        expect_learned(st, 9009U, NULL, "another protocol's open call");
        free_state(st);
    }

    /* An all-space alias. */
    st = new_state();
    open_call(st, 7007U);
    send_alias(opts, st, "              ..");
    expect_learned(st, 7007U, NULL, "empty alias");
    free_state(st);
}

/* The alias belongs to the reception's system: another NXDN system code does not see it. */
static void
test_scope(dsd_opts* opts) {
    dsd_state* st = new_state();
    dsd_rxid_note_nxdn_location(st, 1U, 12U, 3U);
    dsd_alias_scope a;
    if (dsd_alias_scope_current(st, &a) != 1) {
        abort();
    }
    open_call(st, 8008U);
    send_alias(opts, st, "ALICE         ..");
    expect_learned(st, 8008U, "ALICE", "learned on system A");
    dsd_rxid_reset(st);
    dsd_rxid_note_nxdn_location(st, 1U, 13U, 3U);
    expect_learned(st, 8008U, NULL, "not on system B");
    char mode[8];
    char name[DSD_SOURCE_ALIAS_NAME_MAX];
    if (!dsd_source_label_lookup_scoped(st, &a, 8008U, mode, sizeof mode, name, sizeof name)
        || strcmp(name, "ALICE") != 0) {
        DSD_FPRINTF(stderr, "FAIL: system A keeps its alias\n");
        g_failures++;
    }
    free_state(st);
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    if (opts == NULL) {
        return 100;
    }
    test_complete_learned(opts);
    test_not_learned(opts);
    test_scope(opts);
    free(opts);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "NXDN_ALIAS_LEARN: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "NXDN_ALIAS_LEARN: PASS\n");
    return 0;
}
