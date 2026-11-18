// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
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

// The notice row a carrier committed carries the admitted report: its text and its position (the report's own source).
static int
notice_has_position(const Event_History* notice, uint32_t source) {
    return strstr(notice->gps_s, "LIP:") != NULL && notice->pos.valid == 1U && notice->pos.kind == DSD_POS_LIP
           && notice->pos.lat_e7 == -225000000 && notice->pos.lon_e7 == -450000000 && notice->source_id == source;
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
    CHECK(notice_has_position(&state->event_history_s[0].Event_History_Items[1], 123456U));
    char saved[sizeof state->dmr_embedded_gps[0]];
    DSD_SNPRINTF(saved, sizeof saved, "%s", state->dmr_embedded_gps[0]);
    CHECK(truncate_output(path));
    send_udt(opts, state, 1);
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "T8a invalid UDT"));
    CHECK(strcmp(state->dmr_embedded_gps[0], saved) == 0);
    // Under -F the CRC-failed UDT is decoded and its notice committed, marked, without the position (spec 2.11).
    opts->dmr_crc_relaxed_default = 1;
    CHECK(truncate_output(path));
    send_udt(opts, state, 1);
    opts->dmr_crc_relaxed_default = 0;
    const Event_History* failed_notice = &state->event_history_s[0].Event_History_Items[1];
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "T8a relaxed invalid UDT"));
    CHECK(strstr(failed_notice->event_string, "CRC ERR") != NULL);
    CHECK(strstr(failed_notice->event_string, "LIP;") != NULL);
    CHECK(failed_notice->gps_s[0] == '\0' && failed_notice->pos.valid == 0U);
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
    CHECK(notice_has_position(&state->event_history_s[0].Event_History_Items[1], 111U));
    CHECK(strstr(state->event_history_s[0].Event_History_Items[1].event_string, "LIP SRC: 111; DST: 222;") != NULL);
    // A packet whose checks failed (CRC ERR, or RAS) gives no position anywhere; its notice stays (spec 2.11).
    for (uint8_t flags = 1U; flags <= 2U; flags++) {
        state->event_crc_invalid[0] = flags;
        CHECK(truncate_output(path));
        decode_ip_pdu(opts, state, sizeof packet, packet);
        CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "T8b CRC-invalid UDP"));
        const Event_History* unchecked = &state->event_history_s[0].Event_History_Items[1];
        CHECK(strstr(unchecked->event_string, "LIP SRC: 111;") != NULL);
        CHECK(unchecked->gps_s[0] == '\0' && unchecked->pos.valid == 0U);
        CHECK(dsd_event_staged_position(state, 0U)->rev == 0U);
    }
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

/* Issue #800: a LIP report in a UDP datagram (port 5017) is admitted against the reception the packet carrying it
 * began on: the DMR data header's origin, or for P25 Phase 1 data (SAP 0/4 through decode_ip_pdu()) the MDPU header's.
 * A retune between the header and the datagram refuses it; the same reception admits it. */
static int
test_udp_packet_origin(dsd_opts* opts, dsd_state* state, const char* path) {
    static const struct {
        int synctype;
        const char* tag;
    } carriers[] = {{DSD_SYNC_P25P1_POS, "P25 UDP LIP"}, {DSD_SYNC_DMR_BS_DATA_POS, "DMR UDP LIP"}};

    int failed = 0;
    uint8_t packet[38];
    build_packet(packet);
    for (size_t c = 0; c < sizeof carriers / sizeof carriers[0]; c++) {
        state->lastsynctype = carriers[c].synctype;
        dsd_position_origin* origin =
            DSD_SYNC_IS_P25P1(carriers[c].synctype) ? &state->p25_mdpu_origin : &state->dmr_data_origin[0];
        for (int retune = 1; retune >= 0; retune--) {
            dsd_position_origin_capture(opts, state, origin);
            if (retune) {
                dsd_trunk_tuning_generation_advance();
            }
            const uint64_t rev = dsd_position_last_rev();
            CHECK(truncate_output(path));
            CHECK(decode_ip_pdu(opts, state, sizeof packet, packet) == 1);
            const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
            if (retune) {
                CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, carriers[c].tag));
                CHECK(dsd_position_last_rev() == rev);
                CHECK(notice->pos.valid == 0U && notice->gps_s[0] == '\0');
            } else {
                CHECK(expect_rows(path, 1, "00000111", "-22.500000", "-45.000000", 20, 0, carriers[c].tag));
                CHECK(dsd_position_last_rev() == rev + 1U);
                CHECK(notice_has_position(notice, 111U));
            }
        }
        DSD_MEMSET(origin, 0, sizeof(*origin));
    }
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
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
    CHECK(notice_has_position(&state->event_history_s[0].Event_History_Items[1], 777U));
    // The notice consumed the staged report: the slot's next notice takes nothing.
    CHECK(dsd_event_staged_position(state, 0U)->rev == 0U);
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

// An active call whose epoch the event layer has opened, as the voice burst's sync leaves it: detail reaches the call's
// row only once its epoch is open (dsd_event_enrich_*() declines before).
static int
seed_active_call(dsd_opts* opts, dsd_state* state) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_source_id = 111,
        .ota_target_id = 1201,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) < 0) {
        return 0;
    }
    dsd_event_sync_slot(opts, state, 0U);
    return 1;
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
    CHECK(seed_active_call(opts, state));
    CHECK(truncate_output(path));
    send_carrier_report(opts, state, carrier, 111, 0);
    // One history writer per report: the carrier's own notice takes it, the speaker's voice row is left alone.
    Event_History* active = &state->event_history_s[0].Event_History_Items[0];
    CHECK(active->gps_s[0] == '\0' && active->pos.valid == 0U);
    CHECK(notice_has_position(&state->event_history_s[0].Event_History_Items[1], 111U));
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
    CHECK(strcmp(active->gps_s, saved) == 0);
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, "X1 unsupported report"));
    return failed;
}

static int
test_truncated_ownership(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    reset_carrier_state(state);
    CHECK(seed_active_call(opts, state));
    CHECK(truncate_output(path));
    send_carrier_report(opts, state, 1, 111, 0);
    Event_History* active = &state->event_history_s[0].Event_History_Items[0];
    CHECK(notice_has_position(&state->event_history_s[0].Event_History_Items[1], 111U));
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
    DSD_FPRINTF(stderr, "\nY1 ordinary UDP: notice text='%s'; staged text='%s'\n", notice->text_message,
                dsd_event_staged_text(state, 0));
    CHECK(strstr(notice->text_message, "OK") != NULL);
    CHECK(notice->gps_s[0] == '\0');
    CHECK(dsd_event_staged_text(state, 0)[0] == '\0');
    return failed;
}

static int
test_compressed_mixed_ports(dsd_opts* opts, dsd_state* state) {
    int failed = 0;
    reset_carrier_state(state);
    const uint8_t packet[9] = {0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x4F, 0x00, 0x4B};
    dmr_udp_comp_pdu(opts, state, sizeof packet, packet);
    const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
    DSD_FPRINTF(stderr, "\nY1 compressed UDP: notice text='%s'; staged text='%s'\n", notice->text_message,
                dsd_event_staged_text(state, 0));
    CHECK(strstr(notice->text_message, "OK") != NULL);
    CHECK(notice->gps_s[0] == '\0');
    CHECK(dsd_event_staged_text(state, 0)[0] == '\0');
    return failed;
}

static int
test_icmp_enclosed_report(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    reset_carrier_state(state);
    CHECK(seed_active_call(opts, state));
    CHECK(truncate_output(path));
    send_carrier_report(opts, state, 1, 111, 0);
    const Event_History* active = &state->event_history_s[0].Event_History_Items[0];
    CHECK(notice_has_position(&state->event_history_s[0].Event_History_Items[1], 111U));
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
    CHECK(notice_has_position(inner, 111U));
    CHECK(strstr(inner->event_string, "LIP SRC: 111;") != NULL);
    CHECK(outer->source_id == 999);
    DSD_FPRINTF(stderr, "\nX2 ICMP: inner source=%u GPS='%s'; outer source=%u GPS='%s'\n", inner->source_id,
                inner->gps_s, outer->source_id, outer->gps_s);
    CHECK(outer->gps_s[0] == '\0' && outer->pos.valid == 0U);
    CHECK(strcmp(active->gps_s, saved) == 0);
    CHECK(strstr(state->dmr_embedded_gps[0], "LIP:") != NULL);
    CHECK(strcmp(state->dmr_embedded_gps[0], inner->gps_s) == 0);
    return failed;
}

/* A binary NMEA UDT (TS 102 361-4 Annex B.3.6: UDT format 5) of @p blocks appended blocks (1 short format, 2 long)
   with @p pad_nibbles pad nibbles through the real UDT assembly: Q, the fix 22 deg 30 min S, 45 deg W, the UTC hour
   and the course; the header's own CRC as given. */
static void
send_udt_nmea_padded(dsd_opts* opts, dsd_state* state, int header_crc_ok, int q, unsigned hour, unsigned cog,
                     unsigned blocks, unsigned pad_nibbles) {
    uint8_t hdr[12] = {0, 0x05, 0, 0, 42, 0x01, 0xE2, 0x40, 0x01, 0, 0, 0};
    hdr[8] = (uint8_t)((pad_nibbles << 3) | (blocks - 1U));
    uint8_t bits[192] = {0};
    set_bits(bits, 3, 1, (uint32_t)q);
    set_bits(bits, 11, 7, 22);
    set_bits(bits, 18, 6, 30);
    set_bits(bits, 38, 8, 45);
    set_bits(bits, 66, 5, hour);
    if (blocks == 2U) {
        set_bits(bits, 103, 9, cog);
    }
    const size_t crc_at = (size_t)blocks * 96U - 16U;
    set_bits(bits, crc_at, 16, dsd_crc_ccitt16_bits(bits, crc_at));
    uint8_t packed[24] = {0};
    for (size_t i = 0; i < sizeof bits; ++i) {
        packed[i / 8] |= (uint8_t)(bits[i] << (7 - i % 8));
    }
    uint8_t hdr_bits[96];
    dsd_unpack_bytes_to_bits(hdr, sizeof hdr, hdr_bits, sizeof hdr_bits, sizeof hdr);
    dmr_dheader(opts, state, hdr, hdr_bits, header_crc_ok ? 1U : 0U, 0);
    for (unsigned b = 0; b < blocks; ++b) {
        dmr_block_assembler(opts, state, packed + ((size_t)b * 12U), 12, 0x07, 3);
    }
}

/* A long binary NMEA UDT, two appended blocks with no pad. */
static void
send_udt_nmea(dsd_opts* opts, dsd_state* state, int header_crc_ok, int q, unsigned hour, unsigned cog) {
    send_udt_nmea_padded(opts, state, header_crc_ok, q, hour, cog, 2U, 0U);
}

/* The pad nibbles are not user data: a short report with 12 of them in its one block holds Q and the latitude but no
   longitude, and a long one with 17 holds no course; neither is a position (the fields the pad stands in for were never
   sent). Each format with all of its fields is one. */
static int
test_udt_nmea_padding(dsd_opts* opts, dsd_state* state, const char* path) {
    static const struct {
        unsigned blocks;
        unsigned pad_nibbles;
        int rows;
        const char* console;
    } cases[] = {
        {1U, 12U, 0, "truncated (32 of 80 bits)"},
        {2U, 17U, 0, "truncated (108 of 112 bits)"},
        {1U, 0U, 1, "Short Format"},
        {2U, 15U, 1, "Long Format"},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        reset_carrier_state(state);
        CHECK(truncate_output(path));
        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "udt_nmea_pad") != 0) {
            return 1;
        }
        send_udt_nmea_padded(opts, state, 1, 1, 12U, 90U, cases[i].blocks, cases[i].pad_nibbles);
        dsd_test_capture_stderr_end(&cap);
        char output[4096];
        if (dsd_test_capture_stderr_read(&cap, output, sizeof output) != 0) {
            return 1;
        }
        const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
        if (!expect_rows(path, cases[i].rows, "00123456", "-22.500000", "-45.000000", 0, cases[i].blocks == 2U ? 90 : 0,
                         "UDT NMEA padding")
            || strstr(output, cases[i].console) == NULL || strstr(notice->event_string, "NMEA;") == NULL
            || (notice->pos.valid == 1U) != (cases[i].rows == 1)) {
            DSD_FPRINTF(stderr, "\nUDT NMEA padding case %zu: notice '%s' pos %u; console %s\n", i,
                        notice->event_string, notice->pos.valid, output);
            failed = 1;
        }
    }
    return failed;
}

/* Spec 2.5, V3, V4 and 2.11 through dispatch: a checked fix with an invalid hour and course keeps its position (time
   and course unknown); Q = 0 is no position; a header whose CRC failed but -F accepted reaches the UDT's CRC verdict,
   so its fix is no position either. Each notice is still committed. */
static int
test_udt_nmea_dispatch(dsd_opts* opts, dsd_state* state, const char* path) {
    static const struct {
        int header_crc_ok;
        int q;
        int rows;
        const char* console;
        const char* tag;
    } cases[] = {
        {1, 1, 1, "time unknown; COG: unknown", "UDT NMEA hour 24, course 360"},
        {1, 0, 0, "GPS: no fix;", "UDT NMEA Q=0"},
        {0, 1, 0, "Position: (suppressed; CRC ERR)", "UDT NMEA header CRC failed under -F"},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        reset_carrier_state(state);
        opts->dmr_crc_relaxed_default = 1;
        CHECK(truncate_output(path));
        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "udt_nmea") != 0) {
            return 1;
        }
        send_udt_nmea(opts, state, cases[i].header_crc_ok, cases[i].q, 24U, 360U);
        dsd_test_capture_stderr_end(&cap);
        char output[4096];
        if (dsd_test_capture_stderr_read(&cap, output, sizeof output) != 0) {
            return 1;
        }
        opts->dmr_crc_relaxed_default = 0;
        const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
        if (!expect_rows(path, cases[i].rows, "00123456", "-22.500000", "-45.000000", 0, 0, cases[i].tag)
            || strstr(output, cases[i].console) == NULL || strstr(notice->event_string, "NMEA;") == NULL
            || (notice->pos.valid == 1U) != (cases[i].rows == 1)
            || (cases[i].rows == 1 && (notice->pos.lat_e7 != -225000000 || notice->pos.kind != DSD_POS_NMEA))
            || (cases[i].header_crc_ok == 0 && strstr(notice->event_string, "CRC ERR") == NULL)) {
            DSD_FPRINTF(stderr, "\n%s: notice '%s' pos %u; console %s\n", cases[i].tag, notice->event_string,
                        notice->pos.valid, output);
            failed = 1;
        }
    }
    return failed;
}

/* Issue #800: P25 data's IP source is the low 24 address bits, so a fixed network endpoint (here 10.255.255.253, the
 * P25 FNE ID $FFFFFD) reaches the position path as a source ID. It names no radio: a LIP (UDP 5017) or LRRP (UDP 4001)
 * report is kept anonymous, with its checked destination, and no track, `-L` row or notice names $FFFFFD. A DMR packet
 * from the same address keeps its source: DMR addressing is untouched. */
static int
test_p25_fixed_network_source(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    uint8_t packet[38];
    build_packet(packet);
    packet[12] = 10;
    packet[13] = 255;
    packet[14] = 255;
    packet[15] = 253;
    static const int carriers[] = {DSD_SYNC_P25P1_POS, DSD_SYNC_DMR_BS_DATA_POS};
    for (size_t c = 0; c < sizeof carriers / sizeof carriers[0]; c++) {
        const int p25 = DSD_SYNC_IS_P25P1(carriers[c]);
        state->lastsynctype = carriers[c];
        dsd_state_ext_free_all(state);
        CHECK(dsd_position_feed_start(state) == 0);
        CHECK(truncate_output(path));
        CHECK(decode_ip_pdu(opts, state, sizeof packet, packet) == 1);
        dsd_position_fix rec;
        dsd_position_feed_meta meta;
        CHECK(dsd_position_feed_copy_since(state, 0U, &rec, 1, &meta) == 1);
        CHECK(rec.kind == DSD_POS_LIP && rec.target_id == 222U && rec.lat_e7 == -225000000);
        CHECK(rec.source_id == (p25 ? 0U : 0xFFFFFDU));
        CHECK(expect_rows(path, 1, p25 ? "00000000" : "16777213", "-22.500000", "-45.000000", 20, 0,
                          p25 ? "P25 FNE UDP LIP" : "DMR UDP LIP from the same address"));
        const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
        CHECK(notice->pos.valid == 1U && notice->pos.lat_e7 == -225000000);
        CHECK(notice->source_id == (p25 ? 0U : 0xFFFFFDU));
        // The notice's text names the sender the row names: a P25 endpoint is no radio there either.
        CHECK((strstr(notice->event_string, "LIP SRC: 16777213;") != NULL) == !p25);
        CHECK(!p25 || strstr(notice->event_string, "LIP SRC: 0;") != NULL);

        // LRRP on UDP 4001 from the same endpoint: an Immediate Location Response with a 2D point.
        static const uint8_t doc[] = {0x07, 0x09, 0x66, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00};
        uint8_t lrrp[28 + sizeof doc];
        DSD_MEMCPY(lrrp, packet, 28);
        lrrp[3] = (uint8_t)sizeof lrrp;
        lrrp[20] = 0x0F;
        lrrp[21] = 0xA1;
        lrrp[22] = 0x0F;
        lrrp[23] = 0xA1; // UDP 4001
        lrrp[24] = 0;
        lrrp[25] = (uint8_t)(8U + sizeof doc);
        DSD_MEMCPY(lrrp + 28, doc, sizeof doc);
        dsd_state_ext_free_all(state);
        CHECK(dsd_position_feed_start(state) == 0);
        CHECK(truncate_output(path));
        CHECK(decode_ip_pdu(opts, state, sizeof lrrp, lrrp) == 1);
        CHECK(dsd_position_feed_copy_since(state, 0U, &rec, 1, &meta) == 1);
        CHECK(rec.kind == DSD_POS_DMR_LRRP && rec.target_id == 222U);
        CHECK(rec.source_id == (p25 ? 0U : 0xFFFFFDU));
        CHECK(notice->pos.valid == 1U && notice->pos.kind == DSD_POS_DMR_LRRP);
        CHECK(notice->source_id == (p25 ? 0U : 0xFFFFFDU));
        CHECK((strstr(notice->event_string, "16777213") != NULL) == !p25);
    }
    // The whole P25 special range ($FFFFFC-$FFFFFF) is no radio; $FFFFFB is one.
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    static const uint8_t last_octets[] = {251, 252, 253, 254, 255};
    for (size_t i = 0; i < sizeof last_octets; i++) {
        packet[15] = last_octets[i];
        dsd_state_ext_free_all(state);
        CHECK(dsd_position_feed_start(state) == 0);
        CHECK(truncate_output(path));
        CHECK(decode_ip_pdu(opts, state, sizeof packet, packet) == 1);
        dsd_position_fix rec;
        dsd_position_feed_meta meta;
        CHECK(dsd_position_feed_copy_since(state, 0U, &rec, 1, &meta) == 1);
        const uint32_t addr = 0xFFFF00U | last_octets[i];
        CHECK(rec.source_id == (last_octets[i] == 251U ? addr : 0U));
        CHECK(state->event_history_s[0].Event_History_Items[1].source_id == rec.source_id);
    }
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    return failed;
}

/* A decoded datagram that is not whole gives nothing of its payload: no -L row, no position on its notice, no feed
 * record, no service named from octets that are not a UDP header. */
static int
expect_not_decoded(dsd_opts* opts, dsd_state* state, const char* path, uint8_t* packet, uint16_t len,
                   const char* label) {
    int failed = 0;
    const uint64_t rev = dsd_position_last_rev();
    CHECK(truncate_output(path));
    CHECK(decode_ip_pdu(opts, state, len, packet) == 1);
    CHECK(expect_rows(path, 0, NULL, NULL, NULL, 0, 0, label));
    const Event_History* notice = &state->event_history_s[0].Event_History_Items[1];
    CHECK(notice->pos.valid == 0U && notice->gps_s[0] == '\0');
    CHECK(strstr(notice->event_string, "LIP") == NULL);
    CHECK(strstr(notice->event_string, "not decoded") != NULL || strstr(notice->event_string, "Truncated") != NULL);
    CHECK(dsd_position_last_rev() == rev);
    if (failed) {
        DSD_FPRINTF(stderr, "%s: notice '%s'\n", label, notice->event_string);
    }
    return failed;
}

/* Issue #800 (g62-r6): an IPv4 fragment is not reassembled, so nothing of its payload is decoded. A later fragment
 * (offset 1, eight octets in) whose first octets look like UDP port 5017 followed by a LIP report, the first fragment
 * of a datagram whose later fragments follow (More Fragments), a datagram whose total length the PDU does not hold
 * and a UDP length past the datagram give no position. The same report in a whole datagram does. */
static int
test_ip_fragments_give_nothing(dsd_opts* opts, dsd_state* state, const char* path) {
    int failed = 0;
    reset_carrier_state(state);
    uint8_t packet[38];

    build_packet(packet);
    packet[7] = 1; /* fragment offset 1 (eight octets): the "UDP header" is datagram data */
    failed |= expect_not_decoded(opts, state, path, packet, sizeof packet, "non-initial fragment");

    build_packet(packet);
    packet[6] = 0x20; /* More Fragments */
    failed |= expect_not_decoded(opts, state, path, packet, sizeof packet, "first fragment");

    build_packet(packet);
    packet[3] = 60; /* total length 60, 38 octets held */
    failed |= expect_not_decoded(opts, state, path, packet, sizeof packet, "cut datagram");

    build_packet(packet);
    packet[25] = 30; /* UDP length 30, 18 held */
    failed |= expect_not_decoded(opts, state, path, packet, sizeof packet, "cut UDP payload");

    build_packet(packet);
    CHECK(truncate_output(path));
    CHECK(decode_ip_pdu(opts, state, sizeof packet, packet) == 1);
    CHECK(expect_rows(path, 1, "00000111", "-22.500000", "-45.000000", 20, 0, "whole datagram after fragments"));
    CHECK(notice_has_position(&state->event_history_s[0].Event_History_Items[1], 111U));
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
    failed |= test_udp_packet_origin(&opts, &state, path);
    failed |= test_p25_fixed_network_source(&opts, &state, path);
    failed |= test_compressed(&opts, &state, path);
    for (int carrier = 0; carrier < 3; ++carrier) {
        failed |= test_active_call_ownership(&opts, &state, path, carrier);
    }
    failed |= test_truncated_ownership(&opts, &state, path);
    failed |= test_icmp_enclosed_report(&opts, &state, path);
    failed |= test_udp_mixed_ports(&opts, &state);
    failed |= test_compressed_mixed_ports(&opts, &state);
    failed |= test_udt_nmea_dispatch(&opts, &state, path);
    failed |= test_udt_nmea_padding(&opts, &state, path);
    failed |= test_ip_fragments_give_nothing(&opts, &state, path);
    dsd_state_ext_free_all(&state);
    free(state.event_history_s);
    (void)remove(path);
    return failed;
}
