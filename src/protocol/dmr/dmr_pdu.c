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
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/unicode.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dmr_pdu_internal.h"
#include "dmr_text.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

static inline void dsd_append(char* dst, size_t dstsz, const char* src);

//convert a value that is stored as a string decimal into a decimal uint16_t
static uint16_t
convert_hex_to_dec(uint16_t input) {
    char num_str[10];
    int n = DSD_SNPRINTF(num_str, sizeof(num_str), "%X", input);
    if (n <= 0 || (size_t)n >= sizeof(num_str)) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(num_str, &end, 10);
    if (errno != 0 || end == num_str || *end != '\0' || value > 0xFFFFUL) {
        return 0;
    }
    return (uint16_t)value;
}

static void DSD_ATTR_USED
utf16_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    uint8_t slot = state->currentslot;
    dsd_event_history_transaction transaction;
    if (wr == 1) {
        dsd_event_history_transaction_begin(state, &transaction);
        DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].text_message,
                     sizeof(state->event_history_s[slot].Event_History_Items[0].text_message), "%s",
                     ""); //full text string
    }
    for (uint16_t i = 0; (uint16_t)(i + 1U) < len; i += 2) {
        uint16_t ch16 = (uint16_t)input[i + 0];
        ch16 <<= 8;
        ch16 |= (uint16_t)input[i + 1];

        if (ch16 >= 0x20 && ch16 != 0x040D) { // If not a linebreak or terminal commmands
            if (dsd_unicode_supported()) {
                DSD_FPRINTF(stderr, "%lc", ch16);
            } else {
                /* best-effort ASCII: print low byte if printable */
                unsigned char lo = (unsigned char)(ch16 & 0xFF);
                if (lo >= 0x20 && lo < 0x7F) {
                    fputc((int)lo, stderr);
                } else {
                    fputc('?', stderr);
                }
            }
        } else if (ch16 == 0) { // If padding (0 could also indicate end of text terminator?)
            DSD_FPRINTF(stderr, "_");
        } else if (ch16 == 0x040D) { //Ѝ or 0x040D may be ETLF
            DSD_FPRINTF(stderr, " / ");
        } else {
            DSD_FPRINTF(stderr, "-");
        }

        //convert to ascii range (will break eastern langauge, but can't do much about that right now)
        char c[2];
        c[0] = (char)input[i + 1];
        c[1] = 0;

        //short version (disabled)

        //this is the long version, complete message for logging purposes
        if (wr == 1 && input[i] == 0 && input[i + 1] < 0x7F && input[i + 1] >= 0x20) {
            dsd_append(state->event_history_s[slot].Event_History_Items[0].text_message,
                       sizeof state->event_history_s[slot].Event_History_Items[0].text_message, c);
        }
    }

    //add elipses to indicate this is possibly truncated

    //debug
    if (wr == 1) {
        dsd_event_history_mark_dirty(&state->event_history_s[slot]);
        dsd_event_history_transaction_end(&transaction);
    }
}

void
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    uint8_t slot = state->currentslot;
    DSD_FPRINTF(stderr, "\n UTF8 Text: ");

    dsd_event_history_transaction transaction;
    if (wr == 1) {
        dsd_event_history_transaction_begin(state, &transaction);
        DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].text_message,
                     sizeof(state->event_history_s[slot].Event_History_Items[0].text_message), "%s",
                     ""); //full text string
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
            dsd_append(state->event_history_s[slot].Event_History_Items[0].text_message,
                       sizeof state->event_history_s[slot].Event_History_Items[0].text_message, c_str);
        }
    }

    //add elipses to indicate this is possibly truncated
    if (wr == 1) {
        dsd_event_history_mark_dirty(&state->event_history_s[slot]);
        dsd_event_history_transaction_end(&transaction);
    }
}

static void
dmr_sd_pdu_store_text(dsd_state* state, uint8_t slot, const char* text) {
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    Event_History* item = &state->event_history_s[slot].Event_History_Items[0];
    DSD_SNPRINTF(item->text_message, sizeof(item->text_message), "%s", text != NULL ? text : "");
    dsd_event_history_item_set_metadata(item, DSD_EVENT_SEVERITY_INFO, DSD_EVENT_CATEGORY_DATA);
    dsd_event_history_mark_dirty(&state->event_history_s[slot]);
    dsd_event_history_transaction_end(&transaction);
}

static void
dmr_sd_pdu_print_raw(const uint8_t* dmr_pdu, uint16_t len) {
    DSD_FPRINTF(stderr, "\n Short Data Raw: ");
    for (uint16_t i = 0; i < len; i++) {
        DSD_FPRINTF(stderr, "[%02X]", dmr_pdu[i]);
    }
}

static void
dmr_sd_pdu_copy_location(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint16_t len, const uint8_t* dmr_pdu) {
    dmr_locn(opts, state, len, dmr_pdu);
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].gps_s,
                 sizeof(state->event_history_s[slot].Event_History_Items[0].gps_s), "%s", state->dmr_lrrp_gps[slot]);
    dsd_event_history_item_set_metadata(&state->event_history_s[slot].Event_History_Items[0], DSD_EVENT_SEVERITY_INFO,
                                        DSD_EVENT_CATEGORY_DATA);
    dsd_event_history_mark_dirty(&state->event_history_s[slot]);
    dsd_event_history_transaction_end(&transaction);
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
            dmr_sd_pdu_print_raw(dmr_pdu, len);
            dmr_sd_pdu_copy_location(opts, state, slot, len, dmr_pdu);
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

void
dmr_sd_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU) {
    /* Public compatibility entry point: strict decoding only, with no CRC-proven transmitter workaround. */
    dmr_sd_pdu_process(opts, state, len, DMR_PDU, 0U);
}

//reading ETSI, seems like these aren't compressed, just that they are preset indexed values on the radio
static const char*
dmr_udp_comp_src_idx_desc(uint16_t said) {
    if (said == 0) {
        return "Radio Network";
    }
    if (said == 1) {
        return "Ethernet";
    }
    if (said < 11) {
        return "Reserved";
    }
    return "Manufacturer Specific";
}

static const char*
dmr_udp_comp_dst_idx_desc(uint16_t daid) {
    if (daid == 0) {
        return "Radio Network";
    }
    if (daid == 1) {
        return "Ethernet";
    }
    if (daid == 2) {
        return "Group Network";
    }
    if (daid < 11) {
        return "Reserved";
    }
    return "Manufacturer Specific";
}

static const char*
dmr_udp_comp_port_idx_desc(uint16_t pid) {
    if (pid == 1) {
        return "UTF-16BE Text Message";
    }
    if (pid == 2) {
        return "Location Interface Protocol";
    }
    if (pid < 191) {
        return "Reserved";
    }
    return "Manufacturer Specific";
}

static uint16_t
dmr_udp_comp_resolve_port_ptr(const uint8_t* pdu, uint16_t len, uint16_t* spid, uint16_t* dpid) {
    uint16_t ptr = 5;
    if (*spid == 0 && *dpid == 0) {
        if (len < 9) {
            return len;
        }
        *spid = (uint16_t)((pdu[5] << 8) | pdu[6]);
        *dpid = (uint16_t)((pdu[7] << 8) | pdu[8]);
        ptr = 9;
    } else if (*spid == 0) {
        if (len < 7) {
            return len;
        }
        *spid = (uint16_t)((pdu[5] << 8) | pdu[6]);
        ptr = 7;
    } else if (*dpid == 0) {
        if (len < 7) {
            return len;
        }
        *dpid = (uint16_t)((pdu[5] << 8) | pdu[6]);
        ptr = 7;
    }
    return ptr;
}

static void DSD_ATTR_USED
dmr_udp_comp_decode_payload(const dsd_opts* opts, dsd_state* state, uint16_t spid, uint16_t dpid, uint16_t len,
                            uint16_t ptr, const uint8_t* pdu) {
    if (len <= ptr) {
        return;
    }
    len -= ptr;
    if (spid == 1 || dpid == 1) {
        utf16_to_text(state, 1, len, pdu + ptr); //assumming text starts right at the ptr value
        return;
    }
    if (spid == 2 || dpid == 2) {
        uint8_t bits[127 * 8];
        uint16_t decode_len = len;
        if (decode_len > 127U) {
            decode_len = 127U;
        }
        DSD_MEMSET(bits, 0, sizeof(bits));
        unpack_byte_array_into_bit_array(pdu + ptr, bits, (int)decode_len);
        lip_protocol_decoder(opts, state, bits);
        return;
    }
    DSD_FPRINTF(stderr, "Unknown Decode Format;");
}

void
dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU) {
    if (DMR_PDU == NULL || len < 5U) {
        return;
    }
    uint16_t ipid = (uint16_t)((DMR_PDU[0] << 8) | DMR_PDU[1]);
    uint16_t said = (uint16_t)((DMR_PDU[2] >> 4) & 0xF);
    uint16_t daid = (uint16_t)((DMR_PDU[2] >> 0) & 0xF);
    uint8_t op1 = (uint8_t)((DMR_PDU[3] >> 7) & 1);
    uint8_t op2 = (uint8_t)((DMR_PDU[4] >> 7) & 1);
    uint8_t opcode = (uint8_t)((op1 << 1) | op2);
    uint16_t spid = (uint16_t)((DMR_PDU[3] >> 0) & 0x7F);
    uint16_t dpid = (uint16_t)((DMR_PDU[4] >> 0) & 0x7F);
    uint16_t ptr = dmr_udp_comp_resolve_port_ptr(DMR_PDU, len, &spid, &dpid);

    const char* src_idx_desc = dmr_udp_comp_src_idx_desc(said);
    const char* dst_idx_desc = dmr_udp_comp_dst_idx_desc(daid);
    const char* src_port_desc = dmr_udp_comp_port_idx_desc(spid);
    const char* dst_port_desc = dmr_udp_comp_port_idx_desc(dpid);

    DSD_FPRINTF(stderr, "\n Compressed IP Idx: %d; Opcode: %d; Src Idx: %d (%s); Dst Idx: %d (%s); ", ipid, opcode,
                said, src_idx_desc, daid, dst_idx_desc);
    DSD_FPRINTF(stderr, "\n Src Port Idx: %d (%s); Dst Port Idx: %d (%s); ", spid, src_port_desc, dpid, dst_port_desc);

    dmr_udp_comp_decode_payload(opts, state, spid, dpid, len, ptr, DMR_PDU);

    uint8_t slot = (state->currentslot == 1) ? 1 : 0;
    char comp_string[500];
    DSD_MEMSET(comp_string, 0, sizeof(comp_string));
    DSD_SNPRINTF(comp_string, sizeof(comp_string), "IPC: %d; OP: %d; SRC: %d:%d (%s):(%s); DST: %d:%d (%s):(%s); ",
                 ipid, opcode, said, spid, src_idx_desc, src_port_desc, daid, dpid, dst_idx_desc, dst_port_desc);
    const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, slot, said, daid);
    (void)dsd_event_emit_data_notice(opts, state, slot, &observation, comp_string);
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

static int DSD_ATTR_USED
decode_ip_pdu_parse_udp_tms_address(dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                    uint16_t payload_len, uint8_t* payload, int* tms_ptr) {
    uint8_t tms_adl = payload[(*tms_ptr)++];
    if (tms_adl == 0) {
        return 0;
    }

    (*tms_ptr)--;
    if (tms_adl < 4U || (size_t)(*tms_ptr) + (size_t)tms_adl >= (size_t)payload_len) {
        return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
    }
    payload[*tms_ptr] = 0;
    DSD_FPRINTF(stderr, "Address Len: %d; Address: ", tms_adl);
    utf16_to_text(state, 1, tms_adl - 4, payload + *tms_ptr);
    payload[*tms_ptr] = tms_adl;
    *tms_ptr += tms_adl;
    *tms_ptr += 1;
    DSD_FPRINTF(stderr, "; ");
    return 0;
}

static int DSD_ATTR_USED
decode_ip_pdu_skip_udp_tms_extensions(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24,
                                      uint32_t dst24, uint16_t payload_len, const uint8_t* payload, int* tms_ptr) {
    if ((size_t)*tms_ptr >= (size_t)payload_len) {
        return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
    }

    uint8_t tms_more = payload[*tms_ptr] >> 7;
    while (tms_more) {
        if ((size_t)*tms_ptr >= (size_t)payload_len) {
            return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
        }
        uint8_t tms_b1 = payload[(*tms_ptr)++];
        if (opts->payload == 1) {
            if ((size_t)*tms_ptr >= (size_t)payload_len) {
                return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
            }
            uint8_t tms_b2 = payload[*tms_ptr];
            DSD_FPRINTF(stderr, "B1: %02X; B2: %02X; ", tms_b1, tms_b2);
        }
        tms_more = tms_b1 >> 7;
        if (tms_more) {
            if ((size_t)*tms_ptr >= (size_t)payload_len) {
                return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
            }
            (*tms_ptr)++;
        }
    }
    return 0;
}

static int DSD_ATTR_USED
decode_ip_pdu_prepare_tms_text_span(dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                    uint16_t payload_len, int* tms_ptr, int* tms_len) {
    if ((*tms_ptr % 2) == 0) {
        (*tms_ptr)++;
    }
    if (*tms_len > 3) {
        int consumed = *tms_ptr - 3;
        if (consumed >= *tms_len) {
            return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
        }
        *tms_len -= consumed;
    }
    *tms_ptr -= 2;
    if (*tms_ptr < 0 || (size_t)*tms_ptr >= (size_t)payload_len) {
        return decode_ip_pdu_tms_truncated(state, slot, src24, dst24);
    }
    if ((size_t)*tms_len > ((size_t)payload_len - (size_t)*tms_ptr)) {
        *tms_len = (int)((size_t)payload_len - (size_t)*tms_ptr);
    }
    return 0;
}

static void DSD_ATTR_USED
decode_ip_pdu_handle_udp_tms(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                             uint16_t payload_len, uint8_t* payload) {
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

    int tms_ptr = 2;
    uint8_t tms_hdr = payload[tms_ptr++];
    uint8_t tms_ack = (tms_hdr >> 0) & 0xF;
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "HDR: %02X; ", tms_hdr);
    }
    if (decode_ip_pdu_parse_udp_tms_address(state, slot, src24, dst24, payload_len, payload, &tms_ptr) != 0
        || decode_ip_pdu_skip_udp_tms_extensions(opts, state, slot, src24, dst24, payload_len, payload, &tms_ptr)
               != 0) {
        return;
    }

    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "TMS SRC: %d; DST: %d; ", src24, dst24);
    if (tms_ack != 0) {
        dsd_append(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "Acknowledgment;");
        DSD_FPRINTF(stderr, "Acknowledgment;");
        return;
    }

    if (decode_ip_pdu_prepare_tms_text_span(state, slot, src24, dst24, payload_len, &tms_ptr, &tms_len) != 0) {
        return;
    }
    uint8_t temp = payload[tms_ptr];
    payload[tms_ptr] = 0;
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "Ptr: %d; Len: %d;", tms_ptr, tms_len);
    }
    DSD_FPRINTF(stderr, "\n Text: ");
    utf16_to_text(state, 1, tms_len, payload + tms_ptr);
    payload[tms_ptr] = temp;
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

static int DSD_ATTR_USED
decode_ip_pdu_handle_udp_service_core(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                      uint16_t port, uint16_t payload_len, uint8_t* payload) {
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
            dmr_lrrp(opts, state, payload_len, src24, dst24, payload, 1);
            dsd_event_history_transaction transaction;
            dsd_event_history_transaction_begin(state, &transaction);
            dsd_event_history_item_set_metadata(&state->event_history_s[slot].Event_History_Items[0],
                                                DSD_EVENT_SEVERITY_INFO, DSD_EVENT_CATEGORY_DATA);
            dsd_event_history_mark_dirty(&state->event_history_s[slot]);
            dsd_event_history_transaction_end(&transaction);
            return 1;
        case 4004:
            DSD_FPRINTF(stderr, "XCMP;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "XCMP SRC: %d; DST: %d;", src24,
                         dst24);
            dsd_event_history_transaction_begin(state, &transaction);
            dsd_event_history_item_set_metadata(&state->event_history_s[slot].Event_History_Items[0],
                                                DSD_EVENT_SEVERITY_INFO, DSD_EVENT_CATEGORY_DATA);
            dsd_event_history_mark_dirty(&state->event_history_s[slot]);
            dsd_event_history_transaction_end(&transaction);
            return 1;
        case 4005: {
            DSD_FPRINTF(stderr, "ARS;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "ARS SRC: %d; DST: %d; ", src24,
                         dst24);
            uint16_t ars_len = (payload_len < 10) ? payload_len : 10;
            utf8_to_text(state, 0, ars_len, payload);
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

static int DSD_ATTR_USED
decode_ip_pdu_handle_udp_service_ext(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24,
                                     uint32_t dst24, uint16_t port, uint16_t payload_len, const uint8_t* payload,
                                     const uint8_t* input) {
    switch (port) {
        case 5007: decode_ip_pdu_handle_udp_vtx_tms(opts, state, slot, src24, dst24, payload_len, payload); return 1;
        case 5016:
            DSD_FPRINTF(stderr, "ETSI TMS;");
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "ETSI TMS SRC: %d; DST: %d; ",
                         src24, dst24);
            utf16_to_text(state, 1, payload_len, payload);
            return 1;
        case 5017: {
            uint8_t bits[127 * 12 * 8];
            uint16_t decode_len = payload_len;
            if (decode_len > (uint16_t)(sizeof(bits) / 8U)) {
                decode_len = (uint16_t)(sizeof(bits) / 8U);
            }
            DSD_MEMSET(bits, 0, sizeof(bits));
            unpack_byte_array_into_bit_array(payload, bits, (int)decode_len);
            lip_protocol_decoder(opts, state, bits);
            return 1;
        }
        case 9361:
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                         "P25 Atlas SRC(IP): %d.%d.%d.%d; DST(IP): %d.%d.%d.%d; ", input[12], input[13], input[14],
                         input[15], input[16], input[17], input[18], input[19]);
            DSD_FPRINTF(stderr, "Atlas Data Registration Server;");
            return 1;
        case 49198:
            DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                         "P25 Tier 2 LOCN SRC(IP): %d.%d.%d.%d; DST(IP): %d.%d.%d.%d; ", input[12], input[13],
                         input[14], input[15], input[16], input[17], input[18], input[19]);
            DSD_FPRINTF(stderr, "P25 Tier 2 Location Service;");
            dmr_lrrp(opts, state, payload_len, src24, dst24, payload, 1);
            return 1;
        default: break;
    }
    return 0;
}

static void DSD_ATTR_USED
decode_ip_pdu_handle_udp_service(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                                 uint16_t port, uint16_t payload_len, uint8_t* payload, const uint8_t* input) {
    if (decode_ip_pdu_handle_udp_service_core(opts, state, slot, src24, dst24, port, payload_len, payload)) {
        return;
    }
    if (decode_ip_pdu_handle_udp_service_ext(opts, state, slot, src24, dst24, port, payload_len, payload, input)) {
        return;
    }
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]),
                 "IP SRC: %d.%d.%d.%d:%d; DST: %d.%d.%d.%d:%d; Unknown UDP Port;", input[12], input[13], input[14],
                 input[15], port, input[16], input[17], input[18], input[19], port);
    DSD_FPRINTF(stderr, "Unknown UDP Port;");
}

static void DSD_ATTR_USED
decode_ip_pdu_handle_udp(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t src24, uint32_t dst24,
                         size_t effective_len, size_t ip_header_len, uint8_t* input) {
    if (effective_len < ip_header_len + 8u) {
        DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "Truncated UDP;");
        const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, slot, src24, dst24);
        (void)dsd_event_emit_data_notice(opts, state, slot, &observation, state->dmr_lrrp_gps[slot]);
        return;
    }
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
    uint8_t* payload = input + udp_payload_off;

    decode_ip_pdu_handle_udp_service(opts, state, slot, src24, dst24, dst_port, payload_len, payload, input);
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

//IP PDU header decode and port forward to appropriate decoder
void
decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input) {
    if (!opts) {
        return;
    }
    if (!state) {
        return;
    }
    if (!input) {
        return;
    }
    if (len < 20) {
        return;
    }

    uint8_t slot = state->currentslot;
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
        return;
    }
    if (ihl < 5) {
        return;
    }
    if (ip_header_len > (size_t)len) {
        return;
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
    decode_ip_pdu_dispatch(opts, state, slot, prot, src24, dst24, effective_len, ip_header_len, input);

    const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, slot, src24, dst24);
    (void)dsd_event_emit_data_notice(opts, state, slot, &observation, state->dmr_lrrp_gps[slot]);
}

typedef struct {
    // decoded time (token 0x34)
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;

    // decoded position (point/circle 2D/3D)
    uint32_t lat_raw;
    uint32_t lon_raw;
    uint16_t rad_raw;
    uint32_t alt_raw;
    uint16_t alt_acc_raw;
    uint8_t have_pos;
    uint8_t have_rad;
    uint8_t have_alt;
    uint8_t have_alt_acc;
    // TokenType order for position tokens: CIRCLE_2D, CIRCLE_3D, POINT_2D, POINT_3D.
    uint8_t pos_best_rank;

    // speed/heading
    double velocity_mph; // units are 1/100 mph per SDRTrunk Speed.java
    uint8_t vel_set;
    uint16_t heading_deg; // degrees, 2-degree increments per SDRTrunk Heading.java
    uint8_t heading_set;

    // parser quality metrics
    int known_tokens;
    int unknown_tokens;
    int truncated_tokens;
} dmr_lrrp_parse_result;

static void
dmr_lrrp_parse_result_init(dmr_lrrp_parse_result* r) {
    *r = (dmr_lrrp_parse_result){0};
    r->pos_best_rank = 0xFFu;
}

static size_t
dmr_lrrp_take_bytes(size_t remaining, size_t full, dmr_lrrp_parse_result* r) {
    if (remaining >= full) {
        return full;
    }
    r->truncated_tokens++;
    return remaining;
}

static int
dmr_lrrp_position_token_meta(uint8_t token, size_t* full, uint8_t* rank) {
    switch (token) {
        case 0x51:
            *full = 11u;
            *rank = 0u;
            return 1;
        case 0x55:
            *full = 16u;
            *rank = 1u;
            return 1;
        case 0x66:
            *full = 9u;
            *rank = 2u;
            return 1;
        case 0x69:
            *full = 12u;
            *rank = 3u;
            return 1;
        default: return 0;
    }
}

static void
dmr_lrrp_parse_timestamp_token(const uint8_t* pdu, size_t idx, dmr_lrrp_parse_result* r) {
    if (r->year != 0) {
        return;
    }
    r->year = (uint16_t)((pdu[idx + 1] << 6) + (pdu[idx + 2] >> 2));
    r->month = (uint16_t)(((pdu[idx + 2] & 0x3) << 2) + ((pdu[idx + 3] & 0xC0) >> 6));
    r->day = (uint16_t)(((pdu[idx + 3] & 0x3E) >> 1));
    r->hour = (uint16_t)(((pdu[idx + 3] & 0x01) << 4) + ((pdu[idx + 4] & 0xF0) >> 4));
    r->minute = (uint16_t)(((pdu[idx + 4] & 0x0F) << 2) + ((pdu[idx + 5] & 0xC0) >> 6));
    r->second = (uint16_t)((pdu[idx + 5] & 0x3F));

    int valid = 1;
    if (!(r->month >= 1 && r->month <= 12)) {
        valid = 0;
    }
    if (!(r->day >= 1 && r->day <= 31)) {
        valid = 0;
    }
    if (!(r->hour <= 23 && r->minute <= 59 && r->second <= 59)) {
        valid = 0;
    }
    if (!(r->year >= 2000 && r->year <= 2037)) {
        valid = 0;
    }
    if (!valid) {
        r->year = r->month = r->day = r->hour = r->minute = r->second = 0;
    }
}

static void
dmr_lrrp_parse_position_token(const uint8_t* pdu, size_t idx, uint8_t token, uint8_t rank, dmr_lrrp_parse_result* r) {
    if (r->pos_best_rank <= rank) {
        return;
    }
    r->pos_best_rank = rank;
    r->lat_raw = ((uint32_t)pdu[idx + 1] << 24) | ((uint32_t)pdu[idx + 2] << 16) | ((uint32_t)pdu[idx + 3] << 8)
                 | (uint32_t)pdu[idx + 4];
    r->lon_raw = ((uint32_t)pdu[idx + 5] << 24) | ((uint32_t)pdu[idx + 6] << 16) | ((uint32_t)pdu[idx + 7] << 8)
                 | (uint32_t)pdu[idx + 8];
    r->have_pos = 1;

    if (token == 0x51) {
        r->rad_raw = ((uint16_t)pdu[idx + 9] << 8) | (uint16_t)pdu[idx + 10];
        r->alt_raw = 0;
        r->alt_acc_raw = 0;
        r->have_rad = 1;
        r->have_alt = 0;
        r->have_alt_acc = 0;
    } else if (token == 0x55) {
        r->rad_raw = ((uint16_t)pdu[idx + 9] << 8) | (uint16_t)pdu[idx + 10];
        r->alt_raw = ((uint16_t)pdu[idx + 11] << 8) | (uint16_t)pdu[idx + 12];
        r->alt_acc_raw = ((uint16_t)pdu[idx + 13] << 8) | (uint16_t)pdu[idx + 14];
        r->have_rad = 1;
        r->have_alt = 1;
        r->have_alt_acc = 1;
    } else if (token == 0x66) {
        r->rad_raw = 0;
        r->alt_raw = 0;
        r->alt_acc_raw = 0;
        r->have_rad = 0;
        r->have_alt = 0;
        r->have_alt_acc = 0;
    } else if (token == 0x69) {
        r->alt_raw = ((uint32_t)pdu[idx + 9] << 16) | ((uint32_t)pdu[idx + 10] << 8) | (uint32_t)pdu[idx + 11];
        r->rad_raw = 0;
        r->alt_acc_raw = 0;
        r->have_rad = 0;
        r->have_alt = 1;
        r->have_alt_acc = 0;
    }
}

static int
dmr_lrrp_parse_identity_token(uint8_t token, const uint8_t* pdu, size_t idx, size_t remaining, dmr_lrrp_parse_result* r,
                              size_t* need) {
    if (token != 0x22) {
        return 0;
    }
    if (remaining < 2u) {
        *need = remaining;
        r->truncated_tokens++;
    } else {
        size_t payload = (size_t)pdu[idx + 1];
        *need = dmr_lrrp_take_bytes(remaining, 2u + payload, r);
    }
    return 1;
}

static int
dmr_lrrp_parse_len1_token(uint8_t token, size_t remaining, dmr_lrrp_parse_result* r, size_t* need) {
    switch (token) {
        case 0x23:
        case 0x31:
        case 0x4A:
        case 0x78:
        case 0x61:
        case 0x73: *need = dmr_lrrp_take_bytes(remaining, 2u, r); return 1;
        default: return 0;
    }
}

static int
dmr_lrrp_parse_len0_token(uint8_t token, size_t* need) {
    switch (token) {
        case 0x42:
        case 0x3A:
        case 0x50:
        case 0x52:
        case 0x54:
        case 0x57:
        case 0x62:
        case 0x64:
        case 0x38: *need = 1u; return 1;
        default: return 0;
    }
}

static int
dmr_lrrp_parse_tvr_token(uint8_t token, const uint8_t* pdu, size_t idx, size_t remaining, dmr_lrrp_parse_result* r,
                         size_t* need) {
    if (token == 0x34) {
        *need = dmr_lrrp_take_bytes(remaining, 6u, r);
        if (*need == 6u) {
            dmr_lrrp_parse_timestamp_token(pdu, idx, r);
        }
        return 1;
    }
    if (token == 0x36) {
        *need = dmr_lrrp_take_bytes(remaining, 2u, r);
        return 1;
    }
    if (token == 0x37) {
        if (remaining < 2u) {
            *need = remaining;
            r->truncated_tokens++;
        } else {
            size_t full = (pdu[idx + 1] & 0x80) ? 3u : 2u;
            *need = dmr_lrrp_take_bytes(remaining, full, r);
        }
        return 1;
    }
    return 0;
}

static int
dmr_lrrp_parse_speed_heading_token(uint8_t token, const uint8_t* pdu, size_t idx, size_t remaining,
                                   dmr_lrrp_parse_result* r, size_t* need) {
    if (token == 0x6C) {
        *need = dmr_lrrp_take_bytes(remaining, 3u, r);
        if (*need == 3u && !r->vel_set) {
            r->velocity_mph = ((double)(((uint16_t)pdu[idx + 1] << 8) | (uint16_t)pdu[idx + 2])) * 0.01;
            r->vel_set = 1;
        }
        return 1;
    }
    if (token == 0x56) {
        *need = dmr_lrrp_take_bytes(remaining, 2u, r);
        if (*need == 2u && !r->heading_set) {
            r->heading_deg = (uint16_t)pdu[idx + 1] * 2u;
            r->heading_set = 1;
        }
        return 1;
    }
    return 0;
}

static int
dmr_lrrp_parse_position_class_token(uint8_t token, const uint8_t* pdu, size_t idx, size_t remaining,
                                    dmr_lrrp_parse_result* r, size_t* need) {
    size_t full = 0u;
    uint8_t rank = 0u;
    if (!dmr_lrrp_position_token_meta(token, &full, &rank)) {
        return 0;
    }
    *need = dmr_lrrp_take_bytes(remaining, full, r);
    if (*need == full) {
        dmr_lrrp_parse_position_token(pdu, idx, token, rank, r);
    }
    return 1;
}

static void
dmr_lrrp_parse_response_tokens(const uint8_t* pdu, size_t avail, size_t idx_start, size_t remaining,
                               dmr_lrrp_parse_result* r) {
    size_t idx = idx_start;
    while (remaining > 0 && idx < avail) {
        uint8_t token = pdu[idx];
        size_t need = 1;
        int known = 1;
        int handled = dmr_lrrp_parse_identity_token(token, pdu, idx, remaining, r, &need);
        if (!handled) {
            handled = dmr_lrrp_parse_len1_token(token, remaining, r, &need);
        }
        if (!handled) {
            handled = dmr_lrrp_parse_len0_token(token, &need);
        }
        if (!handled) {
            handled = dmr_lrrp_parse_tvr_token(token, pdu, idx, remaining, r, &need);
        }
        if (!handled) {
            handled = dmr_lrrp_parse_speed_heading_token(token, pdu, idx, remaining, r, &need);
        }
        if (!handled) {
            handled = dmr_lrrp_parse_position_class_token(token, pdu, idx, remaining, r, &need);
        }
        if (!handled) {
            known = 0;
            need = 1u;
        }

        if (known) {
            r->known_tokens++;
        } else {
            r->unknown_tokens++;
        }

        idx += need;
        remaining -= need;
    }
}

static int
dmr_lrrp_parse_score(const dmr_lrrp_parse_result* r, size_t prefix_skip) {
    int score = 0;
    score -= (int)prefix_skip * 5;
    score += r->known_tokens * 10;
    score -= r->unknown_tokens;
    score -= r->truncated_tokens * 50;

    if (r->have_pos) {
        score += 1000 - ((int)r->pos_best_rank * 10);
        // Penalize (0,0) which often shows up as a desync / bogus decode.
        if (r->lat_raw == 0u && r->lon_raw == 0u) {
            score -= 200;
        }
    }
    if (r->year) {
        score += 100;
    }
    if (r->vel_set) {
        score += 50;
    }
    if (r->heading_set) {
        score += 50;
    }
    if (r->have_rad) {
        score += 20;
    }
    if (r->have_alt) {
        score += 20;
    }
    if (r->have_alt_acc) {
        score += 20;
    }

    return score;
}

typedef struct {
    double lat;
    double lon;
    double rad;
    double alt;
    double alt_acc;
} dmr_lrrp_scaled;

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
        case 0x15: *is_response = 1; break;
        default: break;
    }
}

static int
dmr_lrrp_has_position_token(const uint8_t* pdu, size_t avail, size_t token_len) {
    for (size_t i = 0; i < token_len && (2u + i) < avail; i++) {
        uint8_t b = pdu[2u + i];
        if (b == 0x51 || b == 0x55 || b == 0x66 || b == 0x69) {
            return 1;
        }
    }
    return 0;
}

static void
dmr_lrrp_parse_best_response(const uint8_t* pdu, size_t avail, size_t token_len, dmr_lrrp_parse_result* best) {
    const size_t max_skip = 6u;
    int best_score = -1000000;
    for (size_t skip = 0; skip <= max_skip && skip <= token_len; skip++) {
        dmr_lrrp_parse_result cur;
        dmr_lrrp_parse_result_init(&cur);
        dmr_lrrp_parse_response_tokens(pdu, avail, 2u + skip, token_len - skip, &cur);
        int score = dmr_lrrp_parse_score(&cur, skip);
        if (score > best_score) {
            best_score = score;
            *best = cur;
        }
    }
}

static dmr_lrrp_scaled
dmr_lrrp_compute_scaled(const dmr_lrrp_parse_result* best) {
    dmr_lrrp_scaled s;
    s.lat = 0.0;
    s.lon = 0.0;
    s.rad = 0.0;
    s.alt = 0.0;
    s.alt_acc = 0.0;
    if (best->have_pos) {
        s.lat = ((double)((int32_t)best->lat_raw) * 90.0) / 2147483648.0;
        s.lon = ((double)((int32_t)best->lon_raw) * 180.0) / 2147483648.0;
    }
    if (best->have_rad) {
        s.rad = (double)best->rad_raw * 0.01;
    }
    if (best->have_alt) {
        s.alt = (double)best->alt_raw * 0.01;
    }
    if (best->have_alt_acc) {
        s.alt_acc = (double)best->alt_acc_raw * 0.01;
    }
    return s;
}

static void
dmr_lrrp_print_details(const dmr_lrrp_parse_result* best, const dmr_lrrp_scaled* s, uint8_t pdu_crc_ok) {
    if (best->year) {
        DSD_FPRINTF(stderr, "\n Time: %04d.%02d.%02d %02d:%02d:%02d", best->year, best->month, best->day, best->hour,
                    best->minute, best->second);
    }
    if (best->have_pos && pdu_crc_ok) {
        DSD_FPRINTF(stderr, "\n Lat: %.5lf Lon: %.5lf (%.5lf, %.5lf)", s->lat, s->lon, s->lat, s->lon);
    } else if (best->have_pos && !pdu_crc_ok) {
        DSD_FPRINTF(stderr, "\n Position: (suppressed; CRC ERR)");
    }
    if (best->have_rad) {
        DSD_FPRINTF(stderr, "\n Radius: %.2lfm", s->rad);
    }
    if (best->have_alt) {
        DSD_FPRINTF(stderr, "\n Altitude: %.2lfm", s->alt);
    }
    if (best->have_alt_acc) {
        DSD_FPRINTF(stderr, "\n Alt Accuracy: %.2lfm", s->alt_acc);
    }
    if (best->vel_set) {
        DSD_FPRINTF(stderr, "\n Speed: %.2lf mph %.2lf km/h %.2lf m/s", best->velocity_mph,
                    (best->velocity_mph * 1.60934), (best->velocity_mph * 0.44704));
    }
    if (best->heading_set) {
        DSD_FPRINTF(stderr, "\n Track: %d%s", (int)best->heading_deg, dsd_degrees_glyph());
    }
}

static void
dmr_lrrp_write_file_if_needed(const dsd_opts* opts, const dmr_lrrp_parse_result* best, const dmr_lrrp_scaled* s,
                              uint8_t pdu_crc_ok, uint32_t source) {
    if (opts->lrrp_file_output != 1 || !pdu_crc_ok || s->lat == 0.0 || s->lon == 0.0) {
        return;
    }
    char timestr[9];
    char datestr[11];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_DATE_SLASH, datestr, sizeof datestr);
    FILE* pFile = dsd_fopen_private(opts->lrrp_out_file, "a");
    if (pFile == NULL) {
        return;
    }
    DSD_FPRINTF(pFile, "%s\t%s\t", datestr, timestr);
    DSD_FPRINTF(pFile, "%08d\t", source);
    DSD_FPRINTF(pFile, "%.5lf\t", s->lat);
    DSD_FPRINTF(pFile, "%.5lf\t", s->lon);
    DSD_FPRINTF(pFile, "%.3lf\t ", (best->velocity_mph * 1.60934));
    DSD_FPRINTF(pFile, "%d\t", (int)best->heading_deg);
    DSD_FPRINTF(pFile, "\n");
    fclose(pFile);
}

static void
dmr_lrrp_build_summary(char* out, size_t out_sz, const dmr_lrrp_parse_result* best, const dmr_lrrp_scaled* s,
                       uint32_t source, uint32_t dest, uint8_t is_request, uint8_t is_response, uint8_t lrrp_type,
                       uint8_t pdu_crc_ok) {
    if (best->have_pos && pdu_crc_ok) {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %0d; (%lf, %lf)", source, s->lat, s->lon);
    } else if (best->have_pos && !pdu_crc_ok) {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %0d; Position suppressed (CRC ERR);", source);
    } else if (is_request) {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %0d; Request from TGT: %d;", source, dest);
    } else if (is_response) {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %0d; Response to TGT: %d;", source, dest);
    } else {
        DSD_SNPRINTF(out, out_sz, "LRRP SRC: %0d; Unknown Format %02X; TGT: %d;", source, lrrp_type, dest);
    }
}

typedef struct {
    uint32_t source;
    uint32_t dest;
    uint8_t is_request;
    uint8_t is_response;
    uint8_t lrrp_type;
} dmr_lrrp_emit_context;

static void
dmr_lrrp_emit_payload(const dsd_opts* opts, dsd_state* state, uint8_t slot, const dmr_lrrp_parse_result* best,
                      uint8_t payload_len, uint8_t pdu_crc_ok, const dmr_lrrp_emit_context* ctx) {
    if (payload_len == 0) {
        DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot],
                     "LRRP SRC: %0d; Unknown Format %02X; TGT: %d;", ctx->source, ctx->lrrp_type, ctx->dest);
        DSD_FPRINTF(stderr, "\n %s", state->dmr_lrrp_gps[slot]);
        return;
    }
    dmr_lrrp_scaled s = dmr_lrrp_compute_scaled(best);
    DSD_FPRINTF(stderr, "%s", KYEL);
    dmr_lrrp_print_details(best, &s, pdu_crc_ok);
    dmr_lrrp_write_file_if_needed(opts, best, &s, pdu_crc_ok, ctx->source);

    char lrrpstr[100];
    char velstr[20];
    char degstr[20];
    lrrpstr[0] = '\0';
    velstr[0] = '\0';
    degstr[0] = '\0';
    dmr_lrrp_build_summary(lrrpstr, sizeof lrrpstr, best, &s, ctx->source, ctx->dest, ctx->is_request, ctx->is_response,
                           ctx->lrrp_type, pdu_crc_ok);
    if (best->vel_set) {
        DSD_SNPRINTF(velstr, sizeof velstr, " %.2lf km/h", best->velocity_mph * 1.60934);
    }
    if (best->heading_set) {
        DSD_SNPRINTF(degstr, sizeof degstr, " %d%s  ", (int)best->heading_deg, dsd_degrees_glyph());
    }
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "%s%s%s", lrrpstr, velstr, degstr);
    if (!best->have_pos || !pdu_crc_ok) {
        DSD_FPRINTF(stderr, "\n %s", state->dmr_lrrp_gps[slot]);
    }
}

//The contents of this function are mostly trial and error
void
dmr_lrrp(const dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, const uint8_t* DMR_PDU,
         uint8_t pdu_crc_ok) {
    if (!DMR_PDU || len < 2) {
        return;
    }
    uint8_t slot = state->currentslot;
    uint8_t lrrp_type = DMR_PDU[0];
    uint8_t payload_len = DMR_PDU[1];
    uint8_t is_request = 0;
    uint8_t is_response = 0;
    dmr_lrrp_classify_type(lrrp_type, &is_request, &is_response);

    dmr_lrrp_parse_result best;
    dmr_lrrp_parse_result_init(&best);
    size_t avail = (size_t)len;
    size_t token_avail = (avail > 2u) ? (avail - 2u) : 0u;
    size_t token_len = (size_t)payload_len;
    if (token_len > token_avail) {
        token_len = token_avail;
    }

    int want_response_parse = is_response;
    if (!want_response_parse && !is_request && token_len > 0u) {
        want_response_parse = dmr_lrrp_has_position_token(DMR_PDU, avail, token_len);
    }
    if (want_response_parse) {
        dmr_lrrp_parse_best_response(DMR_PDU, avail, token_len, &best);
    }
    if (!source) {
        source = (uint32_t)state->dmr_lrrp_source[state->currentslot];
    }
    dmr_lrrp_emit_context emit_ctx = {source, dest, is_request, is_response, lrrp_type};
    dmr_lrrp_emit_payload(opts, state, slot, &best, payload_len, pdu_crc_ok, &emit_ctx);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

typedef struct {
    uint8_t time;
    uint8_t lat;
    uint8_t lon;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t year;
    uint8_t month;
    uint8_t day;
    uint16_t lat_deg;
    uint16_t lat_min;
    uint16_t lat_sec;
    uint16_t lon_deg;
    uint16_t lon_min;
    uint16_t lon_sec;
    int lat_sign;
    int lon_sign;
} dmr_locn_data;

static void
dmr_locn_validate_time(dmr_locn_data* d) {
    int yy = ((d->year >> 4) & 0xF) * 10 + (d->year & 0xF);
    int mm = ((d->month >> 4) & 0xF) * 10 + (d->month & 0xF);
    int dd = ((d->day >> 4) & 0xF) * 10 + (d->day & 0xF);
    int hh = ((d->hour >> 4) & 0xF) * 10 + (d->hour & 0xF);
    int mi = ((d->minute >> 4) & 0xF) * 10 + (d->minute & 0xF);
    int ss = ((d->second >> 4) & 0xF) * 10 + (d->second & 0xF);
    int full_year = 2000 + yy;
    int valid = 1;
    if (!(mm >= 1 && mm <= 12)) {
        valid = 0;
    }
    if (!(dd >= 1 && dd <= 31)) {
        valid = 0;
    }
    if (!(hh >= 0 && hh <= 23 && mi >= 0 && mi <= 59 && ss >= 0 && ss <= 59)) {
        valid = 0;
    }
    if (!(full_year >= 2000 && full_year <= 2037)) {
        valid = 0;
    }
    if (!valid) {
        d->time = 0;
    }
}

static void
dmr_locn_parse_fields(const uint8_t* pdu, uint16_t len, dmr_locn_data* d) {
    uint16_t i = 0;
    while (i < len) {
        uint16_t advance = 1;
        switch (pdu[i]) {
            case 0x41:
                if ((uint16_t)(i + 12U) >= len) {
                    return;
                }
                d->time = 1;
                d->hour = ((pdu[i + 1] - 0x30) << 4) | (pdu[i + 2] - 0x30);
                d->minute = ((pdu[i + 3] - 0x30) << 4) | (pdu[i + 4] - 0x30);
                d->second = ((pdu[i + 5] - 0x30) << 4) | (pdu[i + 6] - 0x30);
                d->day = ((pdu[i + 7] - 0x30) << 4) | (pdu[i + 8] - 0x30);
                d->month = ((pdu[i + 9] - 0x30) << 4) | (pdu[i + 10] - 0x30);
                d->year = ((pdu[i + 11] - 0x30) << 4) | (pdu[i + 12] - 0x30);
                dmr_locn_validate_time(d);
                advance = 13;
                break;
            case 0x53:
                d->lat_sign = -1;
                /* fall through */
            case 0x4E:
                if ((uint16_t)(i + 9U) >= len) {
                    return;
                }
                d->lat = 1;
                d->lat_deg = ((pdu[i + 1] - 0x30) << 4) | (pdu[i + 2] - 0x30);
                d->lat_min = ((pdu[i + 3] - 0x30) << 4) | (pdu[i + 4] - 0x30);
                d->lat_sec = ((pdu[i + 6] - 0x30) << 12) | ((pdu[i + 7] - 0x30) << 8) | ((pdu[i + 8] - 0x30) << 4)
                             | ((pdu[i + 9] - 0x30) << 0);
                advance = 9;
                break;
            case 0x57:
                d->lon_sign = -1;
                /* fall through */
            case 0x45:
                if ((uint16_t)(i + 10U) >= len) {
                    return;
                }
                d->lon = 1;
                d->lon_deg = ((pdu[i + 1] - 0x30) << 8) | ((pdu[i + 2] - 0x30) << 4) | ((pdu[i + 3] - 0x30) << 0);
                d->lon_min = ((pdu[i + 4] - 0x30) << 4) | (pdu[i + 5] - 0x30);
                d->lon_sec = ((pdu[i + 7] - 0x30) << 12) | ((pdu[i + 8] - 0x30) << 8) | ((pdu[i + 9] - 0x30) << 4)
                             | ((pdu[i + 10] - 0x30) << 0);
                advance = 9;
                break;
            default: break;
        }
        i = (uint16_t)(i + advance);
    }
}

static void
dmr_locn_compute_lat_lon(dmr_locn_data* d, double* latitude, double* longitude) {
    d->lat_deg = convert_hex_to_dec(d->lat_deg);
    d->lat_min = convert_hex_to_dec(d->lat_min);
    d->lat_sec = convert_hex_to_dec(d->lat_sec);
    d->lon_deg = convert_hex_to_dec(d->lon_deg);
    d->lon_min = convert_hex_to_dec(d->lon_min);
    d->lon_sec = convert_hex_to_dec(d->lon_sec);

    *latitude =
        (double)d->lat_sign * ((double)d->lat_deg + ((double)d->lat_min / 60.0) + ((double)d->lat_sec / 600000.0));
    *longitude =
        (double)d->lon_sign * ((double)d->lon_deg + ((double)d->lon_min / 60.0) + ((double)d->lon_sec / 600000.0));
}

static void DSD_ATTR_USED
dmr_locn_write_file(const dsd_opts* opts, const dsd_state* state, double latitude, double longitude) {
    if (opts->lrrp_file_output != 1) {
        return;
    }
    char timestr[9];
    char datestr[11];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_DATE_SLASH, datestr, sizeof datestr);
    FILE* pFile = dsd_fopen_private(opts->lrrp_out_file, "a");
    if (pFile == NULL) {
        return;
    }
    DSD_FPRINTF(pFile, "%s\t%s\t", datestr, timestr);
    DSD_FPRINTF(pFile, "%08lld\t", state->dmr_lrrp_source[state->currentslot]);
    DSD_FPRINTF(pFile, "%.5lf\t", latitude);
    DSD_FPRINTF(pFile, "%.5lf\t", longitude);
    DSD_FPRINTF(pFile, "%.3lf\t ", 0.0);
    DSD_FPRINTF(pFile, "%d\t", 0);
    DSD_FPRINTF(pFile, "\n");
    fclose(pFile);
}

static void DSD_ATTR_USED
dmr_locn_emit(dsd_state* state, uint8_t slot, uint32_t source, const dmr_locn_data* d, double latitude,
              double longitude) {
    const char* deg_glyph = dsd_degrees_glyph();
    char locnstr[50];
    char latstr[75];
    char lonstr[75];
    locnstr[0] = '\0';
    latstr[0] = '\0';
    lonstr[0] = '\0';

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n NMEA / LOCN; Source: %u;", (unsigned)source);
    if (d->time) {
        DSD_FPRINTF(stderr, " 20%02X/%02X/%02X %02X:%02X:%02X", d->year, d->month, d->day, d->hour, d->minute,
                    d->second);
    }
    DSD_FPRINTF(stderr, " (%.5lf%s, %.5lf%s);", latitude, deg_glyph, longitude, deg_glyph);

    DSD_SNPRINTF(locnstr, sizeof locnstr, "NMEA / LOCN; Source: %u ", (unsigned)source);
    DSD_SNPRINTF(latstr, sizeof latstr, "(%.5lf%s, ", latitude, deg_glyph);
    DSD_SNPRINTF(lonstr, sizeof lonstr, "%.5lf%s)", longitude, deg_glyph);
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "%s%s%s", locnstr, latstr, lonstr);
}

void
dmr_locn(const dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU) {
    uint8_t slot = state->currentslot;
    uint32_t source = (uint32_t)state->dmr_lrrp_source[slot];
    dmr_locn_data d;
    DSD_MEMSET(&d, 0, sizeof(d));
    d.lat_sign = 1;
    d.lon_sign = 1;

    dmr_locn_parse_fields(DMR_PDU, len, &d);
    if (!(d.lat && d.lon)) {
        return;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    dmr_locn_compute_lat_lon(&d, &latitude, &longitude);
    dmr_locn_emit(state, slot, source, &d, latitude, longitude);
    dmr_locn_write_file(opts, state, latitude, longitude);
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
