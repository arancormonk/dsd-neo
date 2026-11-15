// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR talker alias learning (plan 4.6): the alias is fed through the header and block entry points dmr_flco() calls
 * once an embedded LC passed its checksum (dmr_talker_alias_lc_header() / _blocks(), and _invalidate() for a failed
 * one). It is learned as the call source's alias of the reception's system only when the header's Data Length in code
 * units (TS 102 361-2 Table 7.26) is covered and every needed block arrived while the same call, with the same source,
 * was active on the same carrier.
 */

#include <assert.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
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
seed_active_call(dsd_state* state, uint8_t slot, uint32_t source) {
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 9U,
        .policy_target_id = 9U,
        .ota_source_id = source,
    };
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) >= 0);
}

static void
put_bits(uint8_t* bits, size_t offset, uint32_t value, uint8_t count) {
    for (uint8_t b = 0; b < count; b++) {
        bits[offset + b] = (uint8_t)((value >> (count - 1U - b)) & 1U);
    }
}

/* Talker_Alias_hdr (Table 7.4): format bits 16-17, length 18-22, 8-bit text from bit 24 (six octets). */
static void
send_header(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t format, uint8_t length, const char* text) {
    uint8_t lc[80];
    DSD_MEMSET(lc, 0, sizeof lc);
    put_bits(lc, 2, 0x04U, 6);
    put_bits(lc, 16, format, 2);
    put_bits(lc, 18, length, 5);
    for (size_t i = 0; i < 6U && text[i] != '\0'; i++) {
        put_bits(lc, 24U + (i * 8U), (uint8_t)text[i], 8);
    }
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_alias_learn") == 0);
    dmr_talker_alias_lc_header(opts, state, slot, lc);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
}

/* Talker_Alias_blk1..3 (Table 7.5): seven octets from bit 16. @p index 0..2 is block 1..3. */
static void
send_block_octets(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t index, const uint8_t* octets, size_t count) {
    uint8_t lc[80];
    DSD_MEMSET(lc, 0, sizeof lc);
    put_bits(lc, 2, 0x05U + index, 6);
    for (size_t i = 0; i < 7U && i < count; i++) {
        put_bits(lc, 16U + (i * 8U), octets[i], 8);
    }
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_alias_learn") == 0);
    dmr_talker_alias_lc_blocks(opts, state, slot, index, lc);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
}

static void
send_block(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t index, const char* text) {
    send_block_octets(opts, state, slot, index, (const uint8_t*)text, strlen(text));
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
    assert(state != NULL);
    return state;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

/* A header and one block: Data Length 8 octets needs block 1 (the header holds six). */
static void
test_learn_complete(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 0U, 1234567U);
    send_header(opts, st, 0U, 1U, 8U, "ENGINE");
    expect_learned(st, 1234567U, NULL, "header alone does not cover 8 octets");
    send_block(opts, st, 0U, 0U, " 7");
    expect_learned(st, 1234567U, "ENGINE 7", "header + block 1 learned");
    /* The other slot's call is untouched. */
    expect_learned(st, 7654321U, NULL, "nothing else learned");
    free_state(st);
}

/* A length the header covers alone: learned with the header (and the text is not cut at the length, as shown). */
static void
test_learn_header_only(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 1U, 2345678U);
    send_header(opts, st, 1U, 1U, 6U, "DISPAT");
    expect_learned(st, 2345678U, "DISPAT", "a six-octet alias is the header's");
    free_state(st);
}

/* Data Length 20 octets needs blocks 1 and 2. A missing block never publishes; a block that completes the set out of
   order waits for the decode that covers the whole length (the next cycle's last block). */
static void
test_missing_and_out_of_order(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 0U, 3456789U);
    send_header(opts, st, 0U, 1U, 20U, "ALPHA ");
    send_block(opts, st, 0U, 1U, "UNIT 77");
    expect_learned(st, 3456789U, NULL, "block 1 missing: not learned");
    send_block(opts, st, 0U, 0U, "BRAVO 1");
    expect_learned(st, 3456789U, NULL, "block 1 decode does not cover 20 octets");
    send_block(opts, st, 0U, 1U, "UNIT 77");
    expect_learned(st, 3456789U, "ALPHA BRAVO 1UNIT 77", "complete set learned");
    free_state(st);
}

/* A block under another call or carrier, a contradicting block and a failed checksum end the assembly. */
static void
test_binding_breaks(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 0U, 1111111U);
    send_header(opts, st, 0U, 1U, 8U, "OLDCAL");
    seed_active_call(st, 0U, 2222222U);
    send_block(opts, st, 0U, 0U, "L1");
    expect_learned(st, 1111111U, NULL, "another call's block: old source not learned");
    expect_learned(st, 2222222U, NULL, "another call's block: new source not learned");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 3333333U);
    send_header(opts, st, 0U, 1U, 8U, "TUNED ");
    dsd_trunk_tuning_generation_advance();
    send_block(opts, st, 0U, 0U, "AB");
    expect_learned(st, 3333333U, NULL, "a retune between header and block");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 4444444U);
    send_header(opts, st, 0U, 1U, 20U, "CONFLI");
    send_block(opts, st, 0U, 0U, "AAAAAAA");
    send_block(opts, st, 0U, 0U, "BBBBBBB");
    send_block(opts, st, 0U, 1U, "CCCCCCC");
    expect_learned(st, 4444444U, NULL, "a contradicting block ends the assembly");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 5555555U);
    send_header(opts, st, 0U, 1U, 8U, "FAILED");
    dmr_talker_alias_lc_invalidate(st, 0U);
    send_block(opts, st, 0U, 0U, "XY");
    expect_learned(st, 5555555U, NULL, "a failed checksum ends the assembly");
    free_state(st);
}

/* No call, an empty alias, a malformed UTF-8 one and an impossible length are never learned. */
static void
test_not_learned(dsd_opts* opts) {
    dsd_state* st = new_state();
    send_header(opts, st, 0U, 1U, 6U, "NOCALL");
    expect_learned(st, 0U, NULL, "no call: nothing learned");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 6666666U);
    send_header(opts, st, 0U, 1U, 6U, "      ");
    expect_learned(st, 6666666U, NULL, "an empty alias");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 7777777U);
    const uint8_t bad[] = {'J', 'O', 0xC3U, 'X', 'Y', 'Z'};
    char text[7];
    DSD_MEMCPY(text, bad, sizeof bad);
    text[6] = '\0';
    send_header(opts, st, 0U, 2U, 6U, text);
    expect_learned(st, 7777777U, NULL, "a malformed UTF-8 alias");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 8888888U);
    send_header(opts, st, 0U, 1U, 31U, "TOOLON");
    send_block(opts, st, 0U, 0U, "GGGGGGG");
    send_block(opts, st, 0U, 1U, "GGGGGGG");
    send_block(opts, st, 0U, 2U, "GGGGGGG");
    expect_learned(st, 8888888U, NULL, "31 octets cannot fit the header and three blocks");
    free_state(st);
}

/* A UTF-8 alias (format 2) learned whole, as UTF-8. */
static void
test_learn_utf8(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 0U, 1212121U);
    const uint8_t text[] = {'J', 'o', 's', 0xC3U, 0xA9U, ' '};
    char hdr[7];
    DSD_MEMCPY(hdr, text, sizeof text);
    hdr[6] = '\0';
    send_header(opts, st, 0U, 2U, 5U, hdr);
    expect_learned(st, 1212121U, "Jos\xC3\xA9", "UTF-8 alias learned as UTF-8");
    free_state(st);
}

/* The alias belongs to the system it was heard on (dsd_alias_scope_current()). */
static void
test_learn_scope(dsd_opts* opts) {
    dsd_state* st = new_state();
    dsd_rxid_note_dmr_site(st, DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 3U, 1U, "TIII Small");
    dsd_alias_scope a;
    assert(dsd_alias_scope_current(st, &a) == 1);
    seed_active_call(st, 0U, 4242424U);
    send_header(opts, st, 0U, 1U, 5U, "ALICE");
    expect_learned(st, 4242424U, "ALICE", "learned on system A");

    dsd_rxid_reset(st);
    dsd_rxid_note_dmr_site(st, DSD_RXID_DMR_TRUNK_TIER3, 1, 13U, 3U, 1U, "TIII Small");
    expect_learned(st, 4242424U, NULL, "system A's alias does not name the radio on system B");
    char mode[8];
    char name[DSD_SOURCE_ALIAS_NAME_MAX];
    if (!dsd_source_label_lookup_scoped(st, &a, 4242424U, mode, sizeof mode, name, sizeof name)
        || strcmp(name, "ALICE") != 0) {
        DSD_FPRINTF(stderr, "FAIL: system A's scope keeps its alias\n");
        g_failures++;
    }
    free_state(st);
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    assert(opts != NULL);
    test_learn_complete(opts);
    test_learn_header_only(opts);
    test_missing_and_out_of_order(opts);
    test_binding_breaks(opts);
    test_not_learned(opts);
    test_learn_utf8(opts);
    test_learn_scope(opts);
    free(opts);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DMR_ALIAS_LEARN: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "DMR_ALIAS_LEARN: PASS\n");
    return 0;
}
