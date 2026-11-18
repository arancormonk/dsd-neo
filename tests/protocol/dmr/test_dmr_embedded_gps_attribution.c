// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR embedded GPS attribution through the real full-LC dispatch (dmr_flco()), decoder, admission and location file
 * (issue #800). The embedded GPS LC carries no source: it takes the one a checked full LC of the same transmission named,
 * by the attribution rule (dsd_position_attribution_grant()), and only under the reception that LC was heard on. A
 * relock to another colour code without a carrier drop, or another site named on the carrier, keeps the call and its
 * transmission, but the next fix is anonymous until a checked full LC under the new reception names the talker again.
 * All IDs and coordinates are made up.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#define SOURCE 1001U
#define TARGET 2002U

static int g_fail;

static void
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        g_fail = 1;
    }
}

typedef struct {
    dsd_opts* opts;
    dsd_state* st;
    char lrrp_path[DSD_TEST_PATH_MAX];
    uint64_t feed_seq;
    uint8_t pos_err; /* each fix its own position error, so none repeats in the feed */
} fixture;

static void
fixture_open(fixture* f) {
    DSD_MEMSET(f, 0, sizeof(*f));
    dsd_decode_clock_use_test(UINT64_C(1767261600000000000));
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
    f->st->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    f->st->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    const int fd = dsd_test_mkstemp(f->lrrp_path, sizeof f->lrrp_path, "dmr_embedded_attr_lrrp");
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
    dsd_state_ext_free_all(f->st);
    free(f->st->event_history_s);
    free(f->st);
    free(f->opts);
    (void)remove(f->lrrp_path);
}

static void
put_bits(uint8_t* bits, unsigned offset, unsigned width, uint32_t value) {
    for (unsigned i = 0; i < width; i++) {
        bits[offset + i] = (uint8_t)((value >> (width - 1U - i)) & 1U);
    }
}

/* A group voice LC (FLCO 0) of SOURCE to TARGET whose own check passed: a voice LC header (@p type 1) or a late-entry
 * embedded LC (type 3), on slot 0. */
static void
voice_lc(fixture* f, uint8_t type) {
    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof bits);
    put_bits(bits, 24U, 24U, TARGET);
    put_bits(bits, 48U, 24U, SOURCE);
    uint32_t irr = 0U;
    f->st->currentslot = 0;
    dmr_flco(f->opts, f->st, bits, 1U, &irr, type);
}

/* An embedded GPS LC (FLCO 0x08) at 22.5 N, 0 E whose checksum passed, on slot 0. */
static void
embedded_gps(fixture* f) {
    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof bits);
    put_bits(bits, 2U, 6U, 0x08U);
    f->pos_err = (uint8_t)((f->pos_err % 5U) + 1U);
    put_bits(bits, 20U, 3U, f->pos_err);
    put_bits(bits, 48U, 24U, 0x200000U);
    uint32_t irr = 0U;
    f->st->currentslot = 0;
    dmr_flco(f->opts, f->st, bits, 1U, &irr, 3U);
}

static int
last_row_source(const fixture* f, char out[16]) {
    FILE* fp = dsd_fopen_private(f->lrrp_path, "r");
    out[0] = '\0';
    if (fp == NULL) {
        return 0;
    }
    char line[256];
    char last[256] = "";
    int rows = 0;
    while (fgets(line, sizeof line, fp) != NULL) {
        rows++;
        DSD_SNPRINTF(last, sizeof last, "%s", line);
    }
    fclose(fp);
    const char* tab = strchr(last, '\t');
    tab = tab ? strchr(tab + 1, '\t') : NULL;
    if (tab != NULL) {
        DSD_SNPRINTF(out, 16, "%.8s", tab + 1);
    }
    return rows;
}

/* The fix the last embedded GPS LC gave: kept, attributed to SOURCE and TARGET or anonymous. */
static void
expect_fix(fixture* f, const char* tag, int named) {
    char label[160];
    dsd_position_fix fix;
    dsd_position_feed_meta meta;
    const int n = dsd_position_feed_copy_since(f->st, f->feed_seq, &fix, 1, &meta);
    DSD_SNPRINTF(label, sizeof label, "%s: feed record", tag);
    expect_true(label, n == 1 && fix.kind == DSD_POS_DMR_EMBEDDED && fix.lat_e7 == 225000000);
    if (n == 1) {
        f->feed_seq = meta.last_seq;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: identity", tag);
    if (named) {
        expect_true(label, n == 1 && fix.source_id == SOURCE && fix.target_id == TARGET
                               && (fix.flags & DSD_POS_FLAG_TARGET_GROUP) != 0U
                               && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) == 0U);
    } else {
        expect_true(label, n == 1 && fix.source_id == 0U && fix.target_id == 0U
                               && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U);
    }
    char src[16];
    DSD_SNPRINTF(label, sizeof label, "%s: location row", tag);
    expect_true(label, last_row_source(f, src) > 0 && strcmp(src, named ? "00001001" : "00000000") == 0);
}

static int
call_open(const fixture* f) {
    dsd_call_snapshot call;
    return dsd_call_state_get(f->st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
           && call.ota_source_id == SOURCE && call.ota_target_id == TARGET;
}

/* One reception change: 0 a colour code relock, 1 another Capacity Plus site named on the carrier. */
static void
change_reception(fixture* f, int change) {
    if (change == 0) {
        dsd_rxid_note_access(f->st, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 5U);
    } else {
        dsd_rxid_note_dmr_site(f->st, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 4U, 0U, NULL);
    }
}

static void
test_reception_change(void) {
    for (int change = 0; change < 2; change++) {
        const char* what = change ? "site" : "colour code";
        char tag[96];
        fixture f;
        fixture_open(&f);
        dsd_rxid_note_access(f.st, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 1U);
        dsd_rxid_note_dmr_site(f.st, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 3U, 0U, NULL);
        voice_lc(&f, 1U);
        expect_true("call open", call_open(&f));
        embedded_gps(&f);
        DSD_SNPRINTF(tag, sizeof tag, "%s: before the change", what);
        expect_fix(&f, tag, 1);

        const uint32_t before = f.st->rx_id.epoch;
        change_reception(&f, change);
        DSD_SNPRINTF(tag, sizeof tag, "%s: reception changed, call kept", what);
        expect_true(tag, f.st->rx_id.epoch != before && call_open(&f));
        embedded_gps(&f);
        DSD_SNPRINTF(tag, sizeof tag, "%s: after the change", what);
        expect_fix(&f, tag, 0);

        // A late-entry LC whose own check passed names the talker again under the new reception.
        voice_lc(&f, 3U);
        embedded_gps(&f);
        DSD_SNPRINTF(tag, sizeof tag, "%s: renamed", what);
        expect_fix(&f, tag, 1);
        fixture_close(&f);
    }
}

/* An unchanged reception (the same colour code noted again, a site repeated) keeps the attribution. */
static void
test_same_reception(void) {
    fixture f;
    fixture_open(&f);
    dsd_rxid_note_access(f.st, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 1U);
    voice_lc(&f, 1U);
    dsd_rxid_note_access(f.st, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 1U);
    embedded_gps(&f);
    expect_fix(&f, "same colour code", 1);
    fixture_close(&f);
}

/* Issue #800, fail closed: an embedded GPS LC whose burst failed its check prints only that its position was
 * suppressed, not the rest of the report (its position error), which only -Z prints. */
static void
test_failed_report_prints_no_content(void) {
    for (int payload = 0; payload <= 1; payload++) {
        fixture f;
        fixture_open(&f);
        f.opts->payload = payload;
        voice_lc(&f, 1U);
        f.st->event_crc_invalid[0] = 1U;
        dsd_test_capture_stderr cap;
        char out[2048];
        out[0] = '\0';
        if (dsd_test_capture_stderr_begin(&cap, "dmr_embedded_failed") != 0) {
            expect_true("capture", 0);
            fixture_close(&f);
            return;
        }
        embedded_gps(&f);
        dsd_test_capture_stderr_end(&cap);
        expect_true("capture read", dsd_test_capture_stderr_read(&cap, out, sizeof out) == 0);
        f.st->event_crc_invalid[0] = 0U;
        expect_true("failed embedded GPS: suppressed", strstr(out, "Position: (suppressed; CRC ERR)") != NULL);
        expect_true(payload ? "failed embedded GPS: -Z shows its error" : "failed embedded GPS: no position error",
                    (strstr(out, "Position Error") != NULL) == (payload == 1));
        dsd_position_fix fix;
        dsd_position_feed_meta meta;
        expect_true("failed embedded GPS: not kept", dsd_position_feed_copy_since(f.st, 0U, &fix, 1, &meta) == 0);
        fixture_close(&f);
    }
}

/* The single-slot decoder carries either over-the-air slot on its one lane (lane 0): its report publishes the slot the
 * CACH proved, not the lane, so the same fix heard on TS2 then on TS1 is two records (the repeat rule compares where it
 * was heard). One heard while no slot is proven names none: slot 0 marked DSD_POS_FLAG_SLOT_UNKNOWN, never a repeat of
 * another, not even of the same fix heard again unproven (issue #800, fail closed). */
static void
test_single_slot_decoder_publishes_proven_slot(void) {
    static const struct {
        uint8_t mono;
        uint8_t slot;
        uint16_t unknown;
        const char* tag;
    } steps[] = {
        {DSD_DMR_MONO_RX_TS2, 1U, 0U, "single-slot TS2"},
        {DSD_DMR_MONO_RX_TS1, 0U, 0U, "single-slot TS1, same fix"},
        {DSD_DMR_MONO_RX_PENDING, 0U, DSD_POS_FLAG_SLOT_UNKNOWN, "single-slot, slot not proven"},
        {DSD_DMR_MONO_RX_PENDING, 0U, DSD_POS_FLAG_SLOT_UNKNOWN, "single-slot, slot not proven, same fix"},
        {DSD_DMR_MONO_RX_TS1, 0U, 0U, "single-slot TS1 again, same fix"},
    };

    fixture f;
    fixture_open(&f);
    f.st->dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS2;
    voice_lc(&f, 1U);
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        f.st->dmr_mono_rx_slot = steps[i].mono;
        f.pos_err = 0U; /* the same fix every time */
        embedded_gps(&f);
        dsd_position_fix fix;
        dsd_position_feed_meta meta;
        const int n = dsd_position_feed_copy_since(f.st, f.feed_seq, &fix, 1, &meta);
        char label[128];
        DSD_SNPRINTF(label, sizeof label, "%s: kept %d slot %u flags %04X", steps[i].tag, n, n == 1 ? fix.slot : 0U,
                     n == 1 ? fix.flags : 0U);
        expect_true(label,
                    n == 1 && fix.slot == steps[i].slot && (fix.flags & DSD_POS_FLAG_SLOT_UNKNOWN) == steps[i].unknown);
        if (n == 1) {
            f.feed_seq = meta.last_seq;
        }
    }
    fixture_close(&f);
}

int
main(void) {
    test_reception_change();
    test_same_reception();
    test_failed_report_prints_no_content();
    test_single_slot_decoder_publishes_proven_slot();
    dsd_decode_clock_use_system();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "DMR_EMBEDDED_GPS_ATTRIBUTION: OK\n");
    return 0;
}
