// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression: IP/UDP LRRP decoding should use IPv4 IHL and UDP length fields to
 * locate and bound the UDP payload, matching SDRTrunk.
 *
 * Historically we assumed a fixed 20-byte IPv4 header (offset +28 to UDP
 * payload) and applied hard-coded length trimming, which can truncate tokens
 * (eg SPEED/HEADING) or fail when IPv4 options are present.
 */

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

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static unsigned int g_lip_calls;
static unsigned int g_datacall_calls;
static uint32_t g_datacall_src;
static uint32_t g_datacall_dst;
static uint8_t g_datacall_slot;
static dsd_event_category g_datacall_category;
static char g_datacall_text[512];
static char g_datacall_gps[256];

static void
reset_spies(void) {
    g_lip_calls = 0;
    g_datacall_calls = 0;
    g_datacall_src = 0;
    g_datacall_dst = 0;
    g_datacall_slot = 0;
    g_datacall_category = DSD_EVENT_CATEGORY_UNKNOWN;
    DSD_MEMSET(g_datacall_text, 0, sizeof(g_datacall_text));
    DSD_MEMSET(g_datacall_gps, 0, sizeof(g_datacall_gps));
}

// Minimal stubs for direct link with dmr_pdu.c
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_unicode_supported(void) {
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input) {
    (void)opts;
    (void)input;
    g_lip_calls++;
    uint8_t slot = (state->currentslot == 1) ? 1U : 0U;
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof(state->dmr_embedded_gps[slot]), "%s",
                 "LIP: 41.500000, -87.250000");
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
dsd_event_emit_data_notice_classified(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                      const dsd_call_observation* observation, dsd_event_category category,
                                      const char* notice) {
    (void)opts;
    (void)state;
    g_datacall_calls++;
    g_datacall_src = observation->ota_source_id;
    g_datacall_dst = observation->ota_target_id;
    g_datacall_slot = slot;
    g_datacall_category = category;
    DSD_SNPRINTF(g_datacall_text, sizeof(g_datacall_text), "%s", notice ? notice : "");
    return 0;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    return dsd_event_emit_data_notice_classified(opts, state, slot, observation, DSD_EVENT_CATEGORY_DATA, notice);
}

int
dsd_event_emit_data_notice_classified_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                               const dsd_call_observation* observation, dsd_event_category category,
                                               const char* notice, const char* gps) {
    (void)dsd_event_emit_data_notice_classified(opts, state, slot, observation, category, notice);
    DSD_SNPRINTF(g_datacall_gps, sizeof(g_datacall_gps), "%s", gps ? gps : "");
    return 0;
}

int
dsd_event_emit_data_notice_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                    const dsd_call_observation* observation, const char* notice, const char* gps) {
    return dsd_event_emit_data_notice_classified_with_gps(opts, state, slot, observation, DSD_EVENT_CATEGORY_DATA,
                                                          notice, gps);
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_SLASH) ? "1999/01/02" : "11:22:33";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

// Under test
int decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input);
void dmr_sd_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU);
void dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU);
void utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input);

static int
expect_has_substr(const char* buf, const char* needle, const char* tag) {
    if (!buf || !strstr(buf, needle)) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, buf ? buf : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_nonempty(const char* buf, const char* tag) {
    if (!buf || buf[0] == '\0') {
        DSD_FPRINTF(stderr, "%s: empty output\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_category(dsd_event_category got, dsd_event_category want, const char* tag) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: category got %d want %d\n", tag, (int)got, (int)want);
        return 1;
    }
    return 0;
}

static size_t
build_ipv4_udp_lrrp(uint8_t* out, size_t cap, uint8_t ihl_words) {
    DSD_MEMSET(out, 0, cap);

    const size_t ip_header_len = (size_t)ihl_words * 4u;
    const size_t lrrp_len = 16u;          // LRRP header (2) + token stream (14)
    const size_t udp_len = 8u + lrrp_len; // UDP header + payload
    const size_t ip_total_len = ip_header_len + udp_len;

    if (cap < ip_total_len || ihl_words < 5) {
        return 0;
    }

    // IPv4 header
    out[0] = (uint8_t)((4u << 4) | (ihl_words & 0x0Fu)); // Version + IHL
    out[1] = 0x00;                                       // TOS
    out[2] = (uint8_t)((ip_total_len >> 8) & 0xFFu);
    out[3] = (uint8_t)(ip_total_len & 0xFFu);
    out[8] = 0x40; // TTL
    out[9] = 0x11; // UDP

    // Src IP 1.2.3.4 / Dst IP 5.6.7.8
    out[12] = 1;
    out[13] = 2;
    out[14] = 3;
    out[15] = 4;
    out[16] = 5;
    out[17] = 6;
    out[18] = 7;
    out[19] = 8;

    // IPv4 options, if any (zero-filled)
    for (size_t i = 20; i < ip_header_len; i++) {
        out[i] = 0x00;
    }

    // UDP header
    const size_t udp_off = ip_header_len;
    const uint16_t port = 4001;
    out[udp_off + 0] = (uint8_t)((port >> 8) & 0xFFu);
    out[udp_off + 1] = (uint8_t)(port & 0xFFu);
    out[udp_off + 2] = (uint8_t)((port >> 8) & 0xFFu);
    out[udp_off + 3] = (uint8_t)(port & 0xFFu);
    out[udp_off + 4] = (uint8_t)((udp_len >> 8) & 0xFFu);
    out[udp_off + 5] = (uint8_t)(udp_len & 0xFFu);
    out[udp_off + 6] = 0x00; // checksum
    out[udp_off + 7] = 0x00;

    // UDP payload (LRRP)
    size_t p = udp_off + 8u;
    out[p++] = 0x07; // Immediate Location Response
    out[p++] = 14;   // token stream length (bytes)

    // 0x66 POINT_2D: lat/lon (big-endian)
    out[p++] = 0x66;
    out[p++] = 0x10;
    out[p++] = 0x00;
    out[p++] = 0x00;
    out[p++] = 0x00; // lat = 0x10000000
    out[p++] = 0x20;
    out[p++] = 0x00;
    out[p++] = 0x00;
    out[p++] = 0x00; // lon = 0x20000000

    // 0x6C SPEED: raw units 1/100 mph -> 10.00 mph (0x03E8)
    out[p++] = 0x6C;
    out[p++] = 0x03;
    out[p++] = 0xE8;

    // 0x56 HEADING: 2-degree increments -> 90 degrees (45)
    out[p++] = 0x56;
    out[p++] = 0x2D;

    return ip_total_len;
}

static size_t
build_ipv4_udp_vertex_tms(uint8_t* out, size_t cap, uint8_t ihl_words) {
    DSD_MEMSET(out, 0, cap);

    const size_t ip_header_len = (size_t)ihl_words * 4u;
    const size_t vtx_hdr_len = 21u;
    const size_t utf16_text_len = 4u; // "HI"
    const size_t udp_payload_len = vtx_hdr_len + utf16_text_len;
    const size_t udp_len = 8u + udp_payload_len;
    const size_t ip_total_len = ip_header_len + udp_len;

    if (cap < ip_total_len || ihl_words < 5) {
        return 0;
    }

    // IPv4 header
    out[0] = (uint8_t)((4u << 4) | (ihl_words & 0x0Fu)); // Version + IHL
    out[1] = 0x00;                                       // TOS
    out[2] = (uint8_t)((ip_total_len >> 8) & 0xFFu);
    out[3] = (uint8_t)(ip_total_len & 0xFFu);
    out[8] = 0x40; // TTL
    out[9] = 0x11; // UDP

    // Src IP 1.2.3.4 / Dst IP 5.6.7.8
    out[12] = 1;
    out[13] = 2;
    out[14] = 3;
    out[15] = 4;
    out[16] = 5;
    out[17] = 6;
    out[18] = 7;
    out[19] = 8;

    for (size_t i = 20; i < ip_header_len; i++) {
        out[i] = 0x00;
    }

    // UDP header
    const size_t udp_off = ip_header_len;
    const uint16_t port = 5007;
    out[udp_off + 0] = (uint8_t)((port >> 8) & 0xFFu);
    out[udp_off + 1] = (uint8_t)(port & 0xFFu);
    out[udp_off + 2] = (uint8_t)((port >> 8) & 0xFFu);
    out[udp_off + 3] = (uint8_t)(port & 0xFFu);
    out[udp_off + 4] = (uint8_t)((udp_len >> 8) & 0xFFu);
    out[udp_off + 5] = (uint8_t)(udp_len & 0xFFu);
    out[udp_off + 6] = 0x00;
    out[udp_off + 7] = 0x00;

    // UDP payload: 21-byte vendor header + UTF-16BE text.
    size_t p = udp_off + 8u;
    const uint8_t vtx_hdr[21] = {
        0x0E, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // observed fixed prefix
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             // reserved/unknown
        0x00, 0x00, 0x00, 0x00, 0x00                          // reserved/unknown
    };
    DSD_MEMCPY(out + p, vtx_hdr, sizeof(vtx_hdr));
    p += sizeof(vtx_hdr);

    out[p++] = 0x00;
    out[p++] = 'H';
    out[p++] = 0x00;
    out[p++] = 'I';

    return ip_total_len;
}

static size_t
build_ipv4_udp_empty_payload(uint8_t* out, size_t cap, uint16_t dst_port) {
    DSD_MEMSET(out, 0, cap);

    const size_t ip_header_len = 20u;
    const size_t udp_len = 8u;
    const size_t ip_total_len = ip_header_len + udp_len;
    if (cap < ip_total_len) {
        return 0;
    }

    out[0] = (uint8_t)((4u << 4) | 5u);
    out[2] = (uint8_t)((ip_total_len >> 8) & 0xFFu);
    out[3] = (uint8_t)(ip_total_len & 0xFFu);
    out[8] = 0x40;
    out[9] = 0x11;

    out[12] = 1;
    out[13] = 2;
    out[14] = 3;
    out[15] = 4;
    out[16] = 5;
    out[17] = 6;
    out[18] = 7;
    out[19] = 8;

    const size_t udp_off = ip_header_len;
    out[udp_off + 0] = 0x30;
    out[udp_off + 1] = 0x39;
    out[udp_off + 2] = (uint8_t)((dst_port >> 8) & 0xFFu);
    out[udp_off + 3] = (uint8_t)(dst_port & 0xFFu);
    out[udp_off + 4] = (uint8_t)((udp_len >> 8) & 0xFFu);
    out[udp_off + 5] = (uint8_t)(udp_len & 0xFFu);

    return ip_total_len;
}

static size_t
build_ipv4_udp_payload(uint8_t* out, size_t cap, uint16_t dst_port, const uint8_t* payload, size_t payload_len) {
    DSD_MEMSET(out, 0, cap);

    const size_t ip_header_len = 20u;
    const size_t udp_len = 8u + payload_len;
    const size_t ip_total_len = ip_header_len + udp_len;
    if (cap < ip_total_len || udp_len > UINT16_MAX) {
        return 0;
    }

    out[0] = (uint8_t)((4u << 4) | 5u);
    out[2] = (uint8_t)((ip_total_len >> 8) & 0xFFu);
    out[3] = (uint8_t)(ip_total_len & 0xFFu);
    out[8] = 0x40;
    out[9] = 0x11;

    out[12] = 1;
    out[13] = 2;
    out[14] = 3;
    out[15] = 4;
    out[16] = 5;
    out[17] = 6;
    out[18] = 7;
    out[19] = 8;

    const size_t udp_off = ip_header_len;
    out[udp_off + 0] = 0x30;
    out[udp_off + 1] = 0x39;
    out[udp_off + 2] = (uint8_t)((dst_port >> 8) & 0xFFu);
    out[udp_off + 3] = (uint8_t)(dst_port & 0xFFu);
    out[udp_off + 4] = (uint8_t)((udp_len >> 8) & 0xFFu);
    out[udp_off + 5] = (uint8_t)(udp_len & 0xFFu);
    if (payload_len != 0U && payload != NULL) {
        DSD_MEMCPY(out + udp_off + 8u, payload, payload_len);
    }

    return ip_total_len;
}

static size_t
build_ipv4_truncated_udp_header(uint8_t* out, size_t cap) {
    DSD_MEMSET(out, 0, cap);
    const size_t ip_total_len = 24u;
    if (cap < ip_total_len) {
        return 0;
    }
    out[0] = (uint8_t)((4u << 4) | 5u);
    out[2] = 0;
    out[3] = (uint8_t)ip_total_len;
    out[8] = 0x40;
    out[9] = 0x11;
    out[12] = 1;
    out[13] = 2;
    out[14] = 3;
    out[15] = 4;
    out[16] = 5;
    out[17] = 6;
    out[18] = 7;
    out[19] = 8;
    out[20] = 0x30;
    out[21] = 0x39;
    out[22] = 0x0F;
    out[23] = 0xA7;
    return ip_total_len;
}

static size_t
build_ipv4_icmp_attached_udp_service(uint8_t* out, size_t cap, uint16_t attached_port) {
    uint8_t attached[96];
    const uint8_t payload[] = {0xA5, 0x5A};
    size_t attached_len = build_ipv4_udp_payload(attached, sizeof attached, attached_port, payload, sizeof payload);
    const size_t ip_header_len = 20u;
    const size_t icmp_len = 8u;
    const size_t ip_total_len = ip_header_len + icmp_len + attached_len;
    if (attached_len == 0U || cap < ip_total_len || ip_total_len > UINT16_MAX) {
        return 0;
    }

    DSD_MEMSET(out, 0, cap);
    out[0] = (uint8_t)((4u << 4) | 5u);
    out[2] = (uint8_t)((ip_total_len >> 8) & 0xFFu);
    out[3] = (uint8_t)(ip_total_len & 0xFFu);
    out[8] = 0x40;
    out[9] = 0x01;
    out[12] = 9;
    out[13] = 8;
    out[14] = 7;
    out[15] = 6;
    out[16] = 5;
    out[17] = 4;
    out[18] = 3;
    out[19] = 2;

    const size_t icmp_off = ip_header_len;
    out[icmp_off + 0] = 0x03;
    out[icmp_off + 1] = 0x03;
    out[icmp_off + 2] = 0x12;
    out[icmp_off + 3] = 0x34;
    DSD_MEMCPY(out + icmp_off + icmp_len, attached, attached_len);
    return ip_total_len;
}

static size_t
build_compressed_udp_utf16_text(uint8_t* out, size_t cap) {
    if (cap < 9U) {
        return 0;
    }
    DSD_MEMSET(out, 0, cap);
    out[0] = 0x12;
    out[1] = 0x34; // compressed IP ID
    out[2] = 0x12; // SAID=Ethernet, DAID=Group Network
    out[3] = 1U;   // opcode MSB clear (table 7.19 defines 00 only) + SPID=UTF-16BE text
    out[4] = 63U;  // DPID=reserved 7-bit index
    out[5] = 0x00;
    out[6] = 'O';
    out[7] = 0x00;
    out[8] = 'K';
    return 9U;
}

static size_t
build_compressed_udp_extended_port(uint8_t* out, size_t cap, uint16_t port, uint8_t extended_source, uint8_t peer_pid,
                                   uint8_t include_payload) {
    const size_t len = include_payload ? 8U : 7U;
    if (cap < len || peer_pid == 0U || peer_pid > 0x7FU) {
        return 0;
    }
    DSD_MEMSET(out, 0, cap);
    out[0] = 0x00;
    out[1] = 0x7C;
    out[2] = 0x10;
    out[3] = extended_source ? 0U : peer_pid;
    out[4] = extended_source ? peer_pid : 0U;
    out[5] = (uint8_t)(port >> 8);
    out[6] = (uint8_t)(port & 0xFFU);
    if (include_payload) {
        out[7] = 0xA5U;
    }
    return len;
}

int
main(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    opts.lrrp_file_output = 0;

    st.event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    if (!st.event_history_s) {
        return 100;
    }

    uint8_t pkt[128];

    // Case 1: standard IPv4 header (IHL=5). Ensure SPEED/HEADING are not truncated.
    {
        reset_spies();
        size_t plen = build_ipv4_udp_lrrp(pkt, sizeof pkt, 5);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_nonempty(st.dmr_lrrp_gps[0], "ihl=5 decoded");
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], " km/h 90", "ihl=5 has speed+heading");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, "udp4001 LRRP category");
    }

    // Case 2: IPv4 options present (IHL=6). Decoder must honor IHL to locate UDP.
    {
        reset_spies();
        size_t plen = build_ipv4_udp_lrrp(pkt, sizeof pkt, 6);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_nonempty(st.dmr_lrrp_gps[0], "ihl=6 decoded");
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], " km/h 90", "ihl=6 has speed+heading");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, "udp4001 options category");
    }

    // Case 3: Vertex TMS on UDP/5007 should not trim valid text when data_block_poc is non-zero.
    {
        reset_spies();
        size_t plen = build_ipv4_udp_vertex_tms(pkt, sizeof pkt, 5);
        st.data_block_poc[0] = 2; // non-zero from RF block framing; not part of UDP payload length
        st.dmr_lrrp_gps[0][0] = '\0';
        st.event_history_s[0].Event_History_Items[0].text_message[0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "VTX TMS SRC:", "vtx5007 label");
        rc |= expect_has_substr(st.event_history_s[0].Event_History_Items[0].text_message, "HI", "vtx5007 text");
    }

    // Case 4: EF Johnson Atlas Data Registration Server on UDP/9361 should be labeled.
    {
        reset_spies();
        size_t plen = build_ipv4_udp_empty_payload(pkt, sizeof pkt, 9361);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "P25 Atlas SRC(IP): 1.2.3.4; DST(IP): 5.6.7.8;", "atlas9361 label");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, "atlas9361 category");

        reset_spies();
        plen = build_ipv4_udp_empty_payload(pkt, sizeof pkt, 65000U);
        pkt[20] = (uint8_t)(9361U >> 8);
        pkt[21] = (uint8_t)(9361U & 0xFFU);
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, "atlas9361 source category");
    }

    // Shared P25 Tier 2 location service remains packet data.
    {
        reset_spies();
        size_t plen = build_ipv4_udp_empty_payload(pkt, sizeof pkt, 49198);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "P25 Tier 2 LOCN SRC(IP):", "location49198 label");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, "location49198 category");
    }

    // Case 5: Short/empty UDP TMS payload should be reported as truncated, not indexed past the payload.
    {
        reset_spies();
        size_t plen = build_ipv4_udp_empty_payload(pkt, sizeof pkt, 4007);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "Truncated;", "tms4007 short payload");
    }

    // Case 6: UTF-8 event text appends one bounded character at a time.
    {
        reset_spies();
        const uint8_t text[] = {'A', 'B', 'C'};
        st.event_history_s[0].Event_History_Items[0].text_message[0] = '\0';
        const uint64_t revision = st.event_history_s[0].revision;
        utf8_to_text(&st, 1, (uint16_t)sizeof text, text);
        if (strcmp(st.event_history_s[0].Event_History_Items[0].text_message, "ABC") != 0) {
            DSD_FPRINTF(stderr, "utf8 text append: got '%s'\n",
                        st.event_history_s[0].Event_History_Items[0].text_message);
            rc |= 1;
        }
        if (st.event_history_s[0].revision != revision + 1U) {
            DSD_FPRINTF(stderr, "utf8 text append did not advance history revision once\n");
            rc |= 1;
        }
    }

    // Case 7: compressed UDP text dispatch resolves index labels, UTF-16 text, and datacall metadata.
    {
        reset_spies();
        size_t plen = build_compressed_udp_utf16_text(pkt, sizeof pkt);
        st.currentslot = 1;
        st.dmr_lrrp_source[1] = 4321;
        st.dmr_lrrp_target[1] = 8765;
        st.event_history_s[1].Event_History_Items[0].text_message[0] = '\0';
        dmr_udp_comp_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.event_history_s[1].Event_History_Items[0].text_message, "OK",
                                "compressed text payload");
        if (g_datacall_calls != 1U || g_datacall_src != 4321U || g_datacall_dst != 8765U || g_datacall_slot != 1U) {
            DSD_FPRINTF(stderr, "compressed datacall metadata mismatch calls=%u src=%u dst=%u slot=%u\n",
                        g_datacall_calls, g_datacall_src, g_datacall_dst, g_datacall_slot);
            rc |= 1;
        }
        rc |= expect_has_substr(g_datacall_text, "SRC: 1:1", "compressed source summary");
        rc |= expect_has_substr(g_datacall_text, "DST: 2:63", "compressed destination summary");
        // ETSI TS 102 361-3 Table 7.14 names the leading 16 bits "IPv4 Identification"; the
        // notice used to call them "IPC", which read like one of the compression index fields.
        rc |= expect_has_substr(g_datacall_text, "IP ID: 1234;", "compressed IPv4 identification label");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, "compressed text category");
    }

    // Case 8 (extended source port selects LIP) moved to test_dmr_udp_comp_header.c, which
    // pins the ETSI port tables directly (#450).

    // Case 9: compressed UDP classifies supported control services at either endpoint.
    {
        static const struct {
            uint16_t port;
            dsd_event_category category;
        } cases[] = {
            {4004U, DSD_EVENT_CATEGORY_CONTROL}, {4005U, DSD_EVENT_CATEGORY_CONTROL},
            {4009U, DSD_EVENT_CATEGORY_CONTROL}, {9361U, DSD_EVENT_CATEGORY_CONTROL},
            {4008U, DSD_EVENT_CATEGORY_DATA},
        };

        for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
            for (uint8_t extended_source = 0U; extended_source <= 1U; extended_source++) {
                reset_spies();
                size_t plen =
                    build_compressed_udp_extended_port(pkt, sizeof pkt, cases[i].port, extended_source, 63U, 0U);
                st.currentslot = 0;
                dmr_udp_comp_pdu(&opts, &st, (uint16_t)plen, pkt);
                rc |= expect_category(g_datacall_category, cases[i].category,
                                      extended_source ? "compressed source service category"
                                                      : "compressed destination service category");
            }
        }
    }

    // Case 10: classified compressed UDP GPS preserves the endpoint-derived category.
    {
        reset_spies();
        size_t plen = build_compressed_udp_extended_port(pkt, sizeof pkt, 4005U, 1U, 2U, 1U);
        st.currentslot = 0;
        dmr_udp_comp_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, "compressed control GPS category");
        rc |= expect_has_substr(g_datacall_gps, "41.500000", "compressed control GPS payload");
    }

    // Case 11: compressed UDP guards short/null PDUs without emitting datacalls.
    {
        reset_spies();
        dmr_udp_comp_pdu(&opts, &st, 4, pkt);
        dmr_udp_comp_pdu(&opts, &st, 5, NULL);
        if (g_datacall_calls != 0U || g_lip_calls != 0U) {
            DSD_FPRINTF(stderr, "compressed short guard emitted calls=%u lip=%u\n", g_datacall_calls, g_lip_calls);
            rc |= 1;
        }
    }

    // Case 12: generic short data emits source/target datacall metadata without requiring LOCN parsing.
    {
        reset_spies();
        const uint8_t text[] = {'H', 'E', 'L', 'L', 'O'};
        st.currentslot = 0;
        st.data_header_format[0] = 0;
        st.dmr_lrrp_source[0] = 1234;
        st.dmr_lrrp_target[0] = 5678;
        dmr_sd_pdu(&opts, &st, (uint16_t)sizeof text, text);
        if (g_datacall_calls != 1U || g_datacall_src != 1234U || g_datacall_dst != 5678U) {
            DSD_FPRINTF(stderr, "short data datacall mismatch calls=%u src=%u dst=%u\n", g_datacall_calls,
                        g_datacall_src, g_datacall_dst);
            rc |= 1;
        }
        rc |= expect_has_substr(g_datacall_text, "Short Data SRC: 1234; TGT: 5678;", "short data summary");
    }

    // Case 13: UDP application service ports classify either endpoint by service kind.
    {
        const struct {
            const char* tag;
            uint16_t port;
            dsd_event_category category;
        } cases[] = {
            {"XCMP SRC:", 4004U, DSD_EVENT_CATEGORY_CONTROL},    {"ARS SRC:", 4005U, DSD_EVENT_CATEGORY_CONTROL},
            {"Telemetry SRC:", 4008U, DSD_EVENT_CATEGORY_DATA},  {"OTAP SRC:", 4009U, DSD_EVENT_CATEGORY_CONTROL},
            {"Batt. Man. SRC:", 4012U, DSD_EVENT_CATEGORY_DATA}, {"JTS SRC:", 4013U, DSD_EVENT_CATEGORY_DATA},
            {"SCADA SRC:", 4069U, DSD_EVENT_CATEGORY_DATA},
        };

        const uint8_t ars_payload[] = {'A', 'R', 'S', 0};
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            reset_spies();
            const uint8_t* payload = (cases[i].port == 4005U) ? ars_payload : NULL;
            size_t payload_len = (cases[i].port == 4005U) ? sizeof(ars_payload) : 0U;
            size_t plen = build_ipv4_udp_payload(pkt, sizeof pkt, cases[i].port, payload, payload_len);
            st.currentslot = 0;
            st.dmr_lrrp_gps[0][0] = '\0';
            decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
            rc |= expect_has_substr(st.dmr_lrrp_gps[0], cases[i].tag, "udp service label");
            rc |= expect_category(g_datacall_category, cases[i].category, "udp service category");

            reset_spies();
            plen = build_ipv4_udp_payload(pkt, sizeof pkt, 65000U, NULL, 0U);
            pkt[20] = (uint8_t)(cases[i].port >> 8);
            pkt[21] = (uint8_t)(cases[i].port & 0xFFU);
            st.dmr_lrrp_gps[0][0] = '\0';
            decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
            rc |= expect_category(g_datacall_category, cases[i].category, "udp source service category");
        }
    }

    // Case 13b: UDP/4005 ARS device registration honours the record length and reports the
    // registered device identifier instead of dumping the record as raw text (issue #337).
    {
        reset_spies();
        // The four octets after the declared 9 byte record stand in for the block trailer the old
        // fixed window used to run into; the decode must stop at the record length, not payload_len.
        const uint8_t ars_reg[] = {0x00, 0x09, 0xF0, 0x20, 0x04, '1', '2', '3', '4', 0x00, 0x00, 0x5E, 0x6C, 0xA7};
        size_t plen = build_ipv4_udp_payload(pkt, sizeof pkt, 4005U, ars_reg, sizeof(ars_reg));
        st.currentslot = 0;
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "ARS Reg: 1234; Initial;", "ars registration device id");
    }

    // Case 14: UDP/4007 TMS acknowledgment and UTF-16 text take distinct state paths.
    {
        reset_spies();
        const uint8_t ack_payload[] = {0x00, 0x05, 0x01, 0x00, 0x00};
        size_t plen = build_ipv4_udp_payload(pkt, sizeof pkt, 4007U, ack_payload, sizeof(ack_payload));
        st.currentslot = 0;
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "Acknowledgment;", "tms acknowledgment");

        reset_spies();
        const uint8_t text_payload[] = {0x00, 0x06, 0x00, 0x00, 'O', 0x00, 'K'};
        plen = build_ipv4_udp_payload(pkt, sizeof pkt, 4007U, text_payload, sizeof(text_payload));
        st.event_history_s[0].Event_History_Items[0].text_message[0] = '\0';
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "TMS SRC:", "tms text label");
        rc |= expect_has_substr(st.event_history_s[0].Event_History_Items[0].text_message, "OK", "tms text payload");
    }

    // Case 15: unknown UDP and truncated UDP headers emit bounded datacall summaries.
    {
        reset_spies();
        size_t plen = build_ipv4_udp_payload(pkt, sizeof pkt, 65000U, NULL, 0U);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "Unknown UDP Port;", "unknown udp label");
        rc |= expect_has_substr(g_datacall_text, "Unknown UDP Port;", "unknown udp datacall");

        reset_spies();
        plen = build_ipv4_truncated_udp_header(pkt, sizeof pkt);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "Truncated UDP;", "truncated udp label");
        rc |= expect_has_substr(g_datacall_text, "Truncated UDP;", "truncated udp datacall");
        if (g_datacall_calls != 1U) {
            DSD_FPRINTF(stderr, "truncated udp datacall count mismatch: %u\n", g_datacall_calls);
            rc |= 1;
        }
    }

    // Case 16: ICMP destination-unreachable with an attached IPv4 message recursively decodes the attachment.
    {
        reset_spies();
        size_t plen = build_ipv4_icmp_attached_udp_service(pkt, sizeof pkt, 4008U);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "Telemetry SRC:", "icmp attached telemetry label");
        rc |= expect_has_substr(g_datacall_text, "Telemetry SRC:", "icmp attached telemetry datacall");
        if (g_datacall_calls != 2U) {
            DSD_FPRINTF(stderr, "icmp attached datacall count mismatch: %u\n", g_datacall_calls);
            rc |= 1;
        }
    }

    // Case 17: malformed TMS address length is bounded and reported as truncated.
    {
        reset_spies();
        const uint8_t malformed_addr_payload[] = {0x00, 0x08, 0x00, 0x04, 0x00};
        size_t plen =
            build_ipv4_udp_payload(pkt, sizeof pkt, 4007U, malformed_addr_payload, sizeof(malformed_addr_payload));
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], "Truncated;", "tms malformed address truncation");
        rc |= expect_has_substr(g_datacall_text, "Truncated;", "tms malformed address datacall");
    }

    free(st.event_history_s);
    st.event_history_s = NULL;

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
