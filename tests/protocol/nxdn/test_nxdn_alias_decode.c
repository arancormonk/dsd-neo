// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused checks for NXDN alias helper decode paths.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>
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
    build_prop_msg(bits, 2U, 2U, "NAME");
    nxdn_alias_decode_prop(&opts, &state, bits, 1U);
    rc |= expect_str("prop-partial", state.generic_talker_alias[0], "NAME");

    build_prop_msg(bits, 1U, 2U, "TEST");
    nxdn_alias_decode_prop(&opts, &state, bits, 1U);
    rc |= expect_str("prop-assembled", state.generic_talker_alias[0], "TESTNAME");

    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof(state.generic_talker_alias[0]), "%s", "KEEP");
    build_prop_msg(bits, 1U, 1U, "FAIL");
    nxdn_alias_decode_prop(&opts, &state, bits, 0U);
    rc |= expect_str("prop-crc-gate", state.generic_talker_alias[0], "KEEP");

    {
        uint8_t packed[12];
        build_arib_packed_alias8(packed, "ARIBTEST", 0x84201F67U);
        build_arib_msg(bits, 1U, 2U, &packed[0]);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
        rc |= expect_str("arib-partial", state.generic_talker_alias[0], "KEEP");

        build_arib_msg(bits, 2U, 2U, &packed[6]);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
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
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
        rc |= expect_str("arib-restart-stale-seed", state.generic_talker_alias[0], "BASE");

        build_arib_msg(bits, 1U, 2U, &fresh_packed[0]);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
        rc |= expect_str("arib-restart-no-mix", state.generic_talker_alias[0], "BASE");
        rc |= expect_u8("arib-restart-mask", state.nxdn_alias_arib_seen_mask, 0x01U);

        build_arib_msg(bits, 2U, 2U, &fresh_packed[6]);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
    }
    rc |= expect_str("arib-restart-assembled", state.generic_talker_alias[0], "GOOD1234");
    rc |= expect_u8("arib-restart-reset-mask", state.nxdn_alias_arib_seen_mask, 0U);
    rc |= expect_u8("arib-restart-reset-total", state.nxdn_alias_arib_total_segments, 0U);

    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof(state.generic_talker_alias[0]), "%s", "STABLE");
    {
        static const uint8_t total3_seg1[6] = {'B', 'A', 'D', 'A', 'L', 'I'};
        static const uint8_t total2_seg2[6] = {'A', 'S', 0x11, 0x22, 0x33, 0x44};

        build_arib_msg(bits, 1U, 3U, total3_seg1);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
        rc |= expect_str("arib-total-mismatch-seed", state.generic_talker_alias[0], "STABLE");

        build_arib_msg(bits, 2U, 2U, total2_seg2);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
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
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
        rc |= expect_str("arib-midseq-seed", state.generic_talker_alias[0], "HOLD");

        build_arib_msg(bits, 2U, 2U, &fresh_packed[6]);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
        rc |= expect_str("arib-midseq-no-mix", state.generic_talker_alias[0], "HOLD");
        rc |= expect_u8("arib-midseq-reset-mask", state.nxdn_alias_arib_seen_mask, 0U);
        rc |= expect_u8("arib-midseq-reset-total", state.nxdn_alias_arib_total_segments, 0U);

        build_arib_msg(bits, 1U, 2U, &fresh_packed[0]);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
        rc |= expect_str("arib-midseq-clean-partial", state.generic_talker_alias[0], "HOLD");

        build_arib_msg(bits, 2U, 2U, &fresh_packed[6]);
        nxdn_alias_decode_arib(&opts, &state, bits, 1U);
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

    if (rc == 0) {
        printf("NXDN_ALIAS_DECODE: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
