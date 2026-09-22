// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/* Issue #550: LOCN and IP/UDP LRRP location rows honor the dispatch CRC verdict. */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dmr_pdu_internal.h"
#include "dsd-neo/core/call_state.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

// Isolate the real dmr_pdu.c writers using the same seams as DMR_LRRP_IP_UDP_LENGTHS.
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value;
    switch (format) {
        case DSD_LOCAL_DATETIME_DATE_COMPACT: value = "19990102"; break;
        case DSD_LOCAL_DATETIME_TIME_COMPACT: value = "112233"; break;
        case DSD_LOCAL_DATETIME_DATE_SLASH: value = "1999/01/02"; break;
        case DSD_LOCAL_DATETIME_TIME_COLON: value = "11:22:33"; break;
        case DSD_LOCAL_DATETIME_DATE_HYPHEN: value = "1999-01-02"; break;
        default: value = ""; break;
    }
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)observation;
    (void)notice;
    return 0;
}

int
dsd_event_emit_data_notice_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                    const dsd_call_observation* observation, const char* notice, const char* gps) {
    (void)gps;
    return dsd_event_emit_data_notice(opts, state, slot, observation, notice);
}

int
dsd_unicode_supported(void) {
    return 0;
}

void
lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count, uint32_t src) {
    (void)bit_count;
    (void)src;
    (void)opts;
    (void)state;
    (void)bits;
}

void
decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

int
dsd_event_emit_data_notice_classified(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                      const dsd_call_observation* observation, dsd_event_category category,
                                      const char* notice) {
    (void)category;
    return dsd_event_emit_data_notice(opts, state, slot, observation, notice);
}

int
dsd_event_emit_data_notice_classified_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                               const dsd_call_observation* observation, dsd_event_category category,
                                               const char* notice, const char* gps) {
    (void)category;
    return dsd_event_emit_data_notice_with_gps(opts, state, slot, observation, notice, gps);
}

static int
truncate_output(const char* path) {
    FILE* file = dsd_fopen_private(path, "w");
    return file != NULL && fclose(file) == 0;
}

static int
expect_rows(const char* path, int expected, const char* src, const char* lat, const char* lon, const char* tag) {
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
    if (strcmp(fields[2], src) != 0 || strcmp(fields[3], lat) != 0 || strcmp(fields[4], lon) != 0) {
        DSD_FPRINTF(stderr, "\n%s: unexpected src/lat/lon: %s / %s / %s\n", tag, fields[2], fields[3], fields[4]);
        return 0;
    }
    return 1;
}

static uint16_t
build_ipv4_udp_lrrp(uint8_t packet[64], uint16_t port) {
    // POINT_2D response from DMR_LRRP_CRC_GATING: lat 11.25, lon 45.
    static const uint8_t payload[] = {0x07, 12, 0x22, 0, 0x66, 0x10, 0, 0, 0, 0x20, 0, 0, 0};
    const uint16_t udp_len = (uint16_t)(8 + sizeof payload);
    const uint16_t ip_len = (uint16_t)(20 + udp_len);
    DSD_MEMSET(packet, 0, 64);
    packet[0] = 0x45; // IPv4, five-word header
    packet[2] = (uint8_t)(ip_len >> 8);
    packet[3] = (uint8_t)ip_len;
    packet[8] = 64;
    packet[9] = 17; // UDP
    packet[12] = 10;
    packet[15] = 111; // source radio 111
    packet[16] = 10;
    packet[19] = 222;
    packet[20] = (uint8_t)(port >> 8);
    packet[21] = (uint8_t)port;
    packet[22] = (uint8_t)(port >> 8);
    packet[23] = (uint8_t)port;
    packet[24] = (uint8_t)(udp_len >> 8);
    packet[25] = (uint8_t)udp_len;
    DSD_MEMCPY(packet + 28, payload, sizeof payload);
    return ip_len;
}

static int
run_cases(dsd_opts* opts, dsd_state* state, const char* path) {
    // LOCN date/time + ddmm.mmmm / dddmm.mmmm, as in DMR_LOCN_TIME_FALLBACK.
    static const uint8_t locn[] = "A123456070826N2230.0000E04500.0000";
    for (int invalid = 1; invalid >= 0; --invalid) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->event_crc_invalid[0] = invalid;
        state->dmr_lrrp_source[0] = 123456;
        state->dmr_lrrp_gps[0][0] = '\0';
        dmr_locn(opts, state, (uint16_t)(sizeof locn - 1), locn);
        if (!expect_rows(path, !invalid, "00123456", "22.50000", "45.00000", "LOCN")) {
            return 1 + (1 - invalid) * 2;
        }
        if (!strstr(state->dmr_lrrp_gps[0], "NMEA / LOCN") || !strstr(state->dmr_lrrp_gps[0], "(22.50000, 45.00000)")) {
            DSD_FPRINTF(stderr, "\nLOCN: missing decoded state: %s\n", state->dmr_lrrp_gps[0]);
            return 2 + (1 - invalid) * 2;
        }
    }
    state->currentslot = 0;
    state->data_header_format[0] = 13;
    state->data_header_dd_format[0] = 0x00;
    for (int invalid = 1; invalid >= 0; --invalid) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->event_crc_invalid[0] = (uint8_t)invalid;
        dmr_sd_pdu_process(opts, state, (uint16_t)(sizeof locn - 1), locn, 1U);
        if (!expect_rows(path, !invalid, "00123456", "22.50000", "45.00000", "T4 short-data LOCN")) {
            return 20;
        }
        if (!strstr(state->dmr_lrrp_gps[0], "NMEA / LOCN")) {
            return 21;
        }
    }
    static const uint16_t ports[] = {4001, 49198};
    for (size_t i = 0; i < sizeof ports / sizeof ports[0]; ++i) {
        uint8_t packet[64];
        uint16_t len = build_ipv4_udp_lrrp(packet, ports[i]);
        for (int invalid = 1; invalid >= 0; --invalid) {
            if (!truncate_output(path)) {
                return 100;
            }
            char tag[64];
            DSD_SNPRINTF(tag, sizeof tag, "UDP/%u %s", (unsigned)ports[i], invalid ? "CRC-invalid" : "clean");
            state->event_crc_invalid[0] = invalid;
            state->dmr_lrrp_gps[0][0] = '\0';
            decode_ip_pdu(opts, state, len, packet);
            if (!expect_rows(path, !invalid, "00000111", "11.25000", "45.00000", tag)) {
                return 5 + (int)i * 4 + (1 - invalid) * 2;
            }
            const char* expected = invalid ? "Position suppressed (CRC ERR)" : "(11.250000, 45.000000)";
            if (!strstr(state->dmr_lrrp_gps[0], expected)) {
                DSD_FPRINTF(stderr, "\n%s: missing '%s' in state: %s\n", tag, expected, state->dmr_lrrp_gps[0]);
                return 6 + (int)i * 4 + (1 - invalid) * 2;
            }
        }
    }
    return 0;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    state.event_history_s = calloc(2, sizeof *state.event_history_s);
    if (state.event_history_s == NULL) {
        return 101;
    }
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dmr_lrrp_event_crc");
    if (fd < 0) {
        free(state.event_history_s);
        return 102;
    }
    if (dsd_close(fd) != 0) {
        remove(path);
        free(state.event_history_s);
        return 103;
    }
    DSD_SNPRINTF(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "%s", path);
    opts.lrrp_file_output = 1;
    opts.aggressive_framesync = 0;
    opts.dmr_crc_relaxed_default = 1;
    state.currentslot = 0;
    int rc = run_cases(&opts, &state, path);
    remove(path);
    free(state.event_history_s);
    state.event_history_s = NULL;
    if (rc == 0) {
        DSD_FPRINTF(stdout, "DMR_LRRP_EVENT_CRC_GATING: PASS\n");
    }
    return rc;
}
