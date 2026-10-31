// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Over-the-air talker aliases through the real P25 protocol and event layers: Motorola (MFID 0x90) records sent as
 * Phase 1 LCWs and Phase 2 MAC messages, and the Harris (MFID 0xA4) Phase 2 alias.
 *
 * The alias octets are scrambled over the air. apx_embedded_alias_unscramble() is the pure inverse step, so records
 * are built by searching for the encoding of the wanted text through it (each decoded octet depends only on its own
 * encoded octet and the ones before it), then framed and CRC'd the way the decoder reads them.
 */

#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors);

static int g_fail;

static void
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        g_fail = 1;
    }
}

static void
set_bits(uint8_t* bits, size_t start, size_t len, uint32_t value) {
    for (size_t i = 0; i < len; i++) {
        bits[start + i] = (uint8_t)((value >> (len - 1U - i)) & 1U);
    }
}

/* ---- Record encoder ---------------------------------------------------------------------------------------- */

enum { RECORD_MAX_BITS = 56 + (200 * 8) + 16 };

typedef struct {
    uint8_t bits[RECORD_MAX_BITS];
    size_t nbits;
} alias_record;

/* The encoded octets that descramble to @p text (UTF-16BE octets). */
static void
find_encoding(const uint8_t* text, size_t count, uint8_t* encoded) {
    uint8_t decoded[200];
    DSD_MEMSET(encoded, 0, count);
    for (size_t i = 0; i < count; i++) {
        int found = 0;
        for (unsigned candidate = 0; candidate < 256U && !found; candidate++) {
            encoded[i] = (uint8_t)candidate;
            apx_embedded_alias_unscramble(encoded, count, (uint16_t)count, decoded, sizeof decoded);
            found = decoded[i] == text[i];
        }
        if (!found) {
            DSD_FPRINTF(stderr, "find_encoding: no encoding for octet %zu\n", i);
            exit(2);
        }
    }
}

/* FQ-SUID, the encoded alias octets, then the CRC the decoder checks. */
static void
build_record_encoded(alias_record* r, uint32_t wacn, uint32_t sys, uint32_t rid, const uint8_t* encoded, size_t count) {
    DSD_MEMSET(r, 0, sizeof(*r));
    set_bits(r->bits, 0, 20, wacn);
    set_bits(r->bits, 20, 12, sys);
    set_bits(r->bits, 32, 24, rid);
    for (size_t i = 0; i < count; i++) {
        set_bits(r->bits, 56U + (i * 8U), 8, encoded[i]);
    }
    const size_t body = 56U + (count * 8U);
    set_bits(r->bits, body, 16, dsd_crc_ccitt16_bits(r->bits, body));
    r->nbits = body + 16U;
}

static void
build_record_units(alias_record* r, uint32_t wacn, uint32_t sys, uint32_t rid, const uint16_t* units, size_t n) {
    uint8_t text[200];
    uint8_t encoded[200];
    for (size_t i = 0; i < n; i++) {
        text[(i * 2U) + 0U] = (uint8_t)(units[i] >> 8);
        text[(i * 2U) + 1U] = (uint8_t)(units[i] & 0xFFU);
    }
    find_encoding(text, n * 2U, encoded);
    build_record_encoded(r, wacn, sys, rid, encoded, n * 2U);
}

static void
build_record_ascii(alias_record* r, uint32_t rid, const char* ascii) {
    uint16_t units[100];
    size_t n = strlen(ascii);
    for (size_t i = 0; i < n; i++) {
        units[i] = (uint16_t)(unsigned char)ascii[i];
    }
    build_record_units(r, 0xBEE00U, 0x4C9U, rid, units, n);
}

/* ---- Phase 1: LCWs through p25_lcw() ----------------------------------------------------------------------- */

static size_t
p1_block_count(const alias_record* r) {
    return (r->nbits + 43U) / 44U;
}

static void
p1_send_header(dsd_opts* opts, dsd_state* st, const alias_record* r, uint8_t sn) {
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof lcw);
    set_bits(lcw, 0, 8, 0x15U);
    set_bits(lcw, 8, 8, 0x90U);
    set_bits(lcw, 32, 8, (uint32_t)p1_block_count(r));
    set_bits(lcw, 56, 4, sn);
    p25_lcw(opts, st, lcw, 0);
}

static void
p1_send_block(dsd_opts* opts, dsd_state* st, const alias_record* r, uint8_t sn, size_t bn) {
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof lcw);
    set_bits(lcw, 0, 8, 0x17U);
    set_bits(lcw, 8, 8, 0x90U);
    set_bits(lcw, 16, 8, (uint32_t)bn);
    set_bits(lcw, 24, 4, sn);
    for (size_t i = 0; i < 44U; i++) {
        const size_t at = ((bn - 1U) * 44U) + i;
        lcw[28U + i] = at < r->nbits ? r->bits[at] : 0U;
    }
    p25_lcw(opts, st, lcw, 0);
}

static void
p1_send_call_termination(dsd_opts* opts, dsd_state* st, uint32_t tgt) {
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof lcw);
    set_bits(lcw, 0, 8, 0x4FU);
    set_bits(lcw, 48, 24, tgt);
    p25_lcw(opts, st, lcw, 0);
}

/* ---- Phase 2: MAC messages through process_MAC_VPDU_crc() ------------------------------------------------- */

static void
p2_send_bits(dsd_opts* opts, dsd_state* st, int currentslot, const uint8_t* msg_bits, size_t octets, int crc_ok) {
    unsigned long long mac[24] = {0};
    for (size_t j = 0; j < octets && (1U + j) < 24U; j++) {
        mac[1U + j] = convert_bits_into_output(&msg_bits[j * 8U], 8);
    }
    st->currentslot = currentslot;
    process_MAC_VPDU_crc(opts, st, 1, P25_MAC_PDU_SIGNAL, mac, crc_ok);
}

static size_t
p2_block_count(const alias_record* r) {
    return r->nbits <= 64U ? 0U : ((r->nbits - 64U) + 99U) / 100U;
}

static void
p2_send_header(dsd_opts* opts, dsd_state* st, int currentslot, const alias_record* r, uint8_t sn) {
    uint8_t bits[136];
    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits(bits, 0, 8, 0x91U);
    set_bits(bits, 8, 8, 0x90U);
    set_bits(bits, 16, 8, 17U);
    set_bits(bits, 40, 8, (uint32_t)p2_block_count(r));
    set_bits(bits, 64, 4, sn);
    for (size_t i = 0; i < 64U && i < r->nbits; i++) {
        bits[72U + i] = r->bits[i];
    }
    p2_send_bits(opts, st, currentslot, bits, 17U, 1);
}

static void
p2_send_block(dsd_opts* opts, dsd_state* st, int currentslot, const alias_record* r, uint8_t sn, size_t bn) {
    uint8_t bits[136];
    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits(bits, 0, 8, 0x95U);
    set_bits(bits, 8, 8, 0x90U);
    set_bits(bits, 16, 8, 17U);
    set_bits(bits, 24, 8, (uint32_t)bn);
    set_bits(bits, 32, 4, sn);
    for (size_t i = 0; i < 100U; i++) {
        const size_t at = 64U + ((bn - 1U) * 100U) + i;
        bits[36U + i] = at < r->nbits ? r->bits[at] : 0U;
    }
    p2_send_bits(opts, st, currentslot, bits, 17U, 1);
}

static void
p2_send_record(dsd_opts* opts, dsd_state* st, int currentslot, const alias_record* r, uint8_t sn) {
    p2_send_header(opts, st, currentslot, r, sn);
    for (size_t bn = 1U; bn <= p2_block_count(r); bn++) {
        p2_send_block(opts, st, currentslot, r, sn, bn);
    }
}

/* Harris 0xA8: seven alias characters after the opcode, MFID and length octets. */
static void
p2_send_harris(dsd_opts* opts, dsd_state* st, int currentslot, const char text[7], int crc_ok) {
    uint8_t bits[24 * 8];
    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits(bits, 0, 8, 0xA8U);
    set_bits(bits, 8, 8, 0xA4U);
    set_bits(bits, 16, 8, 10U);
    for (size_t i = 0; i < 7U; i++) {
        set_bits(bits, 24U + (i * 8U), 8, (uint8_t)text[i]);
    }
    p2_send_bits(opts, st, currentslot, bits, 10U, crc_ok);
}

/* Harris 0xAA GPS location: a zero payload is a valid 0, 0 fix. */
static void
p2_send_harris_gps(dsd_opts* opts, dsd_state* st, int currentslot, int crc_ok) {
    uint8_t bits[24 * 8];
    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits(bits, 0, 8, 0xAAU);
    set_bits(bits, 8, 8, 0xA4U);
    set_bits(bits, 16, 8, 17U);
    p2_send_bits(opts, st, currentslot, bits, 17U, crc_ok);
}

/* ---- Fixture ----------------------------------------------------------------------------------------------- */

typedef struct {
    dsd_opts* opts;
    dsd_state* st;
    char group_path[DSD_TEST_PATH_MAX];
    char log_path[DSD_TEST_PATH_MAX];
} fixture;

static void
temp_path(char* out, size_t out_size, const char* prefix) {
    int fd = dsd_test_mkstemp(out, out_size, prefix);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "mkstemp failed for %s\n", prefix);
        exit(2);
    }
    (void)dsd_close(fd);
    (void)remove(out);
}

static void
fixture_open(fixture* f) {
    DSD_MEMSET(f, 0, sizeof(*f));
    f->opts = (dsd_opts*)calloc(1, sizeof(*f->opts));
    f->st = (dsd_state*)calloc(1, sizeof(*f->st));
    Event_History_I* history = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    if (!f->opts || !f->st || !history) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        exit(2);
    }
    f->st->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    temp_path(f->group_path, sizeof f->group_path, "p25_alias_group");
    temp_path(f->log_path, sizeof f->log_path, "p25_alias_log");
    DSD_SNPRINTF(f->opts->group_in_file, sizeof f->opts->group_in_file, "%s", f->group_path);
    DSD_SNPRINTF(f->opts->event_out_file, sizeof f->opts->event_out_file, "%s", f->log_path);
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->st);
    free(f->st->event_history_s);
    free(f->st);
    free(f->opts);
    (void)remove(f->group_path);
    (void)remove(f->log_path);
}

static void
begin_call(fixture* f, int protocol, uint8_t slot, uint32_t tg, uint32_t src) {
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = tg,
        .policy_target_id = tg,
        .ota_source_id = src,
    };
    if (dsd_call_state_observe(f->st, &observation, DSD_CALL_BOUNDARY_BEGIN) <= 0) {
        DSD_FPRINTF(stderr, "call did not begin\n");
        exit(2);
    }
    dsd_event_sync_slot(f->opts, f->st, slot);
}

static int
history_has_alias(const dsd_state* st, uint8_t slot, const char* needle) {
    for (int i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        if (strstr(st->event_history_s[slot].Event_History_Items[i].alias, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

static int
policy_name_is(const dsd_state* st, uint32_t id, const char* want) {
    dsd_tg_policy_lookup lookup;
    return dsd_tg_policy_lookup_id(st, id, &lookup) == 0 && lookup.match == DSD_TG_POLICY_MATCH_EXACT
           && strcmp(lookup.entry.name, want) == 0;
}

static int
policy_has_exact(const dsd_state* st, uint32_t id) {
    dsd_tg_policy_lookup lookup;
    return dsd_tg_policy_lookup_id(st, id, &lookup) == 0 && lookup.match == DSD_TG_POLICY_MATCH_EXACT;
}

/* Bytes of @p path that contain @p needle, counted per occurrence. */
static int
file_count(const char* path, const char* needle) {
    FILE* fp = dsd_fopen_private(path, "r");
    if (fp == NULL) {
        return 0;
    }
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1U, fp);
    fclose(fp);
    buf[n] = '\0';
    int count = 0;
    for (const char* p = strstr(buf, needle); p != NULL; p = strstr(p + 1, needle)) {
        count++;
    }
    return count;
}

/* ---- Tests ------------------------------------------------------------------------------------------------- */

/* A Phase 2 SACCH message describes the other slot's call: the alias attaches to that voice slot, never to the slot
 * the SACCH was received on. */
static void
test_phase2_sacch_alias_uses_voice_slot(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x0012D1U;
    begin_call(&f, DSD_SYNC_P25P2_POS, 1U, 5204U, rid);
    alias_record r;
    build_record_ascii(&r, rid, "VOICE ONE");
    /* SACCH on slot 0 carries slot 1's signalling. */
    p2_send_record(f.opts, f.st, 0, &r, 0x3U);
    expect_true("p2 SACCH alias lands on the voice slot's row", history_has_alias(f.st, 1U, "VOICE ONE"));
    expect_true("p2 SACCH alias never lands on the receiving slot", !history_has_alias(f.st, 0U, "VOICE ONE"));
    expect_true("p2 SACCH alias learned for the voice slot's talker", policy_name_is(f.st, rid, "VOICE ONE"));
    fixture_close(&f);
}

/* An LCW Call Termination between the blocks (the call ends, the alias goes on in hangtime) keeps the record. */
static void
test_phase1_call_termination_between_blocks_keeps_record(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00A001U;
    begin_call(&f, DSD_SYNC_P25P1_POS, 0U, 300U, rid);
    alias_record r;
    build_record_ascii(&r, rid, "ENGINE 7");
    const size_t blocks = p1_block_count(&r);
    expect_true("p1 record spans several blocks", blocks >= 2U);
    p1_send_header(f.opts, f.st, &r, 0x5U);
    p1_send_block(f.opts, f.st, &r, 0x5U, 1U);
    p1_send_call_termination(f.opts, f.st, 300U);
    for (size_t bn = 2U; bn <= blocks; bn++) {
        p1_send_block(f.opts, f.st, &r, 0x5U, bn);
    }
    expect_true("p1 alias completes after a Call Termination", history_has_alias(f.st, 0U, "ENGINE 7"));
    fixture_close(&f);
}

/* A repeated identical header mid-record keeps the blocks already stored; a different header restarts. */
static void
test_phase1_repeated_and_changed_headers(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00A002U;
    begin_call(&f, DSD_SYNC_P25P1_POS, 0U, 301U, rid);
    alias_record r;
    build_record_ascii(&r, rid, "LADDER 12");
    const size_t blocks = p1_block_count(&r);
    p1_send_header(f.opts, f.st, &r, 0x6U);
    p1_send_block(f.opts, f.st, &r, 0x6U, 1U);
    p1_send_header(f.opts, f.st, &r, 0x6U);
    for (size_t bn = 2U; bn <= blocks; bn++) {
        p1_send_block(f.opts, f.st, &r, 0x6U, bn);
    }
    expect_true("p1 repeated header completes the record", history_has_alias(f.st, 0U, "LADDER 12"));

    fixture_close(&f);
    fixture_open(&f);
    begin_call(&f, DSD_SYNC_P25P1_POS, 0U, 301U, rid);
    p1_send_header(f.opts, f.st, &r, 0x6U);
    p1_send_block(f.opts, f.st, &r, 0x6U, 1U);
    p1_send_header(f.opts, f.st, &r, 0x7U);
    for (size_t bn = 2U; bn <= blocks; bn++) {
        p1_send_block(f.opts, f.st, &r, 0x7U, bn);
    }
    expect_true("p1 changed header restarts the record", !history_has_alias(f.st, 0U, "LADDER 12"));
    p1_send_block(f.opts, f.st, &r, 0x7U, 1U);
    expect_true("p1 restarted record completes on its own blocks", history_has_alias(f.st, 0U, "LADDER 12"));
    fixture_close(&f);
}

/* An identical header heard in another call is that call's record: blocks of the earlier call do not complete it. */
static void
test_phase1_identical_header_in_new_call_restarts(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00A003U;
    begin_call(&f, DSD_SYNC_P25P1_POS, 0U, 302U, rid);
    alias_record r;
    build_record_ascii(&r, rid, "MEDIC 4");
    const size_t blocks = p1_block_count(&r);
    p1_send_header(f.opts, f.st, &r, 0x8U);
    p1_send_block(f.opts, f.st, &r, 0x8U, 1U);
    expect_true("first call ends", dsd_call_state_end_ex(f.st, 0U, 0.0, DSD_CALL_END_TERMINATOR) > 0);
    dsd_event_sync_slot(f.opts, f.st, 0U);
    begin_call(&f, DSD_SYNC_P25P1_POS, 0U, 302U, rid);
    p1_send_header(f.opts, f.st, &r, 0x8U);
    for (size_t bn = 2U; bn <= blocks; bn++) {
        p1_send_block(f.opts, f.st, &r, 0x8U, bn);
    }
    expect_true("p1 header in a new call does not reuse the old call's blocks",
                !history_has_alias(f.st, 0U, "MEDIC 4"));
    fixture_close(&f);
}

/* A Harris alias or position from a MAC that failed its CRC (-F) reaches no event row, no learned policy, no group
 * file and no position. */
static void
test_phase2_harris_failed_crc_publishes_nothing(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00B001U;
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 400U, rid);
    /* SACCH on slot 1 describes slot 0. */
    p2_send_harris(f.opts, f.st, 1, "GARBLED", 0);
    expect_true("failed-CRC Harris alias reaches no event row", !history_has_alias(f.st, 0U, "GARBLED"));
    expect_true("failed-CRC Harris alias learns no policy", !policy_has_exact(f.st, rid));
    expect_true("failed-CRC Harris alias writes no group file row", file_count(f.group_path, "GARBLED") == 0);
    p2_send_harris(f.opts, f.st, 1, "CHECKED", 1);
    expect_true("checked Harris alias reaches the event row", history_has_alias(f.st, 0U, "CHECKED"));
    expect_true("checked Harris alias learns the policy", policy_name_is(f.st, rid, "CHECKED"));

    p2_send_harris_gps(f.opts, f.st, 1, 0);
    expect_true("failed-CRC Harris GPS reaches no position", f.st->dmr_embedded_gps[0][0] == '\0');
    expect_true("failed-CRC Harris GPS reaches no event row",
                f.st->event_history_s[0].Event_History_Items[0].gps_s[0] == '\0');
    p2_send_harris_gps(f.opts, f.st, 1, 1);
    expect_true("checked Harris GPS reaches the position", f.st->dmr_embedded_gps[0][0] != '\0');
    fixture_close(&f);
}

static void
p1_send_record(dsd_opts* opts, dsd_state* st, const alias_record* r, uint8_t sn) {
    p1_send_header(opts, st, r, sn);
    for (size_t bn = 1U; bn <= p1_block_count(r); bn++) {
        p1_send_block(opts, st, r, sn, bn);
    }
}

/* The record ends at its CRC, not at the first zero word: a scrambled alias octet pair can be 0x0000, and so can the
 * CRC itself. Both decode on both phases. */
static void
test_record_length_with_zero_words(void) {
    /* FQ-SUID ABCDE.123.000001, encoded alias octets 07 8A 00 00, CRC 0x0000. */
    static const uint8_t zero_crc_octets[] = {0x07U, 0x8AU, 0x00U, 0x00U};
    alias_record zero_crc;
    build_record_encoded(&zero_crc, 0xABCDEU, 0x123U, 0x000001U, zero_crc_octets, sizeof zero_crc_octets);
    expect_true("vector CRC is zero", convert_bits_into_output(&zero_crc.bits[zero_crc.nbits - 16U], 16) == 0U);
    /* An interior zero word between non-zero alias octets. */
    static const uint8_t interior_octets[] = {0x41U, 0x42U, 0x00U, 0x00U, 0x43U, 0x44U};
    alias_record interior;
    build_record_encoded(&interior, 0x12345U, 0x678U, 0x00C001U, interior_octets, sizeof interior_octets);

    for (int phase = 1; phase <= 2; phase++) {
        fixture f;
        fixture_open(&f);
        char tag[96];
        begin_call(&f, phase == 1 ? DSD_SYNC_P25P1_POS : DSD_SYNC_P25P2_POS, 0U, 500U, 0x000001U);
        if (phase == 1) {
            p1_send_record(f.opts, f.st, &zero_crc, 0x1U);
        } else {
            p2_send_record(f.opts, f.st, 1, &zero_crc, 0x1U);
        }
        DSD_SNPRINTF(tag, sizeof tag, "phase %d zero-CRC record decodes", phase);
        expect_true(tag, history_has_alias(f.st, 0U, "FQ-SUID: ABCDE:123.000001"));
        DSD_SNPRINTF(tag, sizeof tag, "phase %d zero-CRC record learns the talker", phase);
        expect_true(tag, policy_has_exact(f.st, 0x000001U));

        begin_call(&f, phase == 1 ? DSD_SYNC_P25P1_POS : DSD_SYNC_P25P2_POS, 0U, 501U, 0x00C001U);
        if (phase == 1) {
            p1_send_record(f.opts, f.st, &interior, 0x2U);
        } else {
            p2_send_record(f.opts, f.st, 1, &interior, 0x2U);
        }
        DSD_SNPRINTF(tag, sizeof tag, "phase %d interior zero word record decodes", phase);
        expect_true(tag, history_has_alias(f.st, 0U, "FQ-SUID: 12345:678.00C001"));
        fixture_close(&f);
    }
}

/* The stored name is the decoded UTF-16 as UTF-8: controls (C0, NUL, C1) become spaces, a comma a dot, a surrogate
 * pair one scalar and an unpaired half U+FFFD. */
static void
test_stored_name_is_utf8(void) {
    static const uint16_t units[] = {'A', 0x001BU, 0x0000U, 0x0085U, 'B', 0x00E9U, ',', 0xD83DU, 0xDE00U, 0xD800U, 'C'};
    static const char want[] = "A   B\xC3\xA9."
                               "\xF0\x9F\x98\x80"
                               "\xEF\xBF\xBD"
                               "C";
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00D001U;
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 600U, rid);
    alias_record r;
    build_record_units(&r, 0xBEE00U, 0x4C9U, rid, units, sizeof units / sizeof units[0]);
    p2_send_record(f.opts, f.st, 1, &r, 0x4U);
    char want_row[64];
    DSD_SNPRINTF(want_row, sizeof want_row, "%s; ", want);
    expect_true("UTF-16 alias stored as UTF-8 in the learned policy", policy_name_is(f.st, rid, want));
    expect_true("UTF-16 alias stored as UTF-8 in the event row", history_has_alias(f.st, 0U, want_row));
    expect_true("UTF-16 alias stored as UTF-8 in the group file", file_count(f.group_path, want) == 1);
    fixture_close(&f);
}

static unsigned int
policy_generation(const dsd_state* st) {
    uint64_t context = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(st, &context, &generation);
    return generation;
}

/* The learned row follows the radio's alias: a repeat changes nothing (the policy generation stays put), a renamed
 * radio's alias replaces the learned name, and an imported row for the radio always wins. */
static void
test_learned_alias_row(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00E001U;
    alias_record first;
    alias_record renamed;
    build_record_ascii(&first, rid, "BATT 3");
    build_record_ascii(&renamed, rid, "BATT 3 CHIEF");

    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 700U, rid);
    p2_send_record(f.opts, f.st, 1, &first, 0x1U);
    expect_true("first alias learned", policy_name_is(f.st, rid, "BATT 3"));
    const unsigned int learned_generation = policy_generation(f.st);
    p2_send_record(f.opts, f.st, 1, &first, 0x1U);
    p2_send_record(f.opts, f.st, 1, &first, 0x2U);
    expect_true("repeated alias keeps the policy generation", policy_generation(f.st) == learned_generation);
    expect_true("repeated alias writes one group file row", file_count(f.group_path, "BATT 3") == 1);

    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 700U, rid);
    p2_send_record(f.opts, f.st, 1, &renamed, 0x3U);
    expect_true("renamed radio's alias replaces the learned one", policy_name_is(f.st, rid, "BATT 3 CHIEF"));
    fixture_close(&f);

    fixture_open(&f);
    dsd_tg_policy_entry imported;
    if (dsd_tg_policy_make_exact_entry(rid, "A", "Chief Battalion 3", DSD_TG_POLICY_SOURCE_IMPORTED, &imported) != 0
        || dsd_tg_policy_append_exact(f.st, &imported) != 0) {
        DSD_FPRINTF(stderr, "imported row not stored\n");
        exit(2);
    }
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 700U, rid);
    p2_send_record(f.opts, f.st, 1, &first, 0x1U);
    expect_true("imported row wins over the alias", policy_name_is(f.st, rid, "Chief Battalion 3"));
    expect_true("alias still reaches the event row", history_has_alias(f.st, 0U, "BATT 3; "));
    fixture_close(&f);
}

/* The committed row the slot's last call pushed, right behind the staged row. */
static const Event_History*
committed_row(const fixture* f, uint8_t slot) {
    return &f->st->event_history_s[slot].Event_History_Items[1];
}

static void
end_call_and_commit(fixture* f, uint8_t slot) {
    if (dsd_call_state_end_ex(f->st, slot, 0.0, DSD_CALL_END_TERMINATOR) <= 0) {
        DSD_FPRINTF(stderr, "call did not end\n");
        exit(2);
    }
    dsd_event_sync_slot(f->opts, f->st, slot);
}

static void
held_view(const fixture* f, uint8_t slot, dsd_app_slot_call* out) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(f->st, slot, &call) <= 0) {
        DSD_FPRINTF(stderr, "no call to view\n");
        exit(2);
    }
    dsd_app_slot_call_view(f->st, slot, call.ended_m + 0.5, out);
}

/* Motorola sends the alias in hangtime, after the call's row has committed. The alias still names that row (its source
 * label, its re-rendered line and one continuation in the event log, on the row's own stamp) and the held view of the
 * ended call, which reads the label the event layer cached rather than resolving it per render. */
static void
test_late_alias_names_the_committed_call(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00F001U;
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 800U, rid);
    end_call_and_commit(&f, 0U);
    expect_true("call row committed", committed_row(&f, 0U)->source_id == rid);
    expect_true("committed row has no name yet", committed_row(&f, 0U)->s_name[0] == '\0');

    alias_record r;
    build_record_ascii(&r, rid, "TRUCK 9");
    p2_send_record(f.opts, f.st, 1, &r, 0x1U);
    expect_true("late alias reaches the committed row", strstr(committed_row(&f, 0U)->alias, "TRUCK 9; ") != NULL);
    expect_true("late alias names the committed row", strcmp(committed_row(&f, 0U)->s_name, "TRUCK 9") == 0);
    expect_true("late alias re-renders the committed row",
                strstr(committed_row(&f, 0U)->event_string, "SName: TRUCK 9;") != NULL);
    expect_true("late alias logged once", file_count(f.log_path, "Talker Alias: TRUCK 9") == 1);
    expect_true("late alias continuation logged once", file_count(f.log_path, "Enriched: ") == 1);
    expect_true("late alias continuation carries the new name", file_count(f.log_path, "SName: TRUCK 9;") == 1);

    dsd_app_slot_call view;
    held_view(&f, 0U, &view);
    expect_true("held view of the ended call is still held", view.state == DSD_APP_CALL_LINE_ENDED);
    expect_true("held view names the late alias", strstr(view.src_text, "TRUCK 9") != NULL);

    /* The repeated alias Motorola sends through hangtime changes nothing and logs nothing more. */
    p2_send_record(f.opts, f.st, 1, &r, 0x2U);
    expect_true("repeated late alias logged once", file_count(f.log_path, "Talker Alias: TRUCK 9") == 1);
    expect_true("repeated late alias adds no continuation", file_count(f.log_path, "Enriched: ") == 1);

    /* The held view reads the cached label: with the source tables emptied, it still names the talker. */
    (void)dsd_tg_policy_clear(f.st);
    held_view(&f, 0U, &view);
    expect_true("held view needs no source lookup per render", strstr(view.src_text, "TRUCK 9") != NULL);
    fixture_close(&f);
}

/* Issue #773 B11, through the real event layer and call view: once the row commits, the held view of the ended call
 * still names the group (here from a range row, D4), from the label cached on the epoch rather than a lookup. */
static void
test_ended_hold_names_the_group(void) {
    fixture f;
    fixture_open(&f);
    dsd_tg_policy_entry range;
    if (dsd_tg_policy_make_exact_entry(900U, "A", "Fireground", DSD_TG_POLICY_SOURCE_IMPORTED, &range) != 0) {
        DSD_FPRINTF(stderr, "range row not built\n");
        exit(2);
    }
    range.id_end = 999U;
    range.kind = DSD_TG_POLICY_ROW_RANGE;
    if (dsd_tg_policy_add_range_entry(f.st, &range) != 0) {
        DSD_FPRINTF(stderr, "range row not stored\n");
        exit(2);
    }
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 950U, 0x00F010U);
    end_call_and_commit(&f, 0U);
    expect_true("group row committed", committed_row(&f, 0U)->target_id == 950U);
    expect_true("committed row named by the range", strcmp(committed_row(&f, 0U)->t_name, "Fireground") == 0);
    dsd_app_slot_call view;
    held_view(&f, 0U, &view);
    expect_true("held view of the ended group call", view.state == DSD_APP_CALL_LINE_ENDED);
    expect_true("held view names the group", strcmp(view.name, "Fireground") == 0);
    (void)dsd_tg_policy_clear(f.st);
    held_view(&f, 0U, &view);
    expect_true("held view needs no group lookup per render", strcmp(view.name, "Fireground") == 0);
    fixture_close(&f);
}

/* An imported name keeps precedence over the learned alias on the committed row: a source alias list entry, then an
 * imported group-list row. */
static void
test_late_alias_keeps_imported_names(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00F002U;
    dsd_source_alias_store* store = dsd_source_alias_store_create();
    dsd_source_alias_entry entry;
    DSD_MEMSET(&entry, 0, sizeof entry);
    entry.id_start = entry.id_end = rid;
    DSD_SNPRINTF(entry.name, sizeof entry.name, "%s", "Rescue Two");
    if (store == NULL || dsd_source_alias_store_append(store, &entry) != 0) {
        DSD_FPRINTF(stderr, "source alias store not built\n");
        exit(2);
    }
    dsd_source_alias_install(f.st, store);
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 801U, rid);
    end_call_and_commit(&f, 0U);
    alias_record r;
    build_record_ascii(&r, rid, "RESCUE 2 OTA");
    p2_send_record(f.opts, f.st, 1, &r, 0x1U);
    expect_true("late alias reaches the row under a source alias",
                strstr(committed_row(&f, 0U)->alias, "RESCUE 2 OTA") != NULL);
    expect_true("source alias keeps the row's name", strcmp(committed_row(&f, 0U)->s_name, "Rescue Two") == 0);
    fixture_close(&f);

    fixture_open(&f);
    dsd_tg_policy_entry imported;
    if (dsd_tg_policy_make_exact_entry(rid, "A", "Rescue Imported", DSD_TG_POLICY_SOURCE_IMPORTED, &imported) != 0
        || dsd_tg_policy_append_exact(f.st, &imported) != 0) {
        DSD_FPRINTF(stderr, "imported row not stored\n");
        exit(2);
    }
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 801U, rid);
    end_call_and_commit(&f, 0U);
    dsd_app_slot_call view;
    held_view(&f, 0U, &view);
    expect_true("held view keeps a known name once the row commits", strstr(view.src_text, "Rescue Imported") != NULL);
    p2_send_record(f.opts, f.st, 1, &r, 0x1U);
    expect_true("imported group-list row keeps the row's name",
                strcmp(committed_row(&f, 0U)->s_name, "Rescue Imported") == 0);
    fixture_close(&f);
}

/* A renamed radio: its next call's row commits under the old learned name, and the alias that follows renames it. */
static void
test_late_alias_renames_a_learned_name(void) {
    fixture f;
    fixture_open(&f);
    const uint32_t rid = 0x00F003U;
    alias_record old_name;
    alias_record new_name;
    build_record_ascii(&old_name, rid, "UNIT OLD");
    build_record_ascii(&new_name, rid, "UNIT NEW");
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 802U, rid);
    p2_send_record(f.opts, f.st, 1, &old_name, 0x1U);
    end_call_and_commit(&f, 0U);
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 802U, rid);
    end_call_and_commit(&f, 0U);
    expect_true("next call commits under the learned name", strcmp(committed_row(&f, 0U)->s_name, "UNIT OLD") == 0);
    p2_send_record(f.opts, f.st, 1, &new_name, 0x2U);
    expect_true("renamed alias renames the committed row", strcmp(committed_row(&f, 0U)->s_name, "UNIT NEW") == 0);
    dsd_app_slot_call view;
    held_view(&f, 0U, &view);
    expect_true("held view follows the rename", strstr(view.src_text, "UNIT NEW") != NULL);
    fixture_close(&f);
}

/* Issue #782: a call is named the same while it is held as while it is on air. A talkgroup with no name of its own is
 * named for the channel it was heard on (a trunk-scan target's label), and a listed talkgroup by its group-list name;
 * both are read from the staged row while the call is active, and that row commits when the call ends, so the held
 * view reads the names the event layer cached on the call. Without them the headline flipped from the target's label to
 * "TG <n>" for the hold after every transmission. */
static void
test_held_call_keeps_its_names(void) {
    fixture f;
    fixture_open(&f);
    f.opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(f.st->trunk_scan_active_id, sizeof f.st->trunk_scan_active_id, "%s",
                 "f4c3562f-afab-49ee-bc0a-1f8df9845cb3");
    DSD_SNPRINTF(f.st->trunk_scan_active_label, sizeof f.st->trunk_scan_active_label, "%s", "M0D");
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 100U, 1001U);
    dsd_call_snapshot call;
    dsd_app_slot_call view;
    if (dsd_call_state_get(f.st, 0U, &call) <= 0) {
        DSD_FPRINTF(stderr, "no call to view\n");
        exit(2);
    }
    dsd_app_slot_call_view(f.st, 0U, call.started_m + 0.5, &view);
    expect_true("an active call with no name of its own is named for the target", strcmp(view.name, "M0D") == 0);
    end_call_and_commit(&f, 0U);
    held_view(&f, 0U, &view);
    expect_true("the held call is still held", view.state == DSD_APP_CALL_LINE_ENDED);
    expect_true("the held call keeps the target's name", strcmp(view.name, "M0D") == 0);

    // A listed talkgroup keeps its group-list name while held.
    dsd_tg_policy_entry entry;
    if (dsd_tg_policy_make_exact_entry(200U, "A", "Fire Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) != 0
        || dsd_tg_policy_append_exact(f.st, &entry) != 0) {
        DSD_FPRINTF(stderr, "group row not stored\n");
        exit(2);
    }
    begin_call(&f, DSD_SYNC_P25P2_POS, 0U, 200U, 1002U);
    if (dsd_call_state_get(f.st, 0U, &call) <= 0) {
        DSD_FPRINTF(stderr, "no call to view\n");
        exit(2);
    }
    dsd_app_slot_call_view(f.st, 0U, call.started_m + 0.5, &view);
    expect_true("an active listed talkgroup is named by the group list", strcmp(view.name, "Fire Dispatch") == 0);
    end_call_and_commit(&f, 0U);
    held_view(&f, 0U, &view);
    expect_true("the held listed talkgroup keeps its name", strcmp(view.name, "Fire Dispatch") == 0);
    fixture_close(&f);
}

int
main(void) {
    test_phase2_sacch_alias_uses_voice_slot();
    test_phase1_call_termination_between_blocks_keeps_record();
    test_phase1_repeated_and_changed_headers();
    test_phase1_identical_header_in_new_call_restarts();
    test_phase2_harris_failed_crc_publishes_nothing();
    test_record_length_with_zero_words();
    test_stored_name_is_utf8();
    test_learned_alias_row();
    test_late_alias_names_the_committed_call();
    test_ended_hold_names_the_group();
    test_late_alias_keeps_imported_names();
    test_late_alias_renames_a_learned_name();
    test_held_call_keeps_its_names();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "P25_MOTOROLA_ALIAS: PASS\n");
    return 0;
}
