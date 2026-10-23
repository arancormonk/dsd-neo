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

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
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

int
main(void) {
    test_phase2_sacch_alias_uses_voice_slot();
    test_phase2_harris_failed_crc_publishes_nothing();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "P25_MOTOROLA_ALIAS: PASS\n");
    return 0;
}
