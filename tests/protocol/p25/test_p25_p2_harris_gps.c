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

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
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
                             (uint8_t)VC_CHANNEL,
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
                             (uint8_t)VC_CHANNEL,
                             (uint8_t)(UNIT_B >> 16),
                             (uint8_t)(UNIT_B >> 8),
                             (uint8_t)UNIT_B,
                             (uint8_t)(src >> 16),
                             (uint8_t)(src >> 8),
                             (uint8_t)src};
    send_mac(f, 0, grant, sizeof grant, crc_ok);
}

/* MAC_PTT as the XCCH decoder emits it (CRC-checked): the group address field and the source; always "group". */
static void
mac_ptt(fixture* f, uint32_t tg, uint32_t src) {
    (void)p25_sm_emit_ptt_call(f->opts, f->st, 0, (int)tg, 0, (int)src, 1, 0x00);
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
        (void)p25_sm_emit_active_call(f.opts, f.st, 0, (int)TG_A, 0, 0, 1, 0x00);
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

    const uint8_t voice_user[] = {
        0x01, 0x00, (uint8_t)(TG_A >> 8), (uint8_t)TG_A, (uint8_t)(SRC_B >> 16), (uint8_t)(SRC_B >> 8), (uint8_t)SRC_B};
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
    const uint8_t same_user[] = {
        0x01, 0x00, (uint8_t)(TG_A >> 8), (uint8_t)TG_A, (uint8_t)(SRC_B >> 16), (uint8_t)(SRC_B >> 8), (uint8_t)SRC_B};
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

    const uint8_t voice_user[] = {
        0x01, 0x00, (uint8_t)(TG_A >> 8), (uint8_t)TG_A, (uint8_t)(SRC_B >> 16), (uint8_t)(SRC_B >> 8), (uint8_t)SRC_B};
    send_mac(&f, 0, voice_user, sizeof voice_user, 0);
    expect_true("conventional -F voice user", slot0_call_names(TG_A, SRC_B, f.st));
    send_gps(&f, 0);
    expect_report(&f, "conventional after a -F voice user", 0U, 0U, 0);
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
            expect_true(label, sm->slots[0].identity_checked == (crc_ok ? P25_SM_IDENTITY_CHECKED_ALL : 0));
            fixture_close(&f);
        }
    }
}

int
main(void) {
    test_checked_call_both_forms();
    test_checked_group_ptt_after_unchecked_grant();
    test_private_destination_inherited();
    test_sourceless_update();
    test_epoch_change();
    test_conventional();
    test_every_grant_kind();
    dsd_decode_clock_use_system();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "P25_P2_HARRIS_GPS: OK\n");
    return 0;
}
