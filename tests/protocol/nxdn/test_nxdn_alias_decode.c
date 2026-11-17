// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused checks for NXDN alias helper decode paths.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static void
write_bits_u8(uint8_t* bits, size_t start, uint8_t value, size_t nbits) {
    for (size_t i = 0U; i < nbits; i++) {
        size_t shift = nbits - 1U - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
build_prop_msg(uint8_t* bits, uint8_t block_number, uint8_t total_blocks, const char* chunk4) {
    DSD_MEMSET(bits, 0, 96);
    write_bits_u8(bits, 32U, block_number, 4U);
    write_bits_u8(bits, 36U, total_blocks, 4U);
    for (size_t i = 0U; i < 4U; i++) {
        uint8_t c = (uint8_t)' ';
        if (chunk4 != NULL && chunk4[i] != '\0') {
            c = (uint8_t)chunk4[i];
        }
        write_bits_u8(bits, 40U + (i * 8U), c, 8U);
    }
}

static void
build_arib_msg(uint8_t* bits, uint8_t seg_num, uint8_t seg_total, const uint8_t payload6[6]) {
    DSD_MEMSET(bits, 0, 96);
    write_bits_u8(bits, 16U, seg_num, 4U);
    write_bits_u8(bits, 20U, seg_total, 4U);
    for (size_t i = 0U; i < 6U; i++) {
        write_bits_u8(bits, 24U + (i * 8U), payload6[i], 8U);
    }
}

static void
build_arib_packed_alias8(uint8_t packed12[12], const char alias8[9], uint32_t crc) {
    DSD_MEMSET(packed12, 0, 12U);
    for (size_t i = 0U; i < 8U; i++) {
        packed12[i] = (uint8_t)alias8[i];
    }
    packed12[8] = (uint8_t)((crc >> 24U) & 0xFFU);
    packed12[9] = (uint8_t)((crc >> 16U) & 0xFFU);
    packed12[10] = (uint8_t)((crc >> 8U) & 0xFFU);
    packed12[11] = (uint8_t)(crc & 0xFFU);
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_size(const char* tag, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %zu want %zu\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* A Kenwood alias fragment as the element decoder receives it: message type 0x3F (PROP_FORM), MFID 0x68, 0x8204,
 * fragment number and count, four octets. */
static void
build_prop_elements(uint8_t* bits, uint8_t block_number, uint8_t total_blocks, const uint8_t octets4[4]) {
    DSD_MEMSET(bits, 0, 96);
    write_bits_u8(bits, 0U, 0x3FU, 8U);
    write_bits_u8(bits, 8U, 0x68U, 8U);
    write_bits_u8(bits, 16U, 0x82U, 8U);
    write_bits_u8(bits, 24U, 0x04U, 8U);
    write_bits_u8(bits, 32U, block_number, 4U);
    write_bits_u8(bits, 36U, total_blocks, 4U);
    for (size_t i = 0U; i < 4U; i++) {
        write_bits_u8(bits, 40U + (i * 8U), octets4[i], 8U);
    }
}

/* Fragment @p block_number of @p total_blocks carrying the four characters @p text, through the element dispatch. */
static void
send_fragment(dsd_opts* opts, dsd_state* state, uint8_t block_number, uint8_t total_blocks, const char* text) {
    uint8_t octets[4];
    uint8_t bits[96];
    for (size_t i = 0U; i < 4U; i++) {
        octets[i] = (uint8_t)text[i];
    }
    build_prop_elements(bits, block_number, total_blocks, octets);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
}

/* A voice call from @p source on canonical slot 0: a new call epoch. */
static void
open_call(dsd_state* state, uint64_t source) {
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof(obs));
    obs.protocol = DSD_SYNC_NXDN_POS;
    obs.slot = 0U;
    obs.kind = DSD_CALL_KIND_GROUP_VOICE;
    obs.ota_target_id = 100U;
    obs.policy_target_id = 100U;
    obs.ota_source_id = source;
    (void)dsd_call_state_observe(state, &obs, DSD_CALL_BOUNDARY_BEGIN);
}

static void
set_alias(dsd_state* state, const char* alias) {
    DSD_SNPRINTF(state->generic_talker_alias[0], sizeof(state->generic_talker_alias[0]), "%s", alias);
}

/*
 * Kenwood alias assembly through the element dispatch (B24). Two other decoders agree on four fragments and 14 text
 * octets (SDRTrunk TalkerAliasAssembler, DSD-FME NXDN_decode_Alias). Main published after every fragment, joined the
 * fragments it had (a missing one spliced the rest together), forced any other count to four and kept the two
 * non-text octets of fragment 4.
 */
static int
test_prop_dispatch(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    nxdn_alias_reset(state);
    set_alias(state, "");
    open_call(state, 1001U);

    // Fragments 1 and 3 of 4 publish nothing: no "DISPCH01" from a missing fragment 2.
    send_fragment(opts, state, 1U, 4U, "DISP");
    rc |= expect_str("prop-fragment1-not-published", state->generic_talker_alias[0], "");
    send_fragment(opts, state, 3U, 4U, "CH01");
    rc |= expect_str("prop-spliced-not-published", state->generic_talker_alias[0], "");
    send_fragment(opts, state, 2U, 4U, "ATCH");
    rc |= expect_str("prop-three-of-four-not-published", state->generic_talker_alias[0], "");
    // Fragment 4 completes it; its last two octets are not text.
    send_fragment(opts, state, 4U, 4U, " 7AZ");
    rc |= expect_str("prop-complete-14-octets", state->generic_talker_alias[0], "DISPATCHCH01 7");

    // A different fragment 1 starts another alias; the last complete one stays until that completes.
    send_fragment(opts, state, 1U, 4U, "XXXX");
    rc |= expect_str("prop-last-complete-kept", state->generic_talker_alias[0], "DISPATCHCH01 7");
    send_fragment(opts, state, 2U, 4U, "YYYY");
    send_fragment(opts, state, 3U, 4U, "ZZZZ");
    rc |= expect_str("prop-last-complete-kept-3", state->generic_talker_alias[0], "DISPATCHCH01 7");

    // Fragments from another call never complete this one.
    set_alias(state, "KEEP");
    nxdn_alias_reset(state);
    open_call(state, 1001U);
    send_fragment(opts, state, 1U, 4U, "AAAA");
    send_fragment(opts, state, 2U, 4U, "BBBB");
    open_call(state, 2002U);
    send_fragment(opts, state, 3U, 4U, "CCCC");
    send_fragment(opts, state, 4U, 4U, "DD  ");
    rc |= expect_str("prop-other-call-not-spliced", state->generic_talker_alias[0], "KEEP");
    send_fragment(opts, state, 1U, 4U, "EEEE");
    send_fragment(opts, state, 2U, 4U, "FFFF");
    rc |= expect_str("prop-same-call-complete", state->generic_talker_alias[0], "EEEEFFFFCCCCDD");

    // Nor do fragments heard on another carrier.
    set_alias(state, "KEEP");
    nxdn_alias_reset(state);
    open_call(state, 3003U);
    send_fragment(opts, state, 1U, 4U, "AAAA");
    send_fragment(opts, state, 2U, 4U, "BBBB");
    dsd_trunk_tuning_generation_advance();
    send_fragment(opts, state, 3U, 4U, "CCCC");
    send_fragment(opts, state, 4U, 4U, "DD  ");
    rc |= expect_str("prop-other-carrier-not-spliced", state->generic_talker_alias[0], "KEEP");

    // A conflicting fragment count abandons the assembly; another count is never published.
    set_alias(state, "KEEP");
    nxdn_alias_reset(state);
    open_call(state, 4004U);
    send_fragment(opts, state, 1U, 4U, "AAAA");
    send_fragment(opts, state, 2U, 4U, "BBBB");
    send_fragment(opts, state, 3U, 3U, "CCCC");
    rc |= expect_str("prop-total-3-not-published", state->generic_talker_alias[0], "KEEP");
    send_fragment(opts, state, 3U, 4U, "CCCC");
    send_fragment(opts, state, 4U, 4U, "DD  ");
    rc |= expect_str("prop-conflicting-total-not-spliced", state->generic_talker_alias[0], "KEEP");
    nxdn_alias_reset(state);
    send_fragment(opts, state, 1U, 2U, "TEST");
    send_fragment(opts, state, 2U, 2U, "NAME");
    rc |= expect_str("prop-total-2-not-published", state->generic_talker_alias[0], "KEEP");
    send_fragment(opts, state, 1U, 0U, "TEST");
    rc |= expect_str("prop-total-0-not-published", state->generic_talker_alias[0], "KEEP");

    // A second, different fragment under a number already held restarts the assembly.
    nxdn_alias_reset(state);
    send_fragment(opts, state, 1U, 4U, "AAAA");
    send_fragment(opts, state, 2U, 4U, "BBBB");
    send_fragment(opts, state, 2U, 4U, "QQQQ");
    send_fragment(opts, state, 3U, 4U, "CCCC");
    send_fragment(opts, state, 4U, 4U, "DD  ");
    rc |= expect_str("prop-conflicting-fragment-not-spliced", state->generic_talker_alias[0], "KEEP");
    // ...while a repeat of the one held keeps it.
    nxdn_alias_reset(state);
    send_fragment(opts, state, 1U, 4U, "AAAA");
    send_fragment(opts, state, 2U, 4U, "BBBB");
    send_fragment(opts, state, 2U, 4U, "BBBB");
    send_fragment(opts, state, 3U, 4U, "CCCC");
    send_fragment(opts, state, 4U, 4U, "DD  ");
    rc |= expect_str("prop-repeat-kept", state->generic_talker_alias[0], "AAAABBBBCCCCDD");
    return rc;
}

/* nxdn_alias_complete() answers only for all four fragments. */
static int
test_prop_complete_api(void) {
    int rc = 0;
    nxdn_alias_assembly a;
    char out[NXDN_ALIAS_TEXT_BYTES + 1U];
    DSD_MEMSET(&a, 0, sizeof(a));
    DSD_MEMCPY(a.fragment[0], "NAME", 4U);
    DSD_MEMCPY(a.fragment[1], "ONE ", 4U);
    DSD_MEMCPY(a.fragment[2], "    ", 4U);
    DSD_MEMCPY(a.fragment[3], "  \x41\x5A", 4U);
    a.seen_mask = 0x7U;
    rc |= expect_size("complete-three-of-four", (size_t)nxdn_alias_complete(&a, out, sizeof(out)), 0U);
    rc |= expect_str("complete-three-of-four-empty", out, "");
    a.seen_mask = 0xFU;
    rc |= expect_size("complete-four", (size_t)nxdn_alias_complete(&a, out, sizeof(out)), 1U);
    rc |= expect_str("complete-four-text", out, "NAMEONE");
    rc |= expect_size("complete-short-buffer", (size_t)nxdn_alias_complete(&a, out, NXDN_ALIAS_TEXT_BYTES), 0U);
    return rc;
}

int
main(void) {
    static dsd_state state;
    static dsd_opts opts;
    uint8_t bits[96];
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(bits, 0, sizeof(bits));

    /*
     * The proprietary and ARIB assemblers both receive out-of-order and invalid
     * fragments here. The expected alias stays stable until a complete coherent
     * sequence arrives, which catches stale-fragment mixing regressions.
     */
    build_prop_msg(bits, 2U, 4U, "NAME");
    nxdn_alias_decode_prop(&opts, &state, bits);
    rc |= expect_str("prop-partial", state.generic_talker_alias[0], "");

    rc |= test_prop_dispatch(&opts, &state);
    rc |= test_prop_complete_api();

    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof(state.generic_talker_alias[0]), "%s", "KEEP");

    {
        uint8_t packed[12];
        build_arib_packed_alias8(packed, "ARIBTEST", 0x84201F67U);
        build_arib_msg(bits, 1U, 2U, &packed[0]);
        nxdn_alias_decode_arib(&opts, &state, bits);
        rc |= expect_str("arib-partial", state.generic_talker_alias[0], "KEEP");

        build_arib_msg(bits, 2U, 2U, &packed[6]);
        nxdn_alias_decode_arib(&opts, &state, bits);
    }
    rc |= expect_str("arib-assembled", state.generic_talker_alias[0], "ARIBTEST");
    rc |= expect_u8("arib-seen-reset", state.nxdn_alias_arib_seen_mask, 0U);
    rc |= expect_u8("arib-total-reset", state.nxdn_alias_arib_total_segments, 0U);

    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof(state.generic_talker_alias[0]), "%s", "BASE");
    {
        static const uint8_t stale_seg2[6] = {'Z', 'Z', 0x11, 0x22, 0x33, 0x44};
        uint8_t fresh_packed[12];
        build_arib_packed_alias8(fresh_packed, "GOOD1234", 0x51265003U);

        build_arib_msg(bits, 2U, 2U, stale_seg2);
        nxdn_alias_decode_arib(&opts, &state, bits);
        rc |= expect_str("arib-restart-stale-seed", state.generic_talker_alias[0], "BASE");

        build_arib_msg(bits, 1U, 2U, &fresh_packed[0]);
        nxdn_alias_decode_arib(&opts, &state, bits);
        rc |= expect_str("arib-restart-no-mix", state.generic_talker_alias[0], "BASE");
        rc |= expect_u8("arib-restart-mask", state.nxdn_alias_arib_seen_mask, 0x01U);

        build_arib_msg(bits, 2U, 2U, &fresh_packed[6]);
        nxdn_alias_decode_arib(&opts, &state, bits);
    }
    rc |= expect_str("arib-restart-assembled", state.generic_talker_alias[0], "GOOD1234");
    rc |= expect_u8("arib-restart-reset-mask", state.nxdn_alias_arib_seen_mask, 0U);
    rc |= expect_u8("arib-restart-reset-total", state.nxdn_alias_arib_total_segments, 0U);

    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof(state.generic_talker_alias[0]), "%s", "STABLE");
    {
        static const uint8_t total3_seg1[6] = {'B', 'A', 'D', 'A', 'L', 'I'};
        static const uint8_t total2_seg2[6] = {'A', 'S', 0x11, 0x22, 0x33, 0x44};

        build_arib_msg(bits, 1U, 3U, total3_seg1);
        nxdn_alias_decode_arib(&opts, &state, bits);
        rc |= expect_str("arib-total-mismatch-seed", state.generic_talker_alias[0], "STABLE");

        build_arib_msg(bits, 2U, 2U, total2_seg2);
        nxdn_alias_decode_arib(&opts, &state, bits);
    }
    rc |= expect_str("arib-total-mismatch-no-mix", state.generic_talker_alias[0], "STABLE");
    rc |= expect_u8("arib-total-mismatch-mask", state.nxdn_alias_arib_seen_mask, 0x02U);
    rc |= expect_u8("arib-total-mismatch-total", state.nxdn_alias_arib_total_segments, 2U);

    // A mid-sequence restart must discard the old second segment before assembly.
    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof(state.generic_talker_alias[0]), "%s", "HOLD");
    nxdn_alias_reset(&state);
    {
        uint8_t stale_packed[12];
        uint8_t fresh_packed[12];
        build_arib_packed_alias8(stale_packed, "STALE111", 0x15165977U);
        build_arib_packed_alias8(fresh_packed, "FRESH222", 0x5FD0F4FDU);

        build_arib_msg(bits, 1U, 2U, &stale_packed[0]);
        nxdn_alias_decode_arib(&opts, &state, bits);
        rc |= expect_str("arib-midseq-seed", state.generic_talker_alias[0], "HOLD");

        build_arib_msg(bits, 2U, 2U, &fresh_packed[6]);
        nxdn_alias_decode_arib(&opts, &state, bits);
        rc |= expect_str("arib-midseq-no-mix", state.generic_talker_alias[0], "HOLD");
        rc |= expect_u8("arib-midseq-reset-mask", state.nxdn_alias_arib_seen_mask, 0U);
        rc |= expect_u8("arib-midseq-reset-total", state.nxdn_alias_arib_total_segments, 0U);

        build_arib_msg(bits, 1U, 2U, &fresh_packed[0]);
        nxdn_alias_decode_arib(&opts, &state, bits);
        rc |= expect_str("arib-midseq-clean-partial", state.generic_talker_alias[0], "HOLD");

        build_arib_msg(bits, 2U, 2U, &fresh_packed[6]);
        nxdn_alias_decode_arib(&opts, &state, bits);
    }
    rc |= expect_str("arib-midseq-clean-assembled", state.generic_talker_alias[0], "FRESH222");

    {
        // Shift-JIS decoding is normalized whether full multibyte support exists.
        char out[32];
        static const uint8_t sjis_ascii[] = {'A', 'B', ' ', ' ', 0x00};
        static const uint8_t sjis_halfwidth[] = {0xA1, 0x00};
        static const uint8_t sjis_nihon[] = {0x93, 0xFA, 0x96, 0x7B, 0x00};
        size_t out_len = nxdn_alias_decode_shift_jis_like(sjis_ascii, sizeof(sjis_ascii), out, sizeof(out));
        rc |= expect_str("sjis-ascii-trim", out, "AB");
        rc |= expect_size("sjis-ascii-trim-len", out_len, strlen(out));

        out_len = nxdn_alias_decode_shift_jis_like(sjis_halfwidth, sizeof(sjis_halfwidth), out, sizeof(out));
        rc |= expect_str("sjis-halfwidth", out, "\xEF\xBD\xA1");
        rc |= expect_size("sjis-halfwidth-len", out_len, strlen(out));

        out_len = nxdn_alias_decode_shift_jis_like(sjis_nihon, sizeof(sjis_nihon), out, sizeof(out));
        if (strcmp(out, "\xE6\x97\xA5\xE6\x9C\xAC") != 0 && strcmp(out, "\xEF\xBF\xBD\xEF\xBF\xBD") != 0) {
            DSD_FPRINTF(stderr, "sjis-multibyte: unexpected normalized output '%s'\n", out);
            rc |= 1;
        }
        rc |= expect_size("sjis-multibyte-len", out_len, strlen(out));
    }

    dsd_state_ext_free_all(&state);
    if (rc == 0) {
        printf("NXDN_ALIAS_DECODE: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
