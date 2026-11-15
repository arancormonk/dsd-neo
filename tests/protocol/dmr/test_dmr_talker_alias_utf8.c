// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR talker alias, UTF-8 format (ETSI TS 102 361-2 clause 7.2.18 Table 7.25: Data Format 10 is Unicode UTF-8, not
 * ISO 8 bit). The alias is fed through the header and block entry points dmr_flco() calls once an embedded LC passed
 * its checksum. Multi-octet characters straddle the LC PDUs, the header's Data Length counts code units (octets for
 * UTF-8, Table 7.26), a malformed sequence shows as U+FFFD, and a character the received blocks cut short waits for the
 * next block rather than showing as U+FFFD.
 */

#include <assert.h>
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
#include "test_env.h"
#include "test_support.h"

int
dsd_event_enrich_alias(dsd_state* state, uint8_t slot, int protocol, uint64_t epoch, const char* alias) {
    (void)protocol;
    dsd_call_snapshot call;
    if (state == NULL || state->event_history_s == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || alias == NULL
        || dsd_call_state_get(state, slot, &call) <= 0 || call.epoch != epoch) {
        return 0;
    }
    DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].alias,
                 sizeof(state->event_history_s[slot].Event_History_Items[0].alias), "%s", alias);
    DSD_SNPRINTF(state->generic_talker_alias[slot], sizeof(state->generic_talker_alias[slot]), "%s", alias);
    state->event_history_s[slot].revision++;
    return 1;
}

int
dsd_event_enrich_alias_ex(const dsd_opts* opts, dsd_state* state, uint8_t slot, int protocol, uint64_t epoch,
                          const char* alias) {
    (void)opts;
    return dsd_event_enrich_alias(state, slot, protocol, epoch, alias);
}

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
    state->generic_talker_alias[slot][0] = '\0';
}

static void
put_bits(uint8_t* bits, size_t offset, uint32_t value, uint8_t count) {
    for (uint8_t b = 0; b < count; b++) {
        bits[offset + b] = (uint8_t)((value >> (count - 1U - b)) & 1U);
    }
}

/* A Talker_Alias_hdr LC (Table 7.4): format at bits 16-17, length at 18-22, and for 8-bit formats the text from bit 24
   (bit 23 is reserved). @p octets holds up to six text octets. */
static void
build_header(uint8_t lc[80], uint8_t format, uint8_t length, const uint8_t* octets, size_t count) {
    DSD_MEMSET(lc, 0, 80);
    put_bits(lc, 2, 0x04U, 6);
    put_bits(lc, 16, format, 2);
    put_bits(lc, 18, length, 5);
    for (size_t i = 0; i < count && i < 6U; i++) {
        put_bits(lc, 24U + (i * 8U), octets[i], 8);
    }
}

/* A Talker_Alias_blk LC (Table 7.5): seven text octets from bit 16. */
static void
build_block(uint8_t lc[80], uint8_t block_index, const uint8_t* octets, size_t count) {
    DSD_MEMSET(lc, 0, 80);
    put_bits(lc, 2, 0x05U + block_index, 6);
    for (size_t i = 0; i < count && i < 7U; i++) {
        put_bits(lc, 16U + (i * 8U), octets[i], 8);
    }
}

static void
expect_alias(const dsd_state* st, uint8_t slot, const char* want, const char* tag) {
    if (strcmp(st->generic_talker_alias[slot], want) != 0) {
        DSD_FPRINTF(stderr, "%s: stored alias '%s', want '%s'\n", tag, st->generic_talker_alias[slot], want);
        g_failures++;
    }
}

static void
expect_contains(const char* buf, const char* needle, const char* tag) {
    if (strstr(buf, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, buf);
        g_failures++;
    }
}

static void
expect_absent(const char* buf, const char* needle, const char* tag) {
    if (strstr(buf, needle) != NULL) {
        DSD_FPRINTF(stderr, "%s: unexpected '%s' in '%s'\n", tag, needle, buf);
        g_failures++;
    }
}

typedef struct {
    dsd_test_capture_stderr cap;
    char buf[4096];
} capture;

static void
capture_begin(capture* c) {
    assert(dsd_test_capture_stderr_begin(&c->cap, "dmr_talker_alias_utf8") == 0);
}

static void
capture_end(capture* c) {
    assert(dsd_test_capture_stderr_end(&c->cap) == 0);
    assert(dsd_test_capture_stderr_read(&c->cap, c->buf, sizeof c->buf) == 0);
}

/* "AB" then U+65E5 U+672C (E6 97 A5 / E6 9C AC): the second character starts in the header's last octet and ends in
   block 1. The header's length is 8, in code units (octets), so after the header alone the cut character is still to
   come: the alias shows the two complete characters and no U+FFFD. Read as a character count (4) the header would
   already be complete and the cut octet malformed. */
static void
test_multibyte_across_blocks(dsd_opts* opts, dsd_state* st) {
    static const uint8_t head[] = {'A', 'B', 0xE6, 0x97, 0xA5, 0xE6};
    static const uint8_t blk1[] = {0x9C, 0xAC};
    uint8_t lc[80];
    capture c;

    seed_active_call(st, 0U, 1234567U);
    build_header(lc, 2U, 8U, head, sizeof head);
    capture_begin(&c);
    dmr_talker_alias_lc_header(opts, st, 0, lc);
    capture_end(&c);
    expect_alias(st, 0U, "AB\xE6\x97\xA5; ", "utf8 header waits for the cut character");
    expect_contains(c.buf, "Length: 8 code units", "utf8 header length in code units");
    expect_contains(c.buf, "Format: UTF-8", "utf8 header format name");
    expect_contains(c.buf, " Slot 1 - Talker Alias Header", "utf8 header slot is one-based");
    expect_absent(c.buf, "\xEF\xBF\xBD", "utf8 header cut character not replaced");

    build_block(lc, 0U, blk1, sizeof blk1);
    capture_begin(&c);
    dmr_talker_alias_lc_blocks(opts, st, 0, 0, lc);
    capture_end(&c);
    expect_alias(st, 0U, "AB\xE6\x97\xA5\xE6\x9C\xAC; ", "utf8 block completes the character");
    expect_contains(c.buf, "AB\xE6\x97\xA5\xE6\x9C\xAC", "utf8 block printed");
    expect_contains(c.buf, " Slot 1 - Talker Alias Block 1;", "utf8 block label");
}

/* Nine three-octet characters fill all 27 code units of header + three blocks; characters straddle the block 1/2 and
   2/3 boundaries (offsets 13 and 20). */
static void
test_full_length_alias(dsd_opts* opts, dsd_state* st) {
    uint8_t text[27];
    uint8_t lc[80];
    char want[64];
    size_t w = 0;
    for (size_t i = 0; i < 9U; i++) {
        text[i * 3U] = 0xE3U;
        text[(i * 3U) + 1U] = 0x81U;
        text[(i * 3U) + 2U] = (uint8_t)(0x82U + (i * 2U)); /* U+3042, U+3044, ... */
        want[w++] = '\xE3';
        want[w++] = '\x81';
        want[w++] = (char)(0x82U + (i * 2U));
    }
    want[w++] = ';';
    want[w++] = ' ';
    want[w] = '\0';

    seed_active_call(st, 1U, 1234568U);
    build_header(lc, 2U, 27U, text, 6U);
    dmr_talker_alias_lc_header(opts, st, 1, lc);
    for (uint8_t b = 0; b < 3U; b++) {
        build_block(lc, b, text + 6U + ((size_t)b * 7U), 7U);
        dmr_talker_alias_lc_blocks(opts, st, 1, b, lc);
    }
    expect_alias(st, 1U, want, "utf8 27 code units across four PDUs");
}

/* U+1F600 needs four octets and is one character. */
static void
test_supplementary_character(dsd_opts* opts, dsd_state* st) {
    static const uint8_t head[] = {'X', 0xF0, 0x9F, 0x98, 0x80};
    uint8_t lc[80];
    seed_active_call(st, 0U, 1234569U);
    build_header(lc, 2U, 5U, head, sizeof head);
    dmr_talker_alias_lc_header(opts, st, 0, lc);
    expect_alias(st, 0U, "X\xF0\x9F\x98\x80; ", "utf8 supplementary character");
}

/* Malformed input becomes U+FFFD, one for each maximal ill-formed subpart: a lead byte without its continuation, a lone
   continuation byte, and a lead whose second byte is out of range (ED A0 would be a surrogate). */
static void
test_malformed_sequences(dsd_opts* opts, dsd_state* st) {
    static const uint8_t head[] = {'A', 0xC3, 'B', 0x80, 'C', 'D'};
    static const uint8_t blk1[] = {0xED, 0xA0, 0x80, 'E'};
    uint8_t lc[80];
    seed_active_call(st, 0U, 1234570U);
    build_header(lc, 2U, 10U, head, sizeof head);
    dmr_talker_alias_lc_header(opts, st, 0, lc);
    expect_alias(st, 0U,
                 "A\xEF\xBF\xBD"
                 "B\xEF\xBF\xBD"
                 "CD; ",
                 "utf8 malformed header");
    build_block(lc, 0U, blk1, sizeof blk1);
    dmr_talker_alias_lc_blocks(opts, st, 0, 0, lc);
    expect_alias(st, 0U,
                 "A\xEF\xBF\xBD"
                 "B\xEF\xBF\xBD"
                 "CD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
                 "E; ",
                 "utf8 malformed block");
}

/* A character cut short where the header says the alias ends is malformed (U+FFFD); the same octets with a longer
   header length are only waiting for the next block. */
static void
test_truncation_at_scalar_boundary(dsd_opts* opts, dsd_state* st) {
    static const uint8_t head[] = {'A', 'B', 'C', 'D', 'E', 0xE6};
    uint8_t lc[80];
    seed_active_call(st, 0U, 1234571U);
    build_header(lc, 2U, 6U, head, sizeof head);
    dmr_talker_alias_lc_header(opts, st, 0, lc);
    expect_alias(st, 0U, "ABCDE\xEF\xBF\xBD; ", "utf8 cut character at the declared end");

    seed_active_call(st, 0U, 1234572U);
    build_header(lc, 2U, 7U, head, sizeof head);
    dmr_talker_alias_lc_header(opts, st, 0, lc);
    expect_alias(st, 0U, "ABCDE; ", "utf8 cut character before the declared end waits");
}

/* Format 01 stays ISO 8 bit: E9 is one Latin-1 character, not a UTF-8 lead byte. */
static void
test_iso8_unchanged(dsd_opts* opts, dsd_state* st) {
    static const uint8_t head[] = {'J', 'o', 's', 0xE9};
    uint8_t lc[80];
    seed_active_call(st, 0U, 1234573U);
    build_header(lc, 1U, 4U, head, sizeof head);
    dmr_talker_alias_lc_header(opts, st, 0, lc);
    expect_alias(st, 0U, "Jos\xC3\xA9; ", "iso8 alias stays Latin-1");
}

/* UTF-16BE: a unit above U+00FE is stored as its character (was '*'), and a surrogate pair cut by the end of the
   received blocks waits for its low half when the header says more units follow. */
static void
test_utf16_kept_and_cut_pair(dsd_opts* opts, dsd_state* st) {
    uint8_t lc[80];
    capture c;
    seed_active_call(st, 0U, 1234574U);
    DSD_MEMSET(lc, 0, sizeof lc);
    put_bits(lc, 2, 0x04U, 6);
    put_bits(lc, 16, 3U, 2);
    put_bits(lc, 18, 4U, 5);
    put_bits(lc, 24, 0x0416U, 16); /* U+0416 */
    put_bits(lc, 40, 0x0041U, 16);
    put_bits(lc, 56, 0xD83DU, 16); /* high half of U+1F600 */
    capture_begin(&c);
    dmr_talker_alias_lc_header(opts, st, 0, lc);
    capture_end(&c);
    expect_alias(st, 0U,
                 "\xD0\x96"
                 "A; ",
                 "utf16 cut pair waits for its low half");
    expect_contains(c.buf, "Format: UTF-16BE", "utf16 format name");

    DSD_MEMSET(lc, 0, sizeof lc);
    put_bits(lc, 2, 0x05U, 6);
    put_bits(lc, 16, 0xDE00U, 16);
    dmr_talker_alias_lc_blocks(opts, st, 0, 0, lc);
    expect_alias(st, 0U,
                 "\xD0\x96"
                 "A\xF0\x9F\x98\x80; ",
                 "utf16 pair completed by block 1");
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(opts != NULL && st != NULL);
    st->event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    assert(st->event_history_s != NULL);
    assert(dsd_test_setenv("DSD_FORCE_UTF8", "1", 1) == 0);
    assert(dsd_test_unsetenv("DSD_FORCE_ASCII") == 0);

    test_multibyte_across_blocks(opts, st);
    test_full_length_alias(opts, st);
    test_supplementary_character(opts, st);
    test_malformed_sequences(opts, st);
    test_truncation_at_scalar_boundary(opts, st);
    test_iso8_unchanged(opts, st);
    test_utf16_kept_and_cut_pair(opts, st);

    dsd_state_ext_free_all(st);
    free(st->event_history_s);
    free(st);
    free(opts);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DMR_TALKER_ALIAS_UTF8: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "DMR_TALKER_ALIAS_UTF8: PASS\n");
    return 0;
}
