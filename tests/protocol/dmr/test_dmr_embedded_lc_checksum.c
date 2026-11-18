// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * Discussion #602: an embedded LC is protected only by its 5-bit checksum, so clean FEC alone does not vouch for it.
 * A talker alias or GPS LC whose checksum failed must apply nothing -- not to the call it rides on, whose identity was
 * checked, and not to the alias still being assembled -- and an LC that failed its check must not hold a scan.
 */
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int
check(int ok, const char* expression, int line) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL line %d: %s\n", line, expression);
    }
    return !ok;
}

#define CHECK(expression) (failed |= check((expression), #expression, __LINE__))

static int s_scan_calls = 0;

static void
capture_scan_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source, int is_private,
                      int encrypted, int data_call) {
    (void)opts;
    (void)state;
    (void)target;
    (void)source;
    (void)is_private;
    (void)encrypted;
    (void)data_call;
    s_scan_calls++;
}

static void
set_bits(uint8_t* bits, size_t offset, unsigned width, uint32_t value) {
    for (unsigned i = 0; i < width; ++i) {
        bits[offset + i] = (uint8_t)((value >> (width - i - 1U)) & 1U);
    }
}

static void
set_text(uint8_t* bits, size_t offset, const char* text, size_t chars) {
    for (size_t c = 0; c < chars; ++c) {
        set_bits(bits, offset + c * 8U, 8U, (uint8_t)text[c]);
    }
}

static void
build_voice_lc(uint8_t bits[96], uint32_t target, uint32_t source) {
    DSD_MEMSET(bits, 0, 96);
    set_bits(bits, 24, 24, target);
    set_bits(bits, 48, 24, source);
}

// Talker alias header: ISO 8-bit format announcing `chars` characters, carrying the first six.
static void
build_alias_header(uint8_t bits[96], uint8_t fid, unsigned chars, const char text[6]) {
    DSD_MEMSET(bits, 0, 96);
    set_bits(bits, 2, 6, 0x04U);
    set_bits(bits, 8, 8, fid);
    set_bits(bits, 16, 2, 1U);
    set_bits(bits, 18, 5, chars);
    set_text(bits, 24, text, 6);
}

// Talker alias block 0..2 (FLCO 0x05..0x07), seven ISO 8-bit characters each.
static void
build_alias_block(uint8_t bits[96], uint8_t fid, unsigned block, const char text[7]) {
    DSD_MEMSET(bits, 0, 96);
    set_bits(bits, 2, 6, 0x05U + block);
    set_bits(bits, 8, 8, fid);
    set_text(bits, 16, text, 7);
}

static void
build_gps(uint8_t bits[96]) {
    DSD_MEMSET(bits, 0, 96);
    set_bits(bits, 2, 6, 0x08U);
    set_bits(bits, 20, 3, 1);
    set_bits(bits, 23, 1, 1);
    set_bits(bits, 24, 24, 0xC00000);
    set_bits(bits, 48, 1, 1);
    set_bits(bits, 49, 23, 0x600000);
}

// An LC through dmr_flco() inside the CRC scope the burst handler would give it: failed when its check failed.
static void
send_lc(dsd_opts* opts, dsd_state* state, uint8_t bits[96], int crc_ok, uint8_t type) {
    const uint8_t saved = state->event_crc_invalid[0];
    state->event_crc_invalid[0] = (uint8_t)(saved | (crc_ok ? 0U : DSD_CRC_FAILED));
    uint32_t errors = 0;
    dmr_flco(opts, state, bits, crc_ok ? 1U : 0U, &errors, type);
    state->event_crc_invalid[0] = saved;
    dsd_event_sync_slot(opts, state, 0);
}

// CRC policies an embedded LC's checksum must hold under: strict, -F, and DMR's separate relaxed-CRC configuration.
enum { MODE_STRICT = 0, MODE_F, MODE_RELAXED_DEFAULT, MODE_COUNT };

static void
fixture_mode(dsd_opts* opts, dsd_state* state, int mode) {
    dsd_state_ext_free_all(state);
    Event_History_I* history = state->event_history_s;
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    DSD_MEMSET(history, 0, 2U * sizeof *history);
    state->event_history_s = history;
    init_event_history(&state->event_history_s[0], 0, 255);
    init_event_history(&state->event_history_s[1], 0, 255);
    opts->aggressive_framesync = mode == MODE_F ? 0 : 1;
    opts->dmr_crc_relaxed_default = (uint8_t)(mode != MODE_STRICT);
    state->currentslot = 0;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    uint8_t bits[96];
    build_voice_lc(bits, 2002U, 1001U);
    send_lc(opts, state, bits, 1, 1);
}

static void
fixture(dsd_opts* opts, dsd_state* state) {
    fixture_mode(opts, state, MODE_STRICT);
}

static int
call_is_clean(const dsd_state* state) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE && call.crc_invalid == 0U
           && call.ota_source_id == 1001U;
}

static int
test_failed_checksum_applies_nothing(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    for (int run = 0; run < 2 * MODE_COUNT; ++run) {
        const uint8_t fid = (run & 1) ? 0x68U : 0x00U;
        fixture_mode(opts, state, run / 2);
        const unsigned so = state->dmr_so;
        uint8_t bits[96];
        build_alias_header(bits, fid, 13U, "ALPHA ");
        send_lc(opts, state, bits, 0, 3);
        build_alias_block(bits, fid, 0U, "BRAVO12");
        send_lc(opts, state, bits, 0, 3);
        build_gps(bits);
        set_bits(bits, 8, 8, fid);
        send_lc(opts, state, bits, 0, 3);
        const Event_History* row = &state->event_history_s[0].Event_History_Items[0];
        CHECK(state->generic_talker_alias[0][0] == '\0');
        CHECK(row->alias[0] == '\0');
        CHECK(state->dmr_embedded_gps[0][0] == '\0');
        CHECK(row->gps_s[0] == '\0');
        CHECK(state->dmr_alias_char_size[0] == 0U);
        CHECK(call_is_clean(state));
        CHECK(row->crc_invalid == 0U);
        CHECK(strstr(row->event_string, "[CRC ERR]") == NULL);
        // The failed alias/GPS bytes were still claimed by their FLCOs, never read as a voice LC.
        CHECK(state->dmr_so == so);
    }
    return failed;
}

static int
test_checked_alias_and_gps_apply(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    fixture(opts, state);
    uint8_t bits[96];
    build_alias_header(bits, 0x00U, 13U, "ALPHA ");
    send_lc(opts, state, bits, 1, 3);
    build_alias_block(bits, 0x00U, 0U, "BRAVO12");
    send_lc(opts, state, bits, 1, 3);
    build_gps(bits);
    send_lc(opts, state, bits, 1, 3);
    const Event_History* row = &state->event_history_s[0].Event_History_Items[0];
    CHECK(strstr(state->generic_talker_alias[0], "ALPHA BRAVO12") != NULL);
    CHECK(strstr(row->alias, "ALPHA BRAVO12") != NULL);
    CHECK(strstr(state->dmr_embedded_gps[0], "GPS:") != NULL);
    CHECK(strstr(row->gps_s, "GPS:") != NULL);
    CHECK(call_is_clean(state));
    return failed;
}

// A UTF-8 talker alias (Data Format 10, TS 102 361-2 Table 7.25) through dmr_flco() and its checksum gate: "AB" then
// U+65E5 U+672C, the second character split between the header and block 1. The header's length (8) counts octets, so
// the cut character waits for block 1 instead of showing as U+FFFD.
static int
test_checked_utf8_alias_through_flco(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    static const uint8_t head[6] = {'A', 'B', 0xE6, 0x97, 0xA5, 0xE6};
    static const uint8_t blk1[2] = {0x9C, 0xAC};
    fixture(opts, state);
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits(bits, 2, 6, 0x04U);
    set_bits(bits, 16, 2, 2U);
    set_bits(bits, 18, 5, 8U);
    for (size_t i = 0; i < sizeof head; ++i) {
        set_bits(bits, 24U + i * 8U, 8U, head[i]);
    }
    send_lc(opts, state, bits, 1, 3);
    CHECK(strcmp(state->generic_talker_alias[0], "AB\xE6\x97\xA5; ") == 0);
    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits(bits, 2, 6, 0x05U);
    for (size_t i = 0; i < sizeof blk1; ++i) {
        set_bits(bits, 16U + i * 8U, 8U, blk1[i]);
    }
    send_lc(opts, state, bits, 1, 3);
    const Event_History* row = &state->event_history_s[0].Event_History_Items[0];
    CHECK(strcmp(state->generic_talker_alias[0], "AB\xE6\x97\xA5\xE6\x9C\xAC; ") == 0);
    CHECK(strstr(row->alias, "AB\xE6\x97\xA5\xE6\x9C\xAC") != NULL);
    CHECK(call_is_clean(state));
    return failed;
}

static int
test_failed_fragment_abandons_assembly(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    for (int run = 0; run < 2 * MODE_COUNT; ++run) {
        const int header_fails = run & 1;
        fixture_mode(opts, state, run / 2);
        uint8_t bits[96];
        build_alias_header(bits, 0x00U, 20U, "ALPHA ");
        send_lc(opts, state, bits, 1, 3);
        build_alias_block(bits, 0x00U, 0U, "BRAVO12");
        send_lc(opts, state, bits, 1, 3);
        CHECK(strstr(state->generic_talker_alias[0], "ALPHA BRAVO12") != NULL);

        // A failed header (the next cycle's) or a failed block abandons the assembly; the shown alias stays.
        if (header_fails) {
            build_alias_header(bits, 0x00U, 20U, "XXXXXX");
        } else {
            build_alias_block(bits, 0x00U, 1U, "XXXXXXX");
        }
        send_lc(opts, state, bits, 0, 3);
        CHECK(state->dmr_alias_char_size[0] == 0U);
        CHECK(strstr(state->generic_talker_alias[0], "ALPHA BRAVO12") != NULL);
        CHECK(strstr(state->generic_talker_alias[0], "XXXX") == NULL);

        // A checked block cannot continue it without a checked header.
        build_alias_block(bits, 0x00U, 2U, "ZULU999");
        send_lc(opts, state, bits, 1, 3);
        CHECK(strstr(state->generic_talker_alias[0], "ZULU") == NULL);

        // A checked header recovers. With block 0 of this cycle missing, block 1 decodes over the gap, and the
        // abandoned assembly's "BRAVO12" must not come back through it.
        build_alias_header(bits, 0x00U, 20U, "CHARLI");
        send_lc(opts, state, bits, 1, 3);
        CHECK(strstr(state->generic_talker_alias[0], "CHARLI") != NULL);
        build_alias_block(bits, 0x00U, 1U, "DELTA34");
        send_lc(opts, state, bits, 1, 3);
        CHECK(strstr(state->generic_talker_alias[0], "CHARLI") != NULL);
        CHECK(strstr(state->generic_talker_alias[0], "BRAVO") == NULL);
        CHECK(strstr(state->event_history_s[0].Event_History_Items[0].alias, "BRAVO") == NULL);
        CHECK(call_is_clean(state));
    }
    return failed;
}

static int
test_failed_lc_holds_no_scan(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    dsd_trunk_scan_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.dmr_conventional_activity = capture_scan_activity;
    dsd_trunk_scan_hooks_set(&hooks);
    for (uint8_t type = 1U; type <= 3U; type += 2U) {
        fixture(opts, state);
        uint8_t bits[96];
        build_voice_lc(bits, 3003U, 4004U);
        s_scan_calls = 0;
        send_lc(opts, state, bits, 0, type);
        CHECK(s_scan_calls == 0);
        // A checked LC (or, under -F, a RAS-accepted one, which reaches here with the same verdict) still holds.
        send_lc(opts, state, bits, 1, type);
        CHECK(s_scan_calls == 1);
    }
    dsd_trunk_scan_hooks_set(NULL);
    return failed;
}

/* The embedded GPS LC names the destination the call's checked voice LC named, with its group flag (issue #800): one
 * radio's fix sent in a group call and then in a private call is two records. */
static int
test_checked_lc_names_destination(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    fixture(opts, state); /* group voice LC: talkgroup 2002, source 1001 */
    CHECK(dsd_position_feed_start(state) == 0);
    uint8_t bits[96];
    build_gps(bits);
    send_lc(opts, state, bits, 1, 3);
    build_voice_lc(bits, 3003U, 1001U);
    set_bits(bits, 2, 6, 0x03U); /* unit to unit voice channel user */
    send_lc(opts, state, bits, 1, 1);
    build_gps(bits);
    send_lc(opts, state, bits, 1, 3);
    dsd_position_fix recs[4];
    dsd_position_feed_meta meta;
    CHECK(dsd_position_feed_copy_since(state, 0U, recs, 4, &meta) == 2);
    CHECK(recs[0].source_id == 1001U && recs[0].target_id == 2002U
          && (recs[0].flags & DSD_POS_FLAG_TARGET_GROUP) != 0U);
    CHECK(recs[1].source_id == 1001U && recs[1].target_id == 3003U
          && (recs[1].flags & DSD_POS_FLAG_TARGET_GROUP) == 0U);
    return failed;
}

/* A voice LC header begins another transmission even when its own check failed and the call stays ACTIVE (radio 1001's
 * terminator was missed): the embedded GPS LC after it is no longer 1001's, and is anonymous until a checked LC names
 * the radio sending it (issue #800). A RAS-accepted header names no one either. */
static int
test_failed_header_unbinds_embedded_gps(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    for (int ras = 0; ras <= 1; ras++) {
        fixture(opts, state); /* group voice LC: talkgroup 2002, source 1001 */
        CHECK(dsd_position_feed_start(state) == 0);
        uint8_t bits[96];
        build_gps(bits);
        send_lc(opts, state, bits, 1, 3);
        build_voice_lc(bits, 2002U, 1002U);
        if (ras) {
            /* The burst handler's RAS acceptance: CRCCorrect 1, the slot's scope flagged. */
            const uint8_t saved = state->event_crc_invalid[0];
            state->event_crc_invalid[0] = (uint8_t)(saved | DSD_CRC_RAS_UNVERIFIED);
            uint32_t errors = 0;
            dmr_flco(opts, state, bits, 1U, &errors, 1);
            state->event_crc_invalid[0] = saved;
        } else {
            send_lc(opts, state, bits, 0, 1);
        }
        CHECK(ras || call_is_clean(state)); /* the call layer keeps 1001's call: nothing checked replaced it */
        build_gps(bits);
        bits[30] ^= 1U; /* another place, so it is no repeat of the first fix */
        send_lc(opts, state, bits, 1, 3);
        build_voice_lc(bits, 2002U, 1002U);
        send_lc(opts, state, bits, 1, 1);
        build_gps(bits);
        bits[31] ^= 1U;
        send_lc(opts, state, bits, 1, 3);
        dsd_position_fix recs[4];
        dsd_position_feed_meta meta;
        CHECK(dsd_position_feed_copy_since(state, 0U, recs, 4, &meta) == 3);
        CHECK(recs[0].source_id == 1001U);
        CHECK(recs[1].source_id == 0U && recs[1].target_id == 0U);
        CHECK(recs[2].source_id == 1002U && recs[2].target_id == 2002U);
    }
    return failed;
}

int
main(void) {
    InitAllFecFunction();
    dsd_opts* opts = calloc(1, sizeof *opts);
    dsd_state* state = calloc(1, sizeof *state);
    Event_History_I* history = calloc(2, sizeof *history);
    if (opts == NULL || state == NULL || history == NULL) {
        free(opts);
        free(state);
        free(history);
        return 100;
    }
    state->event_history_s = history;
    int failed = test_failed_checksum_applies_nothing(opts, state);
    failed |= test_checked_alias_and_gps_apply(opts, state);
    failed |= test_checked_utf8_alias_through_flco(opts, state);
    failed |= test_failed_fragment_abandons_assembly(opts, state);
    failed |= test_failed_lc_holds_no_scan(opts, state);
    failed |= test_checked_lc_names_destination(opts, state);
    failed |= test_failed_header_unbinds_embedded_gps(opts, state);
    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    free(opts);
    if (failed == 0) {
        DSD_FPRINTF(stdout, "DMR_EMBEDDED_LC_CHECKSUM: PASS\n");
    }
    return failed;
}
