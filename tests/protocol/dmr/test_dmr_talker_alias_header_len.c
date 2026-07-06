// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_unicode_supported;

// Minimal stubs needed to link `src/core/util/dsd_alias.c` in isolation.
uint16_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len) {
    (void)buf;
    (void)len;
    return 0;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t out = 0;
    const uint8_t* p = BufferIn;
    uint32_t n = BitLength;

    while (n--) {
        out = (out << 1) | (uint64_t)(*p++ & 1u);
    }
    return out;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_unicode_supported(void) {
    return g_unicode_supported;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors) {
    (void)opts;
    (void)state;
    (void)lcw_bits;
    (void)irrecoverable_errors;
}

static void
bytes_to_bits_msb(uint8_t* bits_out, size_t bits_out_sz, const uint8_t* in, size_t in_sz) {
    const size_t need = in_sz * 8u;
    if (bits_out_sz < need) {
        DSD_FPRINTF(stderr, "bytes_to_bits_msb: need=%zu have=%zu\n", need, bits_out_sz);
        exit(2);
    }

    for (size_t i = 0; i < in_sz; i++) {
        for (size_t b = 0; b < 8; b++) {
            bits_out[i * 8u + b] = (uint8_t)((in[i] >> (7u - b)) & 1u);
        }
    }
}

static void
value_to_bits_msb(uint8_t* bits_out, size_t bit_offset, size_t bits_out_sz, uint32_t value, uint8_t bit_count) {
    if (bit_offset + bit_count > bits_out_sz) {
        DSD_FPRINTF(stderr, "value_to_bits_msb: need=%zu have=%zu\n", bit_offset + bit_count, bits_out_sz);
        exit(2);
    }

    for (uint8_t b = 0; b < bit_count; b++) {
        bits_out[bit_offset + b] = (uint8_t)((value >> (bit_count - 1u - b)) & 1u);
    }
}

static int
expect_has_substr(const char* buf, const char* needle, const char* tag) {
    if (!buf || !strstr(buf, needle)) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, buf ? buf : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_str(const char* got, const char* want, const char* tag) {
    if (!got || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got ? got : "(null)", want);
        return 1;
    }
    return 0;
}

static int
expect_u8(uint8_t got, uint8_t want, const char* tag) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%02X want 0x%02X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_policy_name(const dsd_state* st, uint32_t id, const char* want, const char* tag) {
    dsd_tg_policy_lookup lookup;
    if (dsd_tg_policy_lookup_id(st, id, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
        || strcmp(lookup.entry.name, want) != 0) {
        DSD_FPRINTF(stderr, "%s: missing policy id=%u name='%s'\n", tag, id, want);
        return 1;
    }
    return 0;
}

static int
test_direct_dmr_alias_decoders(dsd_opts* opts, dsd_state* st) {
    int rc = 0;

    DSD_MEMSET(st->dmr_pdu_sf[0], 0, sizeof(st->dmr_pdu_sf[0]));
    value_to_bits_msb(st->dmr_pdu_sf[0], 0, sizeof(st->dmr_pdu_sf[0]), 'K', 7);
    value_to_bits_msb(st->dmr_pdu_sf[0], 7, sizeof(st->dmr_pdu_sf[0]), ',', 7);
    value_to_bits_msb(st->dmr_pdu_sf[0], 14, sizeof(st->dmr_pdu_sf[0]), '7', 7);
    st->lastsrc = 700001u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;
    dmr_talker_alias_lc_decode(opts, st, 0, 0, 7, 3);
    rc |= expect_str(st->generic_talker_alias[0], "Talker Alias: K,7; ", "iso7 generic alias");
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "K,7; ", "iso7 event alias");
    if (st->generic_talker_alias_src[0] != (uint32_t)st->lastsrc) {
        DSD_FPRINTF(stderr, "iso7 generic alias source mismatch\n");
        rc = 1;
    }

    DSD_MEMSET(st->dmr_pdu_sf[1], 0, sizeof(st->dmr_pdu_sf[1]));
    value_to_bits_msb(st->dmr_pdu_sf[1], 0, sizeof(st->dmr_pdu_sf[1]), 'A', 16);
    value_to_bits_msb(st->dmr_pdu_sf[1], 16, sizeof(st->dmr_pdu_sf[1]), 0, 16);
    value_to_bits_msb(st->dmr_pdu_sf[1], 32, sizeof(st->dmr_pdu_sf[1]), 0x0100U, 16);
    st->lastsrcR = 700002u;
    st->event_history_s[1].Event_History_Items[0].source_id = st->lastsrcR;
    dmr_talker_alias_lc_decode(opts, st, 1, 1, 16, 3);
    rc |= expect_str(st->generic_talker_alias[1], "Talker Alias: A *; ", "utf16 generic alias");
    rc |= expect_str(st->event_history_s[1].Event_History_Items[0].alias, "A *; ", "utf16 event alias");
    if (st->generic_talker_alias_src[1] != (uint32_t)st->lastsrcR) {
        DSD_FPRINTF(stderr, "utf16 generic alias source mismatch\n");
        rc = 1;
    }

    return rc;
}

static int
test_harris_l3h_alias_state(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    uint8_t input[12];
    DSD_MEMSET(input, 0, sizeof input);
    input[4] = 'B';
    input[5] = 'O';
    input[6] = 'B';
    input[7] = ',';
    input[8] = 0x01;

    DSD_MEMSET(st->dmr_pdu_sf[0], 0xA5, sizeof(st->dmr_pdu_sf[0]));
    st->lastsrc = 700003u;
    st->lasttg = 42u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;

    l3h_embedded_alias_decode(opts, st, 0, 8, input);

    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "BOB. ", "l3h event alias");
    if (st->dmr_pdu_sf[0][0] != 0) {
        DSD_FPRINTF(stderr, "l3h decode did not clear alias storage\n");
        rc = 1;
    }

    rc |= expect_policy_name(st, st->lastsrc, "BOB. ", "l3h runtime alias policy");

    DSD_MEMSET(input, 0, sizeof input);
    input[4] = 'R';
    input[5] = 'O';
    input[6] = ',';
    input[7] = 0x01;
    input[8] = '2';

    DSD_MEMSET(st->dmr_pdu_sf[1], 0xA5, sizeof(st->dmr_pdu_sf[1]));
    st->lastsrcR = 700004u;
    st->lasttgR = 43u;
    st->event_history_s[1].Event_History_Items[0].source_id = st->lastsrcR;

    l3h_embedded_alias_decode(opts, st, 1, 8, input);

    rc |= expect_str(st->event_history_s[1].Event_History_Items[0].alias, "RO. 2", "l3h slot1 event alias");
    rc |= expect_u8(st->dmr_pdu_sf[1][0], 0U, "l3h slot1 clears storage");
    rc |= expect_policy_name(st, st->lastsrcR, "RO. 2", "l3h slot1 runtime alias policy");

    return rc;
}

static void
build_l3h_alias_lcw(uint8_t bits[72], uint8_t opcode, const char payload[7]) {
    DSD_MEMSET(bits, 0, 72);
    value_to_bits_msb(bits, 0, 72, opcode, 8);
    value_to_bits_msb(bits, 8, 72, 0xA4U, 8);
    for (size_t i = 0; i < 7U; i++) {
        value_to_bits_msb(bits, 16U + (i * 8U), 72, (uint8_t)payload[i], 8);
    }
}

static int
expect_no_policy(const dsd_state* st, uint32_t id, const char* tag) {
    dsd_tg_policy_lookup lookup;
    if (dsd_tg_policy_lookup_id(st, id, &lookup) == 0 && lookup.match == DSD_TG_POLICY_MATCH_EXACT) {
        DSD_FPRINTF(stderr, "%s: unexpected policy id=%u name='%s'\n", tag, id, lookup.entry.name);
        return 1;
    }
    return 0;
}

static int
test_harris_l3h_phase1_block_assembly(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    uint8_t lcw[72];

    st->lastsrc = 710001u;
    st->lasttg = 44u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;
    st->event_history_s[0].Event_History_Items[0].target_id = st->lasttg;
    st->event_history_s[0].Event_History_Items[0].alias[0] = '\0';

    build_l3h_alias_lcw(lcw, 0x32U, "ALPHA  ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "", "l3h block1 waits for block2");

    build_l3h_alias_lcw(lcw, 0x33U, "UNIT   ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "ALPHAUNIT", "l3h block1+2 alias");
    rc |= expect_no_policy(st, st->lastsrc, "l3h block1+2 no policy");

    build_l3h_alias_lcw(lcw, 0x34U, "ALPHA  ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "ALPHAUNIT", "l3h duplicate block3");
    rc |= expect_no_policy(st, st->lastsrc, "l3h block1+2+3 no policy");

    build_l3h_alias_lcw(lcw, 0x35U, "7      ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "ALPHAUNIT7", "l3h unique block4");
    rc |= expect_policy_name(st, st->lastsrc, "ALPHAUNIT7", "l3h complete policy");

    st->lastsrc = 710004u;
    st->lasttg = 47u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;
    st->event_history_s[0].Event_History_Items[0].target_id = st->lasttg;
    st->event_history_s[0].Event_History_Items[0].alias[0] = '\0';

    build_l3h_alias_lcw(lcw, 0x32U, "ECHO   ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    build_l3h_alias_lcw(lcw, 0x33U, "ONE    ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "ECHOONE", "l3h repeated block1+2 alias");
    rc |= expect_no_policy(st, st->lastsrc, "l3h repeated block1+2 no policy");
    build_l3h_alias_lcw(lcw, 0x34U, "ECHO   ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    build_l3h_alias_lcw(lcw, 0x35U, "ONE    ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "ECHOONE", "l3h repeated complete alias");
    rc |= expect_policy_name(st, st->lastsrc, "ECHOONE", "l3h repeated complete policy");

    st->lastsrc = 710005u;
    st->lasttg = 48u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;
    st->event_history_s[0].Event_History_Items[0].target_id = st->lasttg;
    st->event_history_s[0].Event_History_Items[0].alias[0] = '\0';

    build_l3h_alias_lcw(lcw, 0x32U, "ALPHA  ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    build_l3h_alias_lcw(lcw, 0x33U, "A      ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "ALPHAA", "l3h contained fragment retained");
    rc |= expect_no_policy(st, st->lastsrc, "l3h contained fragment no policy");
    build_l3h_alias_lcw(lcw, 0x34U, "ALPHA  ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    build_l3h_alias_lcw(lcw, 0x35U, "A      ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "ALPHAA",
                     "l3h contained fragment repeated complete alias");
    rc |= expect_policy_name(st, st->lastsrc, "ALPHAA", "l3h contained fragment complete policy");

    st->lastsrc = 710003u;
    st->lasttg = 46u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;
    st->event_history_s[0].Event_History_Items[0].target_id = st->lasttg;
    st->event_history_s[0].Event_History_Items[0].alias[0] = '\0';

    build_l3h_alias_lcw(lcw, 0x33U, "STALE  ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "", "l3h stray block2 ignored");
    rc |= expect_no_policy(st, st->lastsrc, "l3h stray block2 no policy");

    st->lastsrc = 710002u;
    st->lasttg = 45u;
    st->event_history_s[0].Event_History_Items[0].source_id = 999999u;
    st->event_history_s[0].Event_History_Items[0].target_id = st->lasttg;
    st->event_history_s[0].Event_History_Items[0].alias[0] = '\0';

    build_l3h_alias_lcw(lcw, 0x32U, "BAD    ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    build_l3h_alias_lcw(lcw, 0x33U, "CACHE  ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "", "l3h mismatch no event alias");
    rc |= expect_no_policy(st, st->lastsrc, "l3h mismatch no policy");

    st->lastsrc = 710006u;
    st->lasttg = 49u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;
    st->event_history_s[0].Event_History_Items[0].target_id = st->lasttg;
    st->event_history_s[0].Event_History_Items[0].alias[0] = '\0';

    build_l3h_alias_lcw(lcw, 0x33U, "GOOD   ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "",
                     "l3h deferred mismatch clears block1 before stray block2");
    rc |= expect_no_policy(st, st->lastsrc, "l3h deferred mismatch stray block2 no policy");

    build_l3h_alias_lcw(lcw, 0x32U, "GOOD   ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    build_l3h_alias_lcw(lcw, 0x33U, "TWO    ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "GOODTWO",
                     "l3h current sequence recovers after deferred mismatch");
    rc |= expect_no_policy(st, st->lastsrc, "l3h recovered partial sequence no policy");

    st->lastsrc = 710007u;
    st->lasttg = 50u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;
    st->event_history_s[0].Event_History_Items[0].target_id = st->lasttg;
    st->event_history_s[0].Event_History_Items[0].alias[0] = '\0';

    build_l3h_alias_lcw(lcw, 0x32U, "OLD    ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);

    st->lastsrc = 710008u;
    st->lasttg = 51u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;
    st->event_history_s[0].Event_History_Items[0].target_id = st->lasttg;
    st->event_history_s[0].Event_History_Items[0].alias[0] = '\0';

    build_l3h_alias_lcw(lcw, 0x33U, "NEW    ");
    l3h_embedded_alias_blocks_phase1(opts, st, 0, lcw);
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "",
                     "l3h source change clears stale block1 before block2");
    rc |= expect_no_policy(st, st->lastsrc, "l3h source change stray block2 no policy");

    return rc;
}

static int
test_apx_alias_phase_storage(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    uint8_t header1[72];
    uint8_t block1[72];
    uint8_t header2[136];
    uint8_t block2[144];

    DSD_MEMSET(header1, 0, sizeof(header1));
    value_to_bits_msb(header1, 0, sizeof(header1), 0x1590U, 16);
    value_to_bits_msb(header1, 32, sizeof(header1), 2U, 8);
    value_to_bits_msb(header1, 56, sizeof(header1), 0x0AU, 4);
    DSD_MEMSET(st->dmr_pdu_sf[0], 0xA5, sizeof(st->dmr_pdu_sf[0]));
    apx_embedded_alias_header_phase1(opts, st, 0, header1);
    rc |= expect_u8(st->dmr_pdu_sf[0][0], header1[0], "apx-p1-header-copy-first");
    rc |= expect_u8(st->dmr_pdu_sf[0][32], header1[32], "apx-p1-header-len-copy");
    rc |= expect_u8(st->dmr_pdu_sf[0][71], header1[71], "apx-p1-header-copy-last");

    DSD_MEMSET(block1, 0, sizeof(block1));
    value_to_bits_msb(block1, 16, sizeof(block1), 1U, 8);
    value_to_bits_msb(block1, 24, sizeof(block1), 0x0AU, 4);
    for (size_t i = 28U; i < sizeof(block1); i++) {
        block1[i] = (uint8_t)(i & 1U);
    }
    apx_embedded_alias_blocks_phase1(opts, st, 0, block1);
    for (size_t i = 0U; i < 44U; i++) {
        rc |= expect_u8(st->dmr_pdu_sf[0][72U + i], block1[28U + i], "apx-p1-block-copy");
    }

    apx_embedded_alias_header_phase1(opts, st, 0, header1);
    DSD_MEMSET(block1, 0, sizeof(block1));
    value_to_bits_msb(block1, 16, sizeof(block1), 2U, 8);
    value_to_bits_msb(block1, 24, sizeof(block1), 0x0AU, 4);
    apx_embedded_alias_blocks_phase1(opts, st, 0, block1);
    rc |= expect_u8(st->dmr_pdu_sf[0][0], 0U, "apx-p1-out-of-order-clears-first");

    DSD_MEMSET(st->dmr_pdu_sf[0], 0xA5, sizeof(st->dmr_pdu_sf[0]));
    DSD_MEMSET(block1, 0, sizeof(block1));
    apx_embedded_alias_blocks_phase1(opts, st, 0, block1);
    rc |= expect_u8(st->dmr_pdu_sf[0][0], 0U, "apx-p1-missing-header-clears-first");
    rc |= expect_u8(st->dmr_pdu_sf[0][120], 0U, "apx-p1-missing-header-clears-later");

    DSD_MEMSET(header2, 0, sizeof(header2));
    value_to_bits_msb(header2, 0, sizeof(header2), 0x9190U, 16);
    value_to_bits_msb(header2, 40, sizeof(header2), 3U, 8);
    value_to_bits_msb(header2, 56, sizeof(header2), 1U, 8);
    value_to_bits_msb(header2, 64, sizeof(header2), 0x0BU, 4);
    DSD_MEMSET(st->dmr_pdu_sf[1], 0xA5, sizeof(st->dmr_pdu_sf[1]));
    apx_embedded_alias_header_phase2(opts, st, 1, header2);
    rc |= expect_u8(st->dmr_pdu_sf[1][0], header2[0], "apx-p2-header-copy-first");
    rc |= expect_u8(st->dmr_pdu_sf[1][32], header2[40], "apx-p2-header-len-repacked");
    rc |= expect_u8(st->dmr_pdu_sf[1][56], header2[56], "apx-p2-header-tail-copy");

    DSD_MEMSET(block2, 0, sizeof(block2));
    value_to_bits_msb(block2, 24, sizeof(block2), 1U, 8);
    value_to_bits_msb(block2, 32, sizeof(block2), 0x0BU, 4);
    for (size_t i = 36U; i < 136U; i++) {
        block2[i] = (uint8_t)((i + 1U) & 1U);
    }
    apx_embedded_alias_blocks_phase2(opts, st, 1, block2);
    for (size_t i = 0U; i < 100U; i++) {
        rc |= expect_u8(st->dmr_pdu_sf[1][136U + i], block2[36U + i], "apx-p2-block-copy");
    }

    apx_embedded_alias_header_phase2(opts, st, 1, header2);
    DSD_MEMSET(block2, 0, sizeof(block2));
    value_to_bits_msb(block2, 24, sizeof(block2), 1U, 8);
    value_to_bits_msb(block2, 32, sizeof(block2), 0x0CU, 4);
    apx_embedded_alias_blocks_phase2(opts, st, 1, block2);
    rc |= expect_u8(st->dmr_pdu_sf[1][0], 0U, "apx-p2-sequence-mismatch-clears-first");

    DSD_MEMSET(st->dmr_pdu_sf[1], 0xA5, sizeof(st->dmr_pdu_sf[1]));
    DSD_MEMSET(block2, 0, sizeof(block2));
    apx_embedded_alias_blocks_phase2(opts, st, 1, block2);
    rc |= expect_u8(st->dmr_pdu_sf[1][0], 0U, "apx-p2-missing-header-clears-first");
    rc |= expect_u8(st->dmr_pdu_sf[1][200], 0U, "apx-p2-missing-header-clears-later");
    return rc;
}

static int
test_tait_iso7_alias_sanitizes_and_policies(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    uint8_t tait_bits[64];
    DSD_MEMSET(tait_bits, 0, sizeof tait_bits);
    value_to_bits_msb(tait_bits, 16, sizeof(tait_bits), 'A', 7);
    value_to_bits_msb(tait_bits, 23, sizeof(tait_bits), ',', 7);
    value_to_bits_msb(tait_bits, 30, sizeof(tait_bits), 0x1FU, 7);
    value_to_bits_msb(tait_bits, 37, sizeof(tait_bits), 'Z', 7);

    st->lastsrc = 700005u;
    st->nac = 0x293u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;

    tait_iso7_embedded_alias_decode(opts, st, 0, 4, tait_bits);

    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "A. Z", "tait event alias");
    rc |= expect_policy_name(st, st->lastsrc, "A. Z", "tait runtime alias policy");

    return rc;
}

static int
test_dmr_alias_block_guards_leave_state(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    uint8_t lc_bits[80];
    DSD_MEMSET(lc_bits, 0, sizeof lc_bits);

    st->dmr_alias_char_size[0] = 0;
    DSD_MEMSET(st->dmr_pdu_sf[0], 0xA5, sizeof(st->dmr_pdu_sf[0]));
    dmr_talker_alias_lc_blocks(opts, st, 0, 0, lc_bits);
    rc |= expect_u8(st->dmr_pdu_sf[0][0], 0xA5U, "dmr alias missing header leaves storage");
    rc |= expect_u8(st->dmr_pdu_sf[0][48], 0xA5U, "dmr alias missing header leaves later storage");

    st->dmr_alias_char_size[0] = 8;
    DSD_MEMSET(st->dmr_pdu_sf[0], 0x5A, sizeof(st->dmr_pdu_sf[0]));
    dmr_talker_alias_lc_blocks(opts, st, 0, 4, lc_bits);
    rc |= expect_u8(st->dmr_pdu_sf[0][48], 0x5AU, "dmr alias invalid block leaves payload start");
    rc |= expect_u8(st->dmr_pdu_sf[0][104], 0x5AU, "dmr alias invalid block leaves next payload");

    return rc;
}

static int
test_dmr_alias_header_and_block_format_variants(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    uint8_t lc_bits[96];

    DSD_MEMSET(lc_bits, 0, sizeof lc_bits);
    value_to_bits_msb(lc_bits, 16, sizeof(lc_bits), 0U, 2);
    value_to_bits_msb(lc_bits, 18, sizeof(lc_bits), 5U, 5);
    value_to_bits_msb(lc_bits, 23, sizeof(lc_bits), 'H', 7);
    value_to_bits_msb(lc_bits, 30, sizeof(lc_bits), 'I', 7);
    st->lastsrc = 700006u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;

    dmr_talker_alias_lc_header(opts, st, 0, lc_bits);
    rc |= expect_u8(st->dmr_alias_char_size[0], 7U, "dmr alias iso7 char size");
    rc |= expect_u8(st->dmr_alias_block_len[0], 5U, "dmr alias iso7 block len");
    rc |= expect_str(st->generic_talker_alias[0], "Talker Alias: HI; ", "dmr alias iso7 header text");
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "HI; ", "dmr alias iso7 event text");

    DSD_MEMSET(lc_bits, 0, sizeof lc_bits);
    value_to_bits_msb(lc_bits, 16, sizeof(lc_bits), 'J', 7);
    value_to_bits_msb(lc_bits, 23, sizeof(lc_bits), 'K', 7);
    dmr_talker_alias_lc_blocks(opts, st, 0, 0, lc_bits);
    rc |= expect_str(st->generic_talker_alias[0], "Talker Alias: HI     JK; ", "dmr alias iso7 block text");
    rc |= expect_u8(st->dmr_pdu_sf[0][49], lc_bits[16], "dmr alias iso7 block copy");

    DSD_MEMSET(lc_bits, 0, sizeof lc_bits);
    value_to_bits_msb(lc_bits, 16, sizeof(lc_bits), 3U, 2);
    value_to_bits_msb(lc_bits, 18, sizeof(lc_bits), 3U, 5);
    value_to_bits_msb(lc_bits, 24, sizeof(lc_bits), 'Q', 16);
    value_to_bits_msb(lc_bits, 40, sizeof(lc_bits), 0U, 16);
    value_to_bits_msb(lc_bits, 56, sizeof(lc_bits), 0x0100U, 16);
    st->lastsrcR = 700007u;
    st->event_history_s[1].Event_History_Items[0].source_id = st->lastsrcR;

    dmr_talker_alias_lc_header(opts, st, 1, lc_bits);
    rc |= expect_u8(st->dmr_alias_char_size[1], 16U, "dmr alias utf16 char size");
    rc |= expect_u8(st->dmr_alias_block_len[1], 3U, "dmr alias utf16 block len");
    rc |= expect_str(st->generic_talker_alias[1], "Talker Alias: Q *; ", "dmr alias utf16 header text");
    rc |= expect_str(st->event_history_s[1].Event_History_Items[0].alias, "Q *; ", "dmr alias utf16 event text");

    DSD_MEMSET(st->dmr_pdu_sf[1], 0, sizeof(st->dmr_pdu_sf[1]));
    value_to_bits_msb(st->dmr_pdu_sf[1], 0, sizeof(st->dmr_pdu_sf[1]), 'Z', 16);
    st->dmr_alias_char_size[1] = 16;
    DSD_MEMSET(lc_bits, 0, sizeof lc_bits);
    value_to_bits_msb(lc_bits, 16, sizeof(lc_bits), '1', 16);
    value_to_bits_msb(lc_bits, 32, sizeof(lc_bits), '2', 16);
    value_to_bits_msb(lc_bits, 48, sizeof(lc_bits), '3', 16);
    dmr_talker_alias_lc_blocks(opts, st, 1, 0, lc_bits);
    rc |= expect_str(st->generic_talker_alias[1], "Talker Alias: Z  123; ", "dmr alias utf16 block text");
    rc |= expect_u8(st->dmr_pdu_sf[1][48], lc_bits[16], "dmr alias utf16 block copy");

    return rc;
}

static int
test_dmr_alias_invalid_characters_and_unicode_branch(dsd_opts* opts, dsd_state* st) {
    int rc = 0;

    DSD_MEMSET(st->dmr_pdu_sf[0], 0, sizeof(st->dmr_pdu_sf[0]));
    value_to_bits_msb(st->dmr_pdu_sf[0], 0, sizeof(st->dmr_pdu_sf[0]), 'A', 8);
    value_to_bits_msb(st->dmr_pdu_sf[0], 8, sizeof(st->dmr_pdu_sf[0]), 0x7FU, 8);
    value_to_bits_msb(st->dmr_pdu_sf[0], 16, sizeof(st->dmr_pdu_sf[0]), 0xFFU, 8);
    value_to_bits_msb(st->dmr_pdu_sf[0], 24, sizeof(st->dmr_pdu_sf[0]), 'B', 8);
    st->lastsrc = 700008u;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;

    dmr_talker_alias_lc_decode(opts, st, 0, 2, 8, 4);
    rc |= expect_str(st->generic_talker_alias[0], "Talker Alias: A  B; ", "dmr alias iso8 invalid chars");
    rc |= expect_str(st->event_history_s[0].Event_History_Items[0].alias, "A  B; ", "dmr alias iso8 event chars");

    DSD_MEMSET(st->dmr_pdu_sf[1], 0, sizeof(st->dmr_pdu_sf[1]));
    value_to_bits_msb(st->dmr_pdu_sf[1], 0, sizeof(st->dmr_pdu_sf[1]), 'C', 16);
    st->lastsrcR = 700009u;
    st->event_history_s[1].Event_History_Items[0].source_id = st->lastsrcR;
    g_unicode_supported = 1;
    dmr_talker_alias_lc_decode(opts, st, 1, 3, 16, 1);
    g_unicode_supported = 0;
    rc |= expect_str(st->generic_talker_alias[1], "Talker Alias: C; ", "dmr alias unicode-supported text");
    rc |= expect_str(st->event_history_s[1].Event_History_Items[0].alias, "C; ", "dmr alias unicode-supported event");

    return rc;
}

static int
test_apx_alias_dump_updates_history_and_policy(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    uint8_t input[160];
    uint8_t decoded[8];
    DSD_MEMSET(input, 0, sizeof input);
    DSD_MEMSET(decoded, 0, sizeof decoded);

    const uint32_t rid = 0x000ABCU;
    value_to_bits_msb(input, 72, sizeof(input), 0x12345U, 20);
    value_to_bits_msb(input, 92, sizeof(input), 0x678U, 12);
    value_to_bits_msb(input, 104, sizeof(input), rid, 24);
    decoded[1] = 'M';
    decoded[3] = ',';
    decoded[5] = 0x01;

    st->event_history_s[0].Event_History_Items[0].source_id = rid;

    apx_embedded_alias_dump(opts, st, 0, 6, input, decoded);

    rc |= expect_has_substr(st->event_history_s[0].Event_History_Items[0].alias, "M. ;  FQ-SUID: 12345:678.000ABC",
                            "apx dump event alias");
    rc |= expect_policy_name(st, rid, "M. ", "apx dump runtime alias policy");

    const uint32_t rid_mismatch = 0x00DEF0U;
    DSD_MEMSET(input, 0, sizeof input);
    DSD_MEMSET(decoded, 0, sizeof decoded);
    value_to_bits_msb(input, 72, sizeof(input), 0x12345U, 20);
    value_to_bits_msb(input, 92, sizeof(input), 0x678U, 12);
    value_to_bits_msb(input, 104, sizeof(input), rid_mismatch, 24);
    decoded[1] = 'X';
    st->event_history_s[0].Event_History_Items[0].source_id = rid_mismatch + 1U;
    apx_embedded_alias_dump(opts, st, 0, 2, input, decoded);
    rc |= expect_no_policy(st, rid_mismatch, "apx dump mismatch no policy");

    return rc;
}

int
main(void) {
    int rc = 0;

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!opts || !st) {
        free(st);
        free(opts);
        return 100;
    }

    st->event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    if (!st->event_history_s) {
        free(st);
        free(opts);
        return 100;
    }

    st->currentslot = 0;
    st->lastsrc = 123;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;

    uint8_t lc_bits[80];
    DSD_MEMSET(lc_bits, 0, sizeof lc_bits);

    // FLCO=0x04 (talker alias header), FID=0, SO byte=0x84 (format=2, bad len=2),
    // alias payload bytes are ASCII "KE8NAX".
    const uint8_t payload[] = {0x04, 0x00, 0x84, 0x4B, 0x45, 0x38, 0x4E, 0x41, 0x58};
    bytes_to_bits_msb(lc_bits, sizeof lc_bits, payload, sizeof payload);

    dmr_talker_alias_lc_header(opts, st, 0, lc_bits);
    rc |= expect_has_substr(st->generic_talker_alias[0], "KE8NAX", "talker_alias_header");
    rc |= test_direct_dmr_alias_decoders(opts, st);
    rc |= test_harris_l3h_alias_state(opts, st);
    rc |= test_harris_l3h_phase1_block_assembly(opts, st);
    rc |= test_apx_alias_phase_storage(opts, st);
    rc |= test_tait_iso7_alias_sanitizes_and_policies(opts, st);
    rc |= test_dmr_alias_block_guards_leave_state(opts, st);
    rc |= test_dmr_alias_header_and_block_format_variants(opts, st);
    rc |= test_dmr_alias_invalid_characters_and_unicode_branch(opts, st);
    rc |= test_apx_alias_dump_updates_history_and_policy(opts, st);

    // Runtime alias handling stores learned aliases through the policy table.
    st->lastsrc = 600000u;
    st->late_entry_mi_fragment[0][0][0] = 0x1122334455667788ULL;

    uint8_t tait_bits[64];
    DSD_MEMSET(tait_bits, 0, sizeof tait_bits);
    tait_iso7_embedded_alias_decode(opts, st, 0, 1, tait_bits);

    dsd_tg_policy_lookup lookup;
    if (dsd_tg_policy_lookup_id(st, st->lastsrc, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT) {
        DSD_FPRINTF(stderr, "runtime alias policy insert failed\n");
        rc = 1;
    }
    if (st->late_entry_mi_fragment[0][0][0] != 0x1122334455667788ULL) {
        DSD_FPRINTF(stderr, "runtime alias handling changed sentinel\n");
        rc = 1;
    }

    dsd_state_ext_free_all(st);
    free(st->event_history_s);
    st->event_history_s = NULL;
    free(st);
    free(opts);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
