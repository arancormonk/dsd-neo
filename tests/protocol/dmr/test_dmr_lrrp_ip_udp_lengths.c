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

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static char g_datacall_text[512];

static void
reset_spies(void) {
    g_lip_calls = 0;
    g_datacall_calls = 0;
    g_datacall_src = 0;
    g_datacall_dst = 0;
    g_datacall_slot = 0;
    DSD_MEMSET(g_datacall_text, 0, sizeof(g_datacall_text));
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
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    (void)input;
    if (len > 0) {
        DSD_MEMSET(output, 0, (size_t)len);
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)input;
    g_lip_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

void
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* str, uint8_t slot) {
    (void)opts;
    (void)state;
    g_datacall_calls++;
    g_datacall_src = src;
    g_datacall_dst = dst;
    g_datacall_slot = slot;
    DSD_SNPRINTF(g_datacall_text, sizeof(g_datacall_text), "%s", str ? str : "");
}

void
getTimeC_buf(char out[9]) {
    DSD_SNPRINTF(out, 9, "%s", "11:22:33");
}

void
getDateS_buf(char out[11]) {
    DSD_SNPRINTF(out, 11, "%s", "1999/01/02");
}

// Under test
void decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input);
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
    out[1] = 0x34;                  // compressed IP ID
    out[2] = 0x12;                  // SAID=Ethernet, DAID=Group Network
    out[3] = (uint8_t)(0x80U | 1U); // opcode bit + SPID=UTF-16BE text
    out[4] = 63U;                   // DPID=reserved 7-bit index
    out[5] = 0x00;
    out[6] = 'O';
    out[7] = 0x00;
    out[8] = 'K';
    return 9U;
}

static size_t
build_compressed_udp_lip_with_extended_src_port(uint8_t* out, size_t cap) {
    if (cap < 10U) {
        return 0;
    }
    DSD_MEMSET(out, 0, cap);
    out[0] = 0x00;
    out[1] = 0x7B;
    out[2] = 0xB0; // SAID=manufacturer-specific, DAID=radio network
    out[3] = 0x00; // SPID extended at byte 5
    out[4] = 0x03; // DPID=reserved; SPID extension below selects LIP
    out[5] = 0x00;
    out[6] = 0x02; // SPID=Location Interface Protocol
    out[7] = 0xA5;
    out[8] = 0x5A;
    out[9] = 0xC3;
    return 10U;
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
    }

    // Case 2: IPv4 options present (IHL=6). Decoder must honor IHL to locate UDP.
    {
        reset_spies();
        size_t plen = build_ipv4_udp_lrrp(pkt, sizeof pkt, 6);
        st.dmr_lrrp_gps[0][0] = '\0';
        decode_ip_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_nonempty(st.dmr_lrrp_gps[0], "ihl=6 decoded");
        rc |= expect_has_substr(st.dmr_lrrp_gps[0], " km/h 90", "ihl=6 has speed+heading");
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
        utf8_to_text(&st, 1, (uint16_t)sizeof text, text);
        if (strcmp(st.event_history_s[0].Event_History_Items[0].text_message, "ABC") != 0) {
            DSD_FPRINTF(stderr, "utf8 text append: got '%s'\n",
                        st.event_history_s[0].Event_History_Items[0].text_message);
            rc |= 1;
        }
    }

    // Case 7: compressed UDP text dispatch resolves index labels, UTF-16 text, and datacall metadata.
    {
        reset_spies();
        size_t plen = build_compressed_udp_utf16_text(pkt, sizeof pkt);
        st.currentslot = 1;
        st.event_history_s[1].Event_History_Items[0].text_message[0] = '\0';
        dmr_udp_comp_pdu(&opts, &st, (uint16_t)plen, pkt);
        rc |= expect_has_substr(st.event_history_s[1].Event_History_Items[0].text_message, "OK",
                                "compressed text payload");
        if (g_datacall_calls != 1U || g_datacall_src != 1U || g_datacall_dst != 2U || g_datacall_slot != 1U) {
            DSD_FPRINTF(stderr, "compressed datacall metadata mismatch calls=%u src=%u dst=%u slot=%u\n",
                        g_datacall_calls, g_datacall_src, g_datacall_dst, g_datacall_slot);
            rc |= 1;
        }
        rc |= expect_has_substr(g_datacall_text, "SRC: 1:1", "compressed source summary");
        rc |= expect_has_substr(g_datacall_text, "DST: 2:63", "compressed destination summary");
    }

    // Case 8: compressed UDP with an extended source port should dispatch bounded LIP bits once.
    {
        reset_spies();
        size_t plen = build_compressed_udp_lip_with_extended_src_port(pkt, sizeof pkt);
        st.currentslot = 0;
        dmr_udp_comp_pdu(&opts, &st, (uint16_t)plen, pkt);
        if (g_lip_calls != 1U) {
            DSD_FPRINTF(stderr, "compressed LIP dispatch count mismatch: %u\n", g_lip_calls);
            rc |= 1;
        }
        rc |= expect_has_substr(g_datacall_text, "SRC: 11:2", "compressed extended source summary");
    }

    // Case 9: compressed UDP guards short/null PDUs without emitting datacalls.
    {
        reset_spies();
        dmr_udp_comp_pdu(&opts, &st, 4, pkt);
        dmr_udp_comp_pdu(&opts, &st, 5, NULL);
        if (g_datacall_calls != 0U || g_lip_calls != 0U) {
            DSD_FPRINTF(stderr, "compressed short guard emitted calls=%u lip=%u\n", g_datacall_calls, g_lip_calls);
            rc |= 1;
        }
    }

    // Case 10: generic short data emits source/target datacall metadata without requiring LOCN parsing.
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

    // Case 11: UDP application service ports update GPS/event summaries by service kind.
    {
        const struct {
            uint16_t port;
            const char* tag;
        } cases[] = {
            {4005U, "ARS SRC:"},        {4008U, "Telemetry SRC:"}, {4009U, "OTAP SRC:"},
            {4012U, "Batt. Man. SRC:"}, {4013U, "JTS SRC:"},       {4069U, "SCADA SRC:"},
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
        }
    }

    // Case 12: UDP/4007 TMS acknowledgment and UTF-16 text take distinct state paths.
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

    // Case 13: unknown UDP and truncated UDP headers emit bounded datacall summaries.
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
    }

    // Case 14: ICMP destination-unreachable with an attached IPv4 message recursively decodes the attachment.
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

    // Case 15: malformed TMS address length is bounded and reported as truncated.
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
