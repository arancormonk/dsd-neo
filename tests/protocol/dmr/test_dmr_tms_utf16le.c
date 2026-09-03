// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Motorola TMS (UDP 4007) carries its text as UCS-2 little-endian (MOTOTRBO Text Messaging ADK
 * Guide: "The MOTOTRBO radio only supports the UCS2-LE encoding schema"), after a two-octet
 * length, a header octet, an address-length octet and one or more header-extension octets whose
 * bit 7 says another follows. Issue #466: the decoder read the units big-endian from one octet
 * before the text, which only cancels out for ASCII, and the event-log copy kept ASCII only.
 *
 * The first packet is the reporter's vector: the Polish pangram behind a blank subject line
 * (CR LF). The second carries ASCII in the same framing, the case an endianness-only change
 * would have broken. The rest are truncated shapes that must not read past the payload.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
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

// Minimal stubs for direct link with dmr_pdu.c
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_unicode_supported(void) {
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
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

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

// Under test
int decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input);

/* "Zażółć gęślą jaźń" as UTF-8. */
static const char kPangramUtf8[] = "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84";

/* The reporter's packet: IPv4/UDP 4007 -> 4007, TMS length 42, header A0, no address, header
 * extension 8F 04, then CR LF and the pangram, all UCS-2 LE. */
static const uint8_t kPangramPacket[] = {
    0x45, 0x00, 0x00, 0x48, 0x00, 0x01, 0x00, 0x00, 0x40, 0x11, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x01, 0x0D, 0x00,
    0x00, 0x02, 0x0F, 0xA7, 0x0F, 0xA7, 0x00, 0x34, 0x00, 0x00, 0x00, 0x2A, 0xA0, 0x00, 0x8F, 0x04, 0x0D, 0x00,
    0x0A, 0x00, 0x5A, 0x00, 0x61, 0x00, 0x7C, 0x01, 0xF3, 0x00, 0x42, 0x01, 0x07, 0x01, 0x20, 0x00, 0x67, 0x00,
    0x19, 0x01, 0x5B, 0x01, 0x6C, 0x00, 0x05, 0x01, 0x20, 0x00, 0x6A, 0x00, 0x61, 0x00, 0x7A, 0x01, 0x44, 0x01,
};

static const char kAscii[] = "The quick brown fox";

/* Builds the same IPv4/UDP/TMS framing around @p text (ASCII, widened to UCS-2 LE) preceded by
 * the blank subject line. Returns the packet length. */
static size_t
build_ascii_packet(uint8_t* out, size_t out_size, const char* text) {
    size_t text_units = strlen(text);
    size_t tms_payload = 2U + 1U + 1U + 2U + 4U + (text_units * 2U); /* len, hdr, adl, ext, CR LF, text */
    size_t udp_len = 8U + tms_payload;
    size_t total = 20U + udp_len;
    assert(total <= out_size);
    DSD_MEMSET(out, 0, out_size);
    DSD_MEMCPY(out, kPangramPacket, 20U);
    out[2] = (uint8_t)(total >> 8);
    out[3] = (uint8_t)(total & 0xFFU);
    out[20] = 0x0F;
    out[21] = 0xA7;
    out[22] = 0x0F;
    out[23] = 0xA7;
    out[24] = (uint8_t)(udp_len >> 8);
    out[25] = (uint8_t)(udp_len & 0xFFU);
    size_t p = 28U;
    size_t tms_len = tms_payload - 2U;
    out[p++] = (uint8_t)(tms_len >> 8);
    out[p++] = (uint8_t)(tms_len & 0xFFU);
    out[p++] = 0xA0;
    out[p++] = 0x00;
    out[p++] = 0x8F;
    out[p++] = 0x04;
    out[p++] = 0x0D;
    out[p++] = 0x00;
    out[p++] = 0x0A;
    out[p++] = 0x00;
    for (size_t i = 0; i < text_units; i++) {
        out[p++] = (uint8_t)text[i];
        out[p++] = 0x00;
    }
    assert(p == total);
    return total;
}

static void
run_packet(dsd_opts* opts, dsd_state* st, const uint8_t* packet, size_t packet_len, char* buf, size_t buf_size,
           const char* prefix) {
    uint8_t copy[512];
    assert(packet_len <= sizeof copy);
    DSD_MEMCPY(copy, packet, packet_len);
    st->event_history_s[0].Event_History_Items[0].text_message[0] = '\0';
    st->dmr_lrrp_gps[0][0] = '\0';
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, prefix) == 0);
    decode_ip_pdu(opts, st, (uint16_t)packet_len, copy);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, buf, buf_size) == 0);
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    st.event_history_s = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    assert(st.event_history_s != NULL);

    char buf[1024];
    const char* text_message = st.event_history_s[0].Event_History_Items[0].text_message;

    /* 1. The reporter's vector: every accented character survives, including the last one. */
    run_packet(&opts, &st, kPangramPacket, sizeof kPangramPacket, buf, sizeof buf, "dmr_tms_le");
    if (strcmp(text_message, kPangramUtf8) != 0) {
        DSD_FPRINTF(stderr, "event text: '%s'\nstderr: %s\n", text_message, buf);
        assert(0 && "TMS event-log text was not the pangram");
    }
    char expected_line[128];
    DSD_SNPRINTF(expected_line, sizeof expected_line, "Text: --%s", kPangramUtf8);
    if (strstr(buf, expected_line) == NULL) {
        DSD_FPRINTF(stderr, "stderr: %s\n", buf);
        assert(0 && "TMS stderr text was not the pangram");
    }
    assert(strstr(st.dmr_lrrp_gps[0], "TMS SRC: 1; DST: 2;") != NULL);

    /* 2. ASCII in the same framing still decodes verbatim. */
    uint8_t packet[256];
    size_t packet_len = build_ascii_packet(packet, sizeof packet, kAscii);
    run_packet(&opts, &st, packet, packet_len, buf, sizeof buf, "dmr_tms_le");
    if (strcmp(text_message, kAscii) != 0) {
        DSD_FPRINTF(stderr, "event text: '%s'\nstderr: %s\n", text_message, buf);
        assert(0 && "ASCII TMS text changed");
    }
    DSD_SNPRINTF(expected_line, sizeof expected_line, "Text: --%s", kAscii);
    assert(strstr(buf, expected_line) != NULL);

    /* 3. A header-extension octet that promises another, at the end of the payload. */
    (void)build_ascii_packet(packet, sizeof packet, kAscii);
    size_t cut = 28U + 5U; /* keep len, hdr, adl, 8F */
    packet[2] = (uint8_t)(cut >> 8);
    packet[3] = (uint8_t)(cut & 0xFFU);
    packet[24] = 0;
    packet[25] = (uint8_t)(cut - 20U);
    run_packet(&opts, &st, packet, cut, buf, sizeof buf, "dmr_tms_le");
    assert(text_message[0] == '\0');
    assert(strstr(st.dmr_lrrp_gps[0], "Truncated") != NULL);

    /* 4. A TMS length larger than the UDP payload: the text is clamped, not over-read. */
    packet_len = build_ascii_packet(packet, sizeof packet, kAscii);
    packet[28] = 0x01;
    packet[29] = 0x00;
    run_packet(&opts, &st, packet, packet_len, buf, sizeof buf, "dmr_tms_le");
    assert(strcmp(text_message, kAscii) == 0);

    /* 5. An address-length octet that runs past the payload is reported, not followed. */
    packet_len = build_ascii_packet(packet, sizeof packet, kAscii);
    packet[31] = 0xF0;
    run_packet(&opts, &st, packet, packet_len, buf, sizeof buf, "dmr_tms_le");
    assert(text_message[0] == '\0');
    assert(strstr(st.dmr_lrrp_gps[0], "Truncated") != NULL);

    free(st.event_history_s);
    DSD_FPRINTF(stderr, "DMR_TMS_UTF16LE: PASS\n");
    return 0;
}
