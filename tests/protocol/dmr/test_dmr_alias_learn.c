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
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
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

/* The text ended inside what arrived: padding (zeros) after it. A full header waits for block 1. */
static void
test_learn_complete(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 0U, 1234567U);
    send_header(opts, st, 0U, 1U, 8U, "ENGINE");
    expect_learned(st, 1234567U, NULL, "a full header has not visibly ended");
    send_block(opts, st, 0U, 0U, " 7");
    expect_learned(st, 1234567U, "ENGINE 7", "header + padded block 1 learned");
    expect_learned(st, 7654321U, NULL, "nothing else learned");
    free_state(st);
}

/* Padding inside the header: learned with the header alone. */
static void
test_learn_header_only(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 1U, 2345678U);
    send_header(opts, st, 1U, 1U, 4U, "DISP");
    expect_learned(st, 2345678U, "DISP", "a padded header is a whole alias");
    free_state(st);

    /* A six-character callsign sent in the header alone, its Data Length 6: it fills the header exactly and the length
       agrees, so it ended. */
    st = new_state();
    seed_active_call(st, 1U, 2345679U);
    send_header(opts, st, 1U, 1U, 6U, "DISPAT");
    expect_learned(st, 2345679U, "DISPAT", "a header-only callsign whose length agrees");
    free_state(st);

    /* The same six characters with a Data Length of 8: it may go on into block 1, so it waits. */
    st = new_state();
    seed_active_call(st, 1U, 2345680U);
    send_header(opts, st, 1U, 1U, 8U, "DISPAT");
    expect_learned(st, 2345680U, NULL, "a full header whose length says more is coming");
    free_state(st);

    /* Spaces to the edge of the header are no end, whatever the length. */
    st = new_state();
    seed_active_call(st, 1U, 2345681U);
    send_header(opts, st, 1U, 1U, 6U, "BOB   ");
    expect_learned(st, 2345681U, NULL, "space padding is no end");
    free_state(st);
}

/* A radio that understates its Data Length (the KE8NAX sample sends 2): the alias is learned whole once its end shows,
   never cut at the header. */
static void
test_understated_length(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 0U, 3434343U);
    send_header(opts, st, 0U, 1U, 2U, "KE8NAX");
    expect_learned(st, 3434343U, NULL, "KE8NAX not learned at the header");
    send_block(opts, st, 0U, 0U, " JOHN");
    expect_learned(st, 3434343U, "KE8NAX JOHN", "the whole alias learned");
    free_state(st);

    /* A space is part of a name, never its end. */
    st = new_state();
    seed_active_call(st, 0U, 3434344U);
    send_header(opts, st, 0U, 1U, 2U, "ALPHA ");
    expect_learned(st, 3434344U, NULL, "a trailing space is no end");
    send_block(opts, st, 0U, 0U, "BRAVO");
    expect_learned(st, 3434344U, "ALPHA BRAVO", "learned past the space");
    free_state(st);
}

/* A missing block never publishes; a block that completes the set out of order waits for the decode that reaches the
   padding (the next cycle's block 2). All three blocks there: learned even with no padding. */
static void
test_missing_and_out_of_order(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 0U, 3456789U);
    send_header(opts, st, 0U, 1U, 19U, "ALPHA ");
    send_block(opts, st, 0U, 1U, "UNIT 7");
    expect_learned(st, 3456789U, NULL, "block 1 missing: not learned");
    send_block(opts, st, 0U, 0U, "BRAVO 1");
    expect_learned(st, 3456789U, NULL, "block 1's decode has not reached the end");
    send_block(opts, st, 0U, 1U, "UNIT 7");
    expect_learned(st, 3456789U, "ALPHA BRAVO 1UNIT 7", "complete set learned");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 3456790U);
    send_header(opts, st, 0U, 1U, 27U, "ABCDEF");
    send_block(opts, st, 0U, 0U, "GHIJKLM");
    send_block(opts, st, 0U, 1U, "NOPQRST");
    expect_learned(st, 3456790U, NULL, "a full block 2 has not visibly ended");
    send_block(opts, st, 0U, 2U, "UVWXYZ0");
    expect_learned(st, 3456790U, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0", "all three blocks: the longest alias");
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
    send_block(opts, st, 0U, 1U, "CCC");
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

/* A slot keeps another protocol's call whose end never decoded (a P25 or NXDN call on canonical slot 0): a DMR alias
   decoded before its own call opened is no alias of that call's talker. */
static void
test_other_protocols_call(dsd_opts* opts) {
    static const int k_protocols[] = {DSD_SYNC_P25P1_POS, DSD_SYNC_NXDN_POS};
    for (size_t i = 0; i < sizeof k_protocols / sizeof k_protocols[0]; i++) {
        dsd_state* st = new_state();
        const dsd_call_observation other = {
            .protocol = k_protocols[i],
            .slot = 0U,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = 9U,
            .policy_target_id = 9U,
            .ota_source_id = 6666666U,
        };
        assert(dsd_call_state_observe(st, &other, DSD_CALL_BOUNDARY_BEGIN) >= 0);
        send_header(opts, st, 0U, 1U, 4U, "DISP");
        expect_learned(st, 6666666U, NULL, "another protocol's open call takes no DMR alias");
        free_state(st);
    }
}

/* No call, an empty alias, a malformed UTF-8 one and an impossible length are never learned. */
static void
test_not_learned(dsd_opts* opts) {
    dsd_state* st = new_state();
    send_header(opts, st, 0U, 1U, 4U, "NONE");
    expect_learned(st, 0U, NULL, "no call: nothing learned");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 6666666U);
    send_header(opts, st, 0U, 1U, 4U, "    ");
    expect_learned(st, 6666666U, NULL, "an empty alias");
    free_state(st);

    st = new_state();
    seed_active_call(st, 0U, 7777777U);
    send_header(opts, st, 0U, 2U, 5U, "JO\xC3XY");
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
    send_header(opts, st, 0U, 2U, 5U, "Jos\xC3\xA9");
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

/* ---- Through dmr_flco(), where the embedded LC checksum gate lives ---- */

static void
put_lc_bits(uint8_t* bits, size_t offset, uint32_t value, size_t count) {
    for (size_t b = 0; b < count; b++) {
        bits[offset + b] = (uint8_t)((value >> (count - 1U - b)) & 1U);
    }
}

/* An LC through dmr_flco() inside the CRC scope the burst handler gives it: failed when its check failed. */
static void
send_lc(dsd_opts* opts, dsd_state* st, uint8_t bits[96], int crc_ok, uint8_t type) {
    const uint8_t saved = st->event_crc_invalid[0];
    st->event_crc_invalid[0] = (uint8_t)(saved | (crc_ok ? 0U : DSD_CRC_FAILED));
    uint32_t errors = 0;
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_alias_learn_flco") == 0);
    dmr_flco(opts, st, bits, crc_ok ? 1U : 0U, &errors, type);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    st->event_crc_invalid[0] = saved;
}

/* A voice LC (target 2002, source @p source) opens the call; then an ISO 8-bit alias header (length 8, "ENGINE") and
   block 1 (" 7", padded), each passed or failed by its checksum. */
static void
run_flco_alias(dsd_opts* opts, dsd_state* st, uint32_t source, int header_ok, int block_ok) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof bits);
    put_lc_bits(bits, 24, 2002U, 24);
    put_lc_bits(bits, 48, source, 24);
    send_lc(opts, st, bits, 1, 1);
    DSD_MEMSET(bits, 0, sizeof bits);
    put_lc_bits(bits, 2, 0x04U, 6);
    put_lc_bits(bits, 16, 1U, 2);
    put_lc_bits(bits, 18, 8U, 5);
    for (size_t i = 0; i < 6U; i++) {
        put_lc_bits(bits, 24U + (i * 8U), (uint8_t)"ENGINE"[i], 8);
    }
    send_lc(opts, st, bits, header_ok, 3);
    DSD_MEMSET(bits, 0, sizeof bits);
    put_lc_bits(bits, 2, 0x05U, 6);
    put_lc_bits(bits, 16, ' ', 8);
    put_lc_bits(bits, 24, '7', 8);
    send_lc(opts, st, bits, block_ok, 3);
}

static void
test_learn_through_flco(dsd_opts* opts) {
    static const struct {
        int header_ok;
        int block_ok;
        const char* want;
        const char* tag;
    } cases[] = {
        {1, 1, "ENGINE 7", "checked header and block learned through dmr_flco()"},
        {0, 1, NULL, "a header whose checksum failed is never learned"},
        {1, 0, NULL, "a block whose checksum failed is never learned"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_state* st = new_state();
        Event_History_I* history = calloc(2U, sizeof *history);
        assert(history != NULL);
        init_event_history(&history[0], 0, 255);
        init_event_history(&history[1], 0, 255);
        st->event_history_s = history;
        st->currentslot = 0;
        st->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
        run_flco_alias(opts, st, 1001U + (uint32_t)i, cases[i].header_ok, cases[i].block_ok);
        expect_learned(st, 1001U + (uint32_t)i, cases[i].want, cases[i].tag);
        st->event_history_s = NULL;
        free_state(st);
        free(history);
    }
}

/* The talker alias shares dmr_pdu_sf with the data assembler (and the NXDN / P25 decoders on slot 0): a buffer
   cleared or reused between the header and a block leaves pieces that no longer hold what arrived. A data error on
   the other slot runs dmr_reset_blocks(), which clears both slots' buffers; the block after it must not be learned
   with a header that is gone (it read as seven spaces and "7"), and the next whole alias is learned as sent. */
static void
test_buffer_reset_between_pieces(dsd_opts* opts) {
    dsd_state* st = new_state();
    seed_active_call(st, 0U, 1234567U);
    send_header(opts, st, 0U, 1U, 8U, "ENGINE");
    dmr_reset_blocks(opts, st);
    send_block(opts, st, 0U, 0U, " 7");
    expect_learned(st, 1234567U, NULL, "a block after the buffer reset is not learned with the cleared header");
    send_header(opts, st, 0U, 1U, 8U, "ENGINE");
    send_block(opts, st, 0U, 0U, " 7");
    expect_learned(st, 1234567U, "ENGINE 7", "the next whole alias after the reset");
    free_state(st);

    /* Another decoder reusing the buffer (here, writing into the header's bits) between the pieces. */
    st = new_state();
    seed_active_call(st, 1U, 2345678U);
    send_header(opts, st, 1U, 1U, 8U, "ENGINE");
    DSD_MEMSET(st->dmr_pdu_sf[1], 1, 16U);
    send_block(opts, st, 1U, 0U, " 7");
    expect_learned(st, 2345678U, NULL, "a header overwritten by another user of the buffer is not learned");
    free_state(st);
}

/* The alias shown and written into the call's history row (generic_talker_alias, the event row's alias) after the
   same buffer reset: a block whose header was cleared shows nothing new and the last whole alias stays; the next whole
   alias replaces it. */
static void
test_display_after_buffer_reset(dsd_opts* opts) {
    dsd_state* st = new_state();
    Event_History_I* history = calloc(2U, sizeof *history);
    assert(history != NULL);
    init_event_history(&history[0], 0, 255);
    init_event_history(&history[1], 0, 255);
    st->event_history_s = history;
    seed_active_call(st, 0U, 1234567U);
    send_header(opts, st, 0U, 1U, 8U, "ENGINE");
    if (strcmp(st->generic_talker_alias[0], "ENGINE; ") != 0) {
        DSD_FPRINTF(stderr, "FAIL: header shown as '%s', want 'ENGINE; '\n", st->generic_talker_alias[0]);
        g_failures++;
    }
    dmr_reset_blocks(opts, st);
    send_block(opts, st, 0U, 0U, " 7");
    if (strcmp(st->generic_talker_alias[0], "ENGINE; ") != 0) {
        DSD_FPRINTF(stderr, "FAIL: a block after the buffer reset shown as '%s', want the last whole 'ENGINE; '\n",
                    st->generic_talker_alias[0]);
        g_failures++;
    }
    send_header(opts, st, 0U, 1U, 8U, "ENGINE");
    send_block(opts, st, 0U, 0U, " 7");
    if (strcmp(st->generic_talker_alias[0], "ENGINE 7; ") != 0) {
        DSD_FPRINTF(stderr, "FAIL: the next whole alias shown as '%s', want 'ENGINE 7; '\n",
                    st->generic_talker_alias[0]);
        g_failures++;
    }
    st->event_history_s = NULL;
    free_state(st);
    free(history);
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    assert(opts != NULL);
    test_learn_complete(opts);
    test_learn_header_only(opts);
    test_understated_length(opts);
    test_missing_and_out_of_order(opts);
    test_binding_breaks(opts);
    test_other_protocols_call(opts);
    test_not_learned(opts);
    test_learn_utf8(opts);
    test_learn_scope(opts);
    test_learn_through_flco(opts);
    test_buffer_reset_between_pieces(opts);
    test_display_after_buffer_reset(opts);
    free(opts);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DMR_ALIAS_LEARN: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "DMR_ALIAS_LEARN: PASS\n");
    return 0;
}
