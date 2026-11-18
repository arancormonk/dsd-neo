// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #800 2.2, N1, D13: an M17 position comes only from the GNSS META field of a link setup whose CRC passed -- an
 * RF LSF, a stream's LSD assembled from its LICH, an IP stream frame or an MPKT frame, each with its own CRC -- and
 * never from what this station transmits, from an LSF -F decoded despite a failed CRC, or from a packet: M17 defines no
 * GNSS packet protocol, so a 0x91 packet is an unknown protocol. Driven through the real decoder entry points, with the
 * real call state, history rows and position feed. Coordinates are synthetic.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/m17/m17_parse.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "m17_algorithms.h"
#include "m17_internal.h"
#include "test_support.h"

#ifdef USE_CODEC2
#include <codec2/codec2.h>
#endif

struct CODEC2;

enum { LSF_BITS = M17_LSF_BYTES * 8 };

/* Raw 0x200000 is 22.5 degrees of latitude and 45 degrees of longitude, rounded to the nearest 1e-7 degree. */
#define RAW_QUARTER       0x200000
#define LAT_QUARTER_E7    225000027
#define LON_QUARTER_E7    450000054
#define GNSS_ALL_VALID    0xFU
#define GNSS_LATLON_ONLY  0x8U
#define TEST_SRC_CALLSIGN "N0CALL"
#define TEST_DST_CALLSIGN "CQCQCQ"
#define TEST_ALTITUDE_RAW 3000U  /* 1000 m */
#define TEST_SPEED_RAW    0x123U /* 145.5 km/h */
#define TEST_BEARING_DEG  300U

static int g_failures;

static void
expect_int(const char* label, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", label, got, want);
        g_failures++;
    }
}

static void
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: failed\n", label);
        g_failures++;
    }
}

#ifdef USE_CODEC2
/* The vocoder this test never reaches: no voice payload is decoded or encoded. */
void
codec2_decode(struct CODEC2* codec2_state, short speech[], const unsigned char* bits) {
    (void)codec2_state;
    (void)bits;
    if (speech != NULL) {
        DSD_MEMSET(speech, 0, 160U * sizeof(short));
    }
}

void
codec2_encode(struct CODEC2* codec2_state, unsigned char* bits, short speech[]) {
    (void)codec2_state;
    (void)bits;
    (void)speech;
}

int
codec2_samples_per_frame(struct CODEC2* codec2_state) {
    (void)codec2_state;
    return 160;
}
#endif

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    Event_History_I* history;
} rig;

static void
rig_open(rig* r) {
    r->opts = (dsd_opts*)calloc(1, sizeof(*r->opts));
    r->state = (dsd_state*)calloc(1, sizeof(*r->state));
    r->history = (Event_History_I*)calloc(2U, sizeof(*r->history));
    if (!r->opts || !r->state || !r->history) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        exit(1);
    }
    for (uint8_t slot = 0U; slot < 2U; slot++) {
        init_event_history(&r->history[slot], 0, 255);
    }
    r->state->event_history_s = r->history;
    r->state->m17_can_en = -1;
    r->opts->aggressive_framesync = 1;
    expect_int("feed starts", dsd_position_feed_start(r->state), 0);
}

static void
rig_close(rig* r) {
    dsd_state_ext_free_all(r->state);
    free(r->history);
    free(r->state);
    free(r->opts);
}

static int
feed_records(const rig* r, dsd_position_fix* out, size_t cap) {
    dsd_position_feed_meta meta;
    DSD_MEMSET(out, 0, cap * sizeof(*out));
    return dsd_position_feed_copy_since(r->state, 0U, out, cap, &meta);
}

static const Event_History*
row0(const rig* r) {
    return &r->history[0].Event_History_Items[0];
}

static void
write_bits(uint8_t* bits, unsigned long long value, size_t n) {
    for (size_t i = 0U; i < n; i++) {
        bits[i] = (uint8_t)((value >> (n - 1U - i)) & 1U);
    }
}

static void
pack_bits(const uint8_t* bits, size_t bit_count, uint8_t* bytes) {
    for (size_t i = 0U; i < bit_count / 8U; i++) {
        bytes[i] = 0U;
        for (size_t b = 0U; b < 8U; b++) {
            bytes[i] = (uint8_t)((bytes[i] << 1U) | (bits[(i * 8U) + b] & 1U));
        }
    }
}

/* The 14 GNSS META bytes (M17 spec v2.0.11): source OpenRTX, handheld. */
static void
gnss_meta(uint8_t meta[M17_META_BYTES], uint8_t validity, uint8_t radius_exp, uint16_t bearing, int32_t lat_raw,
          int32_t lon_raw) {
    const uint32_t lat = (uint32_t)lat_raw & 0xFFFFFFU;
    const uint32_t lon = (uint32_t)lon_raw & 0xFFFFFFU;
    DSD_MEMSET(meta, 0, M17_META_BYTES);
    meta[0] = 0x12U;
    meta[1] = (uint8_t)((validity << 4U) | ((radius_exp & 0x7U) << 1U) | ((bearing >> 8U) & 0x1U));
    meta[2] = (uint8_t)(bearing & 0xFFU);
    meta[3] = (uint8_t)(lat >> 16U);
    meta[4] = (uint8_t)(lat >> 8U);
    meta[5] = (uint8_t)lat;
    meta[6] = (uint8_t)(lon >> 16U);
    meta[7] = (uint8_t)(lon >> 8U);
    meta[8] = (uint8_t)lon;
    meta[9] = (uint8_t)(TEST_ALTITUDE_RAW >> 8U);
    meta[10] = (uint8_t)(TEST_ALTITUDE_RAW & 0xFFU);
    meta[11] = (uint8_t)(TEST_SPEED_RAW >> 4U);
    meta[12] = (uint8_t)((TEST_SPEED_RAW & 0xFU) << 4U);
}

/* An LSF (240 bits with its CRC) of a clear voice stream (or a packet, @p packet) with META subtype @p subtype. */
static uint16_t
build_lsf(uint8_t bits[LSF_BITS], uint8_t packed[M17_LSF_BYTES], int packet, uint8_t subtype,
          const uint8_t meta[M17_META_BYTES]) {
    DSD_MEMSET(bits, 0, LSF_BITS);
    m17_load_lsf_callsigns(bits, m17_encode_b40_callsign(0ULL, TEST_DST_CALLSIGN),
                           m17_encode_b40_callsign(0ULL, TEST_SRC_CALLSIGN));
    const uint16_t type = packet ? m17_compose_frame_info(0U, 0U, 0U, subtype, 5U, 0U, 0U)
                                 : m17_compose_frame_info(1U, 2U, 0U, subtype, 5U, 0U, 0U);
    write_bits(bits + 96, type, 16U);
    for (size_t i = 0U; i < M17_META_BYTES; i++) {
        write_bits(bits + 112U + (i * 8U), meta[i], 8U);
    }
    return m17_attach_lsf_crc(bits, packed);
}

/* An RF LSF frame, its CRC passing or (@p corrupt) failing. */
static int
rf_lsf(rig* r, int packet, uint8_t subtype, const uint8_t meta[M17_META_BYTES], int corrupt) {
    uint8_t bits[LSF_BITS];
    uint8_t packed[M17_LSF_BYTES];
    const uint16_t crc = build_lsf(bits, packed, packet, subtype, meta);
    DSD_MEMCPY(r->state->m17_lsf, bits, sizeof(bits));
    return m17_finalize_lsf_crc(r->opts, r->state, packed, corrupt ? (uint16_t)(crc ^ 1U) : crc);
}

static void
check_quarter_fix(const char* label, const dsd_position_fix* f) {
    char l[160];
    DSD_SNPRINTF(l, sizeof l, "%s kind", label);
    expect_int(l, f->kind, DSD_POS_M17_GNSS);
    DSD_SNPRINTF(l, sizeof l, "%s protocol is M17", label);
    expect_true(l, DSD_SYNC_IS_M17(f->protocol));
    DSD_SNPRINTF(l, sizeof l, "%s lat", label);
    expect_int(l, f->lat_e7, LAT_QUARTER_E7);
    DSD_SNPRINTF(l, sizeof l, "%s lon", label);
    expect_int(l, f->lon_e7, LON_QUARTER_E7);
    DSD_SNPRINTF(l, sizeof l, "%s source callsign", label);
    expect_true(l, strcmp(f->source_text, TEST_SRC_CALLSIGN) == 0);
    DSD_SNPRINTF(l, sizeof l, "%s source checked", label);
    expect_int(l, f->flags & DSD_POS_FLAG_SOURCE_UNVERIFIED, 0);
}

/* A checked RF LSF: one position, every field from the META, attributed to the LSF's own source; the call's row
 * takes it. Repeated, the feed keeps it once. */
static void
test_rf_lsf_meta_position(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    expect_int("checked LSF passes", rf_lsf(&r, 0, 1U, meta, 0), 0);

    dsd_position_fix f[4];
    expect_int("checked LSF admits one position", feed_records(&r, f, 4U), 1);
    check_quarter_fix("RF LSF", &f[0]);
    expect_int("RF LSF altitude", f[0].alt_dm, 10000);
    expect_int("RF LSF speed", f[0].speed_dkmh, 1455);
    expect_int("RF LSF heading", f[0].heading_ddeg, 3000);
    expect_int("RF LSF radius code 3", f[0].err_m, 8);
    expect_int("RF LSF radius exact", f[0].flags & DSD_POS_FLAG_ERR_LOWER_BOUND, 0);

    const Event_History* row = row0(&r);
    expect_int("call row takes the position", row->pos.valid, 1);
    expect_int("call row position lat", row->pos.lat_e7, LAT_QUARTER_E7);
    expect_true("call row GPS text", strncmp(row->gps_s, "GPS: (22.500003, 45.000005)", 27) == 0);

    rig_close(&r);
}

static void build_ip_stream_frame_fn(uint8_t ip_frame[54], const uint8_t lsf_bits[LSF_BITS], uint16_t sid, uint16_t fn,
                                     int corrupt);

/* The size of the -L file @p path. */
static long
file_size(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    (void)fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    (void)fclose(fp);
    return size;
}

/* A transmission repeats its LSF META (RF: the LSF, then every LICH superframe; IP: every stream frame): one report,
 * admitted once (one revision, one feed record, one row write). Changed META bytes in the same transmission are a
 * new report; the same META in the next transmission is too. M17 writes no -L rows (no -L producer existed for it;
 * issue #800 report, decision 11), with -L on or not. */
static void
test_rf_repeated_meta_is_one_report(void) {
    rig r;
    rig_open(&r);
    char lrrp_path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(lrrp_path, sizeof(lrrp_path), "m17_gnss_lrrp");
    expect_true("-L file created", fd >= 0);
    if (fd >= 0) {
        (void)dsd_close(fd);
    }
    r.opts->lrrp_file_output = 1;
    DSD_SNPRINTF(r.opts->lrrp_out_file, sizeof(r.opts->lrrp_out_file), "%s", lrrp_path);

    uint8_t meta[M17_META_BYTES];
    dsd_position_fix f[4];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    uint64_t rev = dsd_position_last_rev();
    for (int i = 0; i < 4; i++) {
        expect_int("repeated LSF passes", rf_lsf(&r, 0, 1U, meta, 0), 0);
    }
    expect_int("repeated META admitted once", (long long)(dsd_position_last_rev() - rev), 1);
    expect_int("repeated META kept once", feed_records(&r, f, 4U), 1);
    const uint64_t row_rev = r.history[0].Event_History_Items[0].pos.rev;
    expect_int("row written once", (long long)row_rev, (long long)f[0].rev);

    /* Changed META mid-transmission: a second report. */
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, -RAW_QUARTER);
    rev = dsd_position_last_rev();
    expect_int("changed LSF passes", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("changed LSF passes again", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("changed META admitted once more", (long long)(dsd_position_last_rev() - rev), 1);
    expect_int("changed META kept", feed_records(&r, f, 4U), 2);
    expect_int("changed META longitude", f[1].lon_e7, -LON_QUARTER_E7);

    /* The transmission ends; the next one sends the same META: a new report. */
    dsd_call_snapshot call;
    expect_int("first transmission's call", dsd_call_state_get(r.state, 0U, &call), 1);
    const uint64_t first_epoch = call.epoch;
    expect_true("transmission ends", dsd_call_state_end_ex(r.state, 0U, 0.0, DSD_CALL_END_TERMINATOR) > 0);
    rev = dsd_position_last_rev();
    expect_int("next transmission's LSF passes", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("next transmission's call", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_true("next transmission is a new epoch", call.epoch != first_epoch);
    expect_int("same META, next transmission: admitted", (long long)(dsd_position_last_rev() - rev), 1);

    expect_int("no -L rows for M17", file_size(lrrp_path), 0);
    (void)remove(lrrp_path);
    rig_close(&r);
}

/* The IP stream carries the LSF in every frame: one report per stream ID and META content. */
static void
test_ip_repeated_meta_is_one_report(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    uint8_t bits[LSF_BITS];
    uint8_t packed[M17_LSF_BYTES];
    uint8_t frame[54];
    const uint16_t data_stream = m17_compose_frame_info(1U, 1U, 0U, 1U, 5U, 0U, 0U);

    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    (void)build_lsf(bits, packed, 0, 1U, meta);
    write_bits(bits + 96, data_stream, 16U);
    uint64_t rev = dsd_position_last_rev();
    for (uint16_t fn = 1U; fn <= 5U; fn++) {
        build_ip_stream_frame_fn(frame, bits, 0xBEEFU, fn, 0);
        m17_ip_dispatch_frame(r.opts, r.state, frame, (int)sizeof(frame));
    }
    expect_int("IP repeated META admitted once", (long long)(dsd_position_last_rev() - rev), 1);

    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, -RAW_QUARTER, RAW_QUARTER);
    (void)build_lsf(bits, packed, 0, 1U, meta);
    write_bits(bits + 96, data_stream, 16U);
    rev = dsd_position_last_rev();
    for (uint16_t fn = 6U; fn <= 8U; fn++) {
        build_ip_stream_frame_fn(frame, bits, 0xBEEFU, fn, 0);
        m17_ip_dispatch_frame(r.opts, r.state, frame, (int)sizeof(frame));
    }
    expect_int("IP changed META admitted once more", (long long)(dsd_position_last_rev() - rev), 1);

    rev = dsd_position_last_rev();
    for (uint16_t fn = 1U; fn <= 3U; fn++) {
        build_ip_stream_frame_fn(frame, bits, 0xCAFEU, fn, 0);
        m17_ip_dispatch_frame(r.opts, r.state, frame, (int)sizeof(frame));
    }
    expect_int("IP same META, new stream: admitted", (long long)(dsd_position_last_rev() - rev), 1);
    dsd_position_fix f[4];
    /* The feed folds that last report: the same track's last kept fix, equal in every field, seconds ago. */
    expect_int("IP stream reports kept", feed_records(&r, f, 4U), 2);
    rig_close(&r);
}

/* Issue #800: however a network transmission ends -- EOTX, DISC, or a call end with no control frame (a timeout, a
 * lost stream) -- the next stream is another transmission, even with the same stream ID and META: admitted again, and
 * its call's row takes the position. */
static void
test_ip_stream_end_admits_next_stream(void) {
    static const uint8_t eotx[10] = {'E', 'O', 'T', 'X', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    static const uint8_t disc[10] = {'D', 'I', 'S', 'C', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    static const char* const ends[] = {"EOTX", "DISC", "call end"};
    for (int end = 0; end < 3; end++) {
        rig r;
        rig_open(&r);
        uint8_t meta[M17_META_BYTES];
        uint8_t bits[LSF_BITS];
        uint8_t packed[M17_LSF_BYTES];
        uint8_t frame[54];
        gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
        (void)build_lsf(bits, packed, 0, 1U, meta);
        write_bits(bits + 96, m17_compose_frame_info(1U, 1U, 0U, 1U, 5U, 0U, 0U), 16U);
        for (int stream = 0; stream < 2; stream++) {
            const uint64_t rev = dsd_position_last_rev();
            for (uint16_t fn = 1U; fn <= 3U; fn++) {
                build_ip_stream_frame_fn(frame, bits, 0xBEEFU, fn, 0);
                m17_ip_dispatch_frame(r.opts, r.state, frame, (int)sizeof(frame));
            }
            char label[96];
            DSD_SNPRINTF(label, sizeof label, "%s: stream %d admitted once", ends[end], stream + 1);
            expect_int(label, (long long)(dsd_position_last_rev() - rev), 1);
            DSD_SNPRINTF(label, sizeof label, "%s: stream %d's row takes it", ends[end], stream + 1);
            expect_true(label, row0(&r)->pos.valid == 1U && row0(&r)->pos.rev == dsd_position_last_rev());
            if (end == 0) {
                m17_ip_dispatch_frame(r.opts, r.state, eotx, (int)sizeof(eotx));
            } else if (end == 1) {
                m17_ip_dispatch_frame(r.opts, r.state, disc, (int)sizeof(disc));
            } else {
                expect_true("the call ends", dsd_call_state_end(r.state, 0U, 0.0) > 0);
            }
        }
        rig_close(&r);
    }
}

/* A checked RF LSF on Channel Access Number @p can (build_lsf() sends 5). */
static void
rf_lsf_can(rig* r, uint16_t can, const uint8_t meta[M17_META_BYTES]) {
    uint8_t bits[LSF_BITS];
    uint8_t packed[M17_LSF_BYTES];
    (void)build_lsf(bits, packed, 0, 1U, meta);
    write_bits(bits + 96, m17_compose_frame_info(1U, 2U, 0U, 1U, can, 0U, 0U), 16U);
    const uint16_t crc = m17_attach_lsf_crc(bits, packed);
    DSD_MEMCPY(r->state->m17_lsf, bits, sizeof(bits));
    expect_int("CAN LSF passes", m17_finalize_lsf_crc(r->opts, r->state, packed, crc), 0);
}

/* Issue #800: the checked LSF's CAN is the reception its call is heard on. The LSF of a transmission on another CAN
 * names another reception (the received identity's epoch moves), and the call it opens is on that reception from the
 * start, so the META position of the same LSF lands on that call's row. */
static void
test_can_change_position_on_call_row(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    rf_lsf_can(&r, 5U, meta);
    expect_int("CAN 5: the call row takes the position", row0(&r)->pos.valid, 1);
    expect_true("CAN 5: the transmission ends", dsd_call_state_end_ex(r.state, 0U, 0.0, DSD_CALL_END_TERMINATOR) > 0);

    /* The next transmission is on CAN 6. */
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, -RAW_QUARTER, RAW_QUARTER);
    const uint64_t rev = dsd_position_last_rev();
    rf_lsf_can(&r, 6U, meta);
    expect_int("CAN 6: the position is admitted", (long long)(dsd_position_last_rev() - rev), 1);
    dsd_call_snapshot call;
    expect_int("CAN 6: a call", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_int("CAN 6: the call is on the new reception", call.rx_id_epoch, r.state->rx_id.epoch);
    const Event_History* row = row0(&r);
    expect_int("CAN 6: the call row takes the position", row->pos.valid, 1);
    expect_int("CAN 6: the call row's latitude", row->pos.lat_e7, -LAT_QUARTER_E7);
    expect_int("CAN 6: the call row's revision", (long long)row->pos.rev, (long long)dsd_position_last_rev());
    rig_close(&r);
}

/* Issue #800: the next engine run on a reused state (its cleanup frees every extension; the new call state goes on from
 * the last run's epochs) does not take the last run's admitted META for a repeat: the same META on the new run's first
 * call is a new report. */
static void
test_reused_state_admits_meta_again(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    uint64_t rev = dsd_position_last_rev();
    expect_int("first run's LSF passes", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("first run admits the META", (long long)(dsd_position_last_rev() - rev), 1);
    dsd_call_snapshot call;
    expect_true("first run's call", dsd_call_state_get(r.state, 0U, &call) == 1);
    const uint64_t first_epoch = call.epoch;

    dsd_state_ext_free_all(r.state);
    for (uint8_t slot = 0U; slot < 2U; slot++) {
        init_event_history(&r.history[slot], 0, 255);
    }
    expect_int("next run's feed starts", dsd_position_feed_start(r.state), 0);
    rev = dsd_position_last_rev();
    expect_int("next run's LSF passes", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_true("next run's first call is a new epoch",
                dsd_call_state_get(r.state, 0U, &call) == 1 && call.epoch > first_epoch);
    expect_int("next run admits the same META", (long long)(dsd_position_last_rev() - rev), 1);
    expect_int("next run's call row takes it", row0(&r)->pos.valid, 1);
    rig_close(&r);
}

/* Under -F an LSF whose CRC failed still sets the decoder up, but its META is no position, no text and no callsign. */
static void
test_relaxed_bad_lsf_gives_nothing(void) {
    rig r;
    rig_open(&r);
    r.opts->aggressive_framesync = 0;
    uint8_t meta[M17_META_BYTES];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    const uint64_t rev = dsd_position_last_rev();
    expect_int("relaxed bad LSF reports its CRC error", rf_lsf(&r, 0, 1U, meta, 1), 1);
    expect_int("relaxed bad LSF still decodes its CAN", r.state->m17_can, 5);
    dsd_position_fix f[2];
    expect_int("relaxed bad LSF admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
    expect_int("relaxed bad LSF keeps nothing", feed_records(&r, f, 2U), 0);
    expect_int("relaxed bad LSF writes no row position", row0(&r)->pos.valid, 0);
    rig_close(&r);
}

/* What only a valid, in-range field may carry: the lat/lon validity bit, -2^23 (never used), a bearing above 359
 * (unknown heading, the speed stays) and radius code 7 ("more than 64 m"). */
static void
test_meta_field_ranges(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    dsd_position_fix f[4];
    const uint64_t rev = dsd_position_last_rev();

    gnss_meta(meta, (uint8_t)(GNSS_ALL_VALID & ~GNSS_LATLON_ONLY), 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    expect_int("no lat/lon validity passes CRC", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("no lat/lon validity, no position", (long long)(dsd_position_last_rev() - rev), 0);

    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, -8388608, RAW_QUARTER);
    expect_int("-2^23 latitude passes CRC", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("-2^23 latitude, no position", (long long)(dsd_position_last_rev() - rev), 0);
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, -8388608);
    expect_int("-2^23 longitude passes CRC", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("-2^23 longitude, no position", (long long)(dsd_position_last_rev() - rev), 0);
    expect_int("nothing kept", feed_records(&r, f, 4U), 0);

    gnss_meta(meta, GNSS_ALL_VALID, 7U, 360U, RAW_QUARTER, -RAW_QUARTER);
    expect_int("bearing 360 passes CRC", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("bearing 360 keeps the position", feed_records(&r, f, 4U), 1);
    expect_int("bearing 360 longitude west", f[0].lon_e7, -LON_QUARTER_E7);
    expect_int("bearing 360 heading unknown", f[0].heading_ddeg, DSD_POSITION_HEADING_UNKNOWN);
    expect_int("bearing 360 speed kept", f[0].speed_dkmh, 1455);
    expect_int("radius code 7 is 64 m", f[0].err_m, 64);
    expect_int("radius code 7 is a lower bound", f[0].flags & DSD_POS_FLAG_ERR_LOWER_BOUND,
               DSD_POS_FLAG_ERR_LOWER_BOUND);

    gnss_meta(meta, GNSS_LATLON_ONLY, 0U, 0U, RAW_QUARTER, RAW_QUARTER);
    expect_int("lat/lon only passes CRC", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("lat/lon only kept", feed_records(&r, f, 4U), 2);
    expect_int("lat/lon only altitude unknown", f[1].alt_dm, DSD_POSITION_ALT_UNKNOWN);
    expect_int("lat/lon only speed unknown", f[1].speed_dkmh, DSD_POSITION_SPEED_UNKNOWN);
    expect_int("lat/lon only heading unknown", f[1].heading_ddeg, DSD_POSITION_HEADING_UNKNOWN);
    expect_int("lat/lon only error unknown", f[1].err_m, DSD_POSITION_ERR_UNKNOWN);
    rig_close(&r);
}

/* A stream's LSD assembled from its six LICH chunks: its CRC passing gives the position, failing (-F) gives none. */
static void
test_lich_lsd_position(void) {
    for (int corrupt = 0; corrupt <= 1; corrupt++) {
        rig r;
        rig_open(&r);
        r.opts->aggressive_framesync = 0;
        uint8_t meta[M17_META_BYTES];
        uint8_t bits[LSF_BITS];
        uint8_t packed[M17_LSF_BYTES];
        gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
        (void)build_lsf(bits, packed, 0, 1U, meta);
        if (corrupt) {
            bits[M17_LSF_LSD_BITS + 3U] ^= 1U;
        }
        for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
            uint8_t content[M17_LICH_CONTENT_BITS];
            uint8_t encoded[M17_LICH_BITS];
            DSD_MEMSET(content, 0, sizeof(content));
            (void)m17_lich_build_content(bits, chunk, content);
            m17_lich_encode_bits(content, encoded);
            expect_int("LICH chunk accepted", m17_process_lich(r.state, r.opts, encoded), 0);
        }
        dsd_position_fix f[2];
        const int n = feed_records(&r, f, 2U);
        expect_int(corrupt ? "bad LSD gives no position" : "checked LSD gives the position", n, corrupt ? 0 : 1);
        if (!corrupt && n == 1) {
            check_quarter_fix("LSD", &f[0]);
        }
        rig_close(&r);
    }
}

/* What this station transmits, decoded back to monitor it: its call is shown, but its META is no position. */
static void
test_local_tx_monitor_gives_nothing(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    uint8_t bits[LSF_BITS];
    uint8_t packed[M17_LSF_BYTES];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    (void)build_lsf(bits, packed, 0, 1U, meta);
    uint8_t flush[M17_LSF_TYPE1_FLUSH_BITS];
    uint8_t randomized[M17_PAYLOAD_BITS];
    DSD_MEMSET(flush, 0, sizeof(flush));
    DSD_MEMCPY(flush, bits, M17_LSF_TYPE1_BITS);
    (void)m17_lsf_encode_type1_bits(flush, randomized, NULL);

    const uint64_t rev = dsd_position_last_rev();
    m17_monitor_encoded_lsf(r.opts, r.state, randomized);
    dsd_call_snapshot call;
    expect_int("monitored LSF shows its call", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_int("monitored LSF admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
    dsd_position_fix f[2];
    expect_int("monitored LSF keeps nothing", feed_records(&r, f, 2U), 0);
    expect_int("monitored LSF writes no row position", row0(&r)->pos.valid, 0);

    /* The same LSF received is a position: the gate is the local transmission, not the content. */
    expect_int("received LSF passes", rf_lsf(&r, 0, 1U, meta, 0), 0);
    expect_int("received LSF keeps the position", feed_records(&r, f, 2U), 1);
    rig_close(&r);
}

/* The stream monitor: the LSD from the six LICH chunks of transmitted stream frames shows its call, but is no
 * position. */
static void
test_local_tx_stream_monitor_gives_nothing(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    uint8_t bits[LSF_BITS];
    uint8_t packed[M17_LSF_BYTES];
    uint8_t randomized[M17_PAYLOAD_BITS];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    (void)build_lsf(bits, packed, 0, 1U, meta);
    const uint64_t rev = dsd_position_last_rev();
    dsd_call_snapshot call;
    expect_int("no call before the monitored stream", dsd_call_state_get(r.state, 0U, &call), 0);
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        uint8_t content[M17_LICH_CONTENT_BITS];
        uint8_t decoded[M17_PAYLOAD_BITS];
        DSD_MEMSET(content, 0, sizeof(content));
        DSD_MEMSET(decoded, 0, sizeof(decoded));
        (void)m17_lich_build_content(bits, chunk, content);
        m17_lich_encode_bits(content, decoded);
        m17_payload_encode_bits(decoded, randomized);
        m17_monitor_encoded_stream(r.opts, r.state, randomized);
    }
    expect_int("monitored stream shows its call", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_int("monitored stream admits nothing", (long long)(dsd_position_last_rev() - rev), 0);

    dsd_position_fix f[2];
    expect_int("monitored stream keeps nothing", feed_records(&r, f, 2U), 0);
    rig_close(&r);
}

static void
build_ip_stream_frame_fn(uint8_t ip_frame[54], const uint8_t lsf_bits[LSF_BITS], uint16_t sid, uint16_t fn,
                         int corrupt) {
    uint8_t ip_bits[52U * 8U];
    DSD_MEMSET(ip_bits, 0, sizeof(ip_bits));
    write_bits(ip_bits, ((unsigned long long)'M' << 24U) | ((unsigned long long)'1' << 16U) | ('7' << 8U) | ' ', 32U);
    write_bits(ip_bits + 32, sid, 16U);
    DSD_MEMCPY(ip_bits + 48, lsf_bits, M17_LSF_LSD_BITS);
    write_bits(ip_bits + 273, fn & 0x7FFFU, 15U);
    pack_bits(ip_bits, sizeof(ip_bits), ip_frame);
    const uint16_t crc = m17_crc16(ip_frame, 52U);
    ip_frame[52] = (uint8_t)(crc >> 8U);
    ip_frame[53] = (uint8_t)((crc & 0xFFU) ^ (corrupt ? 1U : 0U));
}

static void
build_ip_stream_frame(uint8_t ip_frame[54], const uint8_t lsf_bits[LSF_BITS], int corrupt) {
    build_ip_stream_frame_fn(ip_frame, lsf_bits, 0xBEEFU, 1U, corrupt);
}

/* The IP stream carrier: the frame's own CRC is the check. */
static void
test_ip_stream_position(void) {
    for (int corrupt = 0; corrupt <= 1; corrupt++) {
        rig r;
        rig_open(&r);
        uint8_t meta[M17_META_BYTES];
        uint8_t bits[LSF_BITS];
        uint8_t packed[M17_LSF_BYTES];
        uint8_t frame[54];
        gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
        (void)build_lsf(bits, packed, 0, 1U, meta);
        /* A data stream: its payload decodes to nothing that needs a vocoder. */
        write_bits(bits + 96, m17_compose_frame_info(1U, 1U, 0U, 1U, 5U, 0U, 0U), 16U);
        build_ip_stream_frame(frame, bits, corrupt);
        m17_ip_dispatch_frame(r.opts, r.state, frame, (int)sizeof(frame));
        dsd_position_fix f[2];
        const int n = feed_records(&r, f, 2U);
        expect_int(corrupt ? "bad IP stream CRC gives no position" : "IP stream frame gives the position", n,
                   corrupt ? 0 : 1);
        if (!corrupt && n == 1) {
            check_quarter_fix("IP stream", &f[0]);
            expect_int("IP stream protocol", f[0].protocol, DSD_SYNC_M17_STR_POS);
            expect_int("IP stream call row takes it", row0(&r)->pos.valid, 1);
        }
        rig_close(&r);
    }
}

/* An MPKT frame: LSF, packet and CRC. */
static size_t
build_mpkt(uint8_t* frame, size_t cap, const uint8_t lsf_bits[LSF_BITS], const uint8_t* app, size_t app_len) {
    const size_t len = 34U + app_len + 2U;
    if (len > cap) {
        return 0U;
    }
    DSD_MEMSET(frame, 0, cap);
    DSD_MEMCPY(frame, "MPKT", 4U);
    frame[4] = 0xBEU;
    frame[5] = 0xEFU;
    pack_bits(lsf_bits, M17_LSF_LSD_BITS, frame + 6);
    DSD_MEMCPY(frame + 34, app, app_len);
    const uint16_t crc = m17_crc16(frame, (uint16_t)(len - 2U));
    frame[len - 2U] = (uint8_t)(crc >> 8U);
    frame[len - 1U] = (uint8_t)crc;
    return len;
}

/* A packet shaped like the GNSS META behind a 0x91 specifier (C2 91), which no M17 source defines. */
static size_t
gnss_shaped_packet(uint8_t* app) {
    app[0] = 0xC2U;
    app[1] = 0x91U;
    gnss_meta(app + 2, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    return 2U + M17_META_BYTES;
}

/* The MPKT carrier. A packet-mode LSF has no META subtype (M17 v2.0.11: its TYPE defines only P/S and the CAN), so a
 * real MPKT names no position, and a GNSS-shaped 0x91 packet in it is an unknown protocol. A checked LSD in it is read
 * as any other: the frame's own CRC gates it. */
static void
test_mpkt_meta_and_0x91_packet(void) {
    uint8_t zero_meta[M17_META_BYTES];
    uint8_t meta[M17_META_BYTES];
    uint8_t bits[LSF_BITS];
    uint8_t packed[M17_LSF_BYTES];
    uint8_t app[32];
    uint8_t frame[128];
    DSD_MEMSET(zero_meta, 0, sizeof(zero_meta));
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    const size_t app_len = gnss_shaped_packet(app);

    rig r;
    rig_open(&r);
    (void)build_lsf(bits, packed, 1, 0U, zero_meta);
    size_t len = build_mpkt(frame, sizeof(frame), bits, app, app_len);
    dsd_test_capture_stderr cap;
    char out[4096];
    const int captured = dsd_test_capture_stderr_begin(&cap, "m17_gnss_0x91") == 0;
    m17_ip_dispatch_frame(r.opts, r.state, frame, (int)len);
    if (captured) {
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        expect_true("0x91 packet shown as unknown", strstr(out, "Res/Unk: 000091;") != NULL);
        expect_true("0x91 packet shows no fix", strstr(out, "GPS") == NULL);
    }
    dsd_call_snapshot call;
    expect_int("0x91 MPKT published its call", dsd_call_state_get(r.state, 0U, &call), 1);
    dsd_position_fix f[2];
    expect_int("0x91 packet gives no position", feed_records(&r, f, 2U), 0);
    rig_close(&r);

    rig_open(&r);
    (void)build_lsf(bits, packed, 0, 1U, meta);
    len = build_mpkt(frame, sizeof(frame), bits, app, app_len);
    m17_ip_dispatch_frame(r.opts, r.state, frame, (int)len);
    expect_int("MPKT LSD META gives one position", feed_records(&r, f, 2U), 1);
    check_quarter_fix("MPKT", &f[0]);

    /* The same frame with a broken CRC: nothing. */
    frame[len - 1U] ^= 1U;
    const uint64_t rev = dsd_position_last_rev();
    m17_ip_dispatch_frame(r.opts, r.state, frame, (int)len);
    expect_int("bad MPKT CRC admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
    rig_close(&r);
}

/* RF packet mode: the LSF and the packet are separate frames, each with its own CRC. A GNSS-shaped 0x91 packet is no
 * position whether the LSF before it passed its CRC or failed it (-F), and whether the packet's own CRC passed. */
static void
test_rf_packet_and_lsf_checked_apart(void) {
    uint8_t zero_meta[M17_META_BYTES];
    uint8_t app[32];
    DSD_MEMSET(zero_meta, 0, sizeof(zero_meta));
    const size_t app_len = gnss_shaped_packet(app);

    for (int lsf_bad = 0; lsf_bad <= 1; lsf_bad++) {
        for (int packet_bad = 0; packet_bad <= 1; packet_bad++) {
            rig r;
            rig_open(&r);
            r.opts->aggressive_framesync = 0;
            expect_int("packet LSF CRC as built", rf_lsf(&r, 1, 0U, zero_meta, lsf_bad), lsf_bad);
            DSD_MEMSET(r.state->m17_pkt, 0, sizeof(r.state->m17_pkt));
            DSD_MEMCPY(r.state->m17_pkt, app, app_len);
            const uint16_t crc = m17_crc16(r.state->m17_pkt, (uint16_t)app_len);
            r.state->m17_pkt[app_len] = (uint8_t)(crc >> 8U);
            r.state->m17_pkt[app_len + 1U] = (uint8_t)((crc & 0xFFU) ^ (packet_bad ? 1U : 0U));
            const uint64_t rev = dsd_position_last_rev();
            m17_pkt_finalize_eot(r.opts, r.state, (uint16_t)app_len, (int)app_len + 2);
            expect_int("0x91 RF packet admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
            dsd_position_fix f[2];
            expect_int("0x91 RF packet keeps nothing", feed_records(&r, f, 2U), 0);
            rig_close(&r);
        }
    }
}

/* Issue #800 N1: the Extended Callsign META decodes from the META bytes themselves. */
static void
test_extended_callsign_meta_decodes(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    DSD_MEMSET(meta, 0, sizeof(meta));
    const unsigned long long field = m17_encode_b40_callsign(0ULL, "AB1CD");
    for (int i = 0; i < 6; i++) {
        meta[i] = (uint8_t)(field >> (40U - ((unsigned)i * 8U)));
    }
    dsd_test_capture_stderr cap;
    char out[4096];
    if (dsd_test_capture_stderr_begin(&cap, "m17_ecd") == 0) {
        (void)rf_lsf(&r, 0, 2U, meta, 0);
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        expect_true("ECD META decodes its callsign", strstr(out, " CF1: AB1CD") != NULL);
        expect_true("ECD META is not refused", strstr(out, "Invalid Extended Callsign") == NULL);
    }
    rig_close(&r);
}

/* A stream LSF whose data type is 0 opens no M17 call (m17_publish_lsf() observes only voice and data), so a DMR call
 * still open on the slot is the slot's ACTIVE call when its META position is read. That call is no transmission of
 * the report: each such report has no transmission to repeat within and is admitted again, never deduplicated
 * against the DMR call's epoch, and the DMR call's row is left alone (issue #800). */
static void
test_other_protocol_call_is_no_transmission(void) {
    rig r;
    rig_open(&r);
    dsd_call_observation dmr = dsd_call_observation_data(DSD_SYNC_DMR_BS_VOICE_POS, 0U, 1001U, 2002U);
    dmr.kind = DSD_CALL_KIND_GROUP_VOICE;
    expect_true("DMR call opens", dsd_call_state_observe(r.state, &dmr, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(r.opts, r.state, 0U);
    uint8_t meta[M17_META_BYTES];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    const uint64_t rev = dsd_position_last_rev();
    for (int i = 0; i < 2; i++) {
        uint8_t bits[LSF_BITS];
        uint8_t packed[M17_LSF_BYTES];
        DSD_MEMSET(bits, 0, LSF_BITS);
        m17_load_lsf_callsigns(bits, m17_encode_b40_callsign(0ULL, TEST_DST_CALLSIGN),
                               m17_encode_b40_callsign(0ULL, TEST_SRC_CALLSIGN));
        write_bits(bits + 96, m17_compose_frame_info(1U, 0U, 0U, 1U, 5U, 0U, 0U), 16U);
        for (size_t k = 0U; k < M17_META_BYTES; k++) {
            write_bits(bits + 112U + (k * 8U), meta[k], 8U);
        }
        const uint16_t crc = m17_attach_lsf_crc(bits, packed);
        DSD_MEMCPY(r.state->m17_lsf, bits, sizeof(bits));
        expect_int("data type 0 LSF passes", m17_finalize_lsf_crc(r.opts, r.state, packed, crc), 0);
    }
    dsd_call_snapshot call;
    expect_true("the DMR call is still the slot's",
                dsd_call_state_get(r.state, 0U, &call) > 0 && DSD_SYNC_IS_DMR(call.protocol));
    expect_int("each report admitted", (long long)(dsd_position_last_rev() - rev), 2);
    expect_int("DMR row untouched", row0(&r)->pos.valid, 0);
    rig_close(&r);
}

/* An RF stream LSF from @p src of data type @p dt (2: voice, 0: neither voice nor data) with META subtype 1. */
static int
rf_stream_lsf_from(rig* r, const char* src, uint8_t dt, const uint8_t meta[M17_META_BYTES]) {
    uint8_t bits[LSF_BITS];
    uint8_t packed[M17_LSF_BYTES];
    DSD_MEMSET(bits, 0, LSF_BITS);
    m17_load_lsf_callsigns(bits, m17_encode_b40_callsign(0ULL, TEST_DST_CALLSIGN), m17_encode_b40_callsign(0ULL, src));
    write_bits(bits + 96, m17_compose_frame_info(1U, dt, 0U, 1U, 5U, 0U, 0U), 16U);
    for (size_t k = 0U; k < M17_META_BYTES; k++) {
        write_bits(bits + 112U + (k * 8U), meta[k], 8U);
    }
    const uint16_t crc = m17_attach_lsf_crc(bits, packed);
    DSD_MEMCPY(r->state->m17_lsf, bits, sizeof(bits));
    return m17_finalize_lsf_crc(r->opts, r->state, packed, crc);
}

/* An LSF that observes no call (a stream of data type 0) is bound to no transmission, even while another station's M17
 * call is still ACTIVE on the slot: its GNSS META is never deduplicated against that call's epoch. N0CALL's voice
 * call admits its report; TEST2's data type 0 LSF with the same META bytes is a second report under its own callsign,
 * and N0CALL's row keeps N0CALL's position (issue #800). */
static void
test_unbound_lsf_is_not_the_active_calls(void) {
    rig r;
    rig_open(&r);
    uint8_t meta[M17_META_BYTES];
    gnss_meta(meta, GNSS_ALL_VALID, 3U, TEST_BEARING_DEG, RAW_QUARTER, RAW_QUARTER);
    expect_int("N0CALL voice LSF passes", rf_stream_lsf_from(&r, TEST_SRC_CALLSIGN, 2U, meta), 0);
    dsd_call_snapshot call;
    expect_true("N0CALL's M17 call is active", dsd_call_state_get(r.state, 0U, &call) > 0
                                                   && call.phase == DSD_CALL_PHASE_ACTIVE
                                                   && DSD_SYNC_IS_M17(call.protocol));
    const uint64_t epoch = call.epoch;
    const uint64_t row_rev = row0(&r)->pos.rev;
    expect_int("N0CALL's row takes its position", row0(&r)->pos.valid, 1);
    const uint64_t rev = dsd_position_last_rev();
    expect_int("TEST2 data type 0 LSF passes", rf_stream_lsf_from(&r, "TEST2", 0U, meta), 0);
    expect_int("TEST2's report is admitted", (long long)(dsd_position_last_rev() - rev), 1);
    dsd_position_fix f[4];
    expect_int("both senders kept", feed_records(&r, f, 4U), 2);
    expect_true("first record is N0CALL's", strcmp(f[0].source_text, TEST_SRC_CALLSIGN) == 0);
    expect_true("second record is TEST2's", strcmp(f[1].source_text, "TEST2") == 0);
    expect_true("N0CALL's call is still the slot's", dsd_call_state_get(r.state, 0U, &call) > 0 && call.epoch == epoch);
    expect_int("N0CALL's row keeps its position", (long long)row0(&r)->pos.rev, (long long)row_rev);
    rig_close(&r);
}

int
main(void) {
    test_rf_lsf_meta_position();
    test_rf_repeated_meta_is_one_report();
    test_ip_repeated_meta_is_one_report();
    test_can_change_position_on_call_row();
    test_ip_stream_end_admits_next_stream();
    test_reused_state_admits_meta_again();
    test_relaxed_bad_lsf_gives_nothing();
    test_meta_field_ranges();
    test_lich_lsd_position();
    test_local_tx_monitor_gives_nothing();
    test_local_tx_stream_monitor_gives_nothing();
    test_ip_stream_position();
    test_mpkt_meta_and_0x91_packet();
    test_rf_packet_and_lsf_checked_apart();
    test_extended_callsign_meta_decodes();
    test_other_protocol_call_is_no_transmission();
    test_unbound_lsf_is_not_the_active_calls();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "M17_GNSS_POSITION: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "M17_GNSS_POSITION: OK\n");
    return 0;
}
