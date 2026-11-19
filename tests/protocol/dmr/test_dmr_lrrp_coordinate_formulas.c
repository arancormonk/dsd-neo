// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * LRRP coordinates through the real decoder (issue #800 bugs 2.15 and 2.9), with fixed reference vectors.
 *
 * TIA-102.AABF-D-1 §7.3.37: the latitude is a sign bit (the most significant bit) plus a 31-bit magnitude in units of
 * 90/2^31 degree, 90 itself sent as 2^31-1. §7.3.38: the longitude is a 32-bit two's complement value in units of
 * 360/2^32 degree. SDRTrunk's Point2d reads both the same way. Moto.Net reads the latitude as a signed 32-bit value
 * and ok-dmrlib as an unsigned one with no sign at all, so neither is followed for southern latitudes.
 *
 * Each vector is a point-2d (0x66) response whose raw latitude and longitude are fixed bytes; the expected values are
 * exact integers in 1e-7 degree (the position record's unit), checked on the admitted record the decoder stages for
 * its data notice and on the `-L` row. The coordinates are synthetic.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Minimal stubs for direct link with dmr_pdu.c (the LIP and Cellocator services: dsd-neo_test_dmr_decoder_stubs)
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_unicode_supported(void) {
    return 0;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation;
    (void)notice;
    (void)slot;
    return 0;
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_SLASH) ? "1999/01/02" : "11:22:33";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

// Under test
void dmr_lrrp(const dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest,
              const uint8_t* DMR_PDU, uint8_t pdu_crc_ok);

typedef struct {
    const char* tag;
    uint32_t lat_raw;
    uint32_t lon_raw;
    int admitted;
    int32_t lat_e7;
    int32_t lon_e7;
    const char* row; // the -L row's latitude and longitude columns
} coord_case;

static const coord_case k_cases[] = {
    {"north east", 0x20000000u, 0x20000000u, 1, 225000000, 450000000, "22.50000\t45.00000"},
    // Bug 2.15: A0000000 is -22.5 (sign + magnitude), not -67.5 (two's complement).
    {"south west", 0xA0000000u, 0xE0000000u, 1, -225000000, -450000000, "-22.50000\t-45.00000"},
    {"pole and date line", 0x7FFFFFFFu, 0x80000000u, 1, 900000000, -1800000000, "90.00000\t-180.00000"},
    {"south pole", 0xFFFFFFFFu, 0x7FFFFFFFu, 1, -900000000, 1799999999, "-90.00000\t180.00000"},
    // Bug 2.9: a fix on the equator or the prime meridian is a fix.
    {"equator", 0x00000000u, 0x20000000u, 1, 0, 450000000, "0.00000\t45.00000"},
    {"prime meridian", 0x10000000u, 0x00000000u, 1, 112500000, 0, "11.25000\t0.00000"},
    {"negative zero latitude", 0x80000000u, 0x40000000u, 1, 0, 900000000, "0.00000\t90.00000"},
    // Exactly (0, 0) is refused: no record, no row, no coordinates in the text.
    {"null island", 0x00000000u, 0x00000000u, 0, 0, 0, NULL},
};

static int
truncate_file(const char* path) {
    FILE* f = dsd_fopen_private(path, "w");
    return f != NULL && fclose(f) == 0;
}

static size_t
read_file(const char* path, char* buf, size_t cap) {
    if (cap == 0U) {
        return 0U;
    }
    DSD_MEMSET(buf, 0, cap);    /* terminated wherever the read stops: at most cap - 1 bytes are read */
    FILE* f = fopen(path, "r"); /* text mode: the -L file is written in text mode (CRLF on Windows) */
    if (f == NULL) {
        return 0U;
    }
    const size_t n = fread(buf, 1, cap - 1U, f);
    fclose(f);
    return n;
}

static int
run_case(const dsd_opts* opts, dsd_state* st, const coord_case* c) {
    if (!truncate_file(opts->lrrp_out_file)) {
        return 1;
    }
    dsd_event_stage_clear(st, 0);
    uint8_t pdu[11] = {0x07, 9, 0x66};
    for (int b = 0; b < 4; b++) {
        pdu[3 + b] = (uint8_t)(c->lat_raw >> (24 - 8 * b));
        pdu[7 + b] = (uint8_t)(c->lon_raw >> (24 - 8 * b));
    }
    dmr_lrrp(opts, st, (uint16_t)sizeof pdu, 1234U, 5678U, pdu, 1U);

    int rc = 0;
    const dsd_position_fix* fix = dsd_event_staged_position(st, 0);
    char file[512];
    (void)read_file(opts->lrrp_out_file, file, sizeof file);
    if (!c->admitted) {
        if (fix->rev != 0U || file[0] != '\0'
            || strcmp(st->dmr_lrrp_gps[0], "LRRP SRC: 1234; Position suppressed (not usable);") != 0) {
            DSD_FPRINTF(stderr, "%s: refused report published: rev %llu, row '%s', text '%s'\n", c->tag,
                        (unsigned long long)fix->rev, file, st->dmr_lrrp_gps[0]);
            rc = 1;
        }
        return rc;
    }
    if (fix->rev == 0U || fix->kind != DSD_POS_DMR_LRRP || fix->lat_e7 != c->lat_e7 || fix->lon_e7 != c->lon_e7
        || fix->source_id != 1234U || fix->target_id != 5678U) {
        DSD_FPRINTF(stderr, "%s: record rev %llu kind %u (%d, %d) src %u tgt %u, want (%d, %d)\n", c->tag,
                    (unsigned long long)fix->rev, (unsigned)fix->kind, (int)fix->lat_e7, (int)fix->lon_e7,
                    (unsigned)fix->source_id, (unsigned)fix->target_id, (int)c->lat_e7, (int)c->lon_e7);
        rc = 1;
    }
    if (c->row != NULL) {
        char want[96];
        DSD_SNPRINTF(want, sizeof want, "1999/01/02\t11:22:33\t00001234\t%s\t0.000\t 0\t\n", c->row);
        if (strcmp(file, want) != 0) {
            DSD_FPRINTF(stderr, "%s: -L row '%s', want '%s'\n", c->tag, file, want);
            rc = 1;
        }
    }
    return rc;
}

// The record's target kind: a DMR packet addressed to its data header's group target is a group report; the same
// numbers on P25 (no DMR data header) are not, and the record carries the carrier's protocol.
static int
run_target_kind(const dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    st->dmr_lrrp_target[0] = 5678;
    st->dmr_data_checked_group[0] = 1;
    static const int protocols[2] = {DSD_SYNC_DMR_BS_DATA_POS, DSD_SYNC_P25P1_POS};
    for (size_t i = 0; i < 2U; i++) {
        st->lastsynctype = protocols[i];
        rc |= run_case(opts, st, &k_cases[0]);
        const dsd_position_fix* fix = dsd_event_staged_position(st, 0);
        const int group = (fix->flags & (uint16_t)DSD_POS_FLAG_TARGET_GROUP) != 0U;
        if (fix->protocol != (uint8_t)protocols[i] || group != (i == 0U)) {
            DSD_FPRINTF(stderr, "target kind %u: protocol %u group %d\n", (unsigned)i, (unsigned)fix->protocol, group);
            rc = 1;
        }
    }
    st->dmr_data_checked_group[0] = 0;
    st->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    return rc;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state st;
    st.currentslot = 0;
    st.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    st.event_history_s = calloc(2, sizeof *st.event_history_s);
    if (st.event_history_s == NULL) {
        return 100;
    }
    int fd = dsd_test_mkstemp(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "dmr_lrrp_coordinates");
    if (fd < 0) {
        free(st.event_history_s);
        return 101;
    }
    (void)dsd_close(fd);
    opts.lrrp_file_output = 1;

    int rc = 0;
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        rc |= run_case(&opts, &st, &k_cases[i]);
    }
    rc |= run_target_kind(&opts, &st);

    remove(opts.lrrp_out_file);
    free(st.event_history_s);
    if (rc == 0) {
        DSD_FPRINTF(stdout, "DMR_LRRP_COORDINATE_FORMULAS: PASS\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
