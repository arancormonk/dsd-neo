// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * dmr_bs.c
 * DMR Data (1/2, 3/4, 1) PDU Decoding
 *
 * LWVMOBILE
 * 2022-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/udp_comp_ports.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_ars.h"
#include "dmr_pdu_internal.h"
#include "dmr_text.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

static inline void dsd_append(char* dst, size_t dstsz, const char* src);

static void
utf16_text_emit_scalar(uint32_t scalar) {
    if (!dsd_unicode_scalar_is_control(scalar)) { // If not a linebreak or terminal commmands
        dsd_unicode_fput_scalar(scalar, stderr);
    } else if (scalar == 0U) { // If padding (0 could also indicate end of text terminator?)
        DSD_FPRINTF(stderr, "_");
    } else {
        DSD_FPRINTF(stderr, "-");
    }
}

// The event text is rendered as UTF-8 by the ncurses UI, the Qt model and the event log, so the
// scalar goes in encoded rather than truncated to its low octet (issue #466). Control characters,
// padding included, stay out: the log prints one line per event.
static void
utf16_text_collect_scalar(dsd_state* state, uint8_t slot, uint32_t scalar) {
    if (dsd_unicode_scalar_is_control(scalar)) {
        return;
    }
    char utf8[DSD_UTF8_MAX_BYTES + 1];
    if (dsd_utf8_encode_scalar(scalar, utf8, sizeof utf8) > 0U) {
        dsd_event_stage_text_append(state, slot, utf8);
    }
}

static void
utf16_text_render(dsd_state* state, uint8_t wr, int little_endian, uint16_t len, const uint8_t* input) {
    uint8_t slot = state->currentslot;
    if (wr == 1) {
        dsd_event_stage_text(state, slot, "");
    }
    // Pairs are combined and unpaired halves shown as U+FFFD before anything reaches stderr, so
    // the C runtime never sees a code unit it cannot encode (issue #358).
    dsd_utf16_decoder decoder;
    uint32_t scalars[DSD_UTF16_MAX_SCALARS_PER_UNIT];
    dsd_utf16_decoder_reset(&decoder);
    for (uint16_t i = 0; (uint16_t)(i + 1U) < len; i += 2) {
        uint16_t ch16;
        if (little_endian) {
            ch16 = (uint16_t)((uint16_t)input[i + 1] << 8 | (uint16_t)input[i + 0]);
        } else {
            ch16 = (uint16_t)((uint16_t)input[i + 0] << 8 | (uint16_t)input[i + 1]);
        }

        size_t n = dsd_utf16_decoder_push(&decoder, ch16, scalars, DSD_UTF16_MAX_SCALARS_PER_UNIT);
        for (size_t k = 0; k < n; k++) {
            utf16_text_emit_scalar(scalars[k]);
            if (wr == 1) {
                utf16_text_collect_scalar(state, slot, scalars[k]);
            }
        }
    }
    if (dsd_utf16_decoder_finish(&decoder, scalars, 1U) > 0U) {
        utf16_text_emit_scalar(scalars[0]);
        if (wr == 1) {
            utf16_text_collect_scalar(state, slot, scalars[0]);
        }
    }
}

void
utf16_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    utf16_text_render(state, wr, 0, len, input);
}

void
utf16le_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    utf16_text_render(state, wr, 1, len, input);
}

void
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    utf8_to_text_labelled(state, wr, len, input, "UTF8 Text");
}

void
utf8_to_text_labelled(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input, const char* label) {
    uint8_t slot = state->currentslot;
    DSD_FPRINTF(stderr, "\n %s: ", label != NULL ? label : "UTF8 Text");

    if (wr == 1) {
        dsd_event_stage_text(state, slot, "");
    }

    for (uint16_t i = 0; i < len; i++) {
        if (input[i] >= 0x20 && input[i] < 0x7F) { // If not a linebreak or terminal commmands
            DSD_FPRINTF(stderr, "%c", input[i]);
        } else if (input[i] == 0) { // If padding (0 could also indicate end of text terminator?)
            DSD_FPRINTF(stderr, "_");
        } else {
            DSD_FPRINTF(stderr, "-");
        }

        char c = input[i];

        // For now, just rip the first 40 or so chars lower byte value
        //in the ASCII Range (should be alright for a quick visual)

        //this is the long version, complete message for logging purposes
        if (wr == 1 && c < 0x7F && c >= 0x20) {
            const char c_str[2] = {c, '\0'};
            dsd_event_stage_text_append(state, slot, c_str);
        }
    }
}

static void
dmr_sd_pdu_store_text(dsd_state* state, uint8_t slot, const char* text) {
    dsd_event_stage_text(state, slot, text);
}

// A raw dump, like every payload dump, only with the payload option (-Z): its bytes can be a position (LOCN), which
// the default output and the diagnostics log never show.
static void
dmr_sd_pdu_print_raw(const dsd_opts* opts, const uint8_t* dmr_pdu, uint16_t len) {
    if (opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "\n Short Data Raw: ");
    for (uint16_t i = 0; i < len; i++) {
        DSD_FPRINTF(stderr, "[%02X]", dmr_pdu[i]);
    }
}

static void
dmr_sd_pdu_append_result_flags(char* summary, size_t summary_size, const dmr_text_result* result) {
    if (result->malformed) {
        dsd_append(summary, summary_size, "malformed input replaced; ");
    }
    if (result->truncated) {
        dsd_append(summary, summary_size, "display truncated; ");
    }
}

/* The whole octets of user data in @p len assembled octets less the header's @p bit_padding (TS 102 361-1 9.3.25):
 * an octet the pad bits reach is not the sender's, so a position parser never reads it. 0 when the padding is longer
 * than the data. */
static uint16_t
dmr_sd_pdu_whole_user_octets(uint16_t len, uint8_t bit_padding) {
    const size_t bits = (size_t)len * 8U;
    return (size_t)bit_padding > bits ? 0U : (uint16_t)((bits - (size_t)bit_padding) / 8U);
}

void
dmr_sd_pdu_process(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* dmr_pdu, uint8_t packet_crc_valid) {
    if (opts == NULL || state == NULL || (dmr_pdu == NULL && len != 0U)) {
        return;
    }

    uint8_t slot = (uint8_t)(state->currentslot & 1U);
    uint32_t source = state->dmr_lrrp_source[slot];
    uint32_t target = state->dmr_lrrp_target[slot];
    char summary[2300];
    DSD_MEMSET(summary, 0, sizeof(summary));
    DSD_SNPRINTF(summary, sizeof(summary), "Short Data SRC: %u; TGT: %u; ", source, target);
    dmr_sd_pdu_store_text(state, slot, "");

    if (state->data_header_format[slot] == 13U) {
        uint8_t dd_format = state->data_header_dd_format[slot];
        if (dd_format >= 0x12U && dd_format <= 0x18U) {
            size_t payload_len = 0U;
            dmr_text_result result;
            if (dmr_short_data_payload_bytes((size_t)len * 8U, state->data_header_bit_padding[slot], &payload_len)
                != 0) {
                DSD_FPRINTF(stderr, "\n Short Data Text: invalid bit padding (%u bits)",
                            state->data_header_bit_padding[slot]);
                char detail[96];
                DSD_SNPRINTF(detail, sizeof(detail), "declared %s; invalid/non-byte-aligned padding; ",
                             dmr_defined_data_encoding_name(dd_format));
                dsd_append(summary, sizeof(summary), detail);
            } else {
                (void)dmr_decode_defined_short_data(dd_format, dmr_pdu, payload_len, packet_crc_valid, &result);
                if (result.compatibility) {
                    DSD_FPRINTF(stderr, "\n Short Data Text (declared UTF-32; decoded UTF-16BE compatibility): %s",
                                result.text);
                } else {
                    DSD_FPRINTF(stderr, "\n Short Data Text (declared %s; decoded %s): %s", result.declared_encoding,
                                result.effective_encoding, result.text);
                }
                dmr_sd_pdu_store_text(state, slot, result.text);
                char detail[160];
                DSD_SNPRINTF(detail, sizeof(detail), "declared %s; decoded %s; ", result.declared_encoding,
                             result.effective_encoding);
                dsd_append(summary, sizeof(summary), detail);
                dmr_sd_pdu_append_result_flags(summary, sizeof(summary), &result);
                dsd_append(summary, sizeof(summary), "Text: ");
                dsd_append(summary, sizeof(summary), result.text);
                dsd_append(summary, sizeof(summary), "; ");
            }
        } else {
            char detail[96];
            DSD_SNPRINTF(detail, sizeof(detail), "DD format 0x%02X; raw/unsupported encoding; ", dd_format);
            dsd_append(summary, sizeof(summary), detail);
            dmr_sd_pdu_print_raw(opts, dmr_pdu, len);
            // An admitted LOCN position is staged for this packet's notice by dmr_locn() itself. It reads the user
            // data only: the header's bit padding fills the end of the last octet(s), and a digit it completes was
            // never sent.
            dmr_locn(opts, state, dmr_sd_pdu_whole_user_octets(len, state->data_header_bit_padding[slot]), dmr_pdu);
        }
    } else {
        uint16_t bounded_len = len;
        if (bounded_len >= (127U * 18U)) {
            bounded_len = 127U * 18U;
        }
        utf8_to_text(state, 0, bounded_len, dmr_pdu);
        dsd_append(summary, sizeof(summary), "raw short-data payload; ");
    }

    const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, slot, source, target);
    (void)dsd_event_emit_data_notice(opts, state, slot, &observation, summary);
}

// ETSI TS 102 361-3 V1.3.1 clause 7.2.4: SAID, DAID, SPID and DPID are indices into tables the
// radio holds (7.15-7.18), not compressed values. The labels below use those tables' wording.
static const char*
dmr_udp_comp_src_idx_desc(uint16_t said) {
    // Table 7.15: 0000 Radio Network, 0001 USB (Ethernet Interface) Network,
    // 0010-1011 Reserved, 1100-1111 Manufacturer Specific.
    if (said == 0) {
        return "Radio Network";
    }
    if (said == 1) {
        return "Ethernet";
    }
    if (said < 12) {
        return "Reserved";
    }
    return "Manufacturer Specific";
}

static const char*
dmr_udp_comp_dst_idx_desc(uint16_t daid) {
    // Table 7.16: as 7.15 plus 0010 Group Network; 0011-1011 Reserved, 1100-1111 Manufacturer Specific.
    if (daid == 0) {
        return "Radio Network";
    }
    if (daid == 1) {
        return "Ethernet";
    }
    if (daid == 2) {
        return "Group Network";
    }
    if (daid < 12) {
        return "Reserved";
    }
    return "Manufacturer Specific";
}

static const char*
dmr_udp_comp_port_idx_desc(uint16_t idx) {
    // Tables 7.17/7.18 (identical): 0000000 In Extended Header, 0000001 UTF-16BE Text Message
    // (UDP 5016), 0000010 Location Interface Protocol (UDP 5017), 0000011-1011110 Reserved,
    // 1011111-1111111 Manufacturer Specific. The field is 7 bits wide, so 95 is the boundary.
    if (idx == 0) {
        return "In Extended Header";
    }
    if (idx == 1) {
        return "UTF-16BE Text Message";
    }
    if (idx == 2) {
        return "Location Interface Protocol";
    }
    if (idx < 95) {
        return "Reserved";
    }
    return "Manufacturer Specific";
}

static int
dmr_udp_is_control_service_port(uint16_t port) {
    switch (port) {
        case 4004U:
        case 4005U:
        case 4009U:
        case 9361U: return 1;
        default: return 0;
    }
}

static dsd_event_category
dmr_udp_event_category(uint16_t src_port, uint16_t dst_port) {
    return dmr_udp_is_control_service_port(src_port) || dmr_udp_is_control_service_port(dst_port)
               ? DSD_EVENT_CATEGORY_CONTROL
               : DSD_EVENT_CATEGORY_DATA;
}

enum { DMR_UDP_COMP_PORT_TEXT = 5016U, DMR_UDP_COMP_PORT_LIP = 5017U };

// UDP ports whose number is fixed or a default on the radio side, so a datagram sent from one names
// its service even when its destination is a configurable server port: the radio's ARS port is
// "always 4005 and is not CPS configurable" (MOTOTRBO ARS specification 4.4.2), and 5016/5017 are
// the radio-network defaults of TS 102 361-3 tables 7.17/7.18, which the compressed header has
// always decoded from either endpoint. A policy, not a rule ETSI states for 5016/5017.
static int
dmr_udp_src_port_names_service(uint16_t port) {
    return port == 4005U || port == 5016U || port == 5017U;
}

static int dmr_udp_service_dispatch_port(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                         uint16_t port, uint16_t payload_len, const uint8_t* payload,
                                         const uint8_t* ip_hdr);

// The radios and ports of one datagram. A port is tried only when `*_known`: the compressed header can
// leave one unresolved.
typedef struct {
    uint32_t src24;
    uint32_t dst24;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t src_known;
    uint8_t dst_known;
} dmr_udp_endpoints;

// The one port dispatch both the IPv4 (SAP 4) and the compressed UDP/IPv4 (SAP 3) paths use: the
// destination port when a decoder claims it, else a source port that names its service. Returns 1
// when a decoder ran; 0 leaves the state untouched.
static int
dmr_udp_service_dispatch(dsd_opts* opts, dsd_state* state, uint8_t slot, const dmr_udp_endpoints* ep,
                         uint16_t payload_len, const uint8_t* payload, const uint8_t* ip_hdr) {
    if (ep->dst_known
        && dmr_udp_service_dispatch_port(opts, state, slot, ep->src24, ep->dst24, ep->dst_port, payload_len, payload,
                                         ip_hdr)) {
        return 1;
    }
    return ep->src_known && dmr_udp_src_port_names_service(ep->src_port)
           && dmr_udp_service_dispatch_port(opts, state, slot, ep->src24, ep->dst24, ep->src_port, payload_len, payload,
                                            ip_hdr);
}

// One SPID or DPID: the 7-bit index as sent and the UDP port it stands for. They never share a
// variable, because a port number read from the extended header is not an index (issue #450).
typedef struct {
    uint16_t idx;   // 7-bit index (tables 7.17/7.18); 0 = In Extended Header
    uint16_t port;  // UDP port, meaningful only when known != 0
    uint8_t known;  // idx 1/2 (table default), idx 0 with the extended header present, or a mapped idx
    uint8_t mapped; // a manufacturer-specific idx linked to a port by --dmr-udp-comp-port
} dmr_udp_comp_port;

_Static_assert(sizeof(((dsd_opts*)0)->dmr_udp_comp_ports) / sizeof(((dsd_opts*)0)->dmr_udp_comp_ports[0])
                   == DSD_UDP_COMP_PORT_MAP_SIZE,
               "one dmr_udp_comp_ports entry per manufacturer-specific index");

static void
dmr_udp_comp_port_init(const dsd_opts* opts, dmr_udp_comp_port* entry, uint16_t idx) {
    DSD_MEMSET(entry, 0, sizeof(*entry));
    entry->idx = idx;
    // Tables 7.17/7.18 note 1: the port numbers are defaults within the radio network.
    if (idx == 1) {
        entry->port = DMR_UDP_COMP_PORT_TEXT;
        entry->known = 1;
    } else if (idx == 2) {
        entry->port = DMR_UDP_COMP_PORT_LIP;
        entry->known = 1;
    } else {
        // Note 2 leaves the linkage of 95-127 to each radio's configuration, so only the site can
        // supply it; nothing on the air names the port.
        const uint16_t port = dsd_udp_comp_port_lookup(opts->dmr_udp_comp_ports, idx);
        if (port != 0U) {
            entry->port = port;
            entry->known = 1;
            entry->mapped = 1;
        }
    }
}

// A manufacturer-specific index the site has not linked to a port: its payload is the only clue.
static int
dmr_udp_comp_port_unresolved_mfr(const dmr_udp_comp_port* entry) {
    return entry->idx >= DSD_UDP_COMP_PORT_IDX_FIRST && entry->known == 0U;
}

// Tables 7.20/7.21: a source index of 0 puts the source port in Extended Header 1; a destination
// index of 0 puts the destination port in Extended Header 1, or in Extended Header 2 when the source
// port is there already. Returns the payload offset. A PDU too short for the extended headers it
// announces leaves those ports unknown and returns len, so nothing is decoded from it.
static uint16_t
dmr_udp_comp_resolve_ports(const uint8_t* pdu, uint16_t len, dmr_udp_comp_port* src, dmr_udp_comp_port* dst) {
    uint16_t ptr = 5;
    const uint16_t need = (uint16_t)(5U + (src->idx == 0 ? 2U : 0U) + (dst->idx == 0 ? 2U : 0U));
    if (len < need) {
        return len;
    }
    if (src->idx == 0) {
        src->port = (uint16_t)((pdu[ptr] << 8) | pdu[ptr + 1]);
        src->known = 1;
        ptr = (uint16_t)(ptr + 2U);
    }
    if (dst->idx == 0) {
        dst->port = (uint16_t)((pdu[ptr] << 8) | pdu[ptr + 1]);
        dst->known = 1;
        ptr = (uint16_t)(ptr + 2U);
    }
    return ptr;
}

// The index label, plus the port it resolved to when one is known. The numeric slot in the
// output always carries the index; the port only ever appears inside this label.
static void
dmr_udp_comp_port_desc(const dmr_udp_comp_port* entry, char* out, size_t out_size) {
    const char* label = dmr_udp_comp_port_idx_desc(entry->idx);
    if (entry->mapped) {
        DSD_SNPRINTF(out, out_size, "%s, mapped UDP %u", label, (unsigned)entry->port);
    } else if (entry->known) {
        DSD_SNPRINTF(out, out_size, "%s, UDP %u", label, (unsigned)entry->port);
    } else if (entry->idx == 0) {
        DSD_SNPRINTF(out, out_size, "%s, truncated", label);
    } else {
        DSD_SNPRINTF(out, out_size, "%s", label);
    }
}

// Decodes the payload after the compressed header through the shared UDP port dispatch. Returns 1
// when a decoder ran (the slot string then holds its summary), 0 when nothing claimed the payload.
// The radios are the data header's LLIDs, as on every DMR data notice.
static int
dmr_udp_comp_decode_payload(dsd_opts* opts, dsd_state* state, uint8_t slot, const dmr_udp_comp_port* src,
                            const dmr_udp_comp_port* dst, uint16_t len, uint16_t ptr, const uint8_t* pdu,
                            uint8_t* by_content) {
    *by_content = 0;
    if (len <= ptr) {
        return 0;
    }
    const uint16_t payload_len = (uint16_t)(len - ptr);
    const uint8_t* payload = pdu + ptr;
    const dmr_udp_endpoints ep = {(uint32_t)state->dmr_lrrp_source[slot],
                                  (uint32_t)state->dmr_lrrp_target[slot],
                                  src->port,
                                  dst->port,
                                  src->known,
                                  dst->known};
    if (dmr_udp_service_dispatch(opts, state, slot, &ep, payload_len, payload, NULL)) {
        return 1;
    }
    // An unlinked manufacturer-specific index leaves the payload as the only evidence of the
    // service. ARS is taken only when the payload is exactly one message of the ARS specification's
    // section 3.4; a Motorola TMS acknowledgement starts with the same header octet (0xBF) and is
    // told apart by its length. Reserved indices (3-94) carry nothing a receiver may interpret.
    if ((dmr_udp_comp_port_unresolved_mfr(src) || dmr_udp_comp_port_unresolved_mfr(dst))
        && dmr_ars_record_is_exact(payload, payload_len)) {
        *by_content = 1;
        return dmr_udp_service_dispatch_port(opts, state, slot, ep.src24, ep.dst24, 4005U, payload_len, payload, NULL);
    }
    DSD_FPRINTF(stderr, "Unknown Decode Format;");
    return 0;
}

void
dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU) {
    if (DMR_PDU == NULL || len < 5U) {
        return;
    }
    uint8_t slot = (state->currentslot == 1) ? 1 : 0;
    // ETSI TS 102 361-3 Table 7.14: the leading 16 bits are the IPv4 Identification, carried
    // verbatim because the decompressor cannot regenerate it - not one of the index fields.
    uint16_t ipid = (uint16_t)((DMR_PDU[0] << 8) | DMR_PDU[1]);
    uint8_t op1 = (uint8_t)((DMR_PDU[3] >> 7) & 1);
    uint8_t op2 = (uint8_t)((DMR_PDU[4] >> 7) & 1);
    uint8_t opcode = (uint8_t)((op1 << 1) | op2);
    if (opcode != 0) {
        // ETSI TS 102 361-3 V1.3.1 table 7.19 defines only 00 (UDP/IPv4 Header Compression). The other
        // three values have no published layout, so nothing after the IP ID is read; the IP ID is kept
        // so retransmissions of the same datagram can still be correlated. In practice a reserved
        // opcode marks a damaged frame rather than vendor traffic: on a live Motorola system every
        // CRC32-clean compressed PDU carried 00, and the reserved values came from frames that failed
        // CRC32 and were admitted by the relaxed-CRC options, one of them a healthy ARS registration
        // with a single bit flipped in the SPID octet (#450). Reading such a frame as the 00 layout
        // invented index labels and a payload offset from bytes that were not a header.
        DSD_FPRINTF(stderr, "\n IP ID: %04X; Opcode: %d; Reserved Header Compression Opcode; header not decoded; ",
                    ipid, opcode);
        char reserved_string[128];
        DSD_SNPRINTF(reserved_string, sizeof(reserved_string),
                     "IP ID: %04X; OP: %d; Reserved Header Compression Opcode; header not decoded; ", ipid, opcode);
        const dsd_call_observation observation = dsd_call_observation_data(
            state->lastsynctype, slot, (uint64_t)state->dmr_lrrp_source[slot], (uint64_t)state->dmr_lrrp_target[slot]);
        (void)dsd_event_emit_data_notice(opts, state, slot, &observation, reserved_string);
        return;
    }
    uint16_t said = (uint16_t)((DMR_PDU[2] >> 4) & 0xF);
    uint16_t daid = (uint16_t)((DMR_PDU[2] >> 0) & 0xF);
    dmr_udp_comp_port src;
    dmr_udp_comp_port dst;
    dmr_udp_comp_port_init(opts, &src, (uint16_t)(DMR_PDU[3] & 0x7F));
    dmr_udp_comp_port_init(opts, &dst, (uint16_t)(DMR_PDU[4] & 0x7F));
    uint16_t ptr = dmr_udp_comp_resolve_ports(DMR_PDU, len, &src, &dst);

    const char* src_idx_desc = dmr_udp_comp_src_idx_desc(said);
    const char* dst_idx_desc = dmr_udp_comp_dst_idx_desc(daid);
    char src_port_desc[64];
    char dst_port_desc[64];
    dmr_udp_comp_port_desc(&src, src_port_desc, sizeof(src_port_desc));
    dmr_udp_comp_port_desc(&dst, dst_port_desc, sizeof(dst_port_desc));

    DSD_FPRINTF(stderr, "\n IP ID: %04X; Opcode: %d; Src Idx: %d (%s); Dst Idx: %d (%s); ", ipid, opcode, said,
                src_idx_desc, daid, dst_idx_desc);
    DSD_FPRINTF(stderr, "\n Src Port Idx: %d (%s); Dst Port Idx: %d (%s); ", src.idx, src_port_desc, dst.idx,
                dst_port_desc);

    // As decode_ip_pdu(): a fix counts only when a decoder produced it from this PDU; otherwise the
    // slot keeps the position it had.
    char previous_gps[sizeof(state->dmr_embedded_gps[slot])];
    DSD_SNPRINTF(previous_gps, sizeof(previous_gps), "%s", state->dmr_embedded_gps[slot]);
    state->dmr_embedded_gps[slot][0] = '\0';
    uint8_t by_content = 0;
    const int decoded = dmr_udp_comp_decode_payload(opts, state, slot, &src, &dst, len, ptr, DMR_PDU, &by_content);

    char comp_string[1024];
    DSD_MEMSET(comp_string, 0, sizeof(comp_string));
    DSD_SNPRINTF(comp_string, sizeof(comp_string), "IP ID: %04X; OP: %d; SRC: %d:%d (%s):(%s); DST: %d:%d (%s):(%s); ",
                 ipid, opcode, said, src.idx, src_idx_desc, src_port_desc, daid, dst.idx, dst_idx_desc, dst_port_desc);
    // A decoder writes its summary to the slot string, which the history row then carries after the
    // header fields, as on the IPv4 and MNIS paths.
    if (decoded) {
        dsd_append(comp_string, sizeof(comp_string), state->dmr_lrrp_gps[slot]);
    }
    // The radios on this PDU are the data header's LLIDs, as for every other DMR data notice;
    // SAID/DAID are 4-bit network indices (tables 7.15/7.16), not radio IDs.
    const dsd_call_observation observation = dsd_call_observation_data(
        state->lastsynctype, slot, (uint64_t)state->dmr_lrrp_source[slot], (uint64_t)state->dmr_lrrp_target[slot]);
    const dsd_event_category category =
        by_content ? DSD_EVENT_CATEGORY_CONTROL
                   : dmr_udp_event_category(src.known ? src.port : 0U, dst.known ? dst.port : 0U);
    if (state->dmr_embedded_gps[slot][0] == '\0') {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof(state->dmr_embedded_gps[slot]), "%s", previous_gps);
    }
    // An admitted LIP report was staged with its text by lip_pdu_decoder(); the notice consumes it.
    (void)dsd_event_emit_data_notice_classified(opts, state, slot, &observation, category, comp_string);
}

static void DSD_ATTR_USED
decode_ip_pdu_handle_icmp(dsd_opts* opts, dsd_state* state, size_t effective_len, size_t ip_header_len,
                          uint8_t* input) {
    if (effective_len < ip_header_len + 4u) {
        return;
    }
    uint8_t icmp_type = input[ip_header_len + 0];
    uint8_t icmp_code = input[ip_header_len + 1];
    uint16_t icmp_chk = (uint16_t)((input[ip_header_len + 2] << 8) | input[ip_header_len + 3]);
    DSD_FPRINTF(stderr, "\n ICMP Protocol; Type: %02X; Code: %02X; Checksum: %02X;", icmp_type, icmp_code, icmp_chk);
    if (icmp_type == 3) {
        DSD_FPRINTF(stderr, " Destination");
        if (icmp_code == 0) {
            DSD_FPRINTF(stderr, " Network");
        } else if (icmp_code == 1) {
            DSD_FPRINTF(stderr, " Host");
        } else if (icmp_code == 2) {
            DSD_FPRINTF(stderr, " Protocol");
        } else if (icmp_code == 3) {
            DSD_FPRINTF(stderr, " Port");
        }
        DSD_FPRINTF(stderr, " Unreachable;");
    }
    size_t attached_off = ip_header_len + 8u;
    if (effective_len > attached_off && input[attached_off] == 0x45) {
        DSD_FPRINTF(stderr, "\n ------------Attached Message-------------");
        size_t rem = effective_len - attached_off;
        if (rem > UINT16_MAX) {
            rem = UINT16_MAX;
        }
        decode_ip_pdu(opts, state, (uint16_t)rem, input + attached_off);
    }
}

static void DSD_ATTR_USED
decode_ip_pdu_note_truncated_tms(dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24) {
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "TMS SRC: %d; DST: %d; Truncated;",
                 src24, dst24);
    DSD_FPRINTF(stderr, "TMS Truncated;");
}

static int DSD_ATTR_USED
decode_ip_pdu_tms_truncated(dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24) {
    decode_ip_pdu_note_truncated_tms(state, slot, src24, dst24);
    return -1;
}

// Motorola TMS over UDP 4007, as the MOTOTRBO Text Messaging ADK Guide describes the text and as
// Moto.Net, TRBO-NET and node-dmr-lib build the packet (sdrtrunk agrees on the encoding):
//   0..1  length, big-endian, of every octet that follows it
//   2     header: 0x80 extension present, 0x40 ack requested, 0x20 reserved, 0x10 system, and the
//         message type in the low bits (0 simple text, 0x1F ack)
//   3     address length; that many octets of address follow when non-zero
//   then  header-extension octets while the header says so, each with bit 7 set while another
//         follows (8F 04 on every capture seen: a sequence number and an encoding octet)
//   then  the text, UCS-2 little-endian, an optional subject line and CR LF ahead of the body
// The text is located from that layout rather than by backing up from the end of the header,
// which only lined up for ASCII (issue #466).
static int DSD_ATTR_USED
decode_ip_pdu_parse_udp_tms_address(dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                    uint16_t payload_len, const uint8_t* payload, size_t* tms_ptr) {
    if (*tms_ptr >= (size_t)payload_len) {
        return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
    }
    uint8_t tms_adl = payload[(*tms_ptr)++];
    if (tms_adl == 0) {
        return 0;
    }
    if (*tms_ptr + (size_t)tms_adl > (size_t)payload_len) {
        return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
    }
    DSD_FPRINTF(stderr, "Address Len: %d; Address: ", tms_adl);
    // dsd-fme found four octets past the address text on every capture it had, and no public
    // source parses this field, so the text is still taken as adl - 4 octets: the same characters
    // as before for ASCII, now read as the UCS-2 LE they are.
    if (tms_adl > 4U) {
        utf16le_to_text(state, 1, (uint16_t)(tms_adl - 4U), payload + *tms_ptr);
    }
    *tms_ptr += tms_adl;
    DSD_FPRINTF(stderr, "; ");
    return 0;
}

static int DSD_ATTR_USED
decode_ip_pdu_skip_udp_tms_extensions(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24,
                                      uint32_t dst24, uint16_t payload_len, const uint8_t* payload, size_t* tms_ptr) {
    uint8_t tms_more;
    do {
        if (*tms_ptr >= (size_t)payload_len) {
            return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
        }
        uint8_t tms_ext = payload[(*tms_ptr)++];
        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, "EXT: %02X; ", tms_ext);
        }
        tms_more = tms_ext & 0x80U;
    } while (tms_more);
    return 0;
}

// The text runs from tms_ptr to the end of what the length field covers, clamped to the octets
// the UDP payload actually holds; an odd trailing octet is dropped rather than read as a unit.
static int DSD_ATTR_USED
decode_ip_pdu_prepare_tms_text_span(dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                    uint16_t payload_len, size_t tms_ptr, int tms_len, size_t* text_len) {
    size_t consumed = tms_ptr - 2U; // octets of header after the length field
    if (tms_ptr >= (size_t)payload_len || tms_len <= 0 || (size_t)tms_len <= consumed) {
        return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
    }
    size_t span = (size_t)tms_len - consumed;
    size_t available = (size_t)payload_len - tms_ptr;
    if (span > available) {
        span = available;
    }
    *text_len = span & ~(size_t)1U;
    return 0;
}

static void DSD_ATTR_USED
decode_ip_pdu_handle_udp_tms(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                             uint16_t payload_len, const uint8_t* payload) {
    int tms_len = 0;
    if (payload_len >= 2) {
        tms_len = (payload[0] << 8) | payload[1];
    }
    DSD_FPRINTF(stderr, " TMS ");
    DSD_FPRINTF(stderr, "Len: %d; ", tms_len);
    if (payload_len < 4U) {
        decode_ip_pdu_note_truncated_tms(state, slot, src24, dst24);
        return;
    }

    size_t tms_ptr = 2;
    uint8_t tms_hdr = payload[tms_ptr++];
    uint8_t tms_ack = (tms_hdr >> 0) & 0xF;
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "HDR: %02X; ", tms_hdr);
    }
    if (decode_ip_pdu_parse_udp_tms_address(state, slot, src24, dst24, payload_len, payload, &tms_ptr) != 0) {
        return;
    }
    if ((tms_hdr & 0x80U) != 0U
        && decode_ip_pdu_skip_udp_tms_extensions(opts, state, slot, src24, dst24, payload_len, payload, &tms_ptr)
               != 0) {
        return;
    }

    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "TMS SRC: %d; DST: %d; ", src24, dst24);
    if (tms_ack != 0) {
        dsd_append(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "Acknowledgment;");
        DSD_FPRINTF(stderr, "Acknowledgment;");
        return;
    }

    size_t text_len = 0;
    if (decode_ip_pdu_prepare_tms_text_span(state, slot, src24, dst24, payload_len, tms_ptr, tms_len, &text_len) != 0) {
        return;
    }
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "Ptr: %u; Len: %u;", (unsigned)tms_ptr, (unsigned)text_len);
    }
    DSD_FPRINTF(stderr, "\n Text: ");
    utf16le_to_text(state, 1, (uint16_t)text_len, payload + tms_ptr);
}

static void DSD_ATTR_USED
decode_ip_pdu_handle_udp_vtx_tms(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                 uint16_t payload_len, const uint8_t* payload) {
    const size_t vtx_text_off = 21u;
    const size_t vtx_diag_hdr_len = 9u;
    size_t text_len = 0u;

    DSD_FPRINTF(stderr, "VTX STD TMS;");
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "VTX TMS SRC: %d; DST: %d; ", src24,
                 dst24);

    if (opts->payload == 1) {
        size_t diag_len = ((size_t)payload_len < vtx_diag_hdr_len) ? (size_t)payload_len : vtx_diag_hdr_len;
        DSD_FPRINTF(stderr, " HDR: ");
        for (size_t i = 0; i < diag_len; i++) {
            DSD_FPRINTF(stderr, "%02X", payload[i]);
        }
        if (diag_len < vtx_diag_hdr_len) {
            DSD_FPRINTF(stderr, " (truncated)");
        }
        DSD_FPRINTF(stderr, ";");
    }
    if ((size_t)payload_len > vtx_text_off) {
        text_len = (size_t)payload_len - vtx_text_off;
    }
    text_len &= ~(size_t)1u;
    if (text_len > 0u) {
        DSD_FPRINTF(stderr, " Text: ");
        utf16_to_text(state, 1, (uint16_t)text_len, payload + vtx_text_off);
    } else {
        dsd_append(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "No Text;");
        DSD_FPRINTF(stderr, " No Text;");
    }
}

// Some systems carry LRRP on a UDP port outside the registered set, which the dispatch below drops
// as an unknown port (#453). The extra ports are looked up at run time from what the site supplied,
// so no second port table is compiled in. The lookup runs only for a port neither dispatch table
// claims, so mapping a registered service port (say 4007) leaves that service's decoder in charge.
static int
decode_ip_pdu_udp_port_is_lrrp(const dsd_opts* opts, uint16_t port) {
    int count = opts->lrrp_extra_port_count;
    if (count > DSD_LRRP_EXTRA_PORT_MAX) {
        count = DSD_LRRP_EXTRA_PORT_MAX;
    }
    for (int i = 0; i < count; i++) {
        if (opts->lrrp_extra_ports[i] == port) {
            return 1;
        }
    }
    return 0;
}

static int DSD_ATTR_USED
decode_ip_pdu_handle_udp_service_core(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                      uint16_t port, uint16_t payload_len, const uint8_t* payload) {
    switch (port) {
        case 231:
            DSD_FPRINTF(stderr, "Cellocator;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "Cellocator SRC: %d; DST: %d;",
                         src24, dst24);
            if (payload_len > 0) {
                decode_cellocator(opts, state, payload, (int)payload_len);
            }
            return 1;
        case 4001:
            DSD_FPRINTF(stderr, "LRRP;");
            // dmr_lrrp() reads nothing from under two octets and leaves the slot string alone, which
            // would hand this notice the previous message's summary.
            if (payload_len < 2U) {
                DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                             "LRRP SRC: %u; DST: %u; Truncated;", src24, dst24);
            }
            // P25 Phase 1 arrives with currentslot == 0, matching the event_crc_invalid[0] scope in p25p1_mdpu.c.
            dmr_lrrp(opts, state, payload_len, src24, dst24, payload, (uint8_t)(state->event_crc_invalid[slot] == 0));
            return 1;
        case 4004:
            DSD_FPRINTF(stderr, "XCMP;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "XCMP SRC: %d; DST: %d;", src24,
                         dst24);
            return 1;
        case 4005: {
            DSD_FPRINTF(stderr, "ARS;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "ARS SRC: %d; DST: %d; ", src24,
                         dst24);
            dmr_ars_print_message(state, payload, payload_len);
            dmr_ars_note_activity(opts, state, slot, payload, payload_len, src24);
            return 1;
        }
        case 4007: decode_ip_pdu_handle_udp_tms(opts, state, slot, src24, dst24, payload_len, payload); return 1;
        case 4008:
            DSD_FPRINTF(stderr, "Telemetry;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "Telemetry SRC: %d; DST: %d;",
                         src24, dst24);
            return 1;
        case 4009:
            DSD_FPRINTF(stderr, "OTAP;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "OTAP SRC: %d; DST: %d;", src24,
                         dst24);
            return 1;
        case 4012:
            DSD_FPRINTF(stderr, "Battery Management;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "Batt. Man. SRC: %d; DST: %d;",
                         src24, dst24);
            return 1;
        case 4013:
            DSD_FPRINTF(stderr, "Job Ticket Server;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "JTS SRC: %d; DST: %d;", src24,
                         dst24);
            return 1;
        case 4069:
            DSD_FPRINTF(stderr, "TRBOnet SCADA;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "SCADA SRC: %d; DST: %d;", src24,
                         dst24);
            return 1;
        default: break;
    }
    return 0;
}

// Whether the destination @p dest a DMR data packet names is a talkgroup: it is the slot's checked data header target
// and that header's G/I bit said group (dsd_state::dmr_data_checked_group). Only a DMR header qualifies one.
static uint8_t
dmr_pdu_header_target_is_group(const dsd_state* state, uint8_t slot, uint32_t dest) {
    return (uint8_t)(DSD_SYNC_IS_DMR(state->lastsynctype) && dest != 0U
                     && (uint64_t)state->dmr_lrrp_target[slot & 1U] == (uint64_t)dest
                     && state->dmr_data_checked_group[slot & 1U] != 0U);
}

// `ip_hdr` is the IPv4 header on the IPv4 path and NULL on the compressed one, which carries no
// addresses; the two P25 services that print them name the radios instead.
static int DSD_ATTR_USED
decode_ip_pdu_handle_udp_service_ext(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24,
                                     uint32_t dst24, uint16_t port, uint16_t payload_len, const uint8_t* payload,
                                     const uint8_t* ip_hdr) {
    switch (port) {
        case 5007: decode_ip_pdu_handle_udp_vtx_tms(opts, state, slot, src24, dst24, payload_len, payload); return 1;
        case 5016:
            DSD_FPRINTF(stderr, "ETSI TMS;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "ETSI TMS SRC: %d; DST: %d; ",
                         src24, dst24);
            utf16_to_text(state, 1, payload_len, payload);
            return 1;
        case 5017: {
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "LIP SRC: %u; DST: %u;", src24,
                         dst24);
            uint8_t bits[127 * 12 * 8];
            uint16_t decode_len = payload_len;
            if (decode_len > (uint16_t)(sizeof(bits) / 8U)) {
                decode_len = (uint16_t)(sizeof(bits) / 8U);
            }
            DSD_MEMSET(bits, 0, sizeof(bits));
            dsd_unpack_bytes_to_bits(payload, payload_len, bits, sizeof(bits), decode_len);
            // The checked packet names the source and the destination, a talkgroup as LRRP qualifies one.
            const dsd_gps_address addr = {
                .source_id = src24,
                .target_id = dst24,
                .target_group = dmr_pdu_header_target_is_group(state, slot, dst24),
            };
            lip_pdu_decoder(opts, state, bits, (size_t)decode_len * 8U, &addr);
            return 1;
        }
        case 9361:
            if (ip_hdr == NULL) {
                DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                             "P25 Atlas SRC: %u; DST: %u; ", src24, dst24);
            } else {
                DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                             "P25 Atlas SRC(IP): %d.%d.%d.%d; DST(IP): %d.%d.%d.%d; ", ip_hdr[12], ip_hdr[13],
                             ip_hdr[14], ip_hdr[15], ip_hdr[16], ip_hdr[17], ip_hdr[18], ip_hdr[19]);
            }
            DSD_FPRINTF(stderr, "Atlas Data Registration Server;");
            return 1;
        case 49198:
            if (ip_hdr == NULL) {
                DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                             "P25 Tier 2 LOCN SRC: %u; DST: %u; ", src24, dst24);
            } else {
                DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                             "P25 Tier 2 LOCN SRC(IP): %d.%d.%d.%d; DST(IP): %d.%d.%d.%d; ", ip_hdr[12], ip_hdr[13],
                             ip_hdr[14], ip_hdr[15], ip_hdr[16], ip_hdr[17], ip_hdr[18], ip_hdr[19]);
            }
            DSD_FPRINTF(stderr, "P25 Tier 2 Location Service;");
            dmr_lrrp(opts, state, payload_len, src24, dst24, payload, (uint8_t)(state->event_crc_invalid[slot] == 0));
            return 1;
        default: break;
    }
    return 0;
}

static int
dmr_udp_service_dispatch_port(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                              uint16_t port, uint16_t payload_len, const uint8_t* payload, const uint8_t* ip_hdr) {
    if (decode_ip_pdu_handle_udp_service_core(opts, state, slot, src24, dst24, port, payload_len, payload)) {
        return 1;
    }
    if (decode_ip_pdu_handle_udp_service_ext(opts, state, slot, src24, dst24, port, payload_len, payload, ip_hdr)) {
        return 1;
    }
    if (decode_ip_pdu_udp_port_is_lrrp(opts, port)) {
        // Dispatched as the registered location port, so a mapped port cannot drift away from 4001.
        return decode_ip_pdu_handle_udp_service_core(opts, state, slot, src24, dst24, 4001U, payload_len, payload);
    }
    return 0;
}

static void DSD_ATTR_USED
decode_ip_pdu_handle_udp_service(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                 uint16_t src_port, uint16_t dst_port, uint16_t payload_len, const uint8_t* payload,
                                 const uint8_t* input) {
    const dmr_udp_endpoints ep = {src24, dst24, src_port, dst_port, 1U, 1U};
    if (dmr_udp_service_dispatch(opts, state, slot, &ep, payload_len, payload, input)) {
        return;
    }
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                 "IP SRC: %d.%d.%d.%d:%d; DST: %d.%d.%d.%d:%d; Unknown UDP Port;", input[12], input[13], input[14],
                 input[15], src_port, input[16], input[17], input[18], input[19], dst_port);
    DSD_FPRINTF(stderr, "Unknown UDP Port;");
}

static void DSD_ATTR_USED
decode_ip_pdu_handle_udp(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                         size_t effective_len, size_t ip_header_len, uint8_t* input) {
    if (effective_len < ip_header_len + 8u) {
        DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "Truncated UDP;");
        return;
    }
    uint16_t src_port = (uint16_t)((input[ip_header_len + 0] << 8) | input[ip_header_len + 1]);
    uint16_t dst_port = (uint16_t)((input[ip_header_len + 2] << 8) | input[ip_header_len + 3]);
    uint16_t udp_len = (uint16_t)((input[ip_header_len + 4] << 8) | input[ip_header_len + 5]);
    uint16_t udp_chk = (uint16_t)((input[ip_header_len + 6] << 8) | input[ip_header_len + 7]);
    DSD_FPRINTF(stderr, "\n UDP Protocol; Datagram Len: %d; UDP Checksum: %04X; ", udp_len, udp_chk);

    size_t udp_payload_off = ip_header_len + 8u;
    size_t udp_payload_len = (udp_len >= 8u) ? ((size_t)udp_len - 8u) : 0u;
    size_t max_payload_len = (effective_len > udp_payload_off) ? (effective_len - udp_payload_off) : 0u;
    if (udp_payload_len > max_payload_len) {
        udp_payload_len = max_payload_len;
    }
    uint16_t payload_len = (uint16_t)udp_payload_len;
    const uint8_t* payload = input + udp_payload_off;

    decode_ip_pdu_handle_udp_service(opts, state, slot, src24, dst24, src_port, dst_port, payload_len, payload, input);
}

typedef struct {
    uint8_t version;
    uint8_t ihl;
    uint8_t tos;
    uint16_t tlen;
    uint16_t iden;
    uint8_t ipf;
    uint16_t offset;
    uint8_t ttl;
    uint8_t prot;
    uint16_t hsum;
    uint16_t len;
} dmr_ip_pdu_header;

static void DSD_ATTR_USED
decode_ip_pdu_print_header(const dsd_opts* opts, const dmr_ip_pdu_header* hdr) {
    if (opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(
        stderr,
        "\n IPv%d; IHL: %d; Type of Service: %d; Total Len: %d; IP ID: %04X; Flags: %X;\n Fragment Offset: %d; TTL: "
        "%d; Protocol: 0x%02X; Checksum: %04X; PDU Len: %d;",
        hdr->version, hdr->ihl, hdr->tos, hdr->tlen, hdr->iden, hdr->ipf, hdr->offset, hdr->ttl, hdr->prot, hdr->hsum,
        hdr->len);
}

static void
decode_ip_pdu_print_endpoints(uint8_t prot, uint32_t src24, uint32_t dst24, uint16_t src_port, uint16_t dst_port,
                              const uint8_t* input) {
    DSD_FPRINTF(stderr, "\n SRC(24): %08d; IP: %03d.%03d.%03d.%03d; ", src24, input[12], input[13], input[14],
                input[15]);
    if (prot == 0x11) {
        DSD_FPRINTF(stderr, "Port: %04d; ", src_port);
    }
    DSD_FPRINTF(stderr, "\n DST(24): %08d; IP: %03d.%03d.%03d.%03d; ", dst24, input[16], input[17], input[18],
                input[19]);
    if (prot == 0x11) {
        DSD_FPRINTF(stderr, "Port: %04d; ", dst_port);
    }
}

static void DSD_ATTR_USED
decode_ip_pdu_dispatch(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t prot, uint32_t src24, uint32_t dst24,
                       size_t effective_len, size_t ip_header_len, uint8_t* input) {
    if (prot == 0x01) {
        decode_ip_pdu_handle_icmp(opts, state, effective_len, ip_header_len, input);
        return;
    }
    if (prot == 0x11) {
        decode_ip_pdu_handle_udp(opts, state, slot, src24, dst24, effective_len, ip_header_len, input);
        return;
    }
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                 "IP SRC: %d.%d.%d.%d; DST: %d.%d.%d.%d; Unknown IP Protocol: %d; ", input[12], input[13], input[14],
                 input[15], input[16], input[17], input[18], input[19], prot);
    DSD_FPRINTF(stderr, "Unknown IP Protocol: %02X;", prot);
}

static dsd_event_category
decode_ip_pdu_event_category(uint8_t protocol, uint16_t src_port, uint16_t dst_port) {
    if (protocol != 0x11U) {
        return DSD_EVENT_CATEGORY_DATA;
    }
    return dmr_udp_event_category(src_port, dst_port);
}

static int
decode_ip_pdu_emit_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t prot, uint32_t src24, uint32_t dst24,
                          uint16_t src_port, uint16_t dst_port, const char* previous_gps) {
    const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, slot, src24, dst24);
    const dsd_event_category category = decode_ip_pdu_event_category(prot, src_port, dst_port);
    // An admitted LIP report was staged with its text by lip_pdu_decoder() and this notice consumes it. An ICMP
    // wrapper never claims a fix produced by its enclosed packet: that packet's own notice consumed it.
    if (state->dmr_embedded_gps[slot][0] == '\0') {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "%s", previous_gps);
    }
    // Consuming notices take only the data PDU's staging, never the active call's enrichment.
    return dsd_event_emit_data_notice_classified(opts, state, slot, &observation, category, state->dmr_lrrp_gps[slot])
           == 0;
}

//IP PDU header decode and port forward to appropriate decoder
int
decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input) {
    if (!opts) {
        return 0;
    }
    if (!state) {
        return 0;
    }
    if (!input) {
        return 0;
    }
    if (len < 20) {
        return 0;
    }

    uint8_t slot = (uint8_t)(state->currentslot & 1);
    uint8_t version = input[0] >> 4;
    uint8_t ihl = input[0] & 0xF;
    uint8_t tos = input[1];
    uint16_t tlen = (uint16_t)((input[2] << 8) | input[3]);
    uint16_t iden = (uint16_t)((input[4] << 8) | input[5]);
    uint8_t ipf = input[6] >> 5;
    uint16_t offset = (uint16_t)(((input[6] & 0x1F) << 8) | input[7]);
    uint8_t ttl = input[8];
    uint8_t prot = input[9];
    uint16_t hsum = (uint16_t)((input[10] << 8) | input[11]);

    size_t ip_header_len = (size_t)ihl * 4u;
    if (version != 4) {
        return 0;
    }
    if (ihl < 5) {
        return 0;
    }
    if (ip_header_len > (size_t)len) {
        return 0;
    }
    size_t effective_len = (size_t)len;
    if ((size_t)tlen >= ip_header_len) {
        if ((size_t)tlen <= effective_len) {
            effective_len = (size_t)tlen;
        }
    }
    dmr_ip_pdu_header hdr = {version, ihl, tos, tlen, iden, ipf, offset, ttl, prot, hsum, len};
    decode_ip_pdu_print_header(opts, &hdr);

    uint32_t src24 = (uint32_t)((input[13] << 16) | (input[14] << 8) | input[15]);
    uint32_t dst24 = (uint32_t)((input[17] << 16) | (input[18] << 8) | input[19]);
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    if (prot == 0x11) {
        if (effective_len >= ip_header_len + 8u) {
            src_port = (uint16_t)((input[ip_header_len + 0] << 8) | input[ip_header_len + 1]);
            dst_port = (uint16_t)((input[ip_header_len + 2] << 8) | input[ip_header_len + 3]);
        }
    }
    decode_ip_pdu_print_endpoints(prot, src24, dst24, src_port, dst_port, input);
    char previous_gps[sizeof state->dmr_embedded_gps[slot]];
    DSD_SNPRINTF(previous_gps, sizeof previous_gps, "%s", state->dmr_embedded_gps[slot]);
    state->dmr_embedded_gps[slot][0] = '\0';
    decode_ip_pdu_dispatch(opts, state, slot, prot, src24, dst24, effective_len, ip_header_len, input);

    return decode_ip_pdu_emit_notice(opts, state, slot, prot, src24, dst24, src_port, dst_port, previous_gps);
}

// LRRP (MOTOTRBO Location Request/Response Protocol) documents.
//
// Latitude and longitude are TIA-102.AABF-D-1 §7.3.37-38: latitude is a sign bit plus a 31-bit magnitude in units of
// 90/2^31 degree (90 itself is sent as 2^31-1), longitude a 32-bit two's complement value in units of 360/2^32 degree
// (SDRTrunk's Point2d reads them the same way). TIA-102.BAJC-B gives the units of the rest: metres, metres per
// second, a horizontal direction in 2-degree steps (below 180) and UTC times. No public document gives the binary
// token grammar. The numbers that vary in size (uintvar, sintvar, ufloatvar, sfloatvar) follow ok-dmrlib, Moto.Net and
// SClass TETRALRRP, which agree with each other and with real off-air reports, and Motorola's MOTOTRBO System Planners
// list speed, altitude and radius as variable sized fields. SDRTrunk's fixed widths cite no source and do not fit the
// real reports. Tokens of which no real report was found (0x50, 0x52, 0x54, 0x68, 0x6A) are not guessed: one ends the
// position parse, as does any token this parser does not know.

// The bytes of one uintvar or sintvar at most: five 7-bit groups hold any 32-bit value (a float's fraction is a
// uintvar too, so its denominator is at most 128^5).
#define DMR_LRRP_VARINT_MAX_BYTES 5U
// The leading bytes a resync reading may skip.
#define DMR_LRRP_RESYNC_MAX_SKIP  6U

// A number as sent: whole + frac / 128^frac_bytes, negative when the sign bit was set. frac_bytes is 0 for an
// integer token.
typedef struct {
    uint32_t whole;
    uint32_t frac;
    uint8_t frac_bytes;
    uint8_t negative;
} dmr_lrrp_number;

// The 7-bit groups after a first group that continues: returns the bytes read in all, or 0 when the number runs past
// `avail`, takes more than DMR_LRRP_VARINT_MAX_BYTES or does not fit 32 bits.
static size_t
dmr_lrrp_read_groups(const uint8_t* p, size_t avail, uint64_t value, uint32_t* out) {
    for (size_t i = 1U; i < avail && i < DMR_LRRP_VARINT_MAX_BYTES; i++) {
        value = (value << 7) | (uint64_t)(p[i] & 0x7FU);
        if (value > UINT32_MAX) {
            return 0U;
        }
        if ((p[i] & 0x80U) == 0U) {
            *out = (uint32_t)value;
            return i + 1U;
        }
    }
    return 0U;
}

// uintvar: 7-bit groups, most significant first, the top bit set on every byte but the last.
static size_t
dmr_lrrp_read_uintvar(const uint8_t* p, size_t avail, uint32_t* out) {
    if (avail == 0U) {
        return 0U;
    }
    if ((p[0] & 0x80U) == 0U) {
        *out = p[0];
        return 1U;
    }
    return dmr_lrrp_read_groups(p, avail, (uint64_t)(p[0] & 0x7FU), out);
}

// sintvar: a uintvar whose first byte carries the sign in bit 6 and only six value bits.
static size_t
dmr_lrrp_read_sintvar(const uint8_t* p, size_t avail, uint32_t* magnitude, uint8_t* negative) {
    if (avail == 0U) {
        return 0U;
    }
    *negative = (uint8_t)((p[0] & 0x40U) != 0U);
    if ((p[0] & 0x80U) == 0U) {
        *magnitude = (uint32_t)(p[0] & 0x3FU);
        return 1U;
    }
    return dmr_lrrp_read_groups(p, avail, (uint64_t)(p[0] & 0x3FU), magnitude);
}

// One number: a uintvar or sintvar, followed for a float by its fraction (a uintvar over 128 per byte it takes).
// Returns the bytes read, or 0 when it is malformed or runs past `avail`.
static size_t
dmr_lrrp_read_number(const uint8_t* p, size_t avail, int is_signed, int is_float, dmr_lrrp_number* out) {
    dmr_lrrp_number n = {0U, 0U, 0U, 0U};
    size_t used =
        is_signed ? dmr_lrrp_read_sintvar(p, avail, &n.whole, &n.negative) : dmr_lrrp_read_uintvar(p, avail, &n.whole);
    if (used == 0U) {
        return 0U;
    }
    if (is_float) {
        const size_t frac_used = dmr_lrrp_read_uintvar(p + used, avail - used, &n.frac);
        if (frac_used == 0U) {
            return 0U;
        }
        n.frac_bytes = (uint8_t)frac_used;
        used += frac_used;
    }
    *out = n;
    return used;
}

// |n| x scale, rounded to the nearest whole unit.
static uint64_t
dmr_lrrp_number_scaled(const dmr_lrrp_number* n, uint32_t scale) {
    uint64_t value = (uint64_t)n->whole * scale;
    if (n->frac_bytes != 0U) {
        const uint64_t den = (uint64_t)1U << (7U * n->frac_bytes);
        value += ((uint64_t)n->frac * scale * 2U + den) / (2U * den);
    }
    return value;
}

// The number as a double, for printing only.
static double
dmr_lrrp_number_value(const dmr_lrrp_number* n) {
    double value = (double)n->whole;
    if (n->frac_bytes != 0U) {
        value += (double)n->frac / (double)((uint64_t)1U << (7U * n->frac_bytes));
    }
    return n->negative ? -value : value;
}

typedef struct {
    // token 0x34, UTC; year 0 when absent or invalid
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;

    // the position: the shape of best rank (dmr_lrrp_shapes)
    uint32_t lat_raw;
    uint32_t lon_raw;
    uint8_t have_pos;
    uint8_t pos_rank;
    uint8_t pos_zero;        // the position is exactly 0, 0 (dmr_lrrp_zero_pair())
    dmr_lrrp_number radius;  // metres
    dmr_lrrp_number alt;     // metres
    dmr_lrrp_number alt_acc; // metres
    uint8_t have_rad;
    uint8_t have_alt;
    uint8_t have_alt_acc;

    dmr_lrrp_number speed; // horizontal speed, metres per second (0x6B integer, 0x6C float)
    uint8_t have_speed;
    uint8_t heading_units; // horizontal direction in 2-degree steps, below 180
    uint8_t have_heading;
    uint8_t heading_seen; // a 0x56 was read, in range or not: only the first counts
} dmr_lrrp_parse_result;

static void
dmr_lrrp_parse_result_init(dmr_lrrp_parse_result* r) {
    *r = (dmr_lrrp_parse_result){0};
    r->pos_rank = 0xFFu;
}

// The fields after a shape's latitude and longitude, in their order on the air.
enum { DMR_LRRP_FIELD_RADIUS = 1U, DMR_LRRP_FIELD_ALT = 2U, DMR_LRRP_FIELD_ALT_ACC = 4U };

// The position tokens (shapes). When a document carries more than one, the lowest rank wins: SDRTrunk's TokenType
// order (circle-2d, circle-3d, point-2d, point-3d), an integer variant ranking with its float one.
typedef struct {
    uint8_t token;
    uint8_t rank;
    uint8_t fields;
    uint8_t is_float;
} dmr_lrrp_shape;

static const dmr_lrrp_shape dmr_lrrp_shapes[] = {
    {0x51, 0U, DMR_LRRP_FIELD_RADIUS, 1U},                                               // circle-2d
    {0x53, 1U, DMR_LRRP_FIELD_RADIUS | DMR_LRRP_FIELD_ALT | DMR_LRRP_FIELD_ALT_ACC, 0U}, // circle-3d, integers
    {0x55, 1U, DMR_LRRP_FIELD_RADIUS | DMR_LRRP_FIELD_ALT | DMR_LRRP_FIELD_ALT_ACC, 1U}, // circle-3d
    {0x66, 2U, 0U, 0U},                                                                  // point-2d
    {0x67, 3U, DMR_LRRP_FIELD_ALT, 0U},                                                  // point-3d, integer
    {0x69, 3U, DMR_LRRP_FIELD_ALT, 1U},                                                  // point-3d
};

static const dmr_lrrp_shape*
dmr_lrrp_shape_for(uint8_t token) {
    for (size_t i = 0; i < sizeof dmr_lrrp_shapes / sizeof dmr_lrrp_shapes[0]; i++) {
        if (dmr_lrrp_shapes[i].token == token) {
            return &dmr_lrrp_shapes[i];
        }
    }
    return NULL;
}

static uint32_t
dmr_lrrp_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void dmr_lrrp_fill_coordinates(uint32_t lat_raw, uint32_t lon_raw, dsd_position_fix* fix);

// Whether a shape's coordinates are exactly 0, 0 as the record would hold them: never a position (admission refuses
// it), so such a shape does not take the place of another shape's position, whatever their ranks. One zero coordinate
// is a place.
static int
dmr_lrrp_zero_pair(uint32_t lat_raw, uint32_t lon_raw) {
    dsd_position_fix fix;
    dmr_lrrp_fill_coordinates(lat_raw, lon_raw, &fix);
    return fix.lat_e7 == 0 && fix.lon_e7 == 0;
}

// A shape token at p[0] with `avail` bytes from it. Returns the bytes it takes, or 0 when it is malformed or cut.
static size_t
dmr_lrrp_parse_shape(const uint8_t* p, size_t avail, const dmr_lrrp_shape* shape, dmr_lrrp_parse_result* r) {
    if (avail < 9U) {
        return 0U;
    }
    size_t used = 9U;
    dmr_lrrp_number values[3];
    static const uint8_t order[3] = {DMR_LRRP_FIELD_RADIUS, DMR_LRRP_FIELD_ALT, DMR_LRRP_FIELD_ALT_ACC};
    for (size_t f = 0; f < 3U; f++) {
        if ((shape->fields & order[f]) == 0U) {
            continue;
        }
        const int is_signed = order[f] == DMR_LRRP_FIELD_ALT;
        const size_t n = dmr_lrrp_read_number(p + used, avail - used, is_signed, shape->is_float, &values[f]);
        if (n == 0U) {
            return 0U;
        }
        used += n;
    }
    /* A shape at 0, 0 is kept only while no other shape gave a position (it is then refused as a position). */
    const uint8_t zero = (uint8_t)dmr_lrrp_zero_pair(dmr_lrrp_be32(p + 1), dmr_lrrp_be32(p + 5));
    if (!r->have_pos || zero < r->pos_zero || (zero == r->pos_zero && r->pos_rank > shape->rank)) {
        r->pos_rank = shape->rank;
        r->pos_zero = zero;
        r->lat_raw = dmr_lrrp_be32(p + 1);
        r->lon_raw = dmr_lrrp_be32(p + 5);
        r->have_pos = 1U;
        r->have_rad = (uint8_t)((shape->fields & DMR_LRRP_FIELD_RADIUS) != 0U);
        r->have_alt = (uint8_t)((shape->fields & DMR_LRRP_FIELD_ALT) != 0U);
        r->have_alt_acc = (uint8_t)((shape->fields & DMR_LRRP_FIELD_ALT_ACC) != 0U);
        r->radius = values[0];
        r->alt = values[1];
        r->alt_acc = values[2];
    }
    return used;
}

static uint16_t
dmr_lrrp_days_in_month(uint16_t year, uint16_t month) {
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2U && (year % 4U) == 0U && ((year % 100U) != 0U || (year % 400U) == 0U)) {
        return 29U;
    }
    return (month >= 1U && month <= 12U) ? days[month - 1U] : 0U;
}

// A calendar date and time that exists: day within its month, hours 0-23, minutes and seconds 0-59.
static int
dmr_pdu_datetime_valid(uint16_t year, uint16_t month, uint16_t day, uint16_t hour, uint16_t minute, uint16_t second) {
    return month >= 1U && month <= 12U && day >= 1U && day <= dmr_lrrp_days_in_month(year, month) && hour <= 23U
           && minute <= 59U && second <= 59U;
}

// Seconds since 1970-01-01 00:00:00 UTC of a valid date and time in 2000-2099.
static int64_t
dmr_pdu_unix_time(uint16_t year, uint16_t month, uint16_t day, uint16_t hour, uint16_t minute, uint16_t second) {
    int64_t days = 10957; // 1970-01-01 to 2000-01-01
    for (uint16_t y = 2000U; y < year; y++) {
        days += (dmr_lrrp_days_in_month(y, 2U) == 29U) ? 366 : 365;
    }
    for (uint16_t m = 1U; m < month; m++) {
        days += dmr_lrrp_days_in_month(year, m);
    }
    days += (int64_t)day - 1;
    return days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second;
}

// Token 0x34 (5 bytes after the id): year 14 bits, month 4, day 5, hour 5, minute 6, second 6, UTC. The first valid
// one counts; an impossible date (31 February included) or a year outside 2000-2037 is left out.
static void
dmr_lrrp_parse_timestamp_token(const uint8_t* p, dmr_lrrp_parse_result* r) {
    if (r->year != 0) {
        return;
    }
    const uint16_t year = (uint16_t)((p[1] << 6) + (p[2] >> 2));
    const uint16_t month = (uint16_t)(((p[2] & 0x3) << 2) + ((p[3] & 0xC0) >> 6));
    const uint16_t day = (uint16_t)((p[3] & 0x3E) >> 1);
    const uint16_t hour = (uint16_t)(((p[3] & 0x01) << 4) + ((p[4] & 0xF0) >> 4));
    const uint16_t minute = (uint16_t)(((p[4] & 0x0F) << 2) + ((p[5] & 0xC0) >> 6));
    const uint16_t second = (uint16_t)(p[5] & 0x3F);
    if (year < 2000U || year > 2037U || !dmr_pdu_datetime_valid(year, month, day, hour, minute, second)) {
        return;
    }
    r->year = year;
    r->month = month;
    r->day = day;
    r->hour = hour;
    r->minute = minute;
    r->second = second;
}

// Tokens of a fixed size whose content no output uses. Returns the size, or 0 for another token.
static size_t
dmr_lrrp_opaque_token_len(uint8_t token) {
    switch (token) {
        case 0x38:
        case 0x3A:
        case 0x42:
        case 0x57:
        case 0x62:
        case 0x64: return 1U;
        case 0x23:
        case 0x31:
        case 0x36:
        case 0x4A:
        case 0x61:
        case 0x73:
        case 0x78: return 2U;
        default: return 0U;
    }
}

// The motion tokens: horizontal direction (0x56, 2-degree steps; 180 or more is no direction), horizontal speed in
// m/s (0x6B integer, 0x6C float) and vertical speed (0x70, sfloatvar: real reports carry it; no output uses it). The
// first direction and the first horizontal speed count. Returns the bytes taken, 0 when the token is not one of them or
// is malformed or cut.
static size_t
dmr_lrrp_parse_motion_token(const uint8_t* p, size_t avail, dmr_lrrp_parse_result* r) {
    if (p[0] == 0x56) {
        if (avail < 2U) {
            return 0U;
        }
        if (!r->heading_seen) {
            r->heading_seen = 1U;
            r->heading_units = p[1];
            r->have_heading = (uint8_t)(p[1] < 180U);
        }
        return 2U;
    }
    if (p[0] != 0x6B && p[0] != 0x6C && p[0] != 0x70) {
        return 0U;
    }
    dmr_lrrp_number speed;
    const size_t n = dmr_lrrp_read_number(p + 1, avail - 1U, p[0] == 0x70, p[0] != 0x6B, &speed);
    if (n == 0U) {
        return 0U;
    }
    if (p[0] != 0x70 && !r->have_speed) {
        r->speed = speed;
        r->have_speed = 1U;
    }
    return n + 1U;
}

// The tokens that carry a value (request ID, time, result, level of confidence, motion). Returns the bytes taken, 0
// when the token is not one of them or is malformed or cut.
static size_t
dmr_lrrp_parse_value_token(const uint8_t* p, size_t avail, dmr_lrrp_parse_result* r) {
    uint32_t ignored = 0U;
    size_t n = 0U;
    switch (p[0]) {
        case 0x22: { // request ID: a uintvar length, then that many bytes
            uint32_t id_len = 0U;
            n = dmr_lrrp_read_uintvar(p + 1, avail - 1U, &id_len);
            return (n != 0U && (size_t)id_len <= avail - 1U - n) ? 1U + n + (size_t)id_len : 0U;
        }
        case 0x34:
            if (avail < 6U) {
                return 0U;
            }
            dmr_lrrp_parse_timestamp_token(p, r);
            return 6U;
        case 0x37: // result code
        case 0x65: // level of confidence (a percentage; real reports carry it)
            n = dmr_lrrp_read_uintvar(p + 1, avail - 1U, &ignored);
            return n == 0U ? 0U : n + 1U;
        default: return dmr_lrrp_parse_motion_token(p, avail, r);
    }
}

// One token at p[0] with `avail` (> 0) bytes from it: the bytes it takes, or 0 when the parse ends there (a token
// that is unknown, has no real sample, or is malformed or cut).
static size_t
dmr_lrrp_parse_token(const uint8_t* p, size_t avail, dmr_lrrp_parse_result* r) {
    const size_t opaque = dmr_lrrp_opaque_token_len(p[0]);
    if (opaque != 0U) {
        return opaque <= avail ? opaque : 0U;
    }
    const dmr_lrrp_shape* shape = dmr_lrrp_shape_for(p[0]);
    if (shape != NULL) {
        return dmr_lrrp_parse_shape(p, avail, shape, r);
    }
    return dmr_lrrp_parse_value_token(p, avail, r);
}

// Parse the tokens in pdu[start, start + len) in order until the end or a token that ends the parse.
static void
dmr_lrrp_parse_tokens(const uint8_t* pdu, size_t start, size_t len, dmr_lrrp_parse_result* r) {
    size_t idx = start;
    size_t remaining = len;
    while (remaining > 0U) {
        const size_t need = dmr_lrrp_parse_token(pdu + idx, remaining, r);
        if (need == 0U || need > remaining) {
            return;
        }
        idx += need;
        remaining -= need;
    }
}

// Whether a reading that skips one to DMR_LRRP_RESYNC_MAX_SKIP leading bytes finds a position the document's own
// token order did not. Such a reading is a guess: it is reported as one and never published.
static int
dmr_lrrp_resync_finds_position(const uint8_t* pdu, size_t start, size_t len) {
    for (size_t skip = 1U; skip <= DMR_LRRP_RESYNC_MAX_SKIP && skip < len; skip++) {
        dmr_lrrp_parse_result guess;
        dmr_lrrp_parse_result_init(&guess);
        dmr_lrrp_parse_tokens(pdu, start + skip, len - skip, &guess);
        if (guess.have_pos) {
            return 1;
        }
    }
    return 0;
}

static void
dmr_lrrp_classify_type(uint8_t lrrp_type, uint8_t* is_request, uint8_t* is_response) {
    *is_request = 0;
    *is_response = 0;
    switch (lrrp_type) {
        case 0x0F:
        case 0x05:
        case 0x09:
        case 0x14: *is_request = 1; break;
        case 0x07:
        case 0x0B:
        case 0x0D:
        case 0x11:
        case 0x13: // unsolicited location report
        case 0x15: *is_response = 1; break;
        default: break;
    }
}

// The document length after the type byte: a uintvar. Returns the bytes it takes (0 when it is malformed).
static size_t
dmr_lrrp_read_doc_len(const uint8_t* pdu, size_t avail, uint32_t* doc_len) {
    return avail > 1U ? dmr_lrrp_read_uintvar(pdu + 1, avail - 1U, doc_len) : 0U;
}

// Some senders wrap a conformant LRRP document in a short fixed envelope
// (#453): a leading byte dsd-neo does not classify, a counter and a few
// constant bytes, then an ordinary document whose length runs exactly to
// the end of the UDP payload. Returns the offset of that inner document, or 0
// when the payload holds no such document within the first few bytes. The
// exact-end check is what keeps this from firing on unrelated data.
static size_t
dmr_lrrp_wrapped_document_offset(const uint8_t* pdu, size_t avail) {
    const size_t max_prefix = 8u;
    for (size_t off = 1u; off <= max_prefix && off + 2u < avail; off++) {
        uint8_t is_request = 0;
        uint8_t is_response = 0;
        dmr_lrrp_classify_type(pdu[off], &is_request, &is_response);
        if (!is_request && !is_response) {
            continue;
        }
        uint32_t inner_len = 0U;
        const size_t hdr = dmr_lrrp_read_doc_len(pdu + off, avail - off, &inner_len);
        if (hdr != 0U && inner_len > 0u && off + 1u + hdr + (size_t)inner_len == avail) {
            return off;
        }
    }
    return 0u;
}

static int
dmr_lrrp_has_position_token(const uint8_t* pdu, size_t start, size_t token_len) {
    for (size_t i = 0; i < token_len; i++) {
        if (dmr_lrrp_shape_for(pdu[start + i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    uint32_t source;
    uint32_t dest;
    uint8_t slot;
    uint8_t is_request;
    uint8_t is_response;
    uint8_t lrrp_type;
    uint8_t pos_verified;         // the packet's own checks passed (not RAS)
    uint8_t guessed;              // only a resync guess found a position
    const char* suppressed_label; // why a decoded position is withheld
} dmr_lrrp_emit_context;

// The latitude (sign bit + 31-bit magnitude, x90/2^31) and longitude (two's complement, x360/2^32) in 1e-7 degree,
// rounded to the nearest unit. Every 32-bit value is in range: the magnitude tops out at 2^31-1, which is 90.
static void
dmr_lrrp_fill_coordinates(uint32_t lat_raw, uint32_t lon_raw, dsd_position_fix* fix) {
    const uint32_t lat_mag = lat_raw & 0x7FFFFFFFU;
    const int64_t lat_e7 = (int64_t)(((uint64_t)lat_mag * 900000000U + 0x40000000U) >> 31);
    fix->lat_e7 = (int32_t)(((lat_raw & 0x80000000U) != 0U) ? -lat_e7 : lat_e7);
    const int32_t lon_signed = (int32_t)lon_raw;
    const uint64_t lon_mag = (lon_signed < 0) ? (uint64_t)(-(int64_t)lon_signed) : (uint64_t)lon_signed;
    const int64_t lon_e7 = (int64_t)((lon_mag * 1800000000U + 0x40000000U) >> 31);
    fix->lon_e7 = (int32_t)((lon_signed < 0) ? -lon_e7 : lon_e7);
}

// Altitude, radius (the position error), speed and heading in the record's units; a value the record cannot hold
// stays unknown.
static void
dmr_lrrp_fill_extras(const dmr_lrrp_parse_result* best, dsd_position_fix* fix) {
    if (best->have_alt) {
        const uint64_t dm = dmr_lrrp_number_scaled(&best->alt, 10U);
        if (dm <= (uint64_t)INT32_MAX) {
            fix->alt_dm = best->alt.negative ? -(int32_t)dm : (int32_t)dm;
        }
    }
    if (best->have_rad && best->radius.whole < UINT32_MAX) {
        // A radius is an uncertainty: a fraction rounds up. A radius of 0 (an integer 0x53 radius, or 0.0) is
        // DSD_POSITION_ERR_UNKNOWN: the record has no "exact" error.
        fix->err_m = best->radius.whole + (uint32_t)(best->radius.frac_bytes != 0U && best->radius.frac != 0U);
    }
    if (best->have_speed) {
        const uint64_t dkmh = dmr_lrrp_number_scaled(&best->speed, 36U); // 1 m/s = 3.6 km/h
        if (dkmh < (uint64_t)DSD_POSITION_SPEED_UNKNOWN) {
            fix->speed_dkmh = (uint16_t)dkmh;
        }
    }
    if (best->have_heading) {
        fix->heading_ddeg = (uint16_t)(best->heading_units * 20U);
    }
}

// The carrier's protocol as a position record holds it.
static uint8_t
dmr_pdu_position_protocol(const dsd_state* state) {
    return (state->lastsynctype >= 0 && state->lastsynctype < (int)DSD_POSITION_PROTOCOL_NONE)
               ? (uint8_t)state->lastsynctype
               : (uint8_t)DSD_POSITION_PROTOCOL_NONE;
}

// The reception the packet carrying a position report began on (spec 2.3 step 3): the DMR data header's for a DMR
// packet (dmr_dheader()), the MDPU header's for a P25 Phase 1 one (processMPDU()). NULL when none was captured, which
// takes the current reception.
static const dsd_position_origin*
dmr_pdu_position_origin(const dsd_state* state, uint8_t slot) {
    const dsd_position_origin* origin = NULL;
    if (DSD_SYNC_IS_DMR(state->lastsynctype)) {
        origin = &state->dmr_data_origin[slot & 1U];
    } else if (DSD_SYNC_IS_P25P1(state->lastsynctype)) {
        origin = &state->p25_mdpu_origin;
    }
    return (origin != NULL && origin->valid != 0U) ? origin : NULL;
}

// The decoded document as a position report.
static void
dmr_lrrp_fill_fix(const dsd_state* state, const dmr_lrrp_parse_result* best, const dmr_lrrp_emit_context* ctx,
                  dsd_position_fix* fix) {
    dsd_position_fix_init(fix);
    fix->kind = DSD_POS_DMR_LRRP;
    fix->protocol = dmr_pdu_position_protocol(state);
    fix->slot = ctx->slot;
    dmr_lrrp_fill_coordinates(best->lat_raw, best->lon_raw, fix);
    dmr_lrrp_fill_extras(best, fix);
    if (best->year != 0U) {
        fix->fix_time_s = dmr_pdu_unix_time(best->year, best->month, best->day, best->hour, best->minute, best->second);
    }
    fix->source_id = ctx->source;
    fix->target_id = ctx->dest;
    if (dmr_pdu_header_target_is_group(state, ctx->slot, ctx->dest)) {
        fix->flags |= (uint16_t)DSD_POS_FLAG_TARGET_GROUP;
    }
}

// One `-L` row from an admitted report (LRRP and LOCN share the format): receive date and time, source, latitude,
// longitude, speed in km/h and heading in degrees (0 for an unknown speed or heading, the format's placeholder).
static void
dmr_pdu_write_location_row(const dsd_opts* opts, const dsd_position_fix* fix) {
    if (opts->lrrp_file_output != 1) {
        return;
    }
    char timestr[9];
    char datestr[11];
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_DATE_SLASH, datestr, sizeof datestr);
    FILE* pFile = dsd_fopen_private(opts->lrrp_out_file, "a");
    if (pFile == NULL) {
        return;
    }
    const double speed_kmh = (fix->speed_dkmh != DSD_POSITION_SPEED_UNKNOWN) ? (double)fix->speed_dkmh / 10.0 : 0.0;
    const int heading = (fix->heading_ddeg != DSD_POSITION_HEADING_UNKNOWN) ? (int)(fix->heading_ddeg / 10U) : 0;
    DSD_FPRINTF(pFile, "%s\t%s\t", datestr, timestr);
    DSD_FPRINTF(pFile, "%08u\t", (unsigned)fix->source_id);
    DSD_FPRINTF(pFile, "%.5lf\t", (double)fix->lat_e7 / 10000000.0);
    DSD_FPRINTF(pFile, "%.5lf\t", (double)fix->lon_e7 / 10000000.0);
    DSD_FPRINTF(pFile, "%.3lf\t ", speed_kmh);
    DSD_FPRINTF(pFile, "%d\t", heading);
    DSD_FPRINTF(pFile, "\n");
    fclose(pFile);
}

static void
dmr_lrrp_print_details(const dmr_lrrp_parse_result* best, const dsd_position_fix* fix, int admitted,
                       const dmr_lrrp_emit_context* ctx) {
    /* Each line is a part of the report, so each carries the producer's label (issue #800, D10): the diagnostics log
       removes them. */
    if (best->year) {
        DSD_FPRINTF(stderr, "\n LRRP Time: %04d.%02d.%02d %02d:%02d:%02d", best->year, best->month, best->day,
                    best->hour, best->minute, best->second);
    }
    if (admitted) {
        const double lat = (double)fix->lat_e7 / 10000000.0;
        const double lon = (double)fix->lon_e7 / 10000000.0;
        DSD_FPRINTF(stderr, "\n Lat: %.5lf Lon: %.5lf (%.5lf, %.5lf)", lat, lon, lat, lon);
    } else if (best->have_pos || ctx->guessed) {
        DSD_FPRINTF(stderr, "\n Position: (suppressed; %s)", ctx->suppressed_label);
    }
    if (best->have_rad) {
        DSD_FPRINTF(stderr, "\n LRRP Radius: %.2lfm", dmr_lrrp_number_value(&best->radius));
    }
    if (best->have_alt) {
        DSD_FPRINTF(stderr, "\n Altitude: %.2lfm", dmr_lrrp_number_value(&best->alt));
    }
    if (best->have_alt_acc) {
        DSD_FPRINTF(stderr, "\n LRRP Alt Accuracy: %.2lfm", dmr_lrrp_number_value(&best->alt_acc));
    }
    if (best->have_speed) {
        const double mps = dmr_lrrp_number_value(&best->speed);
        DSD_FPRINTF(stderr, "\n LRRP Speed: %.2lf km/h %.2lf m/s", mps * 3.6, mps);
    }
    if (best->have_heading) {
        DSD_FPRINTF(stderr, "\n LRRP Track: %d%s", (int)best->heading_units * 2, dsd_degrees_glyph());
    }
}

static void
dmr_lrrp_build_summary(char* out, size_t out_sz, const dmr_lrrp_parse_result* best, const dsd_position_fix* fix,
                       int admitted, const dmr_lrrp_emit_context* ctx) {
    const unsigned source = ctx->source;
    const unsigned dest = ctx->dest;
    if (admitted) {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %u; (%lf, %lf)", source, (double)fix->lat_e7 / 10000000.0,
                     (double)fix->lon_e7 / 10000000.0);
    } else if (best->have_pos || ctx->guessed) {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %u; Position suppressed (%s);", source, ctx->suppressed_label);
    } else if (ctx->is_request) {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %u; Request from TGT: %u;", source, dest);
    } else if (ctx->is_response) {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %u; Response to TGT: %u;", source, dest);
    } else {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %u; Unknown Format %02X; TGT: %u;", source, ctx->lrrp_type, dest);
    }
}

// A zero-length message reaches here with an empty `best`: no details print,
// nothing is written to the LRRP file, and the summary is chosen by the type
// byte alone (sdrtrunk and lrrpdec.py label it the same way, #453).
//
// The position goes on only when the packet's checks passed and the document's own token order gave it: admitted, it
// is staged for the packet's data notice (the row's GPS) and written to `-L`; otherwise the summary says it was
// withheld and why.
static void
dmr_lrrp_emit_payload(const dsd_opts* opts, dsd_state* state, const dmr_lrrp_parse_result* best,
                      dmr_lrrp_emit_context* ctx) {
    const uint8_t slot = ctx->slot;
    dsd_position_fix fix;
    dsd_position_fix_init(&fix);
    dsd_position_admit_result admitted = DSD_POSITION_REFUSED;
    if (best->have_pos && ctx->pos_verified) {
        dmr_lrrp_fill_fix(state, best, ctx, &fix);
        // The same checked packet names the source and the target.
        admitted = dsd_position_admit(opts, state, &fix, 1, 1, dmr_pdu_position_origin(state, slot));
        if (admitted == DSD_POSITION_REFUSED) {
            ctx->suppressed_label = "not usable";
        }
    }
    DSD_FPRINTF(stderr, "%s", KYEL);
    dmr_lrrp_print_details(best, &fix, admitted != DSD_POSITION_REFUSED, ctx);

    char lrrpstr[100];
    char velstr[20];
    char degstr[20];
    lrrpstr[0] = '\0';
    velstr[0] = '\0';
    degstr[0] = '\0';
    dmr_lrrp_build_summary(lrrpstr, sizeof lrrpstr, best, &fix, admitted != DSD_POSITION_REFUSED, ctx);
    if (best->have_speed) {
        DSD_SNPRINTF(velstr, sizeof velstr, " %.2lf km/h", dmr_lrrp_number_value(&best->speed) * 3.6);
    }
    if (best->have_heading) {
        DSD_SNPRINTF(degstr, sizeof degstr, " %d%s  ", (int)best->heading_units * 2, dsd_degrees_glyph());
    }
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "%s%s%s", lrrpstr, velstr, degstr);
    if (admitted == DSD_POSITION_REFUSED) {
        DSD_FPRINTF(stderr, "\n %s", state->dmr_lrrp_gps[slot]);
        return;
    }
    (void)dsd_event_stage_position(state, slot, &fix, state->dmr_lrrp_gps[slot]);
    dmr_pdu_write_location_row(opts, &fix);
}

// Why a position is withheld before admission: the packet's checks (a RAS-accepted header alone is not a failed
// check), else a reading only a resync guess found.
static const char*
dmr_lrrp_suppressed_label(const dsd_state* state, uint8_t slot, uint8_t pos_verified) {
    if (!pos_verified) {
        const uint8_t scope = state->event_crc_invalid[slot];
        return (!dsd_crc_is_failed(scope) && (scope & DSD_CRC_RAS_UNVERIFIED) != 0U) ? "RAS" : "CRC ERR";
    }
    return "layout not confirmed";
}

void
dmr_lrrp(const dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, const uint8_t* DMR_PDU,
         uint8_t pdu_crc_ok) {
    if (!DMR_PDU || len < 2) {
        return;
    }
    const uint8_t slot = (uint8_t)(state->currentslot & 1);
    uint8_t lrrp_type = DMR_PDU[0];
    uint8_t is_request = 0;
    uint8_t is_response = 0;
    dmr_lrrp_classify_type(lrrp_type, &is_request, &is_response);
    if (!is_request && !is_response) {
        size_t off = dmr_lrrp_wrapped_document_offset(DMR_PDU, (size_t)len);
        if (off > 0u) {
            DSD_FPRINTF(stderr, "\n Wrapped LRRP: outer type %02X, %u byte prefix;", lrrp_type, (unsigned)off);
            DMR_PDU += off;
            len = (uint16_t)(len - off);
            lrrp_type = DMR_PDU[0];
            dmr_lrrp_classify_type(lrrp_type, &is_request, &is_response);
        }
    }

    uint32_t doc_len = 0U;
    const size_t hdr = dmr_lrrp_read_doc_len(DMR_PDU, (size_t)len, &doc_len);
    const size_t start = 1u + hdr;
    const size_t token_avail = (hdr != 0U && (size_t)len > start) ? (size_t)len - start : 0u;
    const size_t token_len = ((size_t)doc_len < token_avail) ? (size_t)doc_len : token_avail;

    dmr_lrrp_parse_result best;
    dmr_lrrp_parse_result_init(&best);
    int want_response_parse = is_response;
    if (!want_response_parse && !is_request && token_len > 0u) {
        want_response_parse = dmr_lrrp_has_position_token(DMR_PDU, start, token_len);
    }
    uint8_t guessed = 0U;
    if (want_response_parse) {
        dmr_lrrp_parse_tokens(DMR_PDU, start, token_len, &best);
        guessed = (uint8_t)(!best.have_pos && dmr_lrrp_resync_finds_position(DMR_PDU, start, token_len));
    }
    // The source is the caller's: every caller passes the source its notice observation names (IPv4 source address,
    // MNIS or compressed-header data header source), so the record and its notice agree even when it is 0.
    const uint8_t pos_verified = (uint8_t)(pdu_crc_ok == 1U);
    dmr_lrrp_emit_context emit_ctx = {
        .source = source,
        .dest = dest,
        .slot = slot,
        .is_request = is_request,
        .is_response = is_response,
        .lrrp_type = lrrp_type,
        .pos_verified = pos_verified,
        .guessed = guessed,
        .suppressed_label = dmr_lrrp_suppressed_label(state, slot, pos_verified),
    };
    dmr_lrrp_emit_payload(opts, state, &best, &emit_ctx);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

// DMR "LOCN": a text location report reached through MNIS type 0x01 and defined short data. No specification
// defines it; the decoder is dsd-fme's, written from observed traffic. The fields are `A` hhmmss DDMMYY, `N`/`S`
// DDMM.mmmm and `E`/`W` DDDMM.mmmm. Its reports are published as provisional (decision D12). Every field is checked
// before it is used: digits only, a date that exists, degrees within 90/180 (the boundary only with zero minutes),
// minutes below 60.
typedef struct {
    uint8_t time; // a valid date and time was read
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t year; // the full year (dsd_gps_two_digit_year_date()), with `time`
    uint8_t month;
    uint8_t day;
    int64_t days;      // the date's days since 1970-01-01, with `time`
    uint8_t lat;       // a latitude field was read
    uint8_t lon;       // a longitude field was read
    uint8_t lat_valid; // and its value is in range
    uint8_t lon_valid;
    int32_t lat_e7;
    int32_t lon_e7;
} dmr_locn_data;

// `n` ASCII digits as a number; 0 when any of them is not a digit.
static int
dmr_locn_digits(const uint8_t* p, size_t n, uint32_t* out) {
    uint32_t value = 0U;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return 0;
        }
        value = value * 10U + (uint32_t)(p[i] - '0');
    }
    *out = value;
    return 1;
}

// `A` hhmmss DDMMYY at p[0]. Returns 1 when the twelve characters are digits (the field is a time field), and sets
// the time only when the date and time exist. The two-digit year is read in the century that puts the date closest to
// its reception on the decode clock (as an NMEA RMC's).
static int
dmr_locn_parse_time(const uint8_t* p, dmr_locn_data* d) {
    uint32_t v[6];
    for (size_t i = 0; i < 6U; i++) {
        if (!dmr_locn_digits(p + 1 + i * 2U, 2U, &v[i])) {
            return 0;
        }
    }
    int64_t year = 0;
    d->time = (uint8_t)(v[0] <= 23U && v[1] <= 59U && v[2] <= 59U
                        && dsd_gps_two_digit_year_date(v[5], v[4], v[3], (int64_t)dsd_decode_time(), &year, &d->days));
    d->hour = (uint8_t)v[0];
    d->minute = (uint8_t)v[1];
    d->second = (uint8_t)v[2];
    d->day = (uint8_t)v[3];
    d->month = (uint8_t)v[4];
    d->year = d->time ? (uint16_t)year : (uint16_t)(2000U + v[5]);
    return 1;
}

// Whether any of the digits that run on from p[0] (at most `avail` bytes) is not 0: fraction digits past the four
// the format keeps.
static int
dmr_locn_more_digits_nonzero(const uint8_t* p, size_t avail) {
    for (size_t i = 0; i < avail && p[i] >= '0' && p[i] <= '9'; i++) {
        if (p[i] != '0') {
            return 1;
        }
    }
    return 0;
}

// One coordinate: `deg_digits` degree digits, two minute digits, the '.' separator, four digits of 1/10000 minute,
// after the hemisphere letter at p[0], with `avail` bytes from p[0]. Returns 1 when they have that shape (the field is
// a coordinate field); *valid says whether the value is in range (degrees <= max_deg, minutes <= 59, the boundary only
// with zero minutes and a fraction whose every digit is 0, any that run on past the four included).
static int
dmr_locn_parse_coord(const uint8_t* p, size_t avail, size_t deg_digits, uint32_t max_deg, int negative, int32_t* e7,
                     uint8_t* valid) {
    uint32_t deg = 0U;
    uint32_t min = 0U;
    uint32_t frac = 0U;
    const size_t end = 8U + deg_digits;
    if (avail < end || !dmr_locn_digits(p + 1, deg_digits, &deg) || !dmr_locn_digits(p + 1 + deg_digits, 2U, &min)
        || p[3 + deg_digits] != (uint8_t)'.' || !dmr_locn_digits(p + 4 + deg_digits, 4U, &frac)) {
        return 0;
    }
    const int boundary_ok = min == 0U && frac == 0U && !dmr_locn_more_digits_nonzero(p + end, avail - end);
    *valid = (uint8_t)(min <= 59U && (deg < max_deg || (deg == max_deg && boundary_ok)));
    if (*valid) {
        // degrees + (minutes + frac / 10000) / 60, in 1e-7 degree, rounded
        const int64_t value = (int64_t)deg * 10000000 + ((int64_t)(min * 10000U + frac) * 100 + 3) / 6;
        *e7 = (int32_t)(negative ? -value : value);
    }
    return 1;
}

// Returns the characters a field at pdu[i] takes, or 1 when pdu[i] starts no field.
static uint16_t
dmr_locn_parse_field(const uint8_t* pdu, uint16_t len, uint16_t i, dmr_locn_data* d) {
    const uint8_t c = pdu[i];
    if (c == 0x41 && (uint16_t)(i + 12U) < len) {
        return dmr_locn_parse_time(pdu + i, d) ? 13U : 1U;
    }
    if ((c == 0x4E || c == 0x53) && (uint16_t)(i + 9U) < len) {
        if (!dmr_locn_parse_coord(pdu + i, (size_t)(len - i), 2U, 90U, c == 0x53, &d->lat_e7, &d->lat_valid)) {
            return 1U;
        }
        d->lat = 1;
        return 10U;
    }
    if ((c == 0x45 || c == 0x57) && (uint16_t)(i + 10U) < len) {
        if (!dmr_locn_parse_coord(pdu + i, (size_t)(len - i), 3U, 180U, c == 0x57, &d->lon_e7, &d->lon_valid)) {
            return 1U;
        }
        d->lon = 1;
        return 11U;
    }
    return 1U;
}

static void
dmr_locn_parse_fields(const uint8_t* pdu, uint16_t len, dmr_locn_data* d) {
    uint16_t i = 0;
    while (i < len) {
        i = (uint16_t)(i + dmr_locn_parse_field(pdu, len, i, d));
    }
}

static void
dmr_locn_emit(dsd_state* state, uint8_t slot, uint32_t source, const dmr_locn_data* d, const dsd_position_fix* fix,
              const char* withheld) {
    const char* deg_glyph = dsd_degrees_glyph();
    char locnstr[50];
    char posstr[100];
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n NMEA / LOCN; Source: %u;", (unsigned)source);
    if (d->time) {
        DSD_FPRINTF(stderr, " %04u/%02u/%02u %02u:%02u:%02u", d->year, d->month, d->day, d->hour, d->minute, d->second);
    }
    DSD_SNPRINTF(locnstr, sizeof locnstr, "NMEA / LOCN; Source: %u ", (unsigned)source);
    if (withheld == NULL) {
        const double lat = (double)fix->lat_e7 / 10000000.0;
        const double lon = (double)fix->lon_e7 / 10000000.0;
        DSD_FPRINTF(stderr, " (%.5lf%s, %.5lf%s);", lat, deg_glyph, lon, deg_glyph);
        DSD_SNPRINTF(posstr, sizeof posstr, "(%.5lf%s, %.5lf%s)", lat, deg_glyph, lon, deg_glyph);
    } else {
        DSD_FPRINTF(stderr, " Position suppressed (%s);", withheld);
        DSD_SNPRINTF(posstr, sizeof posstr, "Position suppressed (%s);", withheld);
    }
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "%s%s", locnstr, posstr);
}

// The report a checked, in-range LOCN gives: its coordinates, its date and time when the `A` field held a valid one
// (read as UTC, the NMEA convention the format follows; unknown otherwise), the data header's source and target.
static void
dmr_locn_fill_fix(const dsd_state* state, uint8_t slot, uint32_t source, const dmr_locn_data* d,
                  dsd_position_fix* fix) {
    fix->kind = DSD_POS_DMR_LOCN;
    fix->protocol = dmr_pdu_position_protocol(state);
    fix->slot = slot;
    fix->lat_e7 = d->lat_e7;
    fix->lon_e7 = d->lon_e7;
    if (d->time) {
        fix->fix_time_s =
            (d->days * 86400) + ((int64_t)d->hour * 3600) + ((int64_t)d->minute * 60) + (int64_t)d->second;
    }
    fix->flags = (uint16_t)DSD_POS_FLAG_FORMAT_PROVISIONAL;
    fix->source_id = source;
    fix->target_id = (uint32_t)state->dmr_lrrp_target[slot];
    if (DSD_SYNC_IS_DMR(state->lastsynctype) && state->dmr_data_checked_group[slot] != 0U) {
        fix->flags |= (uint16_t)DSD_POS_FLAG_TARGET_GROUP;
    }
}

void
dmr_locn(const dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU) {
    if (DMR_PDU == NULL) {
        return;
    }
    const uint8_t slot = (uint8_t)(state->currentslot & 1);
    const uint32_t source = (uint32_t)state->dmr_lrrp_source[slot];
    dmr_locn_data d;
    DSD_MEMSET(&d, 0, sizeof(d));
    dmr_locn_parse_fields(DMR_PDU, len, &d);
    if (!(d.lat && d.lon)) {
        return;
    }

    const uint8_t scope = state->event_crc_invalid[slot];
    const char* withheld = NULL;
    dsd_position_fix fix;
    dsd_position_fix_init(&fix);
    if (scope != 0U) {
        // The failed check is the reason even when a damaged field is also out of range.
        withheld = (!dsd_crc_is_failed(scope) && (scope & DSD_CRC_RAS_UNVERIFIED) != 0U) ? "RAS" : "CRC ERR";
    } else if (!(d.lat_valid && d.lon_valid)) {
        withheld = "invalid";
    } else {
        dmr_locn_fill_fix(state, slot, source, &d, &fix);
        // The packet's checks passed; its data header names the source and the target.
        if (dsd_position_admit(opts, state, &fix, 1, 1, dmr_pdu_position_origin(state, slot)) == DSD_POSITION_REFUSED) {
            withheld = "not usable";
        }
    }
    dmr_locn_emit(state, slot, source, &d, &fix, withheld);
    if (withheld == NULL) {
        (void)dsd_event_stage_position(state, slot, &fix, state->dmr_lrrp_gps[slot]);
        dmr_pdu_write_location_row(opts, &fix);
    }
}

static inline void
dsd_append(char* dst, size_t dstsz, const char* src) {
    if (!dst || !src || dstsz == 0) {
        return;
    }
    size_t len = strlen(dst);
    if (len >= dstsz) {
        return;
    }
    DSD_SNPRINTF(dst + len, dstsz - len, "%s", src);
}
