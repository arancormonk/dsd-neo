// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/pdu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int
check(int ok, const char* expression, int line) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL line %d: %s\n", line, expression);
    }
    return !ok;
}

#define CHECK(expression) (failed |= check((expression), #expression, __LINE__))

static void
set_bits(uint8_t* bits, size_t offset, unsigned width, uint32_t value) {
    for (unsigned i = 0; i < width; ++i) {
        bits[offset + i] = (uint8_t)((value >> (width - i - 1U)) & 1U);
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
    (void)fclose(file);
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

static void
short_report(uint8_t bytes[12]) {
    uint8_t bits[96] = {0};
    set_bits(bits, 2, 2, 1);
    set_bits(bits, 4, 1, 1);
    set_bits(bits, 5, 24, 0xC00000);
    set_bits(bits, 29, 1, 1);
    set_bits(bits, 30, 23, 0x600000);
    set_bits(bits, 53, 3, 1);
    set_bits(bits, 56, 7, 20);
    set_bits(bits, 68, 8, 32);
    DSD_MEMSET(bytes, 0, 12);
    for (size_t i = 0; i < sizeof bits; ++i) {
        bytes[i / 8] |= (uint8_t)(bits[i] << (7 - i % 8));
    }
}

static void
send_udt(dsd_opts* opts, dsd_state* state, int corrupt) {
    uint8_t hdr[12] = {0, 0x0B, 0, 0, 42, 0x01, 0xE2, 0x40, 0x08, 0, 0, 0};
    uint8_t block[12];
    uint8_t bits[96];
    short_report(block);
    dsd_unpack_bytes_to_bits(block, sizeof block, bits, sizeof bits, sizeof block);
    uint16_t crc = dsd_crc_ccitt16_bits(bits, 80);
    block[10] = (uint8_t)(crc >> 8);
    block[11] = (uint8_t)(crc ^ (corrupt ? 1U : 0U));
    dsd_unpack_bytes_to_bits(hdr, sizeof hdr, bits, sizeof bits, sizeof hdr);
    dmr_dheader(opts, state, hdr, bits, 1, 0);
    dmr_block_assembler(opts, state, block, 12, 0x07, 3);
}

static int
test_udt(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    CHECK(truncate_output(path));
    send_udt(opts, state, 0);
    CHECK(strncmp(state->dmr_embedded_gps[0], "LIP: 22.50000", 12) == 0);
    CHECK(expect_rows(path, 1, "00123456", "-22.500000", "-45.000000", 20, 0, "T8a UDT LIP"));
    char saved[sizeof state->dmr_embedded_gps[0]];
    DSD_SNPRINTF(saved, sizeof saved, "%s", state->dmr_embedded_gps[0]);
    CHECK(truncate_output(path));
    send_udt(opts, state, 1);
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "T8a invalid UDT"));
    CHECK(strcmp(state->dmr_embedded_gps[0], saved) == 0);
    return failed;
}

static void
build_packet(uint8_t packet[38]) {
    DSD_MEMSET(packet, 0, 38);
    packet[0] = 0x45;
    packet[3] = 38;
    packet[8] = 64;
    packet[9] = 17;
    packet[12] = 10;
    packet[15] = 111;
    packet[16] = 10;
    packet[19] = 222;
    packet[20] = packet[22] = 0x13;
    packet[21] = packet[23] = 0x99; // UDP 5017
    packet[25] = 18;
    uint8_t report[12];
    short_report(report);
    DSD_MEMCPY(packet + 28, report, 10);
}

static int
test_udp(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    uint8_t packet[38];
    build_packet(packet);
    CHECK(truncate_output(path));
    CHECK(decode_ip_pdu(opts, state, sizeof packet, packet) == 1);
    CHECK(expect_rows(path, 1, "00000111", "-22.500000", "-45.000000", 20, 0, "T8b UDP LIP"));
    CHECK(strncmp(state->dmr_embedded_gps[0], "LIP:", 4) == 0);
    // Explicit data notices are committed to row 1; row 0 is restored to the active voice call.
    CHECK(strstr(state->event_history_s[0].Event_History_Items[1].gps_s, "LIP:") != NULL);
    CHECK(strstr(state->event_history_s[0].Event_History_Items[1].event_string, "LIP SRC: 111; DST: 222;") != NULL);
    state->event_crc_invalid[0] = 1;
    CHECK(truncate_output(path));
    decode_ip_pdu(opts, state, sizeof packet, packet);
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "T8b CRC-invalid UDP"));
    state->event_crc_invalid[0] = 0;
    char saved[sizeof state->dmr_embedded_gps[0]];
    DSD_SNPRINTF(saved, sizeof saved, "%s", state->dmr_embedded_gps[0]);
    packet[28] = 0x4C; // PDU type 1, extension 3: long report.
    CHECK(truncate_output(path));
    decode_ip_pdu(opts, state, sizeof packet, packet);
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "T8b unsupported UDP"));
    CHECK(state->event_history_s[0].Event_History_Items[1].gps_s[0] == '\0');
    CHECK(strcmp(state->dmr_embedded_gps[0], saved) == 0);
    return failed;
}

static int
test_compressed(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    uint8_t packet[15] = {0, 0, 0, 2, 2}; // Indexed LIP port (DPID=2), no extended ports.
    uint8_t report[12];
    short_report(report);
    DSD_MEMCPY(packet + 5, report, 10);
    state->dmr_lrrp_source[0] = 777;
    CHECK(truncate_output(path));
    dmr_udp_comp_pdu(opts, state, sizeof packet, packet);
    CHECK(expect_rows(path, 1, "00000777", "-22.500000", "-45.000000", 20, 0, "T8c indexed LIP"));
    CHECK(strstr(state->event_history_s[0].Event_History_Items[1].gps_s, "LIP:") != NULL);
    return failed;
}

static void
reset_carrier_state(dsd_state* state) {
    Event_History_I* history = state->event_history_s;
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof *state);
    DSD_MEMSET(history, 0, 2 * sizeof *history);
    state->event_history_s = history;
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
}

static int
seed_active_call(dsd_state* state) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_source_id = 111,
        .ota_target_id = 1201,
    };
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) >= 0;
}

static void
send_udt_report(dsd_opts* opts, dsd_state* state, uint32_t source, int unsupported) {
    uint8_t hdr[12] = {0, 0x0B, 0, 4, 0xB1, 0, 0, 0, 0x08, 0, 0, 0};
    hdr[5] = (uint8_t)(source >> 16);
    hdr[6] = (uint8_t)(source >> 8);
    hdr[7] = (uint8_t)source;
    uint8_t block[12];
    short_report(block);
    if (unsupported) {
        block[0] = 0x4C; // PDU type 1, extension 3.
    }
    uint8_t bits[96];
    dsd_unpack_bytes_to_bits(block, sizeof block, bits, sizeof bits, sizeof block);
    uint16_t crc = dsd_crc_ccitt16_bits(bits, 80);
    block[10] = (uint8_t)(crc >> 8);
    block[11] = (uint8_t)crc;
    dsd_unpack_bytes_to_bits(hdr, sizeof hdr, bits, sizeof bits, sizeof hdr);
    dmr_dheader(opts, state, hdr, bits, 1, 0);
    dmr_block_assembler(opts, state, block, 12, 0x07, 3);
}

static void
send_carrier_report(dsd_opts* opts, dsd_state* state, int carrier, uint32_t source, int unsupported) {
    if (carrier == 0) {
        send_udt_report(opts, state, source, unsupported);
    } else if (carrier == 1) {
        uint8_t packet[38];
        build_packet(packet);
        packet[15] = (uint8_t)source;
        if (unsupported) {
            packet[28] = 0x4C;
        }
        (void)decode_ip_pdu(opts, state, sizeof packet, packet);
    } else {
        uint8_t packet[15] = {0, 0, 0, 2, 2};
        uint8_t report[12];
        short_report(report);
        if (unsupported) {
            report[0] = 0x4C;
        }
        DSD_MEMCPY(packet + 5, report, 10);
        state->dmr_lrrp_source[0] = source;
        dmr_udp_comp_pdu(opts, state, sizeof packet, packet);
    }
}

static int
test_active_call_ownership(dsd_opts* opts, dsd_state* state, const char* path, int carrier) {
    static const char* const carriers[] = {"UDT", "UDP 5017", "compressed UDP"};
    int failed = 0;
    reset_carrier_state(state);
    CHECK(seed_active_call(state));
    CHECK(truncate_output(path));
    send_carrier_report(opts, state, carrier, 111, 0);
    Event_History* active = &state->event_history_s[0].Event_History_Items[0];
    CHECK(strstr(active->gps_s, "LIP:") != NULL);
    CHECK(expect_rows(path, 1, "00000111", "-22.500000", "-45.000000", 20, 0, "X1 positive report"));
    char saved[sizeof active->gps_s];
    DSD_SNPRINTF(saved, sizeof saved, "%s", active->gps_s);
    CHECK(truncate_output(path));
    send_carrier_report(opts, state, carrier, 222, 1);
    const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
    DSD_FPRINTF(stderr, "\nX1 %s: notice source=%u GPS='%s'; active GPS='%s'\n", carriers[carrier], notice->source_id,
                notice->gps_s, active->gps_s);
    CHECK(notice->source_id == 222);
    if (carrier != 2) {
        CHECK(strstr(notice->event_string, "SRC: 222;") != NULL);
    }
    CHECK(notice->gps_s[0] == '\0');
    CHECK(strstr(active->gps_s, "LIP:") != NULL);
    CHECK(strcmp(active->gps_s, saved) == 0);
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "X1 unsupported report"));
    return failed;
}

static int
test_truncated_ownership(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    reset_carrier_state(state);
    CHECK(seed_active_call(state));
    CHECK(truncate_output(path));
    send_carrier_report(opts, state, 1, 111, 0);
    Event_History* active = &state->event_history_s[0].Event_History_Items[0];
    CHECK(strstr(active->gps_s, "LIP:") != NULL);
    char saved[sizeof active->gps_s];
    DSD_SNPRINTF(saved, sizeof saved, "%s", active->gps_s);
    uint8_t packet[38];
    build_packet(packet);
    packet[15] = 222;
    packet[3] = 37; // Nine payload bytes: the type-0 report requires 76 bits.
    packet[25] = 17;
    CHECK(truncate_output(path));
    CHECK(decode_ip_pdu(opts, state, 37, packet) == 1);
    const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
    DSD_FPRINTF(stderr, "\nX1 truncated UDP: notice source=%u GPS='%s'; active GPS='%s'\n", notice->source_id,
                notice->gps_s, active->gps_s);
    CHECK(notice->source_id == 222);
    CHECK(strstr(notice->event_string, "LIP SRC: 222;") != NULL);
    CHECK(notice->gps_s[0] == '\0');
    CHECK(strstr(active->gps_s, "LIP:") != NULL);
    CHECK(strcmp(active->gps_s, saved) == 0);
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "X1 truncated report"));
    return failed;
}

static int
test_udp_mixed_ports(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    reset_carrier_state(state);
    uint8_t packet[38];
    build_packet(packet);
    packet[3] = 32;
    packet[23] = 0x98; // Source port 5017, destination port 5016 (ETSI text).
    packet[25] = 12;
    const uint8_t text[] = {0x00, 0x4F, 0x00, 0x4B};
    DSD_MEMCPY(packet + 28, text, sizeof text);
    CHECK(decode_ip_pdu(opts, state, 32, packet) == 1);
    const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
    const Event_History* staged = &state->event_history_s[0].Event_History_Items[0];
    DSD_FPRINTF(stderr, "\nY1 ordinary UDP: notice text='%s'; staged text='%s'\n", notice->text_message,
                staged->text_message);
    CHECK(strstr(notice->text_message, "OK") != NULL);
    CHECK(notice->gps_s[0] == '\0');
    CHECK(staged->text_message[0] == '\0');
    return failed;
}

static int
test_compressed_mixed_ports(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    reset_carrier_state(state);
    const uint8_t packet[9] = {0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x4F, 0x00, 0x4B};
    dmr_udp_comp_pdu(opts, state, sizeof packet, packet);
    const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
    const Event_History* staged = &state->event_history_s[0].Event_History_Items[0];
    DSD_FPRINTF(stderr, "\nY1 compressed UDP: notice text='%s'; staged text='%s'\n", notice->text_message,
                staged->text_message);
    CHECK(strstr(notice->text_message, "OK") != NULL);
    CHECK(notice->gps_s[0] == '\0');
    CHECK(staged->text_message[0] == '\0');
    return failed;
}

static int
test_icmp_enclosed_report(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    reset_carrier_state(state);
    CHECK(seed_active_call(state));
    CHECK(truncate_output(path));
    send_carrier_report(opts, state, 1, 111, 0);
    const Event_History* active = &state->event_history_s[0].Event_History_Items[0];
    CHECK(strstr(active->gps_s, "LIP:") != NULL);
    CHECK(expect_rows(path, 1, "00000111", "-22.500000", "-45.000000", 20, 0, "X2 active call seed"));
    char saved[sizeof active->gps_s];
    DSD_SNPRINTF(saved, sizeof saved, "%s", active->gps_s);
    uint8_t packet[66] = {0};
    packet[0] = 0x45;
    packet[3] = sizeof packet;
    packet[8] = 64;
    packet[9] = 1;
    packet[12] = 10;
    packet[14] = 3;
    packet[15] = 231; // 10.0.3.231: src24 = 999.
    packet[16] = 10;
    packet[19] = 222;
    packet[20] = 3;
    packet[21] = 3; // Destination unreachable, port unreachable; eight-byte ICMP header.
    build_packet(packet + 28);
    CHECK(decode_ip_pdu(opts, state, sizeof packet, packet) == 1);
    CHECK(expect_rows(path, 2, "00000111", "-22.500000", "-45.000000", 20, 0, "X2 ICMP enclosure"));
    const Event_History* outer = &state->event_history_s[0].Event_History_Items[1];
    const Event_History* inner = &state->event_history_s[0].Event_History_Items[2];
    CHECK(inner->source_id == 111);
    CHECK(strstr(inner->event_string, "LIP SRC: 111;") != NULL);
    CHECK(strstr(inner->gps_s, "LIP:") != NULL);
    CHECK(outer->source_id == 999);
    DSD_FPRINTF(stderr, "\nX2 ICMP: inner source=%u GPS='%s'; outer source=%u GPS='%s'\n", inner->source_id,
                inner->gps_s, outer->source_id, outer->gps_s);
    CHECK(outer->gps_s[0] == '\0');
    CHECK(strstr(active->gps_s, "LIP:") != NULL);
    CHECK(strcmp(active->gps_s, saved) == 0);
    CHECK(strstr(state->dmr_embedded_gps[0], "LIP:") != NULL);
    CHECK(strcmp(state->dmr_embedded_gps[0], inner->gps_s) == 0);
    return failed;
}

int
main(void) {
    InitAllFecFunction();
    if (!dmr_test_check_reference_burst()) {
        return 100;
    }
    static dsd_opts opts;
    static dsd_state state;
    opts.lrrp_file_output = 1;
    opts.aggressive_framesync = 1;
    state.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.event_history_s = calloc(2, sizeof *state.event_history_s);
    if (!state.event_history_s) {
        return 100;
    }
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "lip_carriers");
    if (fd < 0) {
        free(state.event_history_s);
        return 100;
    }
    dsd_close(fd);
    DSD_SNPRINTF(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "%s", path);
    int failed = test_udt(&opts, &state, path);
    failed |= test_udp(&opts, &state, path);
    failed |= test_compressed(&opts, &state, path);
    for (int carrier = 0; carrier < 3; ++carrier) {
        failed |= test_active_call_ownership(&opts, &state, path, carrier);
    }
    failed |= test_truncated_ownership(&opts, &state, path);
    failed |= test_icmp_enclosed_report(&opts, &state, path);
    failed |= test_udp_mixed_ports(&opts, &state);
    failed |= test_compressed_mixed_ports(&opts, &state);
    dsd_state_ext_free_all(&state);
    free(state.event_history_s);
    (void)remove(path);
    return failed;
}
