// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 L3Harris GPS through the real MAC dispatch, trunk state machine, decoder, position admission and
 * location file (issue #800).
 *
 * The message is `AA A4 11` (17 octets) or the legacy shifted `80 AA A4 11` (18 octets: its length counts from the MCO
 * octet), each followed by the 14-octet Part A + Part B body of TIA-102.AABF-D-1 §7.3.35-36. A report is checked when
 * the MAC CRC passed and the segment holds all of it. Its source, target and kind count only when every one of them
 * came from a checked message (spec 2.19): a grant whose MAC failed its CRC (-F) still drives the follow, but a field
 * it set, or one a later message inherited from it, is unchecked. All coordinates and IDs are made up.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25_xcch.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
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

#define T0_S        INT64_C(1767261600)
#define NS_PER_S    UINT64_C(1000000000)
#define SRC_A       3001U
#define SRC_B       3002U
#define TG_A        100U
#define UNIT_B      4002U
#define CC_HZ       851000000L
#define VC_CHANNEL  0x100A /* iden 1, TDMA channel 0x00A: slot 0 */
#define BODY_OCTETS DSD_HARRIS_GPS_BODY_OCTETS

typedef struct {
    dsd_opts* opts;
    dsd_state* st;
    char lrrp_path[DSD_TEST_PATH_MAX];
    uint64_t feed_seq;
    uint8_t sats; /* each report carries its own satellite count, so no two of them repeat in the feed */
} fixture;

static dsd_trunk_tune_result
tune_ok(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    if (freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    opts->trunk_is_tuned = 1;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_ok(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)state;
    (void)request_id;
    opts->trunk_is_tuned = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
fixture_open(fixture* f) {
    DSD_MEMSET(f, 0, sizeof(*f));
    dsd_decode_clock_use_test((uint64_t)T0_S * NS_PER_S);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune_ok,
        .tune_to_cc_request = tune_ok,
        .return_to_cc_request = return_ok,
    });
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
    f->opts->trunk_enable = 1;
    f->opts->trunk_hangtime = 2.0f;
    f->opts->trunk_tune_group_calls = 1;
    f->opts->trunk_tune_private_calls = 1;
    f->opts->trunk_tune_data_calls = 1;
    f->opts->trunk_tune_enc_calls = 1;
    f->st->p25_cc_freq = CC_HZ;
    f->st->trunk_cc_freq = CC_HZ;
    f->st->synctype = DSD_SYNC_P25P2_POS;
    f->st->lastsynctype = DSD_SYNC_P25P2_POS;
    // A TDMA IDEN 1 (two slots, 12.5 kHz) and the site identity the SM needs before it follows a TDMA grant.
    f->st->p25_chan_iden = 1;
    f->st->p25_iden_tdma[1].base_freq = CC_HZ / 5;
    f->st->p25_iden_tdma[1].chan_type = 3;
    f->st->p25_iden_tdma[1].chan_spac = 100;
    f->st->p25_iden_tdma[1].trust = 2;
    f->st->p25_iden_tdma[1].populated = 1;
    f->st->p25_chan_tdma_explicit[1] = 2;
    f->st->p2_wacn = 0xBEE00;
    f->st->p2_sysid = 0x1A2;
    f->st->p2_cc = 0x293;
    p25_sm_init_ctx(p25_sm_get_ctx(), f->opts, f->st);

    const int fd = dsd_test_mkstemp(f->lrrp_path, sizeof f->lrrp_path, "p2_harris_gps_lrrp");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "mkstemp failed\n");
        exit(2);
    }
    (void)dsd_close(fd);
    f->opts->lrrp_file_output = 1;
    DSD_SNPRINTF(f->opts->lrrp_out_file, sizeof f->opts->lrrp_out_file, "%s", f->lrrp_path);
    if (dsd_position_feed_start(f->st) != 0) {
        DSD_FPRINTF(stderr, "feed start failed\n");
        exit(2);
    }
}

static void
fixture_close(fixture* f) {
    p25_sm_init_ctx(p25_sm_get_ctx(), f->opts, f->st);
    dsd_state_ext_free_all(f->st);
    free(f->st->event_history_s);
    free(f->st);
    free(f->opts);
    (void)remove(f->lrrp_path);
}

/* One MAC PDU (MAC[1..]) on a FACCH (@p type 0) or SACCH (1) of slot 0, with the CRC verdict @p crc_ok. */
static void
send_mac(fixture* f, int type, const uint8_t* octets, size_t count, int crc_ok) {
    unsigned long long int mac[24] = {0};
    for (size_t i = 0; i < count && i + 1U < 24U; i++) {
        mac[i + 1U] = octets[i];
    }
    // A SACCH carries the other slot's signalling: currentslot 1 speaks for voice slot 0.
    f->st->currentslot = type == 1 ? 1 : 0;
    process_MAC_VPDU_crc(f->opts, f->st, type, P25_MAC_PDU_SIGNAL, mac, crc_ok);
    f->st->currentslot = 0;
}

/* Group Voice Channel Grant (0x40) of TG_A by @p src on VC_CHANNEL. */
static void
group_grant(fixture* f, uint32_t src, int crc_ok) {
    const uint8_t grant[] = {0x40,
                             0x00,
                             (uint8_t)(VC_CHANNEL >> 8),
                             (uint8_t)(VC_CHANNEL & 0xFFU),
                             (uint8_t)(TG_A >> 8),
                             (uint8_t)TG_A,
                             (uint8_t)(src >> 16),
                             (uint8_t)(src >> 8),
                             (uint8_t)src};
    send_mac(f, 0, grant, sizeof grant, crc_ok);
}

/* Unit to Unit Voice Channel Grant (0x44) from @p src to UNIT_B on VC_CHANNEL. */
static void
private_grant(fixture* f, uint32_t src, int crc_ok) {
    const uint8_t grant[] = {0x44,
                             (uint8_t)(VC_CHANNEL >> 8),
                             (uint8_t)(VC_CHANNEL & 0xFFU),
                             (uint8_t)(UNIT_B >> 16),
                             (uint8_t)(UNIT_B >> 8),
                             (uint8_t)(UNIT_B & 0xFFU),
                             (uint8_t)(src >> 16),
                             (uint8_t)(src >> 8),
                             (uint8_t)src};
    send_mac(f, 0, grant, sizeof grant, crc_ok);
}

/* A FACCH MAC PDU of slot 0 (MAC octets 0-17) with a valid CRC-12, through the real XCCH decoder. */
static void
send_facch(fixture* f, const uint8_t mac[18]) {
    int payload[156] = {0};
    for (int i = 0; i < 144; i++) {
        payload[i] = (mac[i / 8] >> (7 - (i % 8))) & 1;
    }
    for (int crc = 0; crc < 4096; crc++) {
        for (int i = 0; i < 12; i++) {
            payload[144 + i] = (crc >> (11 - i)) & 1;
        }
        if (crc12_xb_bridge(payload, 144) == 0) {
            break;
        }
    }
    f->st->currentslot = 0;
    process_FACCH_MAC_PDU(f->opts, f->st, payload);
}

/* MAC_PTT (opcode 1): the source in octets 13-15 and the group address field in 16-17. */
static void
mac_ptt(fixture* f, uint32_t tg, uint32_t src) {
    uint8_t mac[18] = {0};
    mac[0] = 0x20;
    mac[13] = (uint8_t)(src >> 16);
    mac[14] = (uint8_t)(src >> 8);
    mac[15] = (uint8_t)src;
    mac[16] = (uint8_t)(tg >> 8);
    mac[17] = (uint8_t)tg;
    send_facch(f, mac);
}

/* MAC_ACTIVE (opcode 4) carrying a Group Voice Channel User (clear) of @p tg by @p src (0 = none named). */
static void
mac_active(fixture* f, uint32_t tg, uint32_t src) {
    const uint8_t mac[18] = {
        0x80, 0x01, 0x00, (uint8_t)(tg >> 8), (uint8_t)tg, (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src};
    send_facch(f, mac);
}

/* A SACCH MAC_SIGNAL (opcode 0) for voice slot 0 carrying the MAC message @p octets (MAC octets 1..), with a valid
 * CRC-12, through the real XCCH decoder. */
static void
send_sacch_signal(fixture* f, const uint8_t* octets, size_t count) {
    uint8_t mac[21] = {0};
    for (size_t i = 0; i < count && i + 1U < sizeof mac; i++) {
        mac[i + 1U] = octets[i];
    }
    int payload[180] = {0};
    for (int i = 0; i < 168; i++) {
        payload[i] = (mac[i / 8] >> (7 - (i % 8))) & 1;
    }
    for (int crc = 0; crc < 4096; crc++) {
        for (int i = 0; i < 12; i++) {
            payload[168 + i] = (crc >> (11 - i)) & 1;
        }
        if (crc12_xb_bridge(payload, 168) == 0) {
            break;
        }
    }
    // A SACCH carries the other slot's signalling: currentslot 1 speaks for voice slot 0.
    f->st->currentslot = 1;
    process_SACCH_MAC_PDU(f->opts, f->st, payload);
    f->st->currentslot = 0;
}

/* Group Voice Channel User (abbreviated, clear) of @p tg by @p src. */
static void
sacch_group_voice_user(fixture* f, uint32_t tg, uint32_t src) {
    const uint8_t msg[] = {
        0x01, 0x00, (uint8_t)(tg >> 8), (uint8_t)tg, (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src};
    send_sacch_signal(f, msg, sizeof msg);
}

/* Unit to Unit Voice Channel User (abbreviated, clear) from @p src to @p target. */
static void
sacch_private_voice_user(fixture* f, uint32_t target, uint32_t src) {
    const uint8_t msg[] = {0x02,
                           0x00,
                           (uint8_t)(target >> 16),
                           (uint8_t)(target >> 8),
                           (uint8_t)target,
                           (uint8_t)(src >> 16),
                           (uint8_t)(src >> 8),
                           (uint8_t)src};
    send_sacch_signal(f, msg, sizeof msg);
}

/* 22°30' N, 45° E at 02:00:00, heading 90.0, quality %01, @p sats satellites. */
static void
make_body(uint8_t body[BODY_OCTETS], uint8_t sats) {
    const uint8_t b[BODY_OCTETS] = {0x00, 0x00, 0x1E, 0x16, 0x00, 0x00, 0x00, 0x2D, 0x1C, 0x20, 0x84, 0x00, 0x03, 0x10};
    DSD_MEMCPY(body, b, BODY_OCTETS);
    body[BODY_OCTETS - 1] = (uint8_t)(0x10 | (sats & 0x0F));
}

/* The GPS message, ordinary (`AA A4 11`, on a FACCH) or shifted (`80 AA A4 11`, on a SACCH). */
static void
send_gps(fixture* f, int shifted) {
    uint8_t octets[4 + BODY_OCTETS];
    size_t n = 0;
    if (shifted) {
        octets[n++] = 0x80;
    }
    octets[n++] = 0xAA;
    octets[n++] = 0xA4;
    octets[n++] = 0x11;
    f->sats = (uint8_t)((f->sats % 15U) + 1U);
    make_body(octets + n, f->sats);
    n += BODY_OCTETS;
    send_mac(f, shifted ? 1 : 0, octets, n, 1);
}

static int
lrrp_rows(const fixture* f, char last[256]) {
    FILE* fp = dsd_fopen_private(f->lrrp_path, "r");
    last[0] = '\0';
    if (fp == NULL) {
        return 0;
    }
    int rows = 0;
    char line[256];
    while (fgets(line, sizeof line, fp) != NULL) {
        rows++;
        DSD_SNPRINTF(last, 256, "%s", line);
    }
    fclose(fp);
    return rows;
}

/* The last location row names @p src (8 digits) at 22.5, 45.0. */
static int
last_row_names(const fixture* f, const char* src) {
    char last[256];
    if (lrrp_rows(f, last) == 0) {
        DSD_FPRINTF(stderr, "no location row (want %s)\n", src);
        return 0;
    }
    char want[96];
    DSD_SNPRINTF(want, sizeof want, "\t%s\t22.500000\t45.000000\t", src);
    const int ok = strstr(last, want) != NULL;
    if (!ok) {
        DSD_FPRINTF(stderr, "location row: %s (want ...%s...)\n", last, want);
    }
    return ok;
}

static int
feed_next(fixture* f, dsd_position_fix* out) {
    dsd_position_feed_meta meta;
    const int n = dsd_position_feed_copy_since(f->st, f->feed_seq, out, 1, &meta);
    if (n > 0) {
        f->feed_seq = meta.last_seq;
    }
    return n > 0;
}

/* The report the last GPS message gave: kept in the feed, attributed to @p src (0 = left out as unverified). */
static void
expect_report(fixture* f, const char* tag, uint32_t src, uint32_t target, int group) {
    char label[160];
    dsd_position_fix fix;
    DSD_SNPRINTF(label, sizeof label, "%s: feed record", tag);
    expect_true(label, feed_next(f, &fix));
    char digits[16];
    DSD_SNPRINTF(digits, sizeof digits, "%08u", (unsigned int)src);
    DSD_SNPRINTF(label, sizeof label, "%s: location row", tag);
    expect_true(label, last_row_names(f, digits));
    DSD_SNPRINTF(label, sizeof label, "%s: record fields", tag);
    expect_true(label, fix.kind == DSD_POS_P25_HARRIS && fix.lat_e7 == 225000000 && fix.lon_e7 == 450000000
                           && fix.slot == 0U && fix.sats == f->sats && fix.heading_ddeg == 900U);
    DSD_SNPRINTF(label, sizeof label, "%s: identity", tag);
    if (src != 0U) {
        expect_true(label, fix.source_id == src && fix.target_id == target
                               && ((fix.flags & DSD_POS_FLAG_TARGET_GROUP) != 0U) == (group != 0)
                               && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) == 0U);
    } else {
        expect_true(label, fix.source_id == 0U && fix.target_id == 0U && (fix.flags & DSD_POS_FLAG_TARGET_GROUP) == 0U
                               && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U);
    }
}

static int
slot0_call_names(uint32_t target, uint32_t src, const dsd_state* st) {
    dsd_call_snapshot call;
    return dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == target
           && call.ota_source_id == src;
}

/* A checked grant and a checked MAC_PTT: both forms of the message carry the call's source, target and kind. */
static void
test_checked_call_both_forms(void) {
    fixture f;
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    expect_true("checked grant tuned", f.opts->trunk_is_tuned == 1);
    mac_ptt(&f, TG_A, SRC_A);
    expect_true("checked call open", slot0_call_names(TG_A, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "ordinary form", SRC_A, TG_A, 1);
    send_gps(&f, 1);
    expect_report(&f, "shifted form", SRC_A, TG_A, 1);
    fixture_close(&f);
}

/* A MAC_PTT naming the talkgroup and the source sets all three fields itself: the failed grant leaves no mark. */
static void
test_checked_group_ptt_after_unchecked_grant(void) {
    fixture f;
    fixture_open(&f);
    group_grant(&f, SRC_A, 0);
    expect_true("-F grant still followed", f.opts->trunk_is_tuned == 1);
    mac_ptt(&f, TG_A, SRC_A);
    send_gps(&f, 0);
    expect_report(&f, "group ptt after -F grant", SRC_A, TG_A, 1);
    fixture_close(&f);
}

/* MAC_PTT has no private destination field: the destination and the private kind are the grant's. A failed grant
 * leaves them unchecked, so the report names no one even though the PTT itself passed its CRC. */
static void
test_private_destination_inherited(void) {
    for (int crc_ok = 0; crc_ok < 2; crc_ok++) {
        fixture f;
        fixture_open(&f);
        private_grant(&f, SRC_A, crc_ok);
        expect_true("private grant followed", f.opts->trunk_is_tuned == 1);
        mac_ptt(&f, 0U, SRC_A);
        expect_true("private call open", slot0_call_names(UNIT_B, SRC_A, f.st));
        send_gps(&f, 0);
        if (crc_ok) {
            expect_report(&f, "private call, checked grant", SRC_A, UNIT_B, 0);
        } else {
            expect_report(&f, "private call, -F grant", 0U, 0U, 0);
        }
        fixture_close(&f);
    }
}

/* A checked MAC_ACTIVE naming the talkgroup but no source inherits the grant's talker with the grant's verdict: a
 * source-less message never upgrades it. */
static void
test_sourceless_update(void) {
    for (int crc_ok = 0; crc_ok < 2; crc_ok++) {
        fixture f;
        fixture_open(&f);
        group_grant(&f, SRC_A, crc_ok);
        mac_active(&f, TG_A, 0U);
        expect_true("source-less call open", slot0_call_names(TG_A, SRC_A, f.st));
        send_gps(&f, 0);
        if (crc_ok) {
            expect_report(&f, "source-less update, checked grant", SRC_A, TG_A, 1);
        } else {
            expect_report(&f, "source-less update, -F grant", 0U, 0U, 0);
        }
        fixture_close(&f);
    }
}

/* A voice user from a MAC that failed its CRC names another talker: the canonical call begins a new epoch the state
 * machine never published from a checked message, so the report names no one. */
static void
test_epoch_change(void) {
    fixture f;
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    mac_ptt(&f, TG_A, SRC_A);
    send_gps(&f, 0);
    expect_report(&f, "before the epoch change", SRC_A, TG_A, 1);

    const uint8_t voice_user[] = {0x01,
                                  0x00,
                                  (uint8_t)(TG_A >> 8),
                                  (uint8_t)TG_A,
                                  (uint8_t)(SRC_B >> 16),
                                  (uint8_t)(SRC_B >> 8),
                                  (uint8_t)(SRC_B & 0xFFU)};
    send_mac(&f, 0, voice_user, sizeof voice_user, 0);
    expect_true("-F voice user opened its talker's epoch", slot0_call_names(TG_A, SRC_B, f.st));
    send_gps(&f, 0);
    expect_report(&f, "after a -F voice user", 0U, 0U, 0);

    // A checked MAC_PTT of the new talker names the call again.
    mac_ptt(&f, TG_A, SRC_B);
    send_gps(&f, 0);
    expect_report(&f, "after a checked ptt", SRC_B, TG_A, 1);

    // The call ends unseen by the state machine; a -F voice user naming the same talker opens a new epoch. Its
    // identity equals the one a checked message named, but no checked message named this epoch.
    dsd_call_snapshot before;
    expect_true("call before the end", dsd_call_state_get(f.st, 0U, &before) > 0);
    (void)dsd_call_state_end_ex(f.st, 0U, 1.0, DSD_CALL_END_TERMINATOR);
    const uint8_t same_user[] = {0x01,
                                 0x00,
                                 (uint8_t)(TG_A >> 8),
                                 (uint8_t)TG_A,
                                 (uint8_t)(SRC_B >> 16),
                                 (uint8_t)(SRC_B >> 8),
                                 (uint8_t)(SRC_B & 0xFFU)};
    send_mac(&f, 0, same_user, sizeof same_user, 0);
    dsd_call_snapshot after;
    expect_true("-F voice user opened a new epoch", slot0_call_names(TG_A, SRC_B, f.st)
                                                        && dsd_call_state_get(f.st, 0U, &after) > 0
                                                        && after.epoch != before.epoch);
    send_gps(&f, 0);
    expect_report(&f, "same talker, new epoch", 0U, 0U, 0);

    // Released: no call, no one to name; the position still counts.
    p25_sm_release(p25_sm_get_ctx(), f.opts, f.st, "test-release");
    send_gps(&f, 0);
    expect_report(&f, "after release", 0U, 0U, 0);
    fixture_close(&f);
}

/* A voice start emitted without a verdict (the convenience emitters) is unchecked, whatever the grant was. */
static void
test_unstamped_voice_start(void) {
    fixture f;
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    (void)p25_sm_emit_ptt_call(f.opts, f.st, 0, (int)TG_A, 0, (int)SRC_A, 1, 0x00);
    expect_true("unstamped call open", slot0_call_names(TG_A, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "unstamped ptt", 0U, 0U, 0);
    fixture_close(&f);
}

/* After the call ended (MAC_END_PTT, hangtime) a position names no one: the call is no longer ACTIVE. */
static void
test_hangtime(void) {
    fixture f;
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    mac_ptt(&f, TG_A, SRC_A);
    send_gps(&f, 0);
    expect_report(&f, "before the end", SRC_A, TG_A, 1);
    uint8_t end[18] = {0};
    end[0] = 0x40; // MAC_END_PTT (opcode 2)
    end[13] = (uint8_t)(SRC_A >> 16);
    end[14] = (uint8_t)(SRC_A >> 8);
    end[15] = (uint8_t)(SRC_A & 0xFFU);
    end[16] = (uint8_t)(TG_A >> 8);
    end[17] = (uint8_t)TG_A;
    send_facch(&f, end);
    dsd_call_snapshot call;
    expect_true("call ended", dsd_call_state_get(f.st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    expect_true("still on the voice channel", p25_sm_get_ctx()->state == P25_SM_TUNED);
    send_gps(&f, 0);
    expect_report(&f, "hangtime", 0U, 0U, 0);
    fixture_close(&f);
}

/* With trunking off a checked MAC_PTT names the call by itself; a -F voice user naming another talker does not. */
static void
test_conventional(void) {
    fixture f;
    fixture_open(&f);
    f.opts->trunk_enable = 0;
    mac_ptt(&f, TG_A, SRC_A);
    expect_true("conventional call open", slot0_call_names(TG_A, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "conventional checked ptt", SRC_A, TG_A, 1);

    const uint8_t voice_user[] = {0x01,
                                  0x00,
                                  (uint8_t)(TG_A >> 8),
                                  (uint8_t)TG_A,
                                  (uint8_t)(SRC_B >> 16),
                                  (uint8_t)(SRC_B >> 8),
                                  (uint8_t)(SRC_B & 0xFFU)};
    send_mac(&f, 0, voice_user, sizeof voice_user, 0);
    expect_true("conventional -F voice user", slot0_call_names(TG_A, SRC_B, f.st));
    send_gps(&f, 0);
    expect_report(&f, "conventional after a -F voice user", 0U, 0U, 0);
    fixture_close(&f);
}

static uint64_t
slot0_epoch(const dsd_state* st) {
    dsd_call_snapshot call;
    return dsd_call_state_get(st, 0U, &call) > 0 ? call.epoch : 0U;
}

/* A checked continuation that names the talkgroup but no source (MAC_ACTIVE with source 0) keeps the checked source
 * the call already has: a field a message omits is never cleared or downgraded within the epoch. */
static void
test_sourceless_continuation_keeps_source(void) {
    // Trunking off: the conventional publication.
    fixture f;
    fixture_open(&f);
    f.opts->trunk_enable = 0;
    mac_ptt(&f, TG_A, SRC_A);
    send_gps(&f, 0);
    expect_report(&f, "conventional ptt", SRC_A, TG_A, 1);
    const uint64_t epoch = slot0_epoch(f.st);
    mac_active(&f, TG_A, 0U);
    expect_true("conventional source-less active: same call",
                slot0_call_names(TG_A, SRC_A, f.st) && slot0_epoch(f.st) == epoch);
    send_gps(&f, 0);
    expect_report(&f, "conventional source-less active", SRC_A, TG_A, 1);
    fixture_close(&f);

    // Trunking on, a -F grant: the checked PTT named all three; the source-less MAC_ACTIVE keeps them.
    fixture_open(&f);
    group_grant(&f, SRC_A, 0);
    mac_ptt(&f, TG_A, SRC_A);
    mac_active(&f, TG_A, 0U);
    send_gps(&f, 0);
    expect_report(&f, "trunked source-less active after a checked ptt", SRC_A, TG_A, 1);
    fixture_close(&f);
}

/* A checked message naming the identity the call already has promotes its unchecked fields, without a new epoch. */
static void
test_checked_repeat_promotes(void) {
    // Voice start whose identity is unchanged (MAC_ACTIVE naming the inherited source).
    fixture f;
    fixture_open(&f);
    group_grant(&f, SRC_A, 0);
    mac_active(&f, TG_A, 0U);
    send_gps(&f, 0);
    expect_report(&f, "-F grant, source-less active", 0U, 0U, 0);
    uint64_t epoch = slot0_epoch(f.st);
    mac_active(&f, TG_A, SRC_A);
    expect_true("checked active naming the same source: same epoch", slot0_epoch(f.st) == epoch);
    expect_true("checked active naming the same source: slot verdict",
                p25_sm_get_ctx()->slots[0].identity_checked == P25_SM_IDENTITY_CHECKED_ALL);
    send_gps(&f, 0);
    expect_report(&f, "checked active naming the same source", SRC_A, TG_A, 1);
    fixture_close(&f);

    // A checked repeat of the group grant (the duplicate-grant path).
    fixture_open(&f);
    group_grant(&f, SRC_A, 0);
    mac_active(&f, TG_A, 0U);
    send_gps(&f, 0);
    expect_report(&f, "-F grant before the checked repeat", 0U, 0U, 0);
    epoch = slot0_epoch(f.st);
    group_grant(&f, SRC_A, 1);
    expect_true("checked group grant repeat: same epoch", slot0_epoch(f.st) == epoch);
    expect_true("checked group grant repeat: slot verdict",
                p25_sm_get_ctx()->slots[0].identity_checked == P25_SM_IDENTITY_CHECKED_ALL);
    send_gps(&f, 0);
    expect_report(&f, "checked group grant repeat", SRC_A, TG_A, 1);
    // A failed repeat naming the same identity downgrades nothing.
    group_grant(&f, SRC_A, 0);
    expect_true("-F group grant repeat: slot verdict kept",
                p25_sm_get_ctx()->slots[0].identity_checked == P25_SM_IDENTITY_CHECKED_ALL);
    send_gps(&f, 0);
    expect_report(&f, "-F group grant repeat", SRC_A, TG_A, 1);
    fixture_close(&f);

    // A checked repeat of the private grant verifies the destination the PTT cannot name.
    fixture_open(&f);
    private_grant(&f, SRC_A, 0);
    mac_ptt(&f, 0U, SRC_A);
    send_gps(&f, 0);
    expect_report(&f, "-F private grant", 0U, 0U, 0);
    epoch = slot0_epoch(f.st);
    private_grant(&f, SRC_A, 1);
    expect_true("checked private grant repeat: same epoch", slot0_epoch(f.st) == epoch);
    send_gps(&f, 0);
    expect_report(&f, "checked private grant repeat", SRC_A, UNIT_B, 0);
    fixture_close(&f);

    // A voice start without a passed check naming the same identity (a service change, so it is committed) keeps the
    // verdicts the checked PTT gave.
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    mac_ptt(&f, TG_A, SRC_A);
    epoch = slot0_epoch(f.st);
    (void)p25_sm_emit_active_call(f.opts, f.st, 0, (int)TG_A, 0, (int)SRC_A, 1, 0x01);
    expect_true("unchecked active, same identity: same epoch", slot0_epoch(f.st) == epoch);
    expect_true("unchecked active, same identity: slot verdict kept",
                p25_sm_get_ctx()->slots[0].identity_checked == P25_SM_IDENTITY_CHECKED_ALL);
    send_gps(&f, 0);
    expect_report(&f, "unchecked active, same identity", SRC_A, TG_A, 1);
    fixture_close(&f);
}

/* A repeat of the grant (the duplicate-grant path: the same carrier, slot and talkgroup) naming another talker replaces
 * the assignment's source with its own, and its own verdict: the earlier talker's checked source never names a later
 * report, before voice or during it. */
static void
test_duplicate_grant_replaces_source(void) {
    for (int crc_ok = 0; crc_ok < 2; crc_ok++) {
        char label[160];
        // Before either talker's voice: a source-less MAC_ACTIVE inherits the repeat's talker.
        fixture f;
        fixture_open(&f);
        group_grant(&f, SRC_A, 1);
        group_grant(&f, SRC_B, crc_ok);
        const p25_sm_slot_ctx_t* slot = &p25_sm_get_ctx()->slots[0];
        DSD_SNPRINTF(label, sizeof label, "repeat naming another talker (crc %d): assignment", crc_ok);
        expect_true(label, slot->src == (int)SRC_B
                               && (slot->identity_checked & P25_SM_IDENTITY_CHECKED_SOURCE)
                                      == (crc_ok ? P25_SM_IDENTITY_CHECKED_SOURCE : 0)
                               && (slot->identity_checked & P25_SM_IDENTITY_CHECKED_TARGET) != 0);
        mac_active(&f, TG_A, 0U);
        DSD_SNPRINTF(label, sizeof label, "repeat naming another talker (crc %d): call", crc_ok);
        expect_true(label, slot0_call_names(TG_A, SRC_B, f.st));
        send_gps(&f, 0);
        DSD_SNPRINTF(label, sizeof label, "repeat naming another talker before voice (crc %d)", crc_ok);
        expect_report(&f, label, crc_ok ? SRC_B : 0U, TG_A, 1);
        fixture_close(&f);

        // During the first talker's checked voice: the repeat's talker and verdict name the next report.
        fixture_open(&f);
        group_grant(&f, SRC_A, 1);
        mac_ptt(&f, TG_A, SRC_A);
        send_gps(&f, 0);
        expect_report(&f, "first talker's voice", SRC_A, TG_A, 1);
        group_grant(&f, SRC_B, crc_ok);
        DSD_SNPRINTF(label, sizeof label, "repeat naming another talker during voice (crc %d): call", crc_ok);
        expect_true(label, slot0_call_names(TG_A, SRC_B, f.st));
        send_gps(&f, 0);
        DSD_SNPRINTF(label, sizeof label, "repeat naming another talker during voice (crc %d)", crc_ok);
        expect_report(&f, label, crc_ok ? SRC_B : 0U, TG_A, 1);
        fixture_close(&f);
    }
}

/* MAC_PTT of TG_A by @p src, encrypted: ALG @p algid, KID @p kid, a made-up MI. */
static void
mac_ptt_encrypted(fixture* f, uint32_t src, uint8_t algid, uint16_t kid) {
    uint8_t mac[18] = {0};
    mac[0] = 0x20;
    for (int i = 1; i <= 9; i++) {
        mac[i] = (uint8_t)(0x10 + i);
    }
    mac[10] = algid;
    mac[11] = (uint8_t)(kid >> 8);
    mac[12] = (uint8_t)kid;
    mac[13] = (uint8_t)(src >> 16);
    mac[14] = (uint8_t)(src >> 8);
    mac[15] = (uint8_t)src;
    mac[16] = (uint8_t)(TG_A >> 8);
    mac[17] = (uint8_t)TG_A;
    send_facch(f, mac);
}

/* Group Voice Channel Grant - Explicit (0xC0) of TG_A by @p src on VC_CHANNEL, with the service options @p svc. */
static void
group_grant_svc(fixture* f, uint32_t src, uint8_t svc) {
    const uint8_t grant[] = {0xC0,
                             svc,
                             (uint8_t)(VC_CHANNEL >> 8),
                             (uint8_t)(VC_CHANNEL & 0xFFU),
                             (uint8_t)(VC_CHANNEL >> 8),
                             (uint8_t)(VC_CHANNEL & 0xFFU),
                             (uint8_t)(TG_A >> 8),
                             (uint8_t)TG_A,
                             (uint8_t)(src >> 16),
                             (uint8_t)(src >> 8),
                             (uint8_t)src};
    send_mac(f, 0, grant, sizeof grant, 1);
}

/* A repeated grant that names another talker starts that talker's call with none of the earlier talker's call-specific
 * state: not its ALG ID, key ID or MI (even when the repeat's service options classify the call as the first grant's
 * did), not its emergency service option, not its talker alias. Only what the repeat itself establishes (its service
 * options) describes the new call. */
static void
test_duplicate_grant_new_talker_resets_call_state(void) {
    fixture f;
    fixture_open(&f);
    group_grant_svc(&f, SRC_A, 0xC0); // emergency, encrypted
    mac_ptt_encrypted(&f, SRC_A, 0x84, 0x1234);
    dsd_call_snapshot call;
    expect_true("first talker: encrypted call", dsd_call_state_get(f.st, 0U, &call) > 0 && call.ota_source_id == SRC_A
                                                    && call.algid == 0x84 && call.kid == 0x1234
                                                    && call.emergency == 1U);
    DSD_SNPRINTF(f.st->generic_talker_alias[0], sizeof f.st->generic_talker_alias[0], "%s", "Unit A");

    group_grant_svc(&f, SRC_B, 0x40); // the same carrier, slot and talkgroup (the duplicate path), still encrypted
    expect_true("repeat naming another talker: its call", slot0_call_names(TG_A, SRC_B, f.st));
    expect_true("repeat naming another talker: no inherited ALG/KID",
                dsd_call_state_get(f.st, 0U, &call) > 0 && call.algid == 0U && call.kid == 0U
                    && f.st->payload_algid == 0 && f.st->payload_keyid == 0 && f.st->payload_miP == 0U);
    expect_true("repeat naming another talker: no inherited emergency", call.emergency == 0U);
    expect_true("repeat naming another talker: crypto pending",
                f.st->p25_crypto_state[0] == DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    expect_true("repeat naming another talker: no inherited alias", f.st->generic_talker_alias[0][0] == '\0');
    fixture_close(&f);

    // A repeat naming the same talker keeps the call's crypto.
    fixture_open(&f);
    group_grant_svc(&f, SRC_A, 0x40);
    mac_ptt_encrypted(&f, SRC_A, 0x84, 0x1234);
    group_grant_svc(&f, SRC_A, 0x40);
    expect_true("repeat naming the same talker: keeps ALG/KID", dsd_call_state_get(f.st, 0U, &call) > 0
                                                                    && call.ota_source_id == SRC_A && call.algid == 0x84
                                                                    && call.kid == 0x1234);
    fixture_close(&f);

    // A repeat naming another talker with its own clear service options: the new call is clear.
    fixture_open(&f);
    group_grant_svc(&f, SRC_A, 0x40);
    mac_ptt_encrypted(&f, SRC_A, 0x84, 0x1234);
    group_grant_svc(&f, SRC_B, 0x00);
    expect_true("repeat naming another clear talker: clear",
                slot0_call_names(TG_A, SRC_B, f.st) && f.st->p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR
                    && f.st->payload_algid == 0 && f.st->payload_keyid == 0);
    fixture_close(&f);
}

/* A checked voice user naming another talker within the same assignment supersedes the grant's talker: a source-less
 * MAC_ACTIVE that follows keeps the latest talker (with that voice user's verdict), never the grant's. */
static void
test_voice_user_supersedes_grant_talker(void) {
    fixture f;
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    sacch_group_voice_user(&f, TG_A, SRC_B);
    expect_true("voice user names the next talker", slot0_call_names(TG_A, SRC_B, f.st));
    mac_active(&f, TG_A, 0U);
    expect_true("source-less active keeps the voice user's talker", slot0_call_names(TG_A, SRC_B, f.st));
    send_gps(&f, 0);
    expect_report(&f, "source-less active after a voice user naming another talker", SRC_B, TG_A, 1);
    fixture_close(&f);

    // During voice: the grant's talker spoke first, then a voice user names the next one.
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    mac_ptt(&f, TG_A, SRC_A);
    sacch_group_voice_user(&f, TG_A, SRC_B);
    mac_active(&f, TG_A, 0U);
    expect_true("during voice: source-less active keeps the voice user's talker", slot0_call_names(TG_A, SRC_B, f.st));
    send_gps(&f, 0);
    expect_report(&f, "during voice: source-less active after a voice user naming another talker", SRC_B, TG_A, 1);
    fixture_close(&f);

    // A voice user of another talkgroup on the slot leaves the assignment's talker unusable: the later source-less
    // MAC_ACTIVE of the assignment's talkgroup names no one.
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    sacch_group_voice_user(&f, TG_A + 1U, SRC_B);
    mac_active(&f, TG_A, 0U);
    dsd_call_snapshot call;
    expect_true("other talkgroup's voice user: the grant's talker is not inherited",
                dsd_call_state_get(f.st, 0U, &call) > 0 && call.ota_source_id != SRC_A);
    send_gps(&f, 0);
    expect_report(&f, "other talkgroup's voice user, then a source-less active", 0U, 0U, 0);
    fixture_close(&f);
}

/* On an FDMA carrier a repeat is matched by the target the call is judged by, so it can name another kind or target than
 * the assignment (a private call to radio TG_A on the group's carrier): each field it names with another value takes
 * that value with the repeat's own verdict. */
static void
test_fdma_repeat_replaces_kind(void) {
    for (int crc_ok = 0; crc_ok < 2; crc_ok++) {
        fixture f;
        fixture_open(&f);
        // IDEN 2: an FDMA channel plan on the same band.
        f.st->p25_iden_fdma[2].base_freq = CC_HZ / 5;
        f.st->p25_iden_fdma[2].chan_type = 1;
        f.st->p25_iden_fdma[2].chan_spac = 100;
        f.st->p25_iden_fdma[2].trust = 2;
        f.st->p25_iden_fdma[2].populated = 1;
        f.st->p25_chan_tdma_explicit[2] = 1;
        const uint8_t group[] = {
            0x40, 0x00, 0x20, 0x0A, 0x00, (uint8_t)TG_A, (uint8_t)(SRC_A >> 16), (uint8_t)(SRC_A >> 8), (uint8_t)SRC_A};
        send_mac(&f, 0, group, sizeof group, 1);
        const p25_sm_ctx_t* sm = p25_sm_get_ctx();
        expect_true("fdma group grant followed", sm->state == P25_SM_TUNED && sm->vc_is_tdma == 0
                                                     && sm->slots[0].is_group == 1
                                                     && sm->slots[0].identity_checked == P25_SM_IDENTITY_CHECKED_ALL);
        const uint8_t unit[] = {
            0x44, 0x20, 0x0A, 0x00, 0x00, (uint8_t)TG_A, (uint8_t)(SRC_B >> 16), (uint8_t)(SRC_B >> 8), (uint8_t)SRC_B};
        send_mac(&f, 0, unit, sizeof unit, crc_ok);
        const p25_sm_slot_ctx_t* slot = &sm->slots[0];
        char label[160];
        DSD_SNPRINTF(label, sizeof label, "fdma repeat naming a private call (crc %d)", crc_ok);
        expect_true(label, slot->is_group == 0 && slot->dst == (int)TG_A && slot->ota_tg == 0 && slot->src == (int)SRC_B
                               && slot->identity_checked == (crc_ok ? P25_SM_IDENTITY_CHECKED_ALL : 0));
        fixture_close(&f);
    }
}

/* A checked SACCH MAC_SIGNAL voice user that establishes the call on late entry (no MAC_PTT/MAC_ACTIVE) names it. */
static void
test_sacch_voice_user(void) {
    fixture f;
    fixture_open(&f);
    f.opts->trunk_enable = 0;
    sacch_group_voice_user(&f, TG_A, SRC_A);
    expect_true("conventional sacch group voice user open", slot0_call_names(TG_A, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "conventional sacch group voice user", SRC_A, TG_A, 1);
    fixture_close(&f);

    fixture_open(&f);
    f.opts->trunk_enable = 0;
    sacch_private_voice_user(&f, UNIT_B, SRC_A);
    expect_true("conventional sacch private voice user open", slot0_call_names(UNIT_B, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "conventional sacch private voice user", SRC_A, UNIT_B, 0);
    fixture_close(&f);

    // Trunking on after a -F grant: the checked voice user names all three fields itself.
    fixture_open(&f);
    group_grant(&f, SRC_A, 0);
    sacch_group_voice_user(&f, TG_A, SRC_A);
    expect_true("trunked sacch group voice user open", slot0_call_names(TG_A, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "trunked sacch group voice user after a -F grant", SRC_A, TG_A, 1);
    fixture_close(&f);
}

/* MAC_ACTIVE (opcode 4) carrying no voice user message: the voice start names no identity of its own. */
static void
mac_active_addressless(fixture* f) {
    const uint8_t mac[18] = {0x80};
    send_facch(f, mac);
}

/* A checked Network Status Broadcast (Abbreviated, 0x7B) of @p sysid on the fixture's WACN and NAC. */
static void
network_status(fixture* f, uint16_t sysid) {
    const uint32_t wacn = 0xBEE00U;
    const uint16_t nac = 0x293U;
    const uint8_t nsb[] = {0x7B,
                           0x00,
                           (uint8_t)(wacn >> 12),
                           (uint8_t)(wacn >> 4),
                           (uint8_t)(((wacn & 0x0FU) << 4) | ((sysid >> 8) & 0x0FU)),
                           (uint8_t)sysid,
                           (uint8_t)(VC_CHANNEL >> 8),
                           (uint8_t)(VC_CHANNEL & 0xFFU),
                           0x00,
                           (uint8_t)((nac >> 8) & 0x0FU),
                           (uint8_t)nac};
    send_mac(f, 0, nsb, sizeof nsb, 1);
}

/* A Phase 1 Group Voice Channel User link control (hard decode) of @p tg by @p src, as an LDU1 dispatches it. */
static void
p1_group_voice_user(fixture* f, uint16_t tg, uint32_t src) {
    const uint8_t bytes[9] = {
        0x00,        0x00, 0x00, 0x00, (uint8_t)(tg >> 8), (uint8_t)tg, (uint8_t)(src >> 16), (uint8_t)(src >> 8),
        (uint8_t)src};
    uint8_t lcw[72];
    for (int i = 0; i < 72; i++) {
        lcw[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1U);
    }
    f->st->synctype = DSD_SYNC_P25P1_POS;
    f->st->lastsynctype = DSD_SYNC_P25P1_POS;
    p25_lcw(f->opts, f->st, lcw, 0);
    f->st->synctype = DSD_SYNC_P25P2_POS;
    f->st->lastsynctype = DSD_SYNC_P25P2_POS;
}

/* The attribution rule (issue #800, dsd_position_attribution_grant()): a Phase 2 report borrows its sender only from a
 * Phase 2 call. A conventional Phase 1 call still ACTIVE on slot 0 (its terminator missed), whose own checked voice user
 * named its source, names no Phase 2 report's sender, before or after a Phase 2 call of its own replaces it. */
static void
test_phase1_call_names_no_phase2_report(void) {
    fixture f;
    fixture_open(&f);
    f.opts->trunk_enable = 0;
    p1_group_voice_user(&f, (uint16_t)TG_A, SRC_A);
    dsd_call_snapshot call;
    expect_true("phase 1 call open", dsd_call_state_get(f.st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                                         && DSD_SYNC_IS_P25P1(call.protocol) && call.ota_source_id == SRC_A);
    send_gps(&f, 0);
    expect_report(&f, "phase 2 report during a phase 1 call", 0U, 0U, 0);
    // The Phase 2 call's own checked PTT names it.
    mac_ptt(&f, TG_A, SRC_B);
    expect_true("phase 2 call open", dsd_call_state_get(f.st, 0U, &call) > 0 && DSD_SYNC_IS_P25P2(call.protocol)
                                         && call.ota_source_id == SRC_B);
    send_gps(&f, 0);
    expect_report(&f, "phase 2 report on its own call", SRC_B, TG_A, 1);
    fixture_close(&f);
}

/* The attribution rule's reception: a checked identity counts only under the reception it was named under. A checked
 * Network Status Broadcast naming another system (conventional, no terminator between), or another NAC on the
 * carrier, leaves the next report anonymous until a checked message of the call names it again under the new one. */
static void
test_reception_change(void) {
    for (int change = 0; change < 2; change++) {
        fixture f;
        fixture_open(&f);
        f.opts->trunk_enable = 0;
        dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A3U);
        dsd_rxid_note_access(f.st, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
        mac_ptt(&f, TG_A, SRC_A);
        send_gps(&f, 0);
        expect_report(&f, change ? "nac: before" : "system: before", SRC_A, TG_A, 1);
        const uint32_t before = f.st->rx_id.epoch;
        if (change == 0) {
            network_status(&f, 0x1A4U);
        } else {
            dsd_rxid_note_access(f.st, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x294U);
        }
        expect_true(change ? "nac: reception changed" : "system: reception changed", f.st->rx_id.epoch != before);
        expect_true(change ? "nac: call still open" : "system: call still open", slot0_call_names(TG_A, SRC_A, f.st));
        send_gps(&f, 0);
        expect_report(&f, change ? "nac: after the change" : "system: after the change", 0U, 0U, 0);
        // A source-less continuation keeps the call but names no one under the new reception.
        mac_active(&f, TG_A, 0U);
        send_gps(&f, 0);
        expect_report(&f, change ? "nac: source-less continuation" : "system: source-less continuation", 0U, 0U, 0);
        // A checked voice user naming all three under the new reception names the call again.
        sacch_group_voice_user(&f, TG_A, SRC_A);
        send_gps(&f, 0);
        expect_report(&f, change ? "nac: renamed" : "system: renamed", SRC_A, TG_A, 1);
        fixture_close(&f);
    }

    // Trunked: an assignment accepted under one system is not inherited by a voice start under another.
    fixture f;
    fixture_open(&f);
    dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A3U);
    group_grant(&f, SRC_A, 1);
    dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A4U);
    mac_active(&f, TG_A, 0U);
    expect_true("trunked: source-less call open", slot0_call_names(TG_A, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "trunked: assignment of another reception", 0U, 0U, 0);
    fixture_close(&f);
}

/* Issue #800, fail closed: both P25 phases send one system's identity, but a transmission of the other phase is another
 * reception. A Phase 2 call named by a checked MAC_PTT stays open while Phase 1 frames are heard on the carrier and
 * Phase 2 comes back: the talker named before the change no longer names a report after it, which counts anonymous,
 * until a checked voice user names the talker again. Without the Phase 1 frames the report keeps its sender. */
static void
test_phase_change(void) {
    for (int change = 0; change <= 1; change++) {
        fixture f;
        fixture_open(&f);
        f.opts->trunk_enable = 0;
        dsd_rxid_note_sync(f.st, DSD_SYNC_P25P2_POS);
        dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A3U);
        mac_ptt(&f, TG_A, SRC_A);
        send_gps(&f, 0);
        expect_report(&f, "phase: before", SRC_A, TG_A, 1);
        if (change) {
            dsd_rxid_note_sync(f.st, DSD_SYNC_P25P1_POS);
            dsd_rxid_note_sync(f.st, DSD_SYNC_P25P2_POS);
        }
        expect_true("phase: call still open", slot0_call_names(TG_A, SRC_A, f.st));
        send_gps(&f, 0);
        expect_report(&f, change ? "phase: after the change" : "phase: no change", change ? 0U : SRC_A,
                      change ? 0U : TG_A, change ? 0 : 1);
        sacch_group_voice_user(&f, TG_A, SRC_A);
        send_gps(&f, 0);
        expect_report(&f, "phase: renamed", SRC_A, TG_A, 1);
        fixture_close(&f);
    }
}

/* One call's identity: a checked voice user of another talkgroup takes the slot from the assignment, and an addressless
 * MAC_ACTIVE then starts the assignment's call. Its talker is not the other talkgroup's: the report never pairs that
 * talker with the assignment's talkgroup, a pair no message named. */
static void
test_addressless_active_after_other_talkgroup(void) {
    fixture f;
    fixture_open(&f);
    group_grant(&f, SRC_A, 1);
    sacch_group_voice_user(&f, TG_A + 1U, SRC_B);
    expect_true("other talkgroup's call", slot0_call_names(TG_A + 1U, SRC_B, f.st));
    mac_active_addressless(&f);
    dsd_call_snapshot call;
    expect_true("addressless active: the assignment's call, no talker",
                dsd_call_state_get(f.st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                    && call.ota_target_id == TG_A && call.ota_source_id == 0U);
    send_gps(&f, 0);
    expect_report(&f, "addressless active after another talkgroup's voice user", 0U, 0U, 0);
    fixture_close(&f);
}

/* Issue #800, fail closed: a checked message whose source field holds a special address ($FFFFFC-$FFFFFF: FNE use,
 * System Default, Registration Default, ALL units) names no radio, and is no evidence for the talker the assignment
 * held. After a checked grant and PTT naming SRC_A, a checked PTT (a new transmission) or MAC_ACTIVE naming such an
 * address leaves the next report anonymous in the feed and in -L, and no track of the address is made; a checked PTT
 * naming SRC_A again names it. */
static void
test_special_address_source(void) {
    static const uint32_t specials[] = {0xFFFFFCU, 0xFFFFFDU, 0xFFFFFEU, 0xFFFFFFU};
    for (size_t i = 0; i < sizeof specials / sizeof specials[0]; i++) {
        for (int active = 0; active < 2; active++) {
            fixture f;
            fixture_open(&f);
            char tag[96];
            DSD_SNPRINTF(tag, sizeof tag, "%s from %06X", active ? "MAC_ACTIVE" : "MAC_PTT", (unsigned)specials[i]);
            group_grant(&f, SRC_A, 1);
            mac_ptt(&f, TG_A, SRC_A);
            send_gps(&f, 0);
            expect_report(&f, "special address: before", SRC_A, TG_A, 1);
            if (active) {
                mac_active(&f, TG_A, specials[i]);
            } else {
                mac_ptt(&f, TG_A, specials[i]);
            }
            send_gps(&f, 0);
            expect_report(&f, tag, 0U, 0U, 0);
            mac_ptt(&f, TG_A, SRC_A);
            send_gps(&f, 0);
            expect_report(&f, "special address: named again", SRC_A, TG_A, 1);
            fixture_close(&f);
        }
    }
}

/* Issue #800, fail closed: a checked SACCH MAC_SIGNAL voice user of the call's talkgroup whose source field holds a
 * special address names no radio, and takes the source's verdict away like any other message does: the call keeps
 * SRC_A for display, and the next report is anonymous in the feed and in -L until a checked message names a radio. */
static void
test_special_address_voice_user(void) {
    static const uint32_t specials[] = {0xFFFFFCU, 0xFFFFFDU, 0xFFFFFEU, 0xFFFFFFU};
    for (size_t i = 0; i < sizeof specials / sizeof specials[0]; i++) {
        fixture f;
        fixture_open(&f);
        char tag[96];
        DSD_SNPRINTF(tag, sizeof tag, "sacch voice user from %06X", (unsigned)specials[i]);
        group_grant(&f, SRC_A, 1);
        mac_ptt(&f, TG_A, SRC_A);
        send_gps(&f, 0);
        expect_report(&f, "special voice user: before", SRC_A, TG_A, 1);
        sacch_group_voice_user(&f, TG_A, specials[i]);
        expect_true("special voice user: call kept", slot0_call_names(TG_A, SRC_A, f.st));
        send_gps(&f, 0);
        expect_report(&f, tag, 0U, 0U, 0);
        sacch_group_voice_user(&f, TG_A, SRC_A);
        send_gps(&f, 0);
        expect_report(&f, "special voice user: named again", SRC_A, TG_A, 1);
        fixture_close(&f);
    }
}

#define HOME_WACN    0xBEE00U
#define HOME_SYS     0x1A3U
#define FOREIGN_WACN 0xABCDEU
#define FOREIGN_SYS  0x222U

/* The seven SUID octets (WACN 20 bits, System ID 12 bits, unit ID 24 bits) of @p wacn:@p sys.@p id into @p out. */
static void
put_suid(uint8_t* out, uint32_t wacn, uint16_t sys, uint32_t id) {
    out[0] = (uint8_t)(wacn >> 12);
    out[1] = (uint8_t)(wacn >> 4);
    out[2] = (uint8_t)(((wacn & 0x0FU) << 4) | ((sys >> 8) & 0x0FU));
    out[3] = (uint8_t)sys;
    out[4] = (uint8_t)(id >> 16);
    out[5] = (uint8_t)(id >> 8);
    out[6] = (uint8_t)id;
}

/* Unit to Unit Voice Service Channel Grant - Extended VCH (0xC4, TIA-102.BBAD-A Fig 33): CHAN-T, CHAN-R, the source
 * SUID, the target address UNIT_B. */
static void
extended_private_grant(fixture* f, uint32_t wacn, uint16_t sys, uint32_t src) {
    uint8_t grant[15] = {0xC4, (uint8_t)(VC_CHANNEL >> 8), (uint8_t)(VC_CHANNEL & 0xFFU), (uint8_t)(VC_CHANNEL >> 8),
                         (uint8_t)(VC_CHANNEL & 0xFFU)};
    put_suid(&grant[5], wacn, sys, src);
    grant[12] = (uint8_t)(UNIT_B >> 16);
    grant[13] = (uint8_t)(UNIT_B >> 8);
    grant[14] = (uint8_t)UNIT_B;
    send_mac(f, 0, grant, sizeof grant, 1);
}

/* Group Voice Channel User - Extended (0x21, TIA-102.BBAD-A Fig 20) of TG_A: the working source address @p src and
 * the source SUID @p wacn:@p sys.@p unit, as a SACCH MAC_SIGNAL (@p active 0) or in a FACCH MAC_ACTIVE (1). */
static void
extended_group_voice_user(fixture* f, uint32_t wacn, uint16_t sys, uint32_t src, uint32_t unit, int active) {
    uint8_t msg[15] = {
        0x21, 0x00, (uint8_t)(TG_A >> 8), (uint8_t)TG_A, (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src};
    put_suid(&msg[7], wacn, sys, unit);
    if (!active) {
        send_sacch_signal(f, msg, 14U);
        return;
    }
    uint8_t mac[18] = {0x80};
    DSD_MEMCPY(&mac[1], msg, 14U);
    send_facch(f, mac);
}

/* Unit to Unit Voice Channel User - Extended (0x22, TIA-102.BBAD-A) to UNIT_B: the working source address @p src and
 * the source SUID @p wacn:@p sys.@p unit, as a SACCH MAC_SIGNAL (@p active 0) or in a FACCH MAC_ACTIVE (1). */
static void
extended_private_voice_user(fixture* f, uint32_t wacn, uint16_t sys, uint32_t src, uint32_t unit, int active) {
    uint8_t msg[15] = {0x22,
                       0x00,
                       (uint8_t)(UNIT_B >> 16),
                       (uint8_t)(UNIT_B >> 8),
                       (uint8_t)UNIT_B,
                       (uint8_t)(src >> 16),
                       (uint8_t)(src >> 8),
                       (uint8_t)src};
    put_suid(&msg[8], wacn, sys, unit);
    if (!active) {
        send_sacch_signal(f, msg, 15U);
        return;
    }
    uint8_t mac[18] = {0x80};
    DSD_MEMCPY(&mac[1], msg, 15U);
    send_facch(f, mac);
}

/* Issue #800, fail closed: a source named only by a home SUID is a local radio only when that home is the reception's
 * own system (its checked WACN and System ID). The extended VCH unit-to-unit grant (0xC4) carries the source's SUID
 * alone: from another system, or with the reception's system not known yet, its source is never checked, so the
 * addressless MAC_ACTIVE that follows leaves the next report anonymous (no sender scope, no local radio-list label);
 * from the reception's own system it names the talker. An extended voice user (0x21 group, 0x22 unit to unit) carries
 * the talker's working source address as well as its home SUID: the working address names the talker on this system,
 * whatever system the SUID's home is (another, this one, or none known yet) and whatever unit ID it holds, as a SACCH
 * MAC_SIGNAL and inside a MAC_ACTIVE, so a roaming radio's next report names it. */
static void
test_foreign_home_id(void) {
    for (int home = 0; home < 3; home++) {
        fixture f;
        fixture_open(&f);
        if (home != 2) {
            dsd_rxid_note_p25_network(f.st, HOME_WACN, HOME_SYS);
        }
        extended_private_grant(&f, home == 1 ? HOME_WACN : FOREIGN_WACN, home == 1 ? HOME_SYS : FOREIGN_SYS, SRC_A);
        expect_true("extended grant followed", f.opts->trunk_is_tuned == 1);
        mac_active_addressless(&f);
        send_gps(&f, 0);
        static const char* const tags[] = {"0xC4 from another system", "0xC4 from the home system",
                                           "0xC4 with no reception system"};
        expect_report(&f, tags[home], home == 1 ? SRC_A : 0U, home == 1 ? UNIT_B : 0U, 0);
        fixture_close(&f);
    }
    static const char* const homes[] = {"another system", "the home system", "no known system"};
    for (int k = 0; k < 12; k++) {
        const int active = k & 1;
        const int private_call = (k & 2) != 0;
        const int home = k / 4;
        fixture f;
        fixture_open(&f);
        if (home != 2) {
            dsd_rxid_note_p25_network(f.st, HOME_WACN, HOME_SYS);
        }
        if (private_call) {
            private_grant(&f, SRC_B, 1);
        } else {
            group_grant(&f, SRC_B, 1);
        }
        /* The working address SRC_A; the SUID's unit ID SRC_B. */
        const uint32_t wacn = home == 1 ? HOME_WACN : FOREIGN_WACN;
        const uint16_t sys = home == 1 ? (uint16_t)HOME_SYS : (uint16_t)FOREIGN_SYS;
        if (private_call) {
            extended_private_voice_user(&f, wacn, sys, SRC_A, SRC_B, active);
        } else {
            extended_group_voice_user(&f, wacn, sys, SRC_A, SRC_B, active);
        }
        char tag[128];
        DSD_SNPRINTF(tag, sizeof tag, "%s %s, SUID of %s", private_call ? "0x22" : "0x21",
                     active ? "in MAC_ACTIVE" : "on SACCH", homes[home]);
        expect_true(tag, slot0_call_names(private_call ? UNIT_B : TG_A, SRC_A, f.st));
        send_gps(&f, 0);
        expect_report(&f, tag, SRC_A, private_call ? UNIT_B : TG_A, !private_call);
        fixture_close(&f);
    }
}

/* Issue #800: identity a checked message names after the reception changed is bound to the new reception. A trunked
 * assignment held from system 0x1A3 survives a change of the checked network identity to 0x1A4; the next checked
 * MAC_PTT names the talker (another one, or the grant's again) with its own verdicts under the new reception, so the
 * report names it. Nothing the grant checked under the old one is carried: an addressless MAC_ACTIVE straight after the
 * change still names no one. */
static void
test_retune_rebinds_identity(void) {
    static const uint32_t talkers[] = {SRC_B, SRC_A};
    for (size_t i = 0; i < sizeof talkers / sizeof talkers[0]; i++) {
        fixture f;
        fixture_open(&f);
        dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A3U);
        group_grant(&f, SRC_A, 1);
        mac_ptt(&f, TG_A, SRC_A);
        send_gps(&f, 0);
        expect_report(&f, "rebind: before", SRC_A, TG_A, 1);
        const uint32_t before = f.st->rx_id.epoch;
        dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A4U);
        expect_true("rebind: reception changed", f.st->rx_id.epoch != before);
        mac_ptt(&f, TG_A, talkers[i]);
        expect_true("rebind: call names the talker", slot0_call_names(TG_A, talkers[i], f.st));
        send_gps(&f, 0);
        expect_report(&f, talkers[i] == SRC_B ? "rebind: new talker" : "rebind: same talker", talkers[i], TG_A, 1);
        expect_true("rebind: assignment rebound",
                    p25_sm_get_ctx()->slots[0].identity_rx_gen == dsd_state_reception_generation(f.st));
        fixture_close(&f);
    }
}

/* Issue #800, fail closed: a verdict counts only under the reception it was earned under, whichever record a voice
 * start reuses it from. A trunked call stays ACTIVE while a checked network update names another system; an
 * addressless MAC_ACTIVE then keeps the call's talker for display, but neither the canonical call's record nor the
 * assignment lends it the verdict the old system gave. A later source-less message (a MAC_ACTIVE or a SACCH voice user
 * naming only the talkgroup) checks the target and kind under the new system, never the source: the next report stays
 * anonymous until a checked message names the talker there. The call is established by a PTT, or by a SACCH voice user
 * before any PTT. */
static void
test_reception_change_reuse(void) {
    for (int variant = 0; variant < 4; variant++) {
        const int by_voice_user = (variant & 1) != 0;
        const int then_voice_user = (variant & 2) != 0;
        char tag[128];
        DSD_SNPRINTF(tag, sizeof tag, "reuse after a reception change (%s, then a source-less %s)",
                     by_voice_user ? "voice user" : "ptt", then_voice_user ? "voice user" : "mac_active");
        fixture f;
        fixture_open(&f);
        dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A3U);
        group_grant(&f, SRC_A, 1);
        if (by_voice_user) {
            sacch_group_voice_user(&f, TG_A, SRC_A);
        } else {
            mac_ptt(&f, TG_A, SRC_A);
        }
        send_gps(&f, 0);
        expect_report(&f, "reuse: before", SRC_A, TG_A, 1);
        const uint32_t before = f.st->rx_id.epoch;
        network_status(&f, 0x1A4U);
        expect_true("reuse: reception changed", f.st->rx_id.epoch != before);
        mac_active_addressless(&f);
        if (then_voice_user) {
            sacch_group_voice_user(&f, TG_A, 0U);
        } else {
            mac_active(&f, TG_A, 0U);
        }
        expect_true("reuse: call still names the talker", slot0_call_names(TG_A, SRC_A, f.st));
        send_gps(&f, 0);
        expect_report(&f, tag, 0U, 0U, 0);
        fixture_close(&f);
    }
}

/* Issue #800, fail closed: a stored verdict counts only under the whole reception generation it was earned under, and
 * a change of P25 phase keeps the received identity's epoch. A trunked Phase 2 call's talker is named by a checked PTT
 * or SACCH voice user; Phase 1 frames are then heard on the carrier and Phase 2 comes back with no grant in between.
 * An addressless MAC_ACTIVE and a source-less MAC_ACTIVE or voice user keep the call's talker for display, but neither
 * the assignment nor the canonical call's record lends it the verdict: the next report is anonymous. Without the
 * Phase 1 frames the talker the call holds still names it. */
static void
test_phase_change_reuse(void) {
    for (int variant = 0; variant < 8; variant++) {
        const int by_voice_user = (variant & 1) != 0;
        const int then_voice_user = (variant & 2) != 0;
        const int change = (variant & 4) != 0;
        char tag[160];
        DSD_SNPRINTF(tag, sizeof tag, "reuse %s a phase change (%s, then a source-less %s)",
                     change ? "after" : "without", by_voice_user ? "voice user" : "ptt",
                     then_voice_user ? "voice user" : "mac_active");
        fixture f;
        fixture_open(&f);
        dsd_rxid_note_sync(f.st, DSD_SYNC_P25P2_POS);
        dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A3U);
        group_grant(&f, SRC_A, 1);
        if (by_voice_user) {
            sacch_group_voice_user(&f, TG_A, SRC_A);
        } else {
            mac_ptt(&f, TG_A, SRC_A);
        }
        send_gps(&f, 0);
        expect_report(&f, "phase reuse: before", SRC_A, TG_A, 1);
        const uint32_t epoch = f.st->rx_id.epoch;
        if (change) {
            dsd_rxid_note_sync(f.st, DSD_SYNC_P25P1_POS);
            dsd_rxid_note_sync(f.st, DSD_SYNC_P25P2_POS);
        }
        expect_true("phase reuse: the epoch stays", f.st->rx_id.epoch == epoch);
        mac_active_addressless(&f);
        if (then_voice_user) {
            sacch_group_voice_user(&f, TG_A, 0U);
        } else {
            mac_active(&f, TG_A, 0U);
        }
        expect_true("phase reuse: call still names the talker", slot0_call_names(TG_A, SRC_A, f.st));
        send_gps(&f, 0);
        expect_report(&f, tag, change ? 0U : SRC_A, change ? 0U : TG_A, change ? 0 : 1);
        fixture_close(&f);
    }
}

/* The engine's trunk tuning: the retune is applied to the received identity at once (dsd_rxid_trunk_tune()), as on the
 * decoder thread inside a frame dispatch. */
static dsd_trunk_tune_result
tune_and_retune(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    const dsd_trunk_tune_result result = tune_ok(opts, state, freq, ted_sps, request_id);
    if (dsd_trunk_tune_result_is_ok(result)) {
        dsd_rxid_trunk_tune(state);
    }
    return result;
}

/* Issue #800: a grant's checked talker carries to the channel the grant sends the receiver to, and only there. A
 * checked grant heard on a Phase 1 control channel retunes to a Phase 2 voice channel (another reception generation, and
 * the voice channel's first sync of the other phase moves nothing more); an addressless MAC_ACTIVE starts the call with
 * the grant's talker, and the report names it. Phase 1 frames heard on the voice channel and Phase 2 again end the
 * carry: the next source-less start lends the talker no verdict. */
static void
test_grant_carry_across_its_retune(void) {
    fixture f;
    fixture_open(&f);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune_and_retune,
        .tune_to_cc_request = tune_and_retune,
        .return_to_cc_request = return_ok,
    });
    dsd_rxid_note_sync(f.st, DSD_SYNC_P25P1_POS);
    dsd_rxid_note_p25_network(f.st, 0xBEE00U, 0x1A3U);
    const uint64_t cc_gen = dsd_state_reception_generation(f.st);
    group_grant(&f, SRC_A, 1);
    expect_true("grant carry: the grant retuned", dsd_state_reception_generation(f.st) != cc_gen);
    dsd_rxid_note_sync(f.st, DSD_SYNC_P25P2_POS);
    mac_active_addressless(&f);
    expect_true("grant carry: the grant's talker", slot0_call_names(TG_A, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "grant carry: on the granted channel", SRC_A, TG_A, 1);
    dsd_rxid_note_sync(f.st, DSD_SYNC_P25P1_POS);
    dsd_rxid_note_sync(f.st, DSD_SYNC_P25P2_POS);
    mac_active(&f, TG_A, 0U);
    expect_true("grant carry: call still names the talker", slot0_call_names(TG_A, SRC_A, f.st));
    send_gps(&f, 0);
    expect_report(&f, "grant carry: after a phase change on the channel", 0U, 0U, 0);
    fixture_close(&f);
}

/* Issue #800: an anonymous Phase 2 report stays off a Phase 1 call. A conventional Phase 1 group call whose voice user
 * named no source is still ACTIVE on slot 0 (its terminator missed); a Phase 2 L3Harris report gets no call epoch and
 * goes to the feed and -L anonymous, but never onto the Phase 1 call's history row. */
static void
test_anonymous_phase2_report_stays_off_phase1_row(void) {
    fixture f;
    fixture_open(&f);
    f.opts->trunk_enable = 0;
    p1_group_voice_user(&f, (uint16_t)TG_A, 0U);
    dsd_call_snapshot call;
    expect_true("anonymous phase 1 call open", dsd_call_state_get(f.st, 0U, &call) > 0
                                                   && call.phase == DSD_CALL_PHASE_ACTIVE
                                                   && DSD_SYNC_IS_P25P1(call.protocol) && call.ota_source_id == 0U);
    send_gps(&f, 0);
    expect_report(&f, "phase 2 report beside an anonymous phase 1 call", 0U, 0U, 0);
    const Event_History* row = &f.st->event_history_s[0].Event_History_Items[0];
    expect_true("phase 1 row keeps no phase 2 position", row->pos.valid == 0U && row->gps_s[0] == '\0');
    fixture_close(&f);
}

/* Every grant routing in the MAC dispatch stamps the grant's CRC verdict on the slot's identity. */
static void
test_every_grant_kind(void) {
    struct {
        const char* tag;
        uint8_t octets[17];
        size_t count;
    } grants[] = {
        // Group Voice Channel Grant (implicit)
        {"group grant 0x40", {0x40, 0x00, 0x10, 0x0A, 0x00, 0x64, 0x00, 0x0B, 0xB9}, 9},
        // Group Voice Channel Grant - Explicit (CHAN-T, CHAN-R)
        {"group grant explicit 0xC0", {0xC0, 0x00, 0x10, 0x0A, 0x10, 0x0A, 0x00, 0x64, 0x00, 0x0B, 0xB9}, 11},
        // Group Voice Channel Grant Update - Implicit (two candidates, same channel)
        {"group update 0x42", {0x42, 0x10, 0x0A, 0x00, 0x64, 0x10, 0x0A, 0x00, 0x64}, 9},
        // Unit to Unit Voice Channel Grant (abbreviated)
        {"unit to unit grant 0x44", {0x44, 0x10, 0x0A, 0x00, 0x0F, 0xA2, 0x00, 0x0B, 0xB9}, 9},
        // MFID90 Group Regroup Channel Grant - Implicit
        {"regroup grant A3/90", {0xA3, 0x90, 0x0B, 0x00, 0x10, 0x0A, 0x00, 0x64, 0x00, 0x0B, 0xB9}, 11},
        // Group Voice Channel Grant Update Multiple - Explicit (two entries, same channel)
        {"group update multiple explicit 0x25",
         {0x25, 0x00, 0x10, 0x0A, 0x10, 0x0A, 0x00, 0x64, 0x00, 0x10, 0x0A, 0x10, 0x0A, 0x00, 0x64},
         15},
        // Group Voice Channel Grant Update Multiple - Implicit (three entries, same channel)
        {"group update multiple 0x05",
         {0x05, 0x00, 0x10, 0x0A, 0x00, 0x64, 0x00, 0x10, 0x0A, 0x00, 0x64, 0x00, 0x10, 0x0A, 0x00, 0x64},
         16},
        // SNDCP Data Channel Grant - Explicit (DSO, CHAN-T, CHAN-R, target)
        {"data grant 0x54", {0x54, 0x00, 0x10, 0x0A, 0x10, 0x0A, 0x00, 0x0F, 0xA2}, 9},
        // Group Voice Channel Grant Update - Explicit (SVC, reserved, CHAN-T, CHAN-R, group)
        {"group update explicit 0x43", {0x43, 0x00, 0x00, 0x10, 0x0A, 0x10, 0x0A, 0x00, 0x64}, 9},
        // Group Voice Channel Grant Update - Explicit (SVC, CHAN-T, CHAN-R, group)
        {"group update explicit 0xC3", {0xC3, 0x00, 0x10, 0x0A, 0x10, 0x0A, 0x00, 0x64}, 8},
        // Unit to Unit Voice Channel Grant Update (abbreviated)
        {"unit to unit update 0x46", {0x46, 0x10, 0x0A, 0x00, 0x0F, 0xA2, 0x00, 0x0B, 0xB9}, 9},
        // Unit to Unit Voice Service Channel Grant - Extended (CHAN-T, CHAN-R, source WACN/SYS, source, target)
        {"unit to unit grant extended 0xC4",
         {0xC4, 0x10, 0x0A, 0x10, 0x0A, 0xBE, 0xE0, 0x01, 0xA2, 0x00, 0x0B, 0xB9, 0x00, 0x0F, 0xA2},
         15},
        // MFID90 Group Regroup Channel Grant - Explicit (SVC, CHAN-T, CHAN-R, supergroup, source)
        {"regroup grant explicit A4/90",
         {0xA4, 0x90, 0x0D, 0x00, 0x10, 0x0A, 0x10, 0x0A, 0x00, 0x64, 0x00, 0x0B, 0xB9},
         13},
        // MFID90 Group Regroup Channel Grant Update (channel, supergroup; same channel twice)
        {"regroup update A5/90", {0xA5, 0x90, 0x0B, 0x10, 0x0A, 0x00, 0x64, 0x10, 0x0A, 0x00, 0x64}, 11},
        // MFID90 Group Regroup Voice Channel Update (SVC, supergroup, channel)
        {"regroup voice channel update 83/90", {0x83, 0x90, 0x00, 0x00, 0x64, 0x10, 0x0A}, 7},
        // L3Harris Unit-to-Unit Data Channel Grant (channel, target, source)
        {"harris data grant AC/A4", {0xAC, 0xA4, 0x0C, 0x00, 0x10, 0x0A, 0x00, 0x0F, 0xA2, 0x00, 0x0B, 0xB9}, 12},
    };

    for (size_t g = 0; g < sizeof grants / sizeof grants[0]; g++) {
        for (int crc_ok = 0; crc_ok < 2; crc_ok++) {
            fixture f;
            fixture_open(&f);
            send_mac(&f, 0, grants[g].octets, grants[g].count, crc_ok);
            const p25_sm_ctx_t* sm = p25_sm_get_ctx();
            char label[160];
            DSD_SNPRINTF(label, sizeof label, "%s (crc %d): followed", grants[g].tag, crc_ok);
            expect_true(label, sm->state == P25_SM_TUNED && sm->slots[0].grant_active == 1);
            DSD_SNPRINTF(label, sizeof label, "%s (crc %d): verdict", grants[g].tag, crc_ok);
            // The extended VCH grant names its source by a home SUID, and no reception system is known here: the
            // source is no local address, so it is never checked (test_foreign_home_id()).
            const uint8_t all = grants[g].octets[0] == 0xC4
                                    ? (uint8_t)(P25_SM_IDENTITY_CHECKED_TARGET | P25_SM_IDENTITY_CHECKED_KIND)
                                    : (uint8_t)P25_SM_IDENTITY_CHECKED_ALL;
            expect_true(label, sm->slots[0].identity_checked == (crc_ok ? all : 0));
            fixture_close(&f);
        }
    }

    // A Phase 1 TSBK bridged into the MAC dispatch passed its own CRC-16 before it got there: checked.
    fixture f;
    fixture_open(&f);
    unsigned long long int tsbk[24] = {0};
    const uint8_t grant[] = {0x40, 0x00, 0x10, 0x0A, 0x00, 0x64, 0x00, 0x0B, 0xB9};
    for (size_t i = 0; i < sizeof grant; i++) {
        tsbk[i + 1U] = grant[i];
    }
    process_MAC_VPDU_bridged_p1(f.opts, f.st, tsbk);
    expect_true("bridged tsbk grant: followed", p25_sm_get_ctx()->state == P25_SM_TUNED);
    expect_true("bridged tsbk grant: verdict",
                p25_sm_get_ctx()->slots[0].identity_checked == P25_SM_IDENTITY_CHECKED_ALL);
    fixture_close(&f);
}

int
main(void) {
    test_checked_call_both_forms();
    test_checked_group_ptt_after_unchecked_grant();
    test_private_destination_inherited();
    test_sourceless_update();
    test_sourceless_continuation_keeps_source();
    test_checked_repeat_promotes();
    test_duplicate_grant_replaces_source();
    test_fdma_repeat_replaces_kind();
    test_duplicate_grant_new_talker_resets_call_state();
    test_voice_user_supersedes_grant_talker();
    test_sacch_voice_user();
    test_epoch_change();
    test_conventional();
    test_unstamped_voice_start();
    test_hangtime();
    test_every_grant_kind();
    test_phase1_call_names_no_phase2_report();
    test_reception_change();
    test_phase_change();
    test_addressless_active_after_other_talkgroup();
    test_special_address_source();
    test_retune_rebinds_identity();
    test_reception_change_reuse();
    test_phase_change_reuse();
    test_grant_carry_across_its_retune();
    test_special_address_voice_user();
    test_foreign_home_id();
    test_anonymous_phase2_report_stays_off_phase1_row();
    dsd_decode_clock_use_system();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "P25_P2_HARRIS_GPS: OK\n");
    return 0;
}
