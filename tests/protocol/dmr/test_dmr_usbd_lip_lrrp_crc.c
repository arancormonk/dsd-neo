// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/*
 * Issue #550: real USBD -> LIP -> location-file CRC gating, including relaxed RAS.
 * The test encoder reverses src/fec/bptc.c: place reserved bits and 96 payload bits
 * in a 13x15 matrix, add systematic Hamming(15,11) row and Hamming(13,9) column
 * parity using the literal check matrices from src/fec/fec.c, then interleave.
 * Compare against an independent existing burst before using generated codewords.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static void
unpack(const uint8_t* bytes, uint8_t* bits, size_t count) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - i % 8U)) & 1U);
    }
}

static int
truncate_output(const char* path) {
    FILE* file = dsd_fopen_private(path, "w");
    return file != NULL && fclose(file) == 0;
}

static int
expect_rows(const char* path, int expected, const char* src, const char* lat, const char* lon, int speed, int azimuth,
            const char* tag) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        DSD_FPRINTF(stderr, "\n%s: cannot read output\n", tag);
        return 0;
    }
    int rows = 0;
    char line[512];
    char last[512] = "";
    while (fgets(line, sizeof line, file) != NULL) {
        if (strchr(line, '\n') != NULL) {
            ++rows;
        }
        DSD_SNPRINTF(last, sizeof last, "%s", line);
    }
    int read_error = ferror(file);
    fclose(file);
    if (read_error || rows != expected || (expected == 0 && last[0] != '\0')) {
        DSD_FPRINTF(stderr, "\n%s: expected %d rows, got %d; last row: %s\n", tag, expected, rows, last);
        return 0;
    }
    if (expected == 0) {
        return 1;
    }
    char* fields[7];
    char* cursor = last;
    for (size_t i = 0; i < 7; ++i) {
        if (cursor == NULL) {
            DSD_FPRINTF(stderr, "\n%s: missing column %zu\n", tag, i);
            return 0;
        }
        fields[i] = cursor;
        char* tab = strchr(cursor, '\t');
        if (tab != NULL) {
            *tab = '\0';
            cursor = tab + 1;
        } else {
            cursor = NULL;
        }
    }
    if (strcmp(fields[2], src) != 0 || strcmp(fields[3], lat) != 0 || strcmp(fields[4], lon) != 0
        || strtol(fields[5], NULL, 10) != speed || strtol(fields[6], NULL, 10) != azimuth) {
        DSD_FPRINTF(stderr, "\n%s: unexpected src/lat/lon/speed/azimuth: %s / %s / %s / %s / %s\n", tag, fields[2],
                    fields[3], fields[4], fields[5], fields[6]);
        return 0;
    }
    return 1;
}

// A clean voice call on slot 1 from radio 218, whose number is also the reply's hash: a USBD reply must neither enrich
// nor mark it (spec 2.14).
static void
open_clean_call(dsd_opts* opts, dsd_state* state) {
    dsd_call_observation observation;
    DSD_MEMSET(&observation, 0, sizeof observation);
    observation.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    observation.slot = 1U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 2002U;
    observation.policy_target_id = 2002U;
    observation.ota_source_id = 218U;
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
    dsd_event_sync_slot(opts, state, 1U);
}

static void
encode_usbd(const uint8_t bytes[10], int corrupt, const uint8_t reserved[3], uint8_t info[196]) {
    uint8_t payload[96] = {0};
    unpack(bytes, payload, 10U);
    uint16_t crc = (uint16_t)(ComputeCrcCCITT(payload) ^ 0x3333U);
    for (unsigned i = 0; i < 16; ++i) {
        payload[80 + i] = (uint8_t)((crc >> (15U - i)) & 1U);
    }
    // Flip the Time Elapsed LSB after the CRC: FEC stays valid, the CRC fails, no asserted field changes.
    payload[5] ^= (uint8_t)corrupt;
    dmr_test_encode_bptc_196x96(payload, reserved, info);
}

typedef struct {
    int aggressive;
    int relaxed;
    int corrupt;
    uint8_t reserved[3];
    dmr_burst_origin origin;
    int published; // admitted: GPS text and a notice of its own
    const char* tag;
} usbd_case;

static int
check_case(const dsd_state* state, const usbd_case* c, const Event_History* notice_before) {
    const Event_History_I* history = &state->event_history_s[1];
    const Event_History* call_row = &history->Event_History_Items[0];
    const Event_History* notice = &history->Event_History_Items[1];
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 1U, &call) <= 0 || call.crc_invalid != 0U || call_row->gps_s[0] != '\0'
        || call_row->pos.valid != 0U) {
        DSD_FPRINTF(stderr, "\n%s: the active call was enriched or marked: %s\n", c->tag, call_row->gps_s);
        return 0;
    }
    const char* gps = state->dmr_embedded_gps[1];
    if (!c->published) {
        if (gps[0] != '\0' || memcmp(notice, notice_before, sizeof *notice) != 0) {
            DSD_FPRINTF(stderr, "\n%s: an unpublished reply left text or a notice: %s\n", c->tag, gps);
            return 0;
        }
        return 1;
    }
    if (!strstr(gps, "218; LIP:") || !strstr(gps, "22.50000") || !strstr(gps, "S ") || !strstr(gps, "45.00000")
        || !strstr(gps, "W;") || !strstr(gps, "Err: 20m")) {
        DSD_FPRINTF(stderr, "\n%s: unexpected GPS state: %s\n", c->tag, gps);
        return 0;
    }
    // Its own notice: anonymous (the hash is no radio ID), the hash shown as a hash, the position kept.
    if (notice->source_id != 0U || !strstr(notice->event_string, "SRC(Hash): 218;") || strcmp(notice->gps_s, gps) != 0
        || notice->pos.valid != 1U || notice->pos.lat_e7 != -225000000 || notice->pos.lon_e7 != -450000000
        || notice->pos.kind != DSD_POS_LIP_USBD || (notice->pos.flags & DSD_POS_FLAG_SOURCE_HASHED) == 0U) {
        DSD_FPRINTF(stderr, "\n%s: unexpected notice: src=%u valid=%u lat=%d lon=%d '%s' / '%s'\n", c->tag,
                    notice->source_id, notice->pos.valid, notice->pos.lat_e7, notice->pos.lon_e7, notice->event_string,
                    notice->gps_s);
        return 0;
    }
    return 1;
}

static int
run_cases(dsd_opts* opts, dsd_state* state, const char* path) {
    // Same LIP vector as CORE_GPS_LRRP_CRC_GATING: 22.5 S, 45 W, hash 218, speed 20.
    static const uint8_t lip_bytes[10] = {0x07, 0x80, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x4A, 0x00, 0xDA};

    // Strict CRC publishes no failed DMR data, so a failed USBD is not decoded at all; -F decodes it and withholds the
    // position (spec 2.11). Only an inbound burst is a reply (spec 2.13). A hashed source is never written to -L (D8).
    static const usbd_case cases[] = {
        {1, 0, 0, {0, 0, 0}, DMR_BURST_ORIGIN_MS, 1, "Strict clean USBD"},
        {1, 0, 1, {0, 0, 0}, DMR_BURST_ORIGIN_MS, 0, "Strict CRC-failed USBD"},
        {0, 1, 1, {0, 0, 1}, DMR_BURST_ORIGIN_MS, 0, "Relaxed RAS CRC-failed USBD"},
        {0, 1, 0, {0, 0, 0}, DMR_BURST_ORIGIN_MS, 1, "Relaxed clean USBD"},
        {1, 0, 0, {0, 0, 0}, DMR_BURST_ORIGIN_BS, 0, "Outbound USBD (a Poll Request)"},
        {1, 0, 0, {0, 0, 0}, DMR_BURST_ORIGIN_DIRECT_TS1, 0, "Direct mode slot 1 USBD"},
        {1, 0, 0, {0, 0, 0}, DMR_BURST_ORIGIN_DIRECT_TS2, 0, "Direct mode slot 2 USBD"},
        {1, 0, 0, {0, 0, 0}, DMR_BURST_ORIGIN_UNKNOWN, 0, "Unknown-origin USBD"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t info[196];
        encode_usbd(lip_bytes, cases[i].corrupt, cases[i].reserved, info);
        if (!truncate_output(path)) {
            return 100;
        }
        opts->aggressive_framesync = cases[i].aggressive;
        // Mirror -F; the USBD burst path consults aggressive_framesync alone.
        opts->dmr_crc_relaxed_default = cases[i].relaxed;
        state->currentslot = 1;
        state->dmr_color_code = 16;
        state->data_p_head[1] = 1;
        state->data_conf_data[1] = 0;
        state->data_header_format[1] = 1;
        DSD_MEMSET(state->dmr_embedded_gps[1], 0, sizeof state->dmr_embedded_gps[1]);
        open_clean_call(opts, state);
        Event_History notice_before;
        DSD_MEMCPY(&notice_before, &state->event_history_s[1].Event_History_Items[1], sizeof notice_before);
        dmr_data_burst_handler_origin(opts, state, info, 0x0B, NULL, cases[i].origin);
        if (state->event_crc_invalid[1] != 0) {
            DSD_FPRINTF(stderr, "\n%s: CRC flag was not restored\n", cases[i].tag);
            return 10 + (int)i * 3;
        }
        if (!expect_rows(path, 0, NULL, NULL, NULL, 0, 0, cases[i].tag)) {
            return 11 + (int)i * 3;
        }
        if (!check_case(state, &cases[i], &notice_before)) {
            return 12 + (int)i * 3;
        }
    }

    // A Poll Request read as a reply would be the point (0, 0) with the polled radio's low byte as its "hash": even an
    // inbound burst of that shape is no position (admission refuses exactly (0, 0)).
    static const uint8_t request_bytes[10] = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56};
    uint8_t info[196];
    static const uint8_t no_reserved[3] = {0, 0, 0};
    encode_usbd(request_bytes, 0, no_reserved, info);
    opts->aggressive_framesync = 1;
    opts->dmr_crc_relaxed_default = 0;
    DSD_MEMSET(state->dmr_embedded_gps[1], 0, sizeof state->dmr_embedded_gps[1]);
    if (!truncate_output(path)) {
        return 100;
    }
    dmr_data_burst_handler_origin(opts, state, info, 0x0B, NULL, DMR_BURST_ORIGIN_MS);
    if (!expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "Zero position") || state->dmr_embedded_gps[1][0] != '\0') {
        DSD_FPRINTF(stderr, "\nZero position: published: %s\n", state->dmr_embedded_gps[1]);
        return 60;
    }
    return 0;
}

int
main(void) {
    InitAllFecFunction();
    if (!dmr_test_check_reference_burst()) {
        return 1;
    }
    dsd_opts* opts = calloc(1, sizeof *opts);
    dsd_state* state = calloc(1, sizeof *state);
    if (opts == NULL || state == NULL) {
        free(opts);
        free(state);
        return 101;
    }
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dmr_usbd_lip_crc");
    if (fd < 0) {
        free(opts);
        free(state);
        return 102;
    }
    if (dsd_close(fd) != 0) {
        remove(path);
        free(opts);
        free(state);
        return 103;
    }
    DSD_SNPRINTF(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", path);
    opts->lrrp_file_output = 1;
    opts->use_dsp_output = 0;
    opts->payload = 0;
    state->event_history_s = calloc(2, sizeof(Event_History_I));
    if (state->event_history_s == NULL) {
        remove(path);
        free(opts);
        free(state);
        return 104;
    }
    for (int slot = 0; slot < 2; ++slot) {
        init_event_history(&state->event_history_s[slot], 0, 255);
    }
    int rc = run_cases(opts, state, path);
    remove(path);
    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(opts);
    free(state);
    if (rc == 0) {
        DSD_FPRINTF(stdout, "DMR_USBD_LIP_LRRP_CRC: PASS\n");
    }
    return rc;
}
