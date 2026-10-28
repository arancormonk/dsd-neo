// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 ============================================================================
 Name        : nxdn_element.c (formerly nxdn_lib)
 Author      :
 Version     : 1.0
 Date        : 2018 December 26
 Copyright   : No copyright
 Description : NXDN decoding source lib - modified from nxdn_lib
 Origin      : Originally found at - https://github.com/LouisErigHerve/dsd
 ============================================================================
 */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/nxdn_keystream.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>
#include <dsd-neo/protocol/nxdn/nxdn_cc_hunt.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/secret_redaction.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "nxdn_confirm.h"
#include "nxdn_crc.h"
#include "nxdn_internal.h"

typedef void (*nxdn_element_handler_fn)(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                        size_t elements_bits);

struct nxdn_element_dispatch_entry {
    uint8_t message_type;
    nxdn_element_handler_fn handler;
};

static void nxdn_data_call_option_to_str(uint8_t data_call_option, char* duplex, size_t duplex_sz, char* mode,
                                         size_t mode_sz);
static void nxdn_print_group_label(const dsd_state* state, uint32_t id);
static void nxdn_anchor_control_channel_from_current_tuner(const dsd_opts* opts, dsd_state* state, int only_if_missing);
static nxdn_element_handler_fn nxdn_element_dispatch_handler(uint8_t message_type);
static void nxdn_follow_release(dsd_state* state);
static void nxdn_element_handle_idle(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits);
static void nxdn_element_handle_sdcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                              size_t elements_bits);
static void nxdn_element_handle_sdcall_data(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                            size_t elements_bits);
static void nxdn_element_handle_sdcall_iv(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                          size_t elements_bits);
static void nxdn_element_handle_dcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                             size_t elements_bits);
static void nxdn_element_handle_dcall_data(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                           size_t elements_bits);
static void nxdn_element_handle_call_assignment(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                                size_t elements_bits);
static void nxdn_element_handle_alias(const dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                      size_t elements_bits);
static void nxdn_element_handle_dst_info(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                         size_t elements_bits);
static void nxdn_element_handle_srv_info(const dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                         size_t elements_bits);
static void nxdn_element_handle_cch_info(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                         size_t elements_bits);
static void nxdn_element_handle_site_info(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                          size_t elements_bits);
static void nxdn_element_handle_adj_site(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                         size_t elements_bits);
static void nxdn_element_handle_tx_release(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                           size_t elements_bits);
static void nxdn_element_handle_vcall(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits);
static void nxdn_element_handle_disc(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits);
static void nxdn_element_handle_vcall_iv(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                         size_t elements_bits);
static int nxdn_load_data_aes_key(const dsd_state* state, uint8_t key_id, uint8_t out_key[32]);
static void nxdn_sdcall_header(const dsd_opts* opts, dsd_state* state, const uint8_t* Message);
static void nxdn_dcall_header(const dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits);
static void nxdn_sdcall_iv(dsd_opts* opts, dsd_state* state, const uint8_t* Message);
static int nxdn_dcall_data(dsd_opts* opts, dsd_state* state, int type, const uint8_t* Message, size_t message_bits);
static void NXDN_decode_VCALL(dsd_opts* opts, dsd_state* state, const uint8_t* Message);
static void NXDN_decode_VCALL_ARIB(dsd_opts* opts, dsd_state* state, const uint8_t* Message);
static void NXDN_decode_VCALL_IV(dsd_opts* opts, dsd_state* state, const uint8_t* Message);
void NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, const uint8_t* Message);
static void nxdn_print_dfa_bandwidth(uint8_t bw);
static void nxdn_cch_info_channel_version(dsd_state* state, uint32_t location_id, uint8_t channel1sts,
                                          uint16_t channel1, uint16_t channel2);
static int nxdn_cch_info_dfa_version(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits,
                                     uint32_t location_id, uint8_t channel1sts);
static void nxdn_adj_site_channel_entry(dsd_opts* opts, dsd_state* state, uint32_t site, uint8_t opt, uint16_t chan);
static void nxdn_adj_site_dfa_entry(dsd_opts* opts, dsd_state* state, uint32_t site, uint8_t opt, uint8_t bw,
                                    uint16_t chan);
static void NXDN_decode_cch_info(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits);
static void NXDN_decode_srv_info(const dsd_opts* opts, dsd_state* state, const uint8_t* Message, int control_channel);
static void NXDN_decode_srv_info_type_d(dsd_state* state, const uint8_t* Message);
static void NXDN_decode_adj_site_type_d(const uint8_t* Message);
static void NXDN_decode_site_info(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits);
static void NXDN_decode_adj_site(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits);
static char* NXDN_Call_Type_To_Str(uint8_t CallType);
static void NXDN_Voice_Call_Option_To_Str(uint8_t VoiceCallOption, uint8_t* Duplex, uint8_t* TransmissionMode);
static char* NXDN_Cipher_Type_To_Str(uint8_t CipherType);
static void nxdn_location_id_handler(dsd_state* state, uint32_t location_id, uint8_t type);
static void nxdn_srv_info_handler(dsd_state* state, uint16_t svc_info);
static void nxdn_rst_info_handler(dsd_state* state, uint32_t rst_info);
static void nxdn_ca_info_handler(dsd_state* state, uint32_t ca_info);

static void
nxdn_print_group_label(const dsd_state* state, uint32_t id) {
    char name[50];
    if (id != 0U && dsd_tg_policy_lookup_label(state, id, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, " [%s]", name);
    }
}

static void
nxdn_print_source_label(const dsd_state* state, uint32_t id) {
    char name[DSD_SOURCE_ALIAS_NAME_MAX];
    if (id != 0U && dsd_source_label_lookup(state, id, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, " [%s]", name);
    }
}

static void
nxdn_anchor_control_channel_from_current_tuner(const dsd_opts* opts, dsd_state* state, int only_if_missing) {
    if ((opts->trunk_enable != 1) || opts->trunk_is_tuned != 0 || (only_if_missing && state->p25_cc_freq != 0)) {
        return;
    }

    long int ccfreq = 0;
    if (opts->use_rigctl == 1) {
        ccfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
    } else if (opts->audio_in_type == AUDIO_IN_RTL) {
#ifdef USE_RADIO
        ccfreq = (long int)opts->rtlsdr_center_freq;
#endif
    }

    if (ccfreq != 0) {
        state->p25_cc_freq = ccfreq;
        state->trunk_cc_freq = ccfreq;
        nxdn_cc_hunt_note_tuner_frequency(state, ccfreq);
    }
}

void
NXDN_SACCH_Full_decode(dsd_opts* opts, dsd_state* state) {
    uint8_t SACCH[72]; //72
    uint8_t sacch_bytes[9];

    uint32_t i;
    uint8_t CrcCorrect = 1;

    DSD_MEMSET(SACCH, 0, sizeof(SACCH));
    DSD_MEMSET(sacch_bytes, 0, sizeof(sacch_bytes));

    /* Consider all SACCH CRC parts as correct */
    CrcCorrect = 1;

    /* Reconstitute the full 72 bits SACCH */
    for (i = 0; i < 4; i++) {
        DSD_MEMCPY(&SACCH[(size_t)i * 18], state->nxdn_sacch_frame_segment[i], 18);

        /* Check CRC */
        if (state->nxdn_sacch_frame_segcrc[i] != 0) {
            CrcCorrect = 0;
        }
    }

    /* Decodes the element content */
    // currently only going to run this if all four CRCs are good
    if (CrcCorrect == 1) {
        nxdn_confirm_note_evidence(state, NXDN_EVIDENCE_STRONG);
        NXDN_Elements_Content_decode(opts, state, SACCH, sizeof(SACCH));
    }

    //reset the sacch field -- Github Issue #118
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, " Full SACCH Payload ");
        for (i = 0; i < 9; i++) {
            sacch_bytes[i] = (uint8_t)convert_bits_into_output(&SACCH[(size_t)i * 8], 8);
            DSD_FPRINTF(stderr, "[%02X]", sacch_bytes[i]);
        }
    }

} /* End NXDN_SACCH_Full_decode() */

/* Whether a channel handed over at least the `min_bits` elements a message's parser reads. Each channel's buffer has
 * its own size, and a non-superframe SACCH's is 26 elements (nxdn_handle_sacch()), short of most messages, while its
 * 6-bit CRC passes noise one time in 64: every parser is bounded by the length it was given. */
static int
nxdn_element_has_bits(const char* name, size_t elements_bits, size_t min_bits) {
    if (elements_bits >= min_bits) {
        return 1;
    }
    DSD_FPRINTF(stderr, " %s Too Short (%zu bits); ", name, elements_bits);
    return 0;
}

/* The fields VCALL, TX_REL and DISC read end with the key ID at elements 58-63; the ARIB form shifts them 8 later. */
enum {
    NXDN_VCALL_MIN_BITS = 64U,
    NXDN_VCALL_ARIB_MIN_BITS = 72U,
};

static void
nxdn_element_handle_idle(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    UNUSED4(opts, state, elements, elements_bits);
}

static void
// cppcheck-suppress constParameterCallback -- signature is fixed by the message-type dispatch table.
nxdn_element_handle_sdcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (elements_bits < 79U) {
        DSD_FPRINTF(stderr, " SDCALL Header Too Short (%zu bits); ", elements_bits);
        return;
    }
    nxdn_sdcall_header(opts, state, elements);
}

static void
nxdn_element_handle_sdcall_data(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    nxdn_dcall_data(opts, state, state->data_header_format[0], elements, elements_bits);
}

static void
nxdn_element_handle_sdcall_iv(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    size_t sdcall_iv_min_bits = 72U;
    if (strcmp(state->nxdn_location_category, "Type-D") == 0) {
        sdcall_iv_min_bits = 30U;
    }
    if (elements_bits < sdcall_iv_min_bits) {
        DSD_FPRINTF(stderr, " SDCALL IV Too Short (%zu bits); ", elements_bits);
        return;
    }
    nxdn_sdcall_iv(opts, state, elements);
}

static void
// cppcheck-suppress constParameterCallback -- signature is fixed by the message-type dispatch table.
nxdn_element_handle_dcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    nxdn_dcall_header(opts, state, elements, elements_bits);
}

static void
nxdn_element_handle_dcall_data(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    nxdn_dcall_data(opts, state, state->data_header_format[0], elements, elements_bits);
}

static void
nxdn_element_handle_call_assignment(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    /* The channel ends at element 71; the DFA's OFN at 79. */
    if (!nxdn_element_has_bits("VCALL_ASSGN", elements_bits, state->nxdn_rcn == 1 ? 80U : 72U)) {
        return;
    }
    NXDN_decode_VCALL_ASSGN(opts, state, elements);
}

static int
nxdn_element_is_standard_alias(const uint8_t* elements, size_t elements_bits) {
    if (elements == NULL || elements_bits < 32U) {
        return 0;
    }

    const uint8_t mfid = (uint8_t)convert_bits_into_output(&elements[8], 8);
    const uint16_t subtype = (uint16_t)convert_bits_into_output(&elements[16], 16);
    return (mfid == 0x68U && subtype == 0x8204U) ? 1 : 0;
}

static void
nxdn_element_print_prop_form(const dsd_opts* opts, const uint8_t* elements, size_t elements_bits) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " PROP_FORM");
    if (opts != NULL && opts->payload == 1 && elements != NULL && elements_bits >= 72U) {
        const uint8_t mfid = (uint8_t)convert_bits_into_output(&elements[8], 8);
        DSD_FPRINTF(stderr, "\n MFID: %02X; Message: ", mfid);
        for (int i = 2; i < 9; i++) {
            const size_t bit_offset = (size_t)i * 8U;
            DSD_FPRINTF(stderr, "%02X", (uint8_t)convert_bits_into_output(&elements[bit_offset], 8));
        }
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
nxdn_element_handle_alias(const dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_element_is_standard_alias(elements, elements_bits)) {
        nxdn_element_print_prop_form(opts, elements, elements_bits);
        return;
    }
    /* Four characters from element 40. */
    if (!nxdn_element_has_bits("ALIAS", elements_bits, 72U)) {
        return;
    }
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " ALIAS");
    DSD_FPRINTF(stderr, "%s", KNRM);
    nxdn_alias_decode_prop(opts, state, elements);
}

static const char*
nxdn_dst_info_segment_label(uint8_t start, uint8_t end) {
    if (start != 0U && end != 0U) {
        return "Full";
    }
    if (start != 0U) {
        return "First";
    }
    if (end != 0U) {
        return "Last";
    }
    return "Next";
}

static void
nxdn_element_handle_dst_info(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    enum {
        NXDN_DST_INFO_HEADER_BITS = 16U,
        NXDN_DST_INFO_MAX_CHARS = 25U,
    };

    if (elements_bits < NXDN_DST_INFO_HEADER_BITS) {
        DSD_FPRINTF(stderr, " DST_ID_INFO Too Short (%zu bits); ", elements_bits);
        return;
    }

    char station_id_string[NXDN_DST_INFO_MAX_CHARS + 1U];
    DSD_MEMSET(station_id_string, 0, sizeof(station_id_string));

    const uint8_t start = elements[8] & 1U;
    const uint8_t end = elements[9] & 1U;
    const uint8_t option = (uint8_t)convert_bits_into_output(&elements[8], 8);
    const uint8_t num_chars_field = (uint8_t)convert_bits_into_output(&elements[10], 6);
    size_t requested_chars = (start == 0U) ? 25U : (size_t)num_chars_field + 1U;
    const size_t available_chars = (elements_bits - NXDN_DST_INFO_HEADER_BITS) / 8U;

    if (requested_chars > NXDN_DST_INFO_MAX_CHARS) {
        requested_chars = NXDN_DST_INFO_MAX_CHARS;
    }
    if (requested_chars > available_chars) {
        requested_chars = available_chars;
    }

    for (size_t i = 0; i < requested_chars; i++) {
        uint8_t c = (uint8_t)convert_bits_into_output(&elements[NXDN_DST_INFO_HEADER_BITS + (i * 8U)], 8);
        if (c >= 0x20U && c <= 0x7EU) {
            station_id_string[i] = (char)c;
        }
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Station Identification Information - %s ID: %s ", nxdn_dst_info_segment_label(start, end),
                station_id_string);

    if (start != 0U && end != 0U) {
        char event_string[55];
        DSD_SNPRINTF(event_string, sizeof(event_string), "NXDN Digital Station ID: %s", station_id_string);
        const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, 0U, 65520U, 0U);
        (void)dsd_event_emit_data_notice(opts, state, 0U, &observation, event_string);
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n Option: %02X; Start: %u; End %u; Characters: %zu or Sequence: %02X;", option, start,
                    end, requested_chars, option & 0x3FU);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
nxdn_element_mark_control_sync(const dsd_opts* opts, dsd_state* state) {
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    nxdn_anchor_control_channel_from_current_tuner(opts, state, 1);
}

/*
 * SRV_INFO (0x19) has one layout per system type, chosen by the channel this frame's own LICH names
 * (dsd_state::nxdn_frame_channel), never by the "Type-D" category an earlier SCCH left behind: on a Type-D (IDAS)
 * RTCH2 it is NXDN TS 1-E §6.4.3.1's 80-element System ID / Site Code form, elsewhere the Type-C location ID form of
 * 72 elements. A Type-C SRV_INFO also reaches a traffic channel's SACCH; only a control channel's says the control
 * channel is here.
 */
static void
nxdn_element_handle_srv_info(const dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (state->nxdn_frame_channel == (uint8_t)NXDN_FRAME_CHANNEL_TYPE_D) {
        if (nxdn_element_has_bits("SRV_INFO", elements_bits, 80U)) {
            NXDN_decode_srv_info_type_d(state, elements);
        }
        return;
    }
    if (!nxdn_element_has_bits("SRV_INFO", elements_bits, 72U)) {
        return;
    }
    const int control_channel = state->nxdn_frame_channel == (uint8_t)NXDN_FRAME_CHANNEL_CONTROL;
    NXDN_decode_srv_info(opts, state, elements, control_channel);
    if (control_channel) {
        nxdn_element_mark_control_sync(opts, state);
    }
}

static void
nxdn_element_handle_cch_info(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    NXDN_decode_cch_info(opts, state, elements, elements_bits);
    nxdn_element_mark_control_sync(opts, state);
}

static void
nxdn_element_handle_site_info(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    NXDN_decode_site_info(opts, state, elements, elements_bits);
    nxdn_element_mark_control_sync(opts, state);
}

static void
nxdn_element_handle_adj_site(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    /* On a Type-D (IDAS) RTCH2 the same type code is a traffic channel's ADJ_SITE_INFO (NXDN TS 1-E §6.4.3.2), with
     * a layout of its own, and says nothing of where a control channel is. */
    if (state->nxdn_frame_channel == (uint8_t)NXDN_FRAME_CHANNEL_TYPE_D) {
        if (nxdn_element_has_bits("ADJ_SITE_INFO", elements_bits, 80U)) {
            NXDN_decode_adj_site_type_d(elements);
        }
        return;
    }
    NXDN_decode_adj_site(opts, state, elements, elements_bits);
    nxdn_element_mark_control_sync(opts, state);
}

static void
nxdn_element_handle_tx_release(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_element_has_bits("TX_REL", elements_bits, NXDN_VCALL_MIN_BITS)) {
        return;
    }
    nxdn_reset_data_call_state(state);
    NXDN_decode_VCALL(opts, state, elements);
}

static void
nxdn_element_handle_vcall(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_element_has_bits("VCALL", elements_bits, NXDN_VCALL_MIN_BITS)) {
        return;
    }
    NXDN_decode_VCALL(opts, state, elements);
}

static void
nxdn_element_handle_disc(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_element_has_bits("DISC", elements_bits, NXDN_VCALL_MIN_BITS)) {
        return;
    }
    nxdn_reset_data_call_state(state);
    NXDN_decode_VCALL(opts, state, elements);
    nxdn_alias_reset(state);

    if ((opts->trunk_enable == 1) && state->p25_cc_freq != 0 && (opts->trunk_is_tuned == 1)) {
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, state->p25_cc_freq, 0, NULL);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            return;
        }
        opts->trunk_is_tuned = 0;
        nxdn_follow_release(state);

        DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
        DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
        (void)dsd_recent_activity_clear_all(state);
        if (state->M == 0) {
            state->nxdn_cipher_type = 0;
            nxdn_cipher_class_reset(state);
        }
    }
}

static void
nxdn_element_handle_vcall_iv(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    /* The IV is 64 bits from element 8, 22 on Type-D (nxdn_vcall_iv_extract()). */
    const size_t min_bits = (strcmp(state->nxdn_location_category, "Type-D") == 0) ? 30U : 72U;
    if (!nxdn_element_has_bits("VCALL_IV", elements_bits, min_bits)) {
        return;
    }
    NXDN_decode_VCALL_IV(opts, state, elements);
}

static nxdn_element_handler_fn
nxdn_element_dispatch_handler(uint8_t message_type) {
    static const struct nxdn_element_dispatch_entry dispatch[] = {
        {0x38, nxdn_element_handle_sdcall_header},
        {0x39, nxdn_element_handle_sdcall_data},
        {0x3A, nxdn_element_handle_sdcall_iv},
        {0x09, nxdn_element_handle_dcall_header},
        {0x0B, nxdn_element_handle_dcall_data},
        {0x05, nxdn_element_handle_call_assignment},
        {0x0D, nxdn_element_handle_call_assignment},
        {0x04, nxdn_element_handle_call_assignment},
        {0x0E, nxdn_element_handle_call_assignment},
        {0x17, nxdn_element_handle_dst_info},
        {0x1A, nxdn_element_handle_cch_info},
        {0x18, nxdn_element_handle_site_info},
        {0x1B, nxdn_element_handle_adj_site},
        {0x07, nxdn_element_handle_tx_release},
        {0x08, nxdn_element_handle_tx_release},
        {0x01, nxdn_element_handle_vcall},
        {0x11, nxdn_element_handle_disc},
        {0x10, nxdn_element_handle_idle},
        {0x03, nxdn_element_handle_vcall_iv},
    };

    for (size_t i = 0; i < sizeof(dispatch) / sizeof(dispatch[0]); i++) {
        if (dispatch[i].message_type == message_type) {
            return dispatch[i].handler;
        }
    }

    return NULL;
}

void
NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, const uint8_t* ElementsContent, size_t elements_bits) {
    enum { NXDN_ELEMENTS_MIN_MESSAGE_TYPE_BITS = 8U };

    if (opts == NULL || state == NULL || ElementsContent == NULL) {
        return;
    }

    if (elements_bits < NXDN_ELEMENTS_MIN_MESSAGE_TYPE_BITS) {
        return;
    }

    uint8_t MessageType;
    uint8_t MessageTypeExt;
    uint8_t MessageTypeDispatch;
    /* Get the "Message Type" field */
    MessageType = (ElementsContent[2] & 1) << 5;
    MessageType |= (ElementsContent[3] & 1) << 4;
    MessageType |= (ElementsContent[4] & 1) << 3;
    MessageType |= (ElementsContent[5] & 1) << 2;
    MessageType |= (ElementsContent[6] & 1) << 1;
    MessageType |= (ElementsContent[7] & 1) << 0;

    /* Save the "F1" and "F2" flags */
    state->NxdnElementsContent.F1 = ElementsContent[0];
    state->NxdnElementsContent.F2 = ElementsContent[1];

    MessageTypeExt = (uint8_t)(((state->NxdnElementsContent.F1 & 1U) << 7U)
                               | ((state->NxdnElementsContent.F2 & 1U) << 6U) | MessageType);
    MessageTypeDispatch = MessageType;

    /* Save the "Message Type" field */
    state->NxdnElementsContent.MessageType = MessageType;

    nxdn_message_type(opts, state, MessageTypeExt);

    if (MessageTypeExt == 0xE7U) {
        /* Six characters from element 24. */
        if (nxdn_element_has_bits("ARIB ALIAS", elements_bits, 72U)) {
            nxdn_alias_decode_arib(opts, state, ElementsContent);
        }
        return;
    }

    if (MessageTypeExt == 0xE1U || MessageTypeExt == 0xE8U) {
        if (nxdn_element_has_bits("ARIB VCALL", elements_bits, NXDN_VCALL_ARIB_MIN_BITS)) {
            NXDN_decode_VCALL_ARIB(opts, state, ElementsContent);
        }
        return;
    }

    if (MessageTypeDispatch == 0x19U) {
        nxdn_element_handle_srv_info(opts, state, ElementsContent, elements_bits);
        return;
    }

    if (MessageTypeDispatch == 0x3FU) {
        nxdn_element_handle_alias(opts, state, ElementsContent, elements_bits);
        return;
    }

    nxdn_element_handler_fn handler = nxdn_element_dispatch_handler(MessageTypeDispatch);
    if (handler != NULL) {
        handler(opts, state, ElementsContent, elements_bits);
    }
}

static void
nxdn_data_call_option_to_str(uint8_t data_call_option, char* duplex, size_t duplex_sz, char* mode, size_t mode_sz) {
    static const char* const modes[16] = {
        "4800bps",        "Reserved 1",     "9600bps",        "Reserved 3",     "Reserved 4",  "Reserved 5",
        "Reserved 6",     "Reserved 7",     "4800bps S:1",    "Reserved 9 S:1", "9600bps S:1", "Reserved B S:1",
        "Reserved C S:1", "Reserved D S:1", "Reserved E S:1", "Reserved F S:1",
    };
    if (duplex != NULL && duplex_sz > 0U) {
        DSD_SNPRINTF(duplex, duplex_sz, "%s", (data_call_option & 0x10U) ? "Duplex" : "Half Duplex");
    }

    if (mode != NULL && mode_sz > 0U) {
        const char* mode_str = modes[data_call_option & 0x0FU];
        DSD_SNPRINTF(mode, mode_sz, "%s", mode_str);
    }
}

static int
nxdn_load_data_aes_key(const dsd_state* state, uint8_t key_id, uint8_t out_key[32]) {
    if (state == NULL || out_key == NULL) {
        return 0;
    }

    DSD_MEMSET(out_key, 0, 32U);

    if (state->keyloader == 1) {
        for (int i = 0; i < 8; i++) {
            out_key[i + 0] = (uint8_t)((state->rkey_array[key_id + 0x000] >> (56 - (i * 8))) & 0xFFU);
            out_key[i + 8] = (uint8_t)((state->rkey_array[key_id + 0x101] >> (56 - (i * 8))) & 0xFFU);
            out_key[i + 16] = (uint8_t)((state->rkey_array[key_id + 0x201] >> (56 - (i * 8))) & 0xFFU);
            out_key[i + 24] = (uint8_t)((state->rkey_array[key_id + 0x301] >> (56 - (i * 8))) & 0xFFU);
        }
        // NXDN AES is AES-256: usable only with all four segments present (by their import flags, zero allowed).
        return keyring_kid_aes_segments(state, key_id) >= 4;
    } else {
        for (int i = 0; i < 8; i++) {
            out_key[i + 0] = (uint8_t)((state->K1 >> (56 - (i * 8))) & 0xFFU);
            out_key[i + 8] = (uint8_t)((state->K2 >> (56 - (i * 8))) & 0xFFU);
            out_key[i + 16] = (uint8_t)((state->K3 >> (56 - (i * 8))) & 0xFFU);
            out_key[i + 24] = (uint8_t)((state->K4 >> (56 - (i * 8))) & 0xFFU);
        }
    }

    uint8_t zero[32];
    DSD_MEMSET(zero, 0, sizeof(zero));
    return memcmp(out_key, zero, sizeof(zero)) != 0;
}

static void
nxdn_sdcall_iv(dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
    UNUSED(opts);

    if (state == NULL || Message == NULL) {
        return;
    }

    uint8_t idas = (strcmp(state->nxdn_location_category, "Type-D") == 0) ? 1U : 0U;
    if (idas != 0U) {
        state->payload_mi = (unsigned long long int)convert_bits_into_output(Message + 8, 22);
    } else {
        state->payload_mi = (unsigned long long int)convert_bits_into_output(Message + 8, 64);
        if (state->payload_algid == 3) {
            nxdn_lfsr128_expand_iv_from_mi64((uint64_t)state->payload_mi, state->aes_ivR);
        }
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n  SDCALL_IV: %016llX", state->payload_mi);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

/*
 * Report a decoded data-call header to the trunk-scan coordinator, so a parked conventional NXDN
 * target keeps its activity hold for data traffic and not only for voice. A header is the only
 * data element carrying a call identity; the blocks that follow it carry none.
 *
 * A header reaches here only through a channel decoder whose CRC passed, and is held behind the
 * frame's confirmation for the same reason the VCALL site is: an unverified header carries a
 * garbage identity, and acting on one parks the coordinator on noise for a full activity_hold_ms.
 * The header's own two-bit cipher field is the encryption classification -- data calls have no
 * equivalent of the voice hysteresis in nxdn_enc_class.c -- and a misread costs at most one hold
 * refresh, because talkgroup policy exempts data calls from the encrypted-target lockout ledger.
 */
static void
nxdn_data_header_report_scan_activity(const dsd_opts* opts, const dsd_state* state, uint8_t call_type, uint16_t source,
                                      uint16_t target, uint8_t cipher) {
    if (!nxdn_confirm_is_confirmed(state)) {
        return;
    }
    // call_type 4 is the individual/private call, spelled the same way as the trunked data-grant
    // path in NXDN_decode_VCALL_ASSGN().
    dsd_trunk_scan_hook_nxdn_conventional_activity(opts, state, target, source, (call_type == 4U) ? 1 : 0,
                                                   (cipher != 0U) ? 1 : 0, 1);
}

static void
nxdn_sdcall_header(const dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
    if (state == NULL || Message == NULL) {
        return;
    }

    state->payload_mi = 0ULL;
    DSD_MEMSET(state->aes_ivR, 0, sizeof(state->aes_ivR));

    uint8_t idas = (strcmp(state->nxdn_location_category, "Type-D") == 0) ? 1U : 0U;
    uint8_t cc_option = (uint8_t)convert_bits_into_output(Message + 8, 8);
    uint8_t call_type = (uint8_t)convert_bits_into_output(Message + 16, 3);
    uint8_t dcall_opt = (uint8_t)convert_bits_into_output(Message + 19, 5);
    uint16_t source = (uint16_t)convert_bits_into_output(Message + 24, 16);
    uint16_t target = (uint16_t)convert_bits_into_output(Message + 40, 16);
    uint8_t cipher = (uint8_t)convert_bits_into_output(Message + 56, 2);
    uint8_t key_id = (uint8_t)convert_bits_into_output(Message + 58, 6);
    uint16_t pkt_info = (uint16_t)convert_bits_into_output(Message + 64, 16);

    uint8_t confirmed_delivery = Message[64];
    uint8_t spare1 = Message[65];
    uint8_t selective_retry = Message[66];
    uint8_t spare2 = Message[67];
    uint8_t block_count = (uint8_t)convert_bits_into_output(Message + 68, 4);
    uint8_t pad_bytes = (uint8_t)convert_bits_into_output(Message + 72, 5);
    uint8_t start_frag = Message[77];
    uint8_t circulate = Message[78];

    uint16_t source_ch = 0U;
    uint16_t target_ch = 0U;
    if (idas != 0U) {
        source_ch = (uint16_t)((source >> 11) & 0x1FU);
        target_ch = (uint16_t)((target >> 11) & 0x1FU);
        source &= 0x7FFU;
        target &= 0x7FFU;
    }

    char duplex[32];
    char mode[32];
    DSD_MEMSET(duplex, 0, sizeof(duplex));
    DSD_MEMSET(mode, 0, sizeof(mode));
    nxdn_data_call_option_to_str(dcall_opt, duplex, sizeof(duplex), mode, sizeof(mode));

    DSD_FPRINTF(stderr, "\n %sSD Data Call Header (%04X) ", KCYN, pkt_info);
    if (idas == 0U) {
        DSD_FPRINTF(stderr, "Source: %u; Target: %u; ", source, target);
    } else {
        DSD_FPRINTF(stderr, "Source: %u-%u; Target: %u-%u; ", source_ch, source, target_ch, target);
    }
    DSD_FPRINTF(stderr, "%s %s %s ", NXDN_Call_Type_To_Str(call_type), mode, duplex);
    if (cc_option != 0U) {
        DSD_FPRINTF(stderr, "CCOPT: %02X; ", cc_option);
    }
    DSD_FPRINTF(stderr, "\n Blocks: %u; Padding: %u; ", block_count, pad_bytes);
    if (cipher != 0U) {
        DSD_FPRINTF(stderr, "ENC; Cipher: %u; Key ID: %02X; ", (unsigned)cipher, (unsigned)key_id);
    }
    if (spare1 != 0U) {
        DSD_FPRINTF(stderr, "S1; ");
    }
    if (spare2 != 0U) {
        DSD_FPRINTF(stderr, "S2; ");
    }
    if (start_frag != 0U) {
        DSD_FPRINTF(stderr, "Starting Fragment; ");
    }
    if (circulate != 0U) {
        DSD_FPRINTF(stderr, "Circulate; ");
    }
    if (confirmed_delivery != 0U) {
        DSD_FPRINTF(stderr, "Confirmed Delivery; ");
    }
    if (selective_retry != 0U) {
        DSD_FPRINTF(stderr, "Selective Retry; ");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);

    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    state->data_header_blocks[0] = (block_count > 0U) ? (int)block_count : 1;
    state->data_header_padding[0] = pad_bytes;
    state->data_header_valid[0] = 1U;
    state->payload_algid = cipher;
    state->payload_keyid = key_id;
    state->dmr_lrrp_source[0] = source;
    state->dmr_lrrp_target[0] = target;
    // Not a DMR talkgroup: clear the qualifier so a stale DMR group flag cannot make the
    // --dmr-tg-key-csv lookup treat this address as one.
    state->dmr_data_target_is_group[0] = 0;

    nxdn_data_header_report_scan_activity(opts, state, call_type, source, target, cipher);
}

struct nxdn_dcall_header_info {
    uint8_t idas;
    uint8_t cc_option;
    uint8_t call_type;
    uint8_t dcall_opt;
    uint8_t cipher;
    uint8_t key_id;
    uint8_t confirmed_delivery;
    uint8_t spare1;
    uint8_t selective_retry;
    uint8_t spare2;
    uint8_t block_count;
    uint8_t pad_bytes;
    uint8_t start_frag;
    uint8_t circulate;
    uint8_t iv_available;
    uint16_t source;
    uint16_t target;
    uint16_t source_ch;
    uint16_t target_ch;
    uint16_t tx_frag_count;
    uint32_t pkt_info;
};

static void
nxdn_dcall_header_parse(struct nxdn_dcall_header_info* info, dsd_state* state, const uint8_t* Message,
                        size_t message_bits) {
    enum {
        NXDN_DCALL_IV_OFFSET_BITS = 88U,
        NXDN_DCALL_IV_PRESENCE_BITS = 8U,
        NXDN_DCALL_IDAS_IV_BITS = 22U,
        NXDN_DCALL_WIDE_IV_BITS = 64U,
    };

    DSD_MEMSET(info, 0, sizeof(*info));
    info->idas = (strcmp(state->nxdn_location_category, "Type-D") == 0) ? 1U : 0U;
    info->cc_option = (uint8_t)convert_bits_into_output(Message + 8, 8);
    info->call_type = (uint8_t)convert_bits_into_output(Message + 16, 3);
    info->dcall_opt = (uint8_t)convert_bits_into_output(Message + 19, 5);
    info->source = (uint16_t)convert_bits_into_output(Message + 24, 16);
    info->target = (uint16_t)convert_bits_into_output(Message + 40, 16);
    info->cipher = (uint8_t)convert_bits_into_output(Message + 56, 2);
    info->key_id = (uint8_t)convert_bits_into_output(Message + 58, 6);
    info->pkt_info = (uint32_t)convert_bits_into_output(Message + 64, 24);
    info->confirmed_delivery = Message[64];
    info->spare1 = Message[65];
    info->selective_retry = Message[66];
    info->spare2 = Message[67];
    info->block_count = (uint8_t)convert_bits_into_output(Message + 68, 4);
    info->pad_bytes = (uint8_t)convert_bits_into_output(Message + 72, 5);
    info->start_frag = Message[77];
    info->circulate = Message[78];
    info->tx_frag_count = (uint16_t)convert_bits_into_output(Message + 79, 9);

    if (info->idas != 0U) {
        info->source_ch = (uint16_t)((info->source >> 11) & 0x1FU);
        info->target_ch = (uint16_t)((info->target >> 11) & 0x1FU);
        info->source &= 0x7FFU;
        info->target &= 0x7FFU;
    }

    if (info->cipher > 1U && message_bits >= (NXDN_DCALL_IV_OFFSET_BITS + NXDN_DCALL_IV_PRESENCE_BITS)) {
        const uint8_t iv_presence =
            (uint8_t)convert_bits_into_output(Message + NXDN_DCALL_IV_OFFSET_BITS, NXDN_DCALL_IV_PRESENCE_BITS);
        const size_t iv_bits = (info->idas != 0U) ? NXDN_DCALL_IDAS_IV_BITS : NXDN_DCALL_WIDE_IV_BITS;
        if (iv_presence != 0U && message_bits >= (NXDN_DCALL_IV_OFFSET_BITS + iv_bits)) {
            info->iv_available = 1U;
            state->payload_mi = (unsigned long long int)convert_bits_into_output(Message + NXDN_DCALL_IV_OFFSET_BITS,
                                                                                 (uint32_t)iv_bits);
            if (info->idas == 0U && info->cipher == 3U) {
                nxdn_lfsr128_expand_iv_from_mi64((uint64_t)state->payload_mi, state->aes_ivR);
            }
        }
    }
}

static void
nxdn_dcall_header_print(const struct nxdn_dcall_header_info* info, const dsd_state* state) {
    char duplex[32];
    char mode[32];
    DSD_MEMSET(duplex, 0, sizeof(duplex));
    DSD_MEMSET(mode, 0, sizeof(mode));
    nxdn_data_call_option_to_str(info->dcall_opt, duplex, sizeof(duplex), mode, sizeof(mode));

    DSD_FPRINTF(stderr, "\n %sData Call Header (%06X) ", KCYN, info->pkt_info);
    if (info->idas == 0U) {
        DSD_FPRINTF(stderr, "Source: %u; Target: %u; ", info->source, info->target);
    } else {
        DSD_FPRINTF(stderr, "Source: %u-%u; Target: %u-%u; ", info->source_ch, info->source, info->target_ch,
                    info->target);
    }
    DSD_FPRINTF(stderr, "%s %s %s ", NXDN_Call_Type_To_Str(info->call_type), mode, duplex);
    if (info->cc_option != 0U) {
        DSD_FPRINTF(stderr, "CCOPT: %02X; ", info->cc_option);
    }
    DSD_FPRINTF(stderr, "\n Blocks: %u; Padding: %u; TX Frag: %u; ", info->block_count, info->pad_bytes,
                info->tx_frag_count);
    if (info->cipher != 0U) {
        DSD_FPRINTF(stderr, "ENC; Cipher: %u; Key ID: %02X; ", (unsigned)info->cipher, (unsigned)info->key_id);
        if (info->iv_available != 0U) {
            DSD_FPRINTF(stderr, "IV: %016llX; ", state->payload_mi);
        }
    }
    if (info->spare1 != 0U) {
        DSD_FPRINTF(stderr, "S1; ");
    }
    if (info->spare2 != 0U) {
        DSD_FPRINTF(stderr, "S2; ");
    }
    if (info->start_frag != 0U) {
        DSD_FPRINTF(stderr, "Starting Fragment; ");
    }
    if (info->circulate != 0U) {
        DSD_FPRINTF(stderr, "Circulate; ");
    }
    if (info->confirmed_delivery != 0U) {
        DSD_FPRINTF(stderr, "Confirmed Delivery; ");
    }
    if (info->selective_retry != 0U) {
        DSD_FPRINTF(stderr, "Selective Retry; ");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
nxdn_dcall_header_apply(dsd_state* state, const struct nxdn_dcall_header_info* info) {
    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    state->data_header_blocks[0] = (info->block_count > 0U) ? (int)info->block_count : 1;
    state->data_header_padding[0] = info->pad_bytes;
    state->data_header_valid[0] = 1U;
    state->payload_algid = info->cipher;
    state->payload_keyid = info->key_id;
    state->dmr_lrrp_source[0] = info->source;
    state->dmr_lrrp_target[0] = info->target;
    // Not a DMR talkgroup -- see the sibling header handler above.
    state->dmr_data_target_is_group[0] = 0;
}

static void
nxdn_dcall_header(const dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits) {
    if (state == NULL || Message == NULL) {
        return;
    }

    if (message_bits < 88U) {
        return;
    }

    state->payload_mi = 0ULL;
    DSD_MEMSET(state->aes_ivR, 0, sizeof(state->aes_ivR));

    struct nxdn_dcall_header_info info;
    nxdn_dcall_header_parse(&info, state, Message, message_bits);
    nxdn_dcall_header_print(&info, state);
    nxdn_dcall_header_apply(state, &info);
    nxdn_data_header_report_scan_activity(opts, state, info.call_type, info.source, info.target, info.cipher);
}

enum { NXDN_DCALL_MAX_BITS = 24 * 128, NXDN_DCALL_MAX_BYTES = NXDN_DCALL_MAX_BITS / 8 };

struct nxdn_dcall_data_context {
    int have_events;
    int byte_len;
    int header_blocks;
    int ptr_bits;
    int total_bytes;
    int total_bits;
    int block_bits;
    uint8_t pf_num;
    uint8_t blk_num;
};

static void
nxdn_dcall_invalidate(dsd_state* state) {
    state->data_header_valid[0] = 0U;
}

static int
nxdn_dcall_byte_len(const dsd_state* state, int type) {
    int byte_len = (strcmp(state->nxdn_location_category, "Type-D") == 0) ? 18 : 20;
    if (type == 2) {
        byte_len = 14;
    }
    if (type == 3) {
        byte_len = 8;
    }
    return byte_len;
}

static int
nxdn_dcall_prepare(dsd_state* state, const uint8_t* Message, size_t message_bits, int type,
                   struct nxdn_dcall_data_context* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    if (message_bits < 16U) {
        DSD_FPRINTF(stderr, "Data Call Frame Too Short (%zu bits); ", message_bits);
        nxdn_dcall_invalidate(state);
        return -1;
    }

    ctx->have_events = (state->event_history_s != NULL);
    ctx->pf_num = (uint8_t)convert_bits_into_output(Message + 8, 4);
    ctx->blk_num = (uint8_t)convert_bits_into_output(Message + 12, 4);
    DSD_FPRINTF(stderr, "\n %sData Call (%u/%u); %s", KCYN, (unsigned)ctx->pf_num, (unsigned)ctx->blk_num, KNRM);

    ctx->byte_len = nxdn_dcall_byte_len(state, type);
    ctx->header_blocks = state->data_header_blocks[0];
    if (ctx->header_blocks < 1) {
        ctx->header_blocks = 1;
    }

    if (state->data_header_valid[0] == 0U) {
        DSD_FPRINTF(stderr, "Missing or Invalid Header; ");
        return -1;
    }
    if ((int)ctx->blk_num > ctx->header_blocks) {
        DSD_FPRINTF(stderr, "Block Num Exceeds Header Reported (%d/%u); ", ctx->header_blocks, (unsigned)ctx->blk_num);
        nxdn_dcall_invalidate(state);
        return -1;
    }
    if (ctx->pf_num != ctx->blk_num) {
        DSD_FPRINTF(stderr, "Partial Selective Retry, Previous Delivery Not Retained in Memory; ");
        nxdn_dcall_invalidate(state);
        return -1;
    }

    ctx->ptr_bits = ctx->byte_len * 8 * (ctx->header_blocks - (int)ctx->blk_num);
    ctx->total_bytes = (ctx->header_blocks + 1) * ctx->byte_len;
    if ((int)state->data_header_padding[0] > ctx->total_bytes) {
        DSD_FPRINTF(stderr, "Invalid Header Padding (%u > %d); ", (unsigned)state->data_header_padding[0],
                    ctx->total_bytes);
        nxdn_dcall_invalidate(state);
        return -1;
    }
    ctx->total_bytes -= (int)state->data_header_padding[0];

    if (ctx->total_bytes < 4 || ctx->total_bytes > NXDN_DCALL_MAX_BYTES) {
        DSD_FPRINTF(stderr, "Total Bytes Out of Range (%d); ", ctx->total_bytes);
        nxdn_dcall_invalidate(state);
        return -1;
    }

    ctx->block_bits = ctx->byte_len * 8;
    if (ctx->ptr_bits < 0 || (ctx->ptr_bits + ctx->block_bits) > NXDN_DCALL_MAX_BITS) {
        DSD_FPRINTF(stderr, "PDU Assembly Pointer Out of Range (ptr=%d bits=%d); ", ctx->ptr_bits, ctx->block_bits);
        nxdn_dcall_invalidate(state);
        return -1;
    }

    const size_t required_bits = 16U + (size_t)ctx->block_bits;
    if (message_bits < required_bits) {
        DSD_FPRINTF(stderr, "Data Call Frame Too Short (%zu < %zu bits); ", message_bits, required_bits);
        nxdn_dcall_invalidate(state);
        return -1;
    }

    DSD_MEMCPY(state->dmr_pdu_sf[0] + ctx->ptr_bits, Message + 16, (size_t)ctx->block_bits * sizeof(uint8_t));
    ctx->total_bits = ctx->total_bytes * 8;
    return 0;
}

static void
nxdn_dcall_apply_decryption(const dsd_opts* opts, dsd_state* state, const struct nxdn_dcall_data_context* ctx) {
    uint8_t ks[NXDN_DCALL_MAX_BITS];
    uint8_t aes_key[32];
    DSD_MEMSET(ks, 0, sizeof(ks));
    DSD_MEMSET(aes_key, 0, sizeof(aes_key));

    uint64_t key = 0ULL;
    int key_present = 0;
    int aes_key_loaded = 0;
    if (state->payload_algid != 0) {
        if (state->payload_algid == 3) {
            aes_key_loaded = nxdn_load_data_aes_key(state, (uint8_t)state->payload_keyid, aes_key);
        } else if (state->keyloader == 1) {
            key = state->rkey_array[state->payload_keyid];
            key_present = keyring_kid_scalar_present(state, state->payload_keyid);
        } else {
            key = state->R;
            key_present = dsd_key_scalar_present(state, 0);
        }

        DSD_FPRINTF(stderr, "\n Encrypted Data; Cipher: %d; Key ID: %02X;", state->payload_algid, state->payload_keyid);
        if (state->payload_algid > 1) {
            DSD_FPRINTF(stderr, " IV: %016llX;", state->payload_mi);
        }
    }

    if (state->payload_algid == 1 && key_present) {
        char key_text[16];
        DSD_FPRINTF(stderr, " Key: %s;",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, key, 5U));
        nxdn_pdu_scrambler_keystream_creation(ks, (int)(key & 0x7FFFU), ctx->total_bits);
    } else if (state->payload_algid == 2 && key_present) {
        char key_text[17];
        DSD_FPRINTF(stderr, " Key: %s;",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, key, 16U, 0));
        const int nblocks = (ctx->total_bytes + 7) / 8;
        uint8_t ks_bytes[NXDN_DCALL_MAX_BYTES];
        DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));
        des_ofb_keystream_output(state->payload_mi, key, ks_bytes, nblocks);
        DSD_UNPACK_ARRAY_TO_BITS(ks_bytes, ks, (size_t)nblocks * 8U);
    } else if (state->payload_algid == 3 && aes_key_loaded == 1) {
        if (state->payload_mi != 0ULL) {
            nxdn_lfsr128_expand_iv_from_mi64((uint64_t)state->payload_mi, state->aes_ivR);
        }
        char key_text[65];
        DSD_FPRINTF(stderr, " Key: %s;",
                    dsd_secret_format_byte_hex(key_text, sizeof key_text, opts->show_keys, aes_key, sizeof(aes_key)));
        const int nblocks = (ctx->total_bytes + 15) / 16;
        uint8_t ks_bytes[NXDN_DCALL_MAX_BYTES];
        DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));
        aes_ofb_keystream_output(state->aes_ivR, aes_key, ks_bytes, DSD_AES_KEY_256, nblocks);
        DSD_UNPACK_ARRAY_TO_BITS(ks_bytes, ks, (size_t)nblocks * 16U);
    }

    for (int i = 0; i < ctx->total_bits; i++) {
        state->dmr_pdu_sf[0][i] ^= ks[i];
    }
}

static void
nxdn_dcall_print_payload(const dsd_opts* opts, const dsd_state* state, const struct nxdn_dcall_data_context* ctx) {
    if (opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "\n DATA: ");
    for (int i = 0; i < ctx->total_bytes; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)convert_bits_into_output(state->dmr_pdu_sf[0] + ((size_t)i * 8U), 8));
    }
}

static void
nxdn_dcall_watchdog(dsd_opts* opts, dsd_state* state, const char* event_text) {
    dsd_event_stage_text(state, 0, event_text);
    const uint32_t source = (uint32_t)state->dmr_lrrp_source[0];
    const uint32_t target = (uint32_t)state->dmr_lrrp_target[0];
    char comp_string[128];
    DSD_SNPRINTF(comp_string, sizeof(comp_string), "DATA CALL SRC: %u; TGT: %u;", source, target);
    const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, 0U, source, target);
    (void)dsd_event_emit_data_notice(opts, state, 0U, &observation, comp_string);
}

static void
nxdn_dcall_handle_reverse_gps(const dsd_opts* opts, dsd_state* state, const struct nxdn_dcall_data_context* ctx) {
    uint8_t reverse_bytes[NXDN_DCALL_MAX_BYTES];
    DSD_MEMSET(reverse_bytes, 0, sizeof(reverse_bytes));

    const int reverse_len = ctx->total_bytes - 4;
    int src_idx = ctx->total_bytes - 5;
    for (int i = 0; i < reverse_len; i++, src_idx--) {
        reverse_bytes[i] = (uint8_t)convert_bits_into_output(state->dmr_pdu_sf[0] + ((size_t)src_idx * 8U), 8);
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n  REV: ");
        for (int i = 0; i < reverse_len; i++) {
            DSD_FPRINTF(stderr, "%02X", reverse_bytes[i]);
        }
    }

    const int core_len = reverse_len - 4;
    uint8_t reverse_bits[NXDN_DCALL_MAX_BITS];
    DSD_MEMSET(reverse_bits, 0, sizeof(reverse_bits));
    DSD_UNPACK_ARRAY_TO_BITS(reverse_bytes, reverse_bits, core_len);
    if (core_len >= 2 && (uint16_t)convert_bits_into_output(reverse_bits, 16) == 0xFFFCU) {
        nxdn_gps_report(opts, state, reverse_bits + 16, (uint32_t)state->dmr_lrrp_source[0]);
    }
}

static void
nxdn_dcall_handle_crc_ok(dsd_opts* opts, dsd_state* state, const struct nxdn_dcall_data_context* ctx) {
    const uint8_t opcode = (uint8_t)convert_bits_into_output(state->dmr_pdu_sf[0], 8);
    const uint8_t nmea = (uint8_t)convert_bits_into_output(state->dmr_pdu_sf[0] + 8, 8);
    const uint32_t reverse = (uint32_t)convert_bits_into_output(state->dmr_pdu_sf[0], 24);

    if (opcode == 0x06U && (nmea == (uint8_t)'$' || nmea == (uint8_t)'!')) {
        if (ctx->total_bytes > 1) {
            DSD_FPRINTF(stderr, "\n ");
            nmea_sentence_checker(opts, state, state->dmr_pdu_sf[0] + 8, 0, ctx->total_bytes - 1);
        }
    } else if (reverse == 0U && ctx->total_bytes > 8) {
        nxdn_dcall_handle_reverse_gps(opts, state, ctx);
    } else if (ctx->have_events) {
        const uint16_t fmt = (uint16_t)convert_bits_into_output(state->dmr_pdu_sf[0], 16);
        char event_text[64];
        DSD_SNPRINTF(event_text, sizeof(event_text), "Unknown Data Call Format: %04X;", fmt);
        nxdn_dcall_watchdog(opts, state, event_text);
    }
}

static void
nxdn_dcall_handle_crc_error(dsd_opts* opts, dsd_state* state, const struct nxdn_dcall_data_context* ctx,
                            uint32_t crc_ext, uint32_t crc_chk) {
    DSD_FPRINTF(stderr, " CRC: %08X / %08X; (CRC ERR) ", crc_ext, crc_chk);
    if (state->payload_algid != 0 && ctx->have_events) {
        char event_text[64];
        DSD_SNPRINTF(event_text, sizeof(event_text), "Encrypted PDU; Cipher: %d; KID: %02X;", state->payload_algid,
                     state->payload_keyid);
        nxdn_dcall_watchdog(opts, state, event_text);
    }
}

static int
nxdn_dcall_data(dsd_opts* opts, dsd_state* state, int type, const uint8_t* Message, size_t message_bits) {
    if (opts == NULL || state == NULL || Message == NULL) {
        return -1;
    }

    struct nxdn_dcall_data_context ctx;
    if (nxdn_dcall_prepare(state, Message, message_bits, type, &ctx) != 0) {
        return -1;
    }
    if (ctx.pf_num != 0U) {
        return 0;
    }

    nxdn_dcall_apply_decryption(opts, state, &ctx);
    const size_t crc_offset_bits = (size_t)ctx.total_bits - 32U;
    const uint32_t crc_ext = (uint32_t)convert_bits_into_output(state->dmr_pdu_sf[0] + crc_offset_bits, 32);
    const uint32_t crc_chk = nxdn_crc32_bits(state->dmr_pdu_sf[0], crc_offset_bits);
    nxdn_dcall_print_payload(opts, state, &ctx);
    if (crc_ext == crc_chk) {
        nxdn_dcall_handle_crc_ok(opts, state, &ctx);
    } else {
        nxdn_dcall_handle_crc_error(opts, state, &ctx, crc_ext, crc_chk);
    }

    nxdn_reset_data_call_state(state);
    return 0;
}

//externalize multiple sub-element handlers
static void
nxdn_location_id_handler(dsd_state* state, uint32_t location_id, uint8_t type) {
    //6.5.2 Location ID (NXDN TS 1-A): a 2-bit category, then the system and site codes it splits the other 22 bits into
    uint8_t category_bit = location_id >> 22;
    uint32_t sys_code = 0;
    uint16_t site_code = 0;

    char category[14]; //G, R, or L

    if (category_bit == 0) {
        sys_code = ((location_id & 0x3FFFFF) >> 12); //10 bits
        site_code = location_id & 0xFFF;             //12 bits
        DSD_SNPRINTF(category, sizeof(category), "%s", "Global");
    } else if (category_bit == 2) {
        sys_code = ((location_id & 0x3FFFFF) >> 8); //14 bits
        site_code = location_id & 0xFF;             //8 bits
        DSD_SNPRINTF(category, sizeof(category), "%s", "Regional");
    } else if (category_bit == 1) {
        sys_code = ((location_id & 0x3FFFFF) >> 5); //17 bits
        site_code = location_id & 0x1F;             //5 bits
        DSD_SNPRINTF(category, sizeof(category), "%s", "Local");
    } else {
        //err, or we shouldn't ever get here
        DSD_SNPRINTF(category, sizeof(category), "%s", "Reserved/Err");
    }

    //type 0 is for current site, type 1 is for adjacent sites
    if (type != 0) {
        DSD_FPRINTF(stderr, "\n Adjacent Information - Cat: %s - Sys Code: %d - Site Code %d ", category, sys_code,
                    site_code);
        return;
    }

    DSD_FPRINTF(stderr, "\n Location Information - Cat: %s - Sys Code: %d - Site Code %d ", category, sys_code,
                site_code);
    /* Every caller holds a message its channel's CRC passed (NXDN_Elements_Content_decode()); a short SACCH CRC counts
       once the transmission is confirmed, as the SACCH's own RAN does (nxdn_handle_sacch()). Until then the location is
       decoded and printed, not kept. A reserved category names no site and gives no RAN. */
    if (category_bit == 3U || !nxdn_confirm_is_confirmed(state)) {
        return;
    }
    state->nxdn_last_ran = site_code % 64; //Table 6.3-4 RAN for Trunked Radio Systems
    state->nxdn_last_ran_stand_in = 0U;
    if (site_code != 0) {
        state->nxdn_location_site_code = site_code;
    }
    if (sys_code != 0) {
        state->nxdn_location_sys_code = sys_code;
    }
    DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "%s", category);
    /* The record's category: 0 Global, 1 Regional, 2 Local (the location ID's category bits are 00, 10 and 01). */
    static const uint8_t k_rxid_category[3] = {0U, 2U, 1U};
    dsd_rxid_note_nxdn_location(state, k_rxid_category[category_bit], sys_code, site_code);
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_NXDN;
    note.nxdn_sys = sys_code;
    note.nxdn_site = site_code;
    dsd_signal_search_note_site(state, &note);
}

static void
nxdn_srv_info_handler(dsd_state* state, uint16_t svc_info) {
    UNUSED(state);
    //handle the service information elements
    //Part 1-A Common Air Interface Ver.2.0
    //6.5.33. Service Information
    DSD_FPRINTF(stderr, "\n Services:");
    //check each SIF 1-bit element
    if (svc_info & 0x8000) {
        DSD_FPRINTF(stderr, " Multi-Site;");
    }
    if (svc_info & 0x4000) {
        DSD_FPRINTF(stderr, " Multi-System;");
    }
    if (svc_info & 0x2000) {
        DSD_FPRINTF(stderr, " Location Registration;");
    }
    if (svc_info & 0x1000) {
        DSD_FPRINTF(stderr, " Group Registration;");
    }

    if (svc_info & 0x800) {
        DSD_FPRINTF(stderr, " Authentication;");
    }
    if (svc_info & 0x400) {
        DSD_FPRINTF(stderr, " Composite Control Channel;");
    }
    if (svc_info & 0x200) {
        DSD_FPRINTF(stderr, " Voice Call;");
    }
    if (svc_info & 0x100) {
        DSD_FPRINTF(stderr, " Data Call;");
    }

    if (svc_info & 0x80) {
        DSD_FPRINTF(stderr, " Short Data Call;");
    }
    if (svc_info & 0x40) {
        DSD_FPRINTF(stderr, " Status Call & Remote Control;");
    }
    if (svc_info & 0x20) {
        DSD_FPRINTF(stderr, " PSTN Network Connection;");
    }
    if (svc_info & 0x10) {
        DSD_FPRINTF(stderr, " IP Network Connection;");
    }

    //last 4-bits are spares
}

static void
nxdn_rst_info_handler(dsd_state* state, uint32_t rst_info) {
    UNUSED(state);

    //handle the restriction information elements
    //Part 1-A Common Air Interface Ver.2.0
    //6.5.34. Restriction Information
    DSD_FPRINTF(stderr, "\n RST -");

    //Mobile station operation information (Octet 0, Bits 7 to 4)
    DSD_FPRINTF(stderr, " MS:");
    if (rst_info & 0x800000) {
        DSD_FPRINTF(stderr, " Access Restriction;");
    } else if (rst_info & 0x400000) {
        DSD_FPRINTF(stderr, " Maintenance Restriction;");
    }

    //Access cycle interval (Octet 0, Bits 3 to 0)
    DSD_FPRINTF(stderr, " ACI:");
    uint8_t frames = (rst_info >> 16) & 0xF;
    if (frames) {
        DSD_FPRINTF(stderr, " %d Frame Restriction;", frames * 20);
    }

    //Restriction group specification (Octet 1, Bits 7 to 4)
    DSD_FPRINTF(stderr, " RGS:");
    uint8_t uid = (rst_info >> 12) & 0x7; //MSB is a spare, so only evaluate 3-bits
    DSD_FPRINTF(stderr, " Lower 3 bits of Unit ID = %d %d %d", uid & 1, (uid >> 1) & 1, (uid >> 2) & 1);

    //Restriction Information (Octet 1, Bits 3 to 0)
    DSD_FPRINTF(stderr, " RI:");
    if (rst_info & 0x800) {
        DSD_FPRINTF(stderr, " Location Restriction;");
    } else if (rst_info & 0x400) {
        DSD_FPRINTF(stderr, " Call Restriction;");
    } else if (rst_info & 0x200) {
        DSD_FPRINTF(stderr, " Short Data Restriction;");
    }

    //Restriction group ratio specification (Octet 2, Bits 7 to 6)
    DSD_FPRINTF(stderr, " RT:");
    uint8_t ratio = (rst_info >> 22) & 0x3;
    if (ratio == 1) {
        DSD_FPRINTF(stderr, " 50 Restriction;");
    } else if (ratio == 2) {
        DSD_FPRINTF(stderr, " 75 Restriction;");
    } else if (ratio == 3) {
        DSD_FPRINTF(stderr, " 87.5 Restriction;");
    }

    //Delay time extension specification (Octet 2, Bits 5 to 4)
    DSD_FPRINTF(stderr, " DT:");
    uint8_t dt = (rst_info >> 20) & 0x3;
    if (dt == 0) {
        DSD_FPRINTF(stderr, " Timer T2 max x 1;");
    } else if (dt == 1) {
        DSD_FPRINTF(stderr, " Timer T2 max x 2;");
    } else if (dt == 2) {
        DSD_FPRINTF(stderr, " Timer T2 max x 3;");
    } else {
        DSD_FPRINTF(stderr, " Timer T2 max x 4;");
    }

    //ISO Temporary Isolation Site -- This is valid only if the SIF 1 of Service Information is set to 1.
    if (rst_info & 0x0001) {
        DSD_FPRINTF(stderr, " - Site Isolation;");
    }

    //what a pain...
}

static void
nxdn_ca_info_handler(dsd_state* state, uint32_t ca_info) {
    //handle the channel access info for channel or dfa
    //Part 1-A Common Air Interface Ver.2.0
    //6.5.36. Channel Access Information
    //this element only seems to appear in the SITE_INFO message
    uint32_t RCN = ca_info >> 23;          //Radio Channel Notation
    uint32_t step = (ca_info >> 21) & 0x3; //Stepping
    uint32_t base = (ca_info >> 18) & 0x7; //Base Frequency
    uint32_t spare = ca_info & 0x3FF;
    UNUSED(spare);

    //set state variable here to tell us to use DFA or Channel Versions
    if (RCN == 1) {
        state->nxdn_rcn = RCN;
        state->nxdn_step = step;
        state->nxdn_base_freq = base;
    }
}

//end sub-element handlers

static int
nxdn_policy_tune_allowed(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                         int is_private_call, int data_call, int allow_source_fallback,
                         dsd_tg_policy_decision* out_decision) {
    dsd_tg_policy_decision decision;
    int rc = 0;
    DSD_MEMSET(&decision, 0, sizeof(decision));

    if (is_private_call) {
        rc = dsd_tg_policy_evaluate_private_call(opts, state, source, target, 0, data_call, &decision);
    } else if (allow_source_fallback) {
        // A target with no row is judged by its source's row; the target's final blocks and Hold still apply.
        rc = dsd_tg_policy_evaluate_group_source_fallback(opts, state, target, source, 0, data_call, &decision);
    } else {
        rc = dsd_tg_policy_evaluate_group_call(opts, state, target, source, 0, data_call, &decision);
    }

    if (out_decision) {
        *out_decision = decision;
    }
    return rc == 0 && decision.tune_allowed;
}

static void
nxdn_policy_log_block(const dsd_opts* opts, int is_private_call, uint32_t target, uint32_t source,
                      const dsd_tg_policy_decision* decision) {
    if (!opts || !decision || opts->verbose < 1) {
        return;
    }
    if (decision->block_reasons == DSD_TG_POLICY_BLOCK_NONE) {
        return;
    }
    DSD_FPRINTF(stderr, " [NXDN %s blocked:%s tgt=%u src=%u]", is_private_call ? "private" : "group",
                dsd_tg_policy_block_reason_label(decision->block_reasons), target, source);
}

/* ---- Followed route, held call and preemption (Type-C and Type-D trunking) --------------------------------------- */

/* The followed route holds while the receiver is still tuned to it: an engine-level release (the hangtime return, a
 * trunk scan or operator move) leaves the receiver elsewhere without a word to NXDN. */
static int
nxdn_follow_valid(const dsd_opts* opts, const dsd_state* state) {
    const long int freq = state->nxdn_follow.freq_hz;
    return freq > 0 && dsd_opts_trunk_vc_followed(opts) && state->trunk_vc_freq[0] == freq;
}

/* The frequency the receiver is on: the followed route while it holds, else the voice channel the decoder tuned, else
 * the tuner's own (a radio input), else what a rigctl peer reports (rigctl on an audio input), asked at most once per
 * tuning generation (nxdn_cc_hunt_receiver_frequency()). 0 when nothing names it. */
static long int
nxdn_receiver_freq(const dsd_opts* opts, dsd_state* state) {
    if (nxdn_follow_valid(opts, state)) {
        return state->nxdn_follow.freq_hz;
    }
    if (dsd_opts_trunk_vc_followed(opts) && state->trunk_vc_freq[0] > 0) {
        return state->trunk_vc_freq[0];
    }
    const long int tuned = (long int)dsd_opts_tuned_freq_hz(opts);
    if (tuned > 0) {
        return tuned;
    }
    if (opts->use_rigctl == 1) {
        return nxdn_cc_hunt_receiver_frequency(opts, state);
    }
    return 0;
}

/* Whether a Type-D SCCH naming go-to repeater @p rep speaks of the call on the receiver's repeater. */
typedef enum {
    NXDN_IDAS_UNKNOWN = 0, /* no repeater, no frequency mapped for it, or the receiver's frequency is not known */
    NXDN_IDAS_MATCH,       /* the receiver is on that repeater */
    NXDN_IDAS_OTHER,       /* another repeater */
} nxdn_idas_assoc_t;

static nxdn_idas_assoc_t
nxdn_idas_assoc(const dsd_opts* opts, dsd_state* state, uint8_t rep) {
    if (rep < 1U || rep > 30U) {
        return NXDN_IDAS_UNKNOWN;
    }
    const long int rep_freq = nxdn_channel_to_frequency_quiet(state, rep);
    if (rep_freq <= 0) {
        return NXDN_IDAS_UNKNOWN;
    }
    const long int rx = nxdn_receiver_freq(opts, state);
    if (rx <= 0) {
        return NXDN_IDAS_UNKNOWN;
    }
    return rx == rep_freq ? NXDN_IDAS_MATCH : NXDN_IDAS_OTHER;
}

/* The bound call @p call is the held call's: it holds while active, and once ended only on the followed route within
 * the hangtime (a conversation is held in its gaps); a resident call's identity ends with it. */
static void
nxdn_call_identity_track(const dsd_opts* opts, dsd_state* state, const dsd_call_snapshot* call, double now_m) {
    dsd_nxdn_call_identity* const id = &state->nxdn_call;
    if (id->gap) {
        return;
    }
    if (call->phase == DSD_CALL_PHASE_ACTIVE) {
        id->call_seen_m = now_m;
        return;
    }
    if (call->ended_m > 0.0) {
        id->call_seen_m = call->ended_m;
    }
    if (nxdn_held_call_on_followed_route(opts, state) && (now_m - id->call_seen_m) < (double)opts->trunk_hangtime) {
        return;
    }
    DSD_MEMSET(id, 0, sizeof(*id));
    (void)dsd_tg_policy_clear_active_call(state, -1);
}

/* Bind the held call to the canonical call it names, and let it go when that call is over. One awaiting its call binds
 * to the first to begin after it was named (a resumed older call is not it), and only on the route it was followed to:
 * only a follow leaves it awaiting, so once that route is abandoned (the engine's hangtime or no-carrier return, a
 * release, a retune elsewhere) the call it awaited can never be heard and it goes, before an unrelated call heard later
 * there could take it. Another call ends it; on the followed route, one that began within the hangtime of the named
 * call leaves a gap (unknown, valid 0) whose renaming as the same call keeps the follow time. A new call never inherits
 * the identity, its priority or its dwell. */
static void
nxdn_call_identity_refresh(const dsd_opts* opts, dsd_state* state) {
    dsd_nxdn_call_identity* const id = &state->nxdn_call;
    if (!id->valid && !id->gap) {
        return;
    }
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    const int have = dsd_call_state_get(state, 0U, &call) > 0;
    const uint64_t current = have ? dsd_call_snapshot_transmission(&call) : 0U;
    const double now_m = dsd_decode_now_mono_s();
    if (id->call_epoch == 0U) {
        if (!nxdn_held_call_on_followed_route(opts, state)) {
            DSD_MEMSET(id, 0, sizeof(*id)); // its route was abandoned before its call began
            (void)dsd_tg_policy_clear_active_call(state, -1);
            return;
        }
        if (current <= id->call_floor) {
            return; // still awaiting its call
        }
        id->call_epoch = current;
        id->call_seen_m = now_m;
    }
    if (current != 0U && current == id->call_epoch) {
        nxdn_call_identity_track(opts, state, &call, now_m);
        return;
    }
    if (!id->gap && current != 0U && nxdn_held_call_on_followed_route(opts, state)
        && (call.started_m - id->call_seen_m) < (double)opts->trunk_hangtime) {
        id->valid = 0U;
        id->gap = 1U;
        id->call_epoch = current;
        return;
    }
    DSD_MEMSET(id, 0, sizeof(*id));
    (void)dsd_tg_policy_clear_active_call(state, -1);
}

/* Whether naming @p target (@p is_private, heard on @p rx_freq_hz) names the held call again: its target, kind and
 * frequency (when both are known); a gap record only for the call that followed the named one (@p bound, the call on
 * the air the naming binds to). */
static int
nxdn_call_identity_is_same(const dsd_nxdn_call_identity* id, uint32_t target, uint8_t is_private, long int rx_freq_hz,
                           uint64_t bound) {
    if (!(id->valid || id->gap) || id->target != target || id->private_call != is_private) {
        return 0;
    }
    if (id->rx_freq_hz > 0 && rx_freq_hz > 0 && id->rx_freq_hz != rx_freq_hz) {
        return 0;
    }
    return !id->gap || (bound != 0U && bound == id->call_epoch);
}

/* Bind the held call a naming names to the canonical call: the call on the air when @p on_air (a verified VCALL or SCCH
 * identity of the canonical call); else the first call to begin from now, unless it is the same call named again
 * (@p same) still awaiting its call, still on the air, or faded but still reacquirable (dsd_call_snapshot_may_reacquire():
 * the call a heal reopens keeps its transmission, so it could never pass a floor set now), which keeps its binding.
 * Returns the call on the air it binds to (0: none); @p floor is the epoch after which its call is to begin, UINT64_MAX
 * when it keeps its binding. */
static uint64_t
nxdn_call_identity_bound_call(const dsd_state* state, const dsd_nxdn_call_identity* id, uint32_t target,
                              uint8_t is_private, long int rx_freq_hz, int on_air, int* same, uint64_t* floor) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    const int have = dsd_call_state_get(state, 0U, &call) > 0;
    const uint64_t current = have ? dsd_call_snapshot_transmission(&call) : 0U;
    const int active = have && call.phase == DSD_CALL_PHASE_ACTIVE;
    const uint64_t bound = (on_air && active) ? current : 0U;
    *same = nxdn_call_identity_is_same(id, target, is_private, rx_freq_hz, bound);
    const int bound_here = have && id->call_epoch != 0U && current == id->call_epoch
                           && (active || dsd_call_snapshot_may_reacquire(&call, dsd_decode_now_mono_s()));
    const int keeps = *same && !id->gap && (id->call_epoch == 0U || bound_here);
    *floor = keeps ? UINT64_MAX : call.epoch;
    return bound;
}

/* Take @p target as the held call's identity, a Type-D call when @p type_d, naming the call on the air (@p on_air) or
 * the call a follow is to find (nxdn_call_identity_bound_call()). Its follow time moves only when the identity changes:
 * another target or kind, or another frequency when both are known; a gap record keeps it only for the very call that
 * followed the named one. */
static void
nxdn_call_identity_set(dsd_state* state, uint32_t target, uint32_t source, int private_call, int data_call, int type_d,
                       long int rx_freq_hz, double now_m, int on_air) {
    dsd_nxdn_call_identity* const id = &state->nxdn_call;
    const uint8_t is_private = private_call ? 1U : 0U;
    int same = 0;
    uint64_t floor = 0U;
    const uint64_t bound =
        nxdn_call_identity_bound_call(state, id, target, is_private, rx_freq_hz, on_air, &same, &floor);
    if (!same) {
        id->follow_m = now_m;
        id->source = 0U;
        id->rx_freq_hz = 0;
    }
    if (bound != 0U) {
        id->call_epoch = bound;
        id->call_floor = 0U;
        id->call_seen_m = now_m;
    } else if (floor != UINT64_MAX) {
        id->call_epoch = 0U; // awaiting its call: the first one to begin after the current epoch
        id->call_floor = floor;
        id->call_seen_m = 0.0;
    }
    id->gap = 0U;
    id->valid = 1U;
    id->target = target;
    id->private_call = is_private;
    id->data_call = data_call ? 1U : 0U;
    id->type_d = type_d ? 1U : 0U;
    if (source != 0U) {
        id->source = source;
    }
    if (rx_freq_hz > 0) {
        id->rx_freq_hz = rx_freq_hz;
    }
}

/* How the call @p call names is classified: by the naming message's own classification when it has one (@p msg_private
 * 0/1: a VCALL's call type, an SCCH busy message's G/U bit; -1: none, as for an INFO2 target message), else by the
 * canonical call's group or private kind, else, for a generic voice call (late entry before any VCALL), as the held
 * call is when it is that target. -1: not known; a generic voice call is never read as a group call. */
static int
nxdn_reconcile_private(const dsd_state* state, const dsd_call_snapshot* call, int msg_private) {
    if (msg_private >= 0) {
        return msg_private ? 1 : 0;
    }
    if (call->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        return 1;
    }
    if (call->kind == DSD_CALL_KIND_GROUP_VOICE) {
        return 0;
    }
    /* A gap record (the held call's next call on the followed route) lends its class only to a call naming its target:
       that is the same conversation named again. */
    const dsd_nxdn_call_identity* const id = &state->nxdn_call;
    return ((id->valid || id->gap) && id->target == call->policy_target_id) ? (int)id->private_call : -1;
}

/* The held call is what the canonical call shows once a verified VCALL or an SCCH identity of the receiver's repeater
 * named it: a new call on the same carrier replaces the one followed there. A call whose classification is not known
 * (nxdn_reconcile_private()) leaves the held call unknown, so only Hold displaces it. @p type_d: a Type-D call. */
static void
nxdn_call_identity_reconcile(const dsd_opts* opts, dsd_state* state, int msg_private, int type_d) {
    if (opts->trunk_enable != 1) {
        return;
    }
    nxdn_call_identity_refresh(opts, state);
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_NXDN(call.protocol) || call.policy_target_id == 0U) {
        return;
    }
    const int private_call = nxdn_reconcile_private(state, &call, msg_private);
    if (private_call < 0) {
        DSD_MEMSET(&state->nxdn_call, 0, sizeof(state->nxdn_call));
        return;
    }
    nxdn_call_identity_set(state, call.policy_target_id, call.ota_source_id, private_call, 0, type_d,
                           nxdn_receiver_freq(opts, state), dsd_decode_now_mono_s(), 1);
}

/* A call on the receiver's channel ended (a release, DISC, or a Type-D termination). A held call the receiver followed
 * there stands until its route is released (nxdn_follow_release()), its binding updated from the ended call now
 * (nxdn_call_identity_refresh(): the moment the transmission ended), so a reply within the hangtime of the release is
 * the conversation's gap, however long ago the identity was last read; one known only from the call heard there (a
 * resident call, no grant tune) ended with it, so the next call there is unknown until a message names it and yields
 * only to Hold. */
static void
nxdn_call_identity_end(const dsd_opts* opts, dsd_state* state) {
    if (nxdn_follow_valid(opts, state)) {
        nxdn_call_identity_refresh(opts, state);
        return;
    }
    DSD_MEMSET(&state->nxdn_call, 0, sizeof(state->nxdn_call));
    (void)dsd_tg_policy_clear_active_call(state, -1);
}

/* Whether the receiver is on a Type-D (IDAS) channel: the frame being decoded is an RTCH2's, or the route it followed
 * is a Type-D repeater's. */
static int
nxdn_receiver_on_type_d(const dsd_opts* opts, const dsd_state* state) {
    return state->nxdn_frame_channel == (uint8_t)NXDN_FRAME_CHANNEL_TYPE_D
           || (nxdn_follow_valid(opts, state) && state->nxdn_follow.repeater != 0U);
}

/* The held call when its identity is known here: one heard or followed on another frequency than the receiver's is
 * not, nor one whose call is over (nxdn_call_identity_refresh()). */
static const dsd_nxdn_call_identity*
nxdn_current_identity(const dsd_opts* opts, dsd_state* state, long int rx) {
    nxdn_call_identity_refresh(opts, state);
    const dsd_nxdn_call_identity* const id = &state->nxdn_call;
    if (!id->valid || (id->rx_freq_hz > 0 && rx > 0 && id->rx_freq_hz != rx)) {
        return NULL;
    }
    return id;
}

/* A candidate of the held talkgroup (Hold) takes the receiver from a call that is not of it, a call whose identity is
 * not known included. */
static int
nxdn_hold_takes_over(const dsd_state* state, const dsd_nxdn_call_identity* cur, int hold_matches) {
    return hold_matches && !(cur && cur->target == state->tg_hold);
}

/* The source the held call's policy is judged with: a Type-D call's none, as Type-D admission judges a busy message by
 * its target alone (nxdn_scch_apply_busy_tune()): its source comes from INFO3 updates, which may misname the talker and
 * so never reach policy, group or private. */
static uint32_t
nxdn_held_policy_source(const dsd_nxdn_call_identity* cur) {
    return cur->type_d ? 0U : cur->source;
}

/* A preempting candidate takes the receiver from the held call it outranks (dsd_tg_policy_should_preempt()), judged
 * against that call as described from when it became the held call. A held call whose identity is not known is never
 * preempted. */
static int
nxdn_preempts(const dsd_opts* opts, dsd_state* state, const dsd_nxdn_call_identity* cur, long int rx,
              const dsd_tg_policy_call_route* candidate, const dsd_tg_policy_decision* decision) {
    if (!cur || !decision->preempt_requested) {
        return 0;
    }
    const double now_m = dsd_decode_now_mono_s();
    const uint32_t source = nxdn_held_policy_source(cur);
    dsd_tg_policy_call_route route;
    DSD_MEMSET(&route, 0, sizeof route);
    route.target_id = cur->target;
    route.source_id = source;
    route.freq_hz = rx > 0 ? rx : cur->rx_freq_hz;
    route.channel =
        (nxdn_follow_valid(opts, state) && state->nxdn_follow.freq_hz == route.freq_hz) ? state->nxdn_follow.chan : 0;
    route.slot = -1;
    /* Judged as its admission was: a Type-D call by its target alone, with no source fallback. */
    dsd_tg_policy_decision held;
    (void)nxdn_policy_tune_allowed(opts, state, cur->target, source, cur->private_call, cur->data_call,
                                   cur->type_d ? 0 : 1, &held);
    if (dsd_tg_policy_describe_followed_call(state, &route, &held, cur->follow_m, now_m) != 0) {
        return 0;
    }
    return dsd_tg_policy_should_preempt(opts, state, candidate, decision, now_m);
}

/* How a follow took the receiver: a new call, a preemption, or the call it still holds there followed again. */
typedef enum {
    NXDN_FOLLOW_NEW = 0,
    NXDN_FOLLOW_PREEMPT,
    NXDN_FOLLOW_HELD,
} nxdn_follow_how;

/* Whether the receiver still holds the call @p target names on @p freq, read before the tune: its route holds there and
 * the held call is that one (nxdn_call_identity_set()'s same call). */
static int
nxdn_follow_holds_call(const dsd_opts* opts, dsd_state* state, uint32_t target, int private_call, long int freq) {
    nxdn_call_identity_refresh(opts, state);
    const dsd_nxdn_call_identity* const id = &state->nxdn_call;
    return nxdn_follow_valid(opts, state) && state->nxdn_follow.freq_hz == freq && id->valid && id->target == target
           && id->private_call == (private_call ? 1U : 0U) && (id->rx_freq_hz <= 0 || id->rx_freq_hz == freq);
}

/* The receiver is about to tune to @p freq: whether that leaves the carrier it is on (or one it cannot name), read
 * before the tune. */
static int
nxdn_tune_leaves_carrier(const dsd_opts* opts, dsd_state* state, long int freq) {
    const long int rx = nxdn_receiver_freq(opts, state);
    return rx <= 0 || rx != freq;
}

/* An accepted tune left the carrier the canonical call was heard on (a call a takeover displaced, or one left from an
 * earlier follow): that call is over (EXPLICIT, synchronized, as DMR's dmr_sm_end_left_calls()), so voice heard on the
 * new carrier opens the followed call's own epoch instead of continuing it under its identity and policy, and a sync
 * loss there can never reacquire it. */
static void
nxdn_end_left_call(dsd_opts* opts, dsd_state* state) {
    if (dsd_call_state_end_ex(state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_EXPLICIT) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

/* Whether the call on the air (slot 0's canonical call, active) is the one an announcement names: an NXDN call of
 * @p target, or one nothing has named yet (no target: late entry before its VCALL), whose kind, when it has one, is the
 * announcement's (a generic voice call is of either class). A call another message named is not it. */
static int
nxdn_on_air_call_is(const dsd_state* state, uint32_t target, int private_call, int data_call) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_NXDN(call.protocol) || (call.policy_target_id != 0U && call.policy_target_id != target)) {
        return 0;
    }
    switch (call.kind) {
        case DSD_CALL_KIND_GROUP_VOICE: return !data_call && !private_call;
        case DSD_CALL_KIND_PRIVATE_VOICE: return !data_call && private_call;
        case DSD_CALL_KIND_DATA: return data_call;
        default: return !data_call;
    }
}

/* The receiver went to @p freq for @p target: the route and the held call are the followed one's, and the talkgroup
 * policy's preemption ledger holds it. A new call replaces whatever the ledger held, so nothing left there reads as a
 * takeover; a preemption commits its cooldown; the call the receiver still held is described from its follow time,
 * so a repeat of its announcement never restarts the dwell. Only that still-held call keeps its record: any other
 * follow discards the previous one (the engine's hangtime return leaves it behind, bound to a call that is over), so
 * the followed call never inherits its follow time or binding. The held call binds to the call on the air when that is
 * the announced one or one no message has named yet (nxdn_on_air_call_is()): a follow that left the carrier ended the call there
 * (nxdn_end_left_call()), so a call still active is on the followed carrier, and the follow found it in place; its
 * release then ends the held call, and the next transmission never inherits its identity. Else it awaits the first
 * call to begin after the follow. */
static void
nxdn_note_follow(dsd_state* state, uint32_t target, uint32_t source, int private_call, int data_call, uint16_t chan,
                 long int freq, uint8_t repeater, const dsd_tg_policy_decision* decision, nxdn_follow_how how) {
    const double now_m = dsd_decode_now_mono_s();
    if (how != NXDN_FOLLOW_HELD) {
        DSD_MEMSET(&state->nxdn_call, 0, sizeof(state->nxdn_call));
    }
    state->nxdn_follow.freq_hz = freq;
    state->nxdn_follow.chan = chan;
    state->nxdn_follow.repeater = repeater;
    const int on_air = nxdn_on_air_call_is(state, target, private_call, data_call);
    nxdn_call_identity_set(state, target, source, private_call, data_call, repeater != 0U, freq, now_m, on_air);
    dsd_tg_policy_call_route route;
    DSD_MEMSET(&route, 0, sizeof route);
    route.target_id = target;
    route.source_id = source;
    route.freq_hz = freq;
    route.channel = chan;
    route.slot = -1;
    if (how == NXDN_FOLLOW_HELD) {
        (void)dsd_tg_policy_describe_followed_call(state, &route, decision, state->nxdn_call.follow_m, now_m);
        return;
    }
    if (how == NXDN_FOLLOW_NEW) {
        (void)dsd_tg_policy_clear_active_call(state, -1);
    }
    (void)dsd_tg_policy_note_active_call(state, &route, decision, now_m);
    if (how == NXDN_FOLLOW_PREEMPT) {
        (void)dsd_tg_policy_commit_preemption(state, now_m);
    }
}

/* The receiver went back to the control channel: no route, no held call. */
static void
nxdn_follow_release(dsd_state* state) {
    DSD_MEMSET(&state->nxdn_follow, 0, sizeof(state->nxdn_follow));
    DSD_MEMSET(&state->nxdn_call, 0, sizeof(state->nxdn_call));
    (void)dsd_tg_policy_clear_active_call(state, -1);
}

static uint8_t
nxdn_message_type_from_bits(const uint8_t* Message) {
    uint8_t message_type = (Message[2] & 1U) << 5U;
    message_type |= (Message[3] & 1U) << 4U;
    message_type |= (Message[4] & 1U) << 3U;
    message_type |= (Message[5] & 1U) << 2U;
    message_type |= (Message[6] & 1U) << 1U;
    message_type |= (Message[7] & 1U);
    return message_type;
}

struct nxdn_vcall_assgn_info {
    uint8_t message_type;
    uint8_t cc_option;
    uint8_t call_type;
    uint8_t voice_call_option;
    uint16_t source_unit_id;
    uint16_t destination_id;
    uint16_t channel;
    uint16_t ofn;
    uint8_t dup_after_hangtime; /* a duplicate assignment heard while tuned, past the hangtime of the call there */
};

static void
nxdn_vcall_assgn_parse(dsd_state* state, const uint8_t* Message, struct nxdn_vcall_assgn_info* info) {
    DSD_MEMSET(info, 0, sizeof(*info));
    info->message_type = nxdn_message_type_from_bits(Message);
    info->cc_option = (uint8_t)convert_bits_into_output(&Message[8], 8);
    info->call_type = (uint8_t)convert_bits_into_output(&Message[16], 3);
    info->voice_call_option = (uint8_t)convert_bits_into_output(&Message[19], 5);
    info->source_unit_id = (uint16_t)convert_bits_into_output(&Message[24], 16);
    info->destination_id = (uint16_t)convert_bits_into_output(&Message[40], 16);
    info->channel = (uint16_t)convert_bits_into_output(&Message[62], 10);

    state->NxdnElementsContent.CCOption = info->cc_option;
    state->NxdnElementsContent.CallType = info->call_type;
    state->NxdnElementsContent.VoiceCallOption = info->voice_call_option;
    state->NxdnElementsContent.SourceUnitID = info->source_unit_id;
    state->NxdnElementsContent.DestinationID = info->destination_id;

    if (state->nxdn_rcn == 1) {
        info->ofn = (uint16_t)convert_bits_into_output(&Message[64], 16);
    }
}

static void
nxdn_vcall_assgn_print(const dsd_state* state, const struct nxdn_vcall_assgn_info* info) {
    uint8_t duplex_mode[32] = {0};
    uint8_t transmission_mode[32] = {0};
    const int voice_grant = (info->message_type == 0x04U || info->message_type == 0x05U);

    DSD_FPRINTF(stderr, "%s", voice_grant ? KGRN : KCYN);
    DSD_FPRINTF(stderr, "\n ");
    if (info->cc_option & 0x80U) {
        DSD_FPRINTF(stderr, "Emergency ");
    }
    if (info->cc_option & 0x40U) {
        DSD_FPRINTF(stderr, "Visitor ");
    }
    if (info->cc_option & 0x20U) {
        DSD_FPRINTF(stderr, "Priority Paging ");
    }

    DSD_FPRINTF(stderr, "%s - ", NXDN_Call_Type_To_Str(info->call_type));
    if (voice_grant) {
        NXDN_Voice_Call_Option_To_Str(info->voice_call_option, duplex_mode, transmission_mode);
        DSD_FPRINTF(stderr, "%s %s (%02X) - ", duplex_mode, transmission_mode, info->voice_call_option);
    } else {
        DSD_FPRINTF(stderr, "   Data Call Assignment (%02X) - ", info->voice_call_option);
    }

    DSD_FPRINTF(stderr, "Src=%u - Dst/TG=%u ", info->source_unit_id & 0xFFFF, info->destination_id & 0xFFFF);
    if (state->nxdn_rcn == 0) {
        DSD_FPRINTF(stderr, "- Channel [%03X][%04d] ", info->channel & 0x3FFU, info->channel & 0x3FFU);
    }
    if (state->nxdn_rcn == 1) {
        DSD_FPRINTF(stderr, "- DFA Channel [%04X][%05d] ", info->ofn, info->ofn);
    }
}

/* A duplicate assignment heard past the hangtime of the call the receiver is on reads as an assignment; whether it is
 * followed is admission's to say (nxdn_vcall_assgn_admit()): the receiver stays tuned until a tune leaves. */
static void
nxdn_vcall_assgn_adjust_duplicate(const dsd_opts* opts, const dsd_state* state, time_t now,
                                  struct nxdn_vcall_assgn_info* info) {
    if (info->message_type == 0x05U && opts->trunk_is_tuned == 1 && opts->trunk_enable == 1
        && (now - state->last_vc_sync_time) > opts->trunk_hangtime) {
        info->message_type = 0x04U;
        info->dup_after_hangtime = 1U;
    }
    if (info->message_type == 0x05U && opts->trunk_is_tuned == 1 && opts->trunk_enable == 1 && state->tg_hold != 0
        && state->tg_hold == info->destination_id) {
        info->message_type = 0x04U;
    }
}

static void
nxdn_vcall_assgn_track_active_channel(const dsd_opts* opts, dsd_state* state,
                                      const struct nxdn_vcall_assgn_info* info) {
    const int dup = (info->message_type == 0x05U) ? 1 : 0;
    const uint16_t grant_chan = (state->nxdn_rcn == 1) ? info->ofn : (uint16_t)(info->channel & 0x3FFU);
    const long int grant_freq = nxdn_channel_to_frequency_quiet(state, grant_chan);
    state->nxdn_grant_chan = grant_chan;
    state->nxdn_grant_freq = grant_freq;

    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    if (grant_freq != 0) {
        DSD_SNPRINTF(notice, sizeof notice, "Active Ch: %d (%.6lf MHz) TG: %d SRC: %d; ", grant_chan,
                     (double)grant_freq / 1000000.0, info->destination_id, info->source_unit_id);
    } else if (opts && opts->chan_in_file[0] != '\0') {
        nxdn_trunk_diag_log_missing_channel_once(opts, state, grant_chan, "grant");
        DSD_SNPRINTF(notice, sizeof notice, "Active Ch: %d (no chan_csv freq) TG: %d SRC: %d; ", grant_chan,
                     info->destination_id, info->source_unit_id);
    } else {
        nxdn_trunk_diag_log_missing_channel_once(opts, state, grant_chan, "grant");
        DSD_SNPRINTF(notice, sizeof notice, "Active Ch: %d TG: %d SRC: %d; ", grant_chan, info->destination_id,
                     info->source_unit_id);
    }
    const int data_call = info->message_type == 0x0DU || info->message_type == 0x0EU;
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = data_call               ? DSD_CALL_KIND_DATA
                : info->call_type == 4U ? DSD_CALL_KIND_PRIVATE_VOICE
                                        : DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = info->destination_id,
        .policy_target_id = info->destination_id,
        .ota_source_id = info->source_unit_id,
        .channel = grant_chan,
        .frequency_hz = grant_freq,
        .service_options = info->cc_option,
        .emergency = (uint8_t)((info->cc_option & 0x80U) != 0U),
        .has_service_metadata = 1U,
    };
    (void)dsd_recent_activity_publish(state, (uint8_t)dup, &observation, notice, 0U);
}

/* Assignments, and a duplicate heard while tuned (a candidate to take the receiver from the call it is on), with the
 * call-type gates. A duplicate heard off a voice channel is not followed. */
static int
nxdn_vcall_assgn_should_tune(const dsd_opts* opts, const struct nxdn_vcall_assgn_info* info) {
    if (info->message_type == 0x0DU || info->message_type == 0x0EU) {
        return (opts->trunk_tune_data_calls == 1) ? 1 : 0;
    }
    const int in_call_duplicate = info->message_type == 0x05U && opts->trunk_is_tuned == 1;
    if (info->message_type != 0x04U && !in_call_duplicate) {
        return 0;
    }
    if (info->call_type == 4U) {
        return opts->trunk_tune_private_calls ? 1 : 0;
    }
    return opts->trunk_tune_group_calls ? 1 : 0;
}

static long int
nxdn_vcall_assgn_frequency(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_assgn_info* info) {
    if (state->nxdn_rcn == 0) {
        return nxdn_channel_to_frequency(opts, state, info->channel);
    }
    if (state->nxdn_rcn == 1) {
        return nxdn_channel_to_frequency(opts, state, info->ofn);
    }
    return 0;
}

static int
nxdn_vcall_assgn_setup_tuned_call(dsd_opts* opts, dsd_state* state, long int freq) {
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq, 0, NULL);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        return 0;
    }
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    state->lastsynctype = DSD_SYNC_NONE;
    dsd_nxdn96_sync_clear(&state->nxdn96_sync);
    return 1;
}

static void
nxdn_vcall_assgn_load_scrambler_key(const dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_assgn_info* info) {
    if (keyring_load_scalar_key(state, 0, (int)info->destination_id)) {
        DSD_FPRINTF(stderr, " %s", KYEL);
        char key_text[24];
        DSD_FPRINTF(stderr, " Key Loaded: %s",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, state->R, 0U));
        state->payload_miN = state->R;
    }
    if (state->M == 1) {
        nxdn_cipher_force(state, 0x1);
    }
}

/* How an assignment the receiver may act on is taken (nxdn_vcall_assgn_admit()). */
typedef enum {
    NXDN_ADMIT_REFUSE = 0, /* not followed */
    NXDN_ADMIT_ON_IT,      /* the receiver is on that channel already */
    NXDN_ADMIT_FOLLOW,     /* followed */
    NXDN_ADMIT_PREEMPT,    /* followed, preempting the held call */
} nxdn_admit;

/* Off a voice channel an allowed assignment is followed. On one, an assignment for the channel the receiver is on is
 * that call's when it repeats the held call's (nxdn_assignment_repeats_held_call()); one of the held talkgroup takes
 * over a call that is not of it; a duplicate heard past the call's hangtime is followed as today; while the call is
 * live (within the hangtime) an outranking preempting candidate takes over (nxdn_preempts()); anything else is
 * refused, and the receiver stays tuned for DISC or the hangtime return to take it back. */
/* On the carrier already, a repeat of the held call's assignment is that call's. One naming another call there, or
 * one heard when no held call is known there (its call ended past the hangtime), goes through admission: Hold or a new
 * call after the hangtime is followed in place and becomes the held call, its key loaded. */
static int
nxdn_assignment_repeats_held_call(const dsd_state* state, const dsd_nxdn_call_identity* cur,
                                  const struct nxdn_vcall_assgn_info* info, long int freq) {
    if (!cur || state->trunk_vc_freq[0] != freq) {
        return 0;
    }
    return cur->target == info->destination_id && cur->private_call == (info->call_type == 4U);
}

static nxdn_admit
nxdn_vcall_assgn_admit(const dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_assgn_info* info,
                       const dsd_tg_policy_decision* decision, int policy_allowed, int hold_matches, long int freq,
                       time_t now) {
    if (!opts || !state || opts->trunk_enable != 1 || !policy_allowed || state->p25_cc_freq == 0 || freq == 0) {
        return NXDN_ADMIT_REFUSE;
    }
    if (opts->trunk_is_tuned == 0) {
        return NXDN_ADMIT_FOLLOW;
    }
    const long int rx = nxdn_receiver_freq(opts, state);
    const dsd_nxdn_call_identity* const cur = nxdn_current_identity(opts, state, rx);
    if (nxdn_assignment_repeats_held_call(state, cur, info, freq)) {
        return NXDN_ADMIT_ON_IT;
    }
    if (nxdn_hold_takes_over(state, cur, hold_matches)) {
        return NXDN_ADMIT_FOLLOW;
    }
    if (info->dup_after_hangtime) {
        return NXDN_ADMIT_FOLLOW;
    }
    if ((now - state->last_vc_sync_time) <= opts->trunk_hangtime) {
        dsd_tg_policy_call_route candidate;
        DSD_MEMSET(&candidate, 0, sizeof candidate);
        candidate.target_id = info->destination_id;
        candidate.source_id = info->source_unit_id;
        candidate.freq_hz = freq;
        candidate.channel = (state->nxdn_rcn == 1) ? info->ofn : (int)(info->channel & 0x3FFU);
        candidate.slot = -1;
        candidate.requires_tuner_retune = 1;
        if (nxdn_preempts(opts, state, cur, rx, &candidate, decision)) {
            return NXDN_ADMIT_PREEMPT;
        }
    }
    return NXDN_ADMIT_REFUSE;
}

static void
nxdn_vcall_assgn_apply_tune(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_assgn_info* info, long int freq,
                            time_t now) {
    if (info->destination_id != 0U) {
        nxdn_print_group_label(state, info->destination_id);
    }
    nxdn_print_source_label(state, info->source_unit_id);
    const int is_private_call = (info->call_type == 4U) ? 1 : 0;
    const int data_call = (info->message_type == 0x0DU || info->message_type == 0x0EU) ? 1 : 0;
    const int hold_matches = (state->tg_hold != 0 && state->tg_hold == info->destination_id) ? 1 : 0;
    dsd_tg_policy_decision policy_decision;
    const int policy_allowed = nxdn_policy_tune_allowed(opts, state, info->destination_id, info->source_unit_id,
                                                        is_private_call, data_call, 1, &policy_decision);
    const nxdn_admit admit =
        nxdn_vcall_assgn_admit(opts, state, info, &policy_decision, policy_allowed, hold_matches, freq, now);
    if (admit == NXDN_ADMIT_FOLLOW || admit == NXDN_ADMIT_PREEMPT) {
        const int leaves = nxdn_tune_leaves_carrier(opts, state, freq);
        if (!nxdn_vcall_assgn_setup_tuned_call(opts, state, freq)) {
            return;
        }
        if (leaves) {
            nxdn_end_left_call(opts, state);
        }
        nxdn_vcall_assgn_load_scrambler_key(opts, state, info);
        const uint16_t chan = (state->nxdn_rcn == 1) ? info->ofn : (uint16_t)(info->channel & 0x3FFU);
        nxdn_note_follow(state, info->destination_id, info->source_unit_id, is_private_call, data_call, chan, freq, 0U,
                         &policy_decision, admit == NXDN_ADMIT_PREEMPT ? NXDN_FOLLOW_PREEMPT : NXDN_FOLLOW_NEW);
    } else if (admit == NXDN_ADMIT_REFUSE && opts->trunk_enable == 1) {
        nxdn_policy_log_block(opts, is_private_call, info->destination_id, info->source_unit_id, &policy_decision);
    }
}

void
NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
    const time_t now = dsd_decode_time();
    struct nxdn_vcall_assgn_info info;
    nxdn_vcall_assgn_parse(state, Message, &info);
    nxdn_vcall_assgn_print(state, &info);
    nxdn_vcall_assgn_adjust_duplicate(opts, state, now, &info);
    nxdn_vcall_assgn_track_active_channel(opts, state, &info);
    if (nxdn_vcall_assgn_should_tune(opts, &info)) {
        const long int freq = nxdn_vcall_assgn_frequency(opts, state, &info);
        nxdn_anchor_control_channel_from_current_tuner(opts, state, 1);
        nxdn_vcall_assgn_apply_tune(opts, state, &info, freq, now);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
nxdn_print_dfa_bandwidth(uint8_t bw) {
    if (bw == 0U) {
        DSD_FPRINTF(stderr, "BW: 6.25 kHz - 4800 bps");
    } else if (bw == 1U) {
        DSD_FPRINTF(stderr, "BW: 12.5 kHz - 9600 bps");
    } else {
        DSD_FPRINTF(stderr, "BW: %d Reserved Value", bw);
    }
}

static void
nxdn_cch_info_channel_version(dsd_state* state, uint32_t location_id, uint8_t channel1sts, uint16_t channel1,
                              uint16_t channel2) {
    DSD_FPRINTF(stderr, "  Location ID [%06X] CC1 [%03X][%04d] CC2 [%03X][%04d] Status: ", location_id, channel1,
                channel1, channel2, channel2);
    if (channel1sts & 0x20U) {
        DSD_FPRINTF(stderr, "Current ");
    }
    if (channel1sts & 0x10U) {
        DSD_FPRINTF(stderr, "New ");
    }
    if (channel1sts & 0x08U) {
        DSD_FPRINTF(stderr, "Candidate Added ");
    }
    if (channel1sts & 0x04U) {
        DSD_FPRINTF(stderr, "Candidate Deleted ");
    }
    UNUSED(state);
}

/*
 * Whether the operator has pinned the control channel, in which case a site broadcast must not
 * move it. A learned or imported LCN list is operator intent; the trunk-scan coordinator's
 * slot-0 seed is only the target CSV's park frequency, which may be a few kHz off the real
 * outbound CC, so the broadcast is allowed to correct it. p25_has_user_lcn_list()
 * (src/protocol/p25/p25_trunk_sm.c) draws the same line for P25.
 */
static int
nxdn_cc_is_operator_pinned(const dsd_opts* opts, const dsd_state* state) {
    // Tested by count, not by trunk_lcn_freq[0]: an imported map is positional, so an
    // unparseable row 1 deliberately stores 0 to keep LCN numbering, and reading slot 0
    // alone would mistake that whole list for "nothing imported".
    if (opts->trunk_scan_enabled != 1) {
        return state->lcn_freq_count > 0;
    }

    // Only the parked target's own control channel may move, and only an nxdn-trunk target has one
    // this broadcast can describe. Checked before the "nothing learned yet" case below: under -fa
    // an NXDN element decoded while parked on another target type is stray traffic (or a false
    // sync), and adopting from it would plant a control channel on a target that has none, or move
    // one that belongs to another protocol.
    //   - conventional targets run with trunk_enable == 0 and have no control channel at all;
    //   - a p25-trunk target owns the coordinator's P25 SM context and carries its control channel
    //     in the same p25_cc_freq field this adoption would overwrite;
    //   - a dmr-trunk target owns the DMR SM context (and keeps p25_cc_freq at 0).
    if (opts->trunk_enable != 1 || dsd_trunk_scan_hook_p25_ctx() != NULL || dsd_trunk_scan_hook_dmr_ctx() != NULL) {
        return 1;
    }
    if (state->lcn_freq_count == 0) {
        return 0;
    }
    // More entries than the single seeded slot mean a per-target chan_csv supplied real LCNs.
    return (state->lcn_freq_count > 1 || state->p25_cc_freq == 0) ? 1 : 0;
}

/*
 * Follow the control channel a CCH_INFO element announces, unless the operator pinned it.
 */
static void
nxdn_cch_info_adopt_control_channel(const dsd_opts* opts, dsd_state* state, long int freq1) {
    if (freq1 == 0 || nxdn_cc_is_operator_pinned(opts, state)) {
        return;
    }
    const long int previous_cc = state->trunk_cc_freq;
    state->trunk_lcn_freq[0] = freq1;
    state->p25_cc_freq = freq1;
    state->trunk_cc_freq = freq1;
    // Raise, never assign: reaching here means no operator list is present, but the count can
    // still be above 1 from learned entries that must not be discarded.
    if (state->lcn_freq_count < 1) {
        state->lcn_freq_count = 1;
    }
    // Announce only a real move: under trunk scan this runs on every CCH_INFO, and once the park
    // frequency has been corrected the adoption is idempotent.
    if (previous_cc != 0 && previous_cc != freq1) {
        LOG_INFO("NOTICE: NXDN trunking: site control channel is %.6lf MHz; following it\n", (double)freq1 / 1000000.0);
    }
}

static int
nxdn_cch_info_dfa_version(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits,
                          uint32_t location_id, uint8_t channel1sts) {
    enum {
        NXDN_CCH_INFO_DFA_MIN_BITS = 72U,
        NXDN_CCH_INFO_DFA_SECONDARY_MIN_BITS = 112U,
    };

    if (message_bits < NXDN_CCH_INFO_DFA_MIN_BITS) {
        DSD_FPRINTF(stderr, " CCH_INFO DFA Too Short (%zu bits); ", message_bits);
        return 0;
    }

    const uint8_t bw1 = (uint8_t)convert_bits_into_output(&Message[38], 2);
    const uint16_t OFN1 = (uint16_t)convert_bits_into_output(&Message[40], 16);
    const uint16_t IFN1 = (uint16_t)convert_bits_into_output(&Message[56], 16);

    DSD_FPRINTF(stderr, "  Location ID [%06X] OFN1 [%04X][%05d] IFN1 [%04X][%05d] ", location_id, OFN1, OFN1, IFN1,
                IFN1);

    if (message_bits >= NXDN_CCH_INFO_DFA_SECONDARY_MIN_BITS) {
        const uint16_t OFN2 = (uint16_t)convert_bits_into_output(&Message[80], 16);
        const uint16_t IFN2 = (uint16_t)convert_bits_into_output(&Message[96], 16);
        if (OFN2 && IFN2) {
            DSD_FPRINTF(stderr, "OFN2 [%04X][%05d] IFN2 [%04X][%05d]", OFN2, OFN2, IFN2, IFN2);
        }
        if (OFN2 && IFN2 && OFN2 != OFN1) {
            nxdn_channel_to_frequency(opts, state, OFN2);
            nxdn_channel_to_frequency(opts, state, IFN2);
        }
    }

    DSD_FPRINTF(stderr, "Status: ");
    if (channel1sts & 0x10U) {
        DSD_FPRINTF(stderr, "New ");
    }
    if (channel1sts & 0x02U) {
        DSD_FPRINTF(stderr, "Current 1 ");
    }
    if (channel1sts & 0x01U) {
        DSD_FPRINTF(stderr, "Current 2 ");
    }

    nxdn_print_dfa_bandwidth(bw1);

    const long int freq1 = nxdn_channel_to_frequency(opts, state, OFN1);
    nxdn_channel_to_frequency(opts, state, IFN1);
    nxdn_cch_info_adopt_control_channel(opts, state, freq1);

    return 1;
}

static void
nxdn_adj_site_channel_entry(dsd_opts* opts, dsd_state* state, uint32_t site, uint8_t opt, uint16_t chan) {
    if ((opt & 0x0FU) == 0U) {
        return;
    }
    DSD_FPRINTF(stderr, "\n Adjacent Site %d ", opt & 0x0F);
    DSD_FPRINTF(stderr, "Channel [%03X] [%04d]", chan, chan);
    nxdn_location_id_handler(state, site, 1);
    nxdn_channel_to_frequency(opts, state, chan);
}

static void
nxdn_adj_site_dfa_entry(dsd_opts* opts, dsd_state* state, uint32_t site, uint8_t opt, uint8_t bw, uint16_t chan) {
    if ((opt & 0x0FU) == 0U) {
        return;
    }
    DSD_FPRINTF(stderr, "\n Adjacent Site %d ", opt & 0x0F);
    DSD_FPRINTF(stderr, "Channel [%04X] [%05d] ", chan, chan);
    nxdn_print_dfa_bandwidth(bw);
    nxdn_location_id_handler(state, site, 1);
    nxdn_channel_to_frequency(opts, state, chan);
}

static void
NXDN_decode_cch_info(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits) {
    enum {
        NXDN_CCH_INFO_MIN_BITS = 64U,
    };

    if (message_bits < NXDN_CCH_INFO_MIN_BITS) {
        DSD_FPRINTF(stderr, " CCH_INFO Too Short (%zu bits); ", message_bits);
        return;
    }

    //6.4.3.3. Control Channel Information (CCH_INFO) for more information
    uint32_t location_id = 0;
    uint8_t channel1sts = 0;
    uint16_t channel1 = 0;
    uint16_t channel2 = 0;

    location_id = (uint32_t)convert_bits_into_output(&Message[8], 24);
    channel1sts = (uint8_t)convert_bits_into_output(&Message[32], 6);
    channel1 = (uint16_t)convert_bits_into_output(&Message[38], 10);
    channel2 = (uint16_t)convert_bits_into_output(&Message[54], 10);

    DSD_FPRINTF(stderr, "%s", KYEL);
    nxdn_location_id_handler(state, location_id, 0);

    DSD_FPRINTF(stderr, "\n Control Channel Information \n");

    if (state->nxdn_rcn == 0) {
        nxdn_cch_info_channel_version(state, location_id, channel1sts, channel1, channel2);
    }

    if (state->nxdn_rcn == 1
        && !nxdn_cch_info_dfa_version(opts, state, Message, message_bits, location_id, channel1sts)) {
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
NXDN_decode_srv_info(const dsd_opts* opts, dsd_state* state, const uint8_t* Message, int control_channel) {
    uint32_t location_id = 0;
    uint16_t svc_info = 0; //service information
    uint32_t rst_info = 0; //restriction information

    location_id = (uint32_t)convert_bits_into_output(&Message[8], 24);
    svc_info = (uint16_t)convert_bits_into_output(&Message[32], 16);
    rst_info = (uint32_t)convert_bits_into_output(&Message[48], 24);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Service Information - ");
    DSD_FPRINTF(stderr, "Location ID [%06X] SVC [%04X] RST [%06X] ", location_id, svc_info, rst_info);
    nxdn_location_id_handler(state, location_id, 0);

    //run the srv info
    nxdn_srv_info_handler(state, svc_info);

    //run the rst info, if not zero
    if (rst_info) {
        nxdn_rst_info_handler(state, rst_info);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);

    if (control_channel) {
        nxdn_anchor_control_channel_from_current_tuner(opts, state, 0);
    }

    //clear stale active channel listing -- consider best placement for this (NXDN Type C Trunking -- inside SRV_INFO)
    if (dsd_recent_activity_expire(state, 0U, DSD_RECENT_ACTIVITY_TTL_MS) > 0) {
        state->nxdn_grant_chan = 0;
        state->nxdn_grant_freq = 0;
    }
}

/* A Type-D Site Code (NXDN TS 1-E §6.6.5, Table 6.6-3): 1-250 name a site, 0 and 251-255 are reserved. */
static int
nxdn_idas_site_code_valid(uint8_t site_code) {
    return site_code != 0U && site_code <= 250U;
}

/*
 * Type-D (IDAS) Service Information (NXDN TS 1-E §6.4.3.1, Fig 6.4-39), a traffic channel's message: after the message
 * type, 5 spare bits and the 19-bit System ID (§6.6.2: Integrator Code 4 bits, System Code 15), the 8-bit Site Code
 * (§6.6.5), then 16 bits of Service Information (§6.6.41) and 24 of Restriction Information (§6.6.42); 80 elements. It
 * carries no Type-C location ID and no RAN. The identity is kept once the transmission is confirmed (a FACCH1 or
 * FACCH3 confirms it with its own CRC).
 */
static void
NXDN_decode_srv_info_type_d(dsd_state* state, const uint8_t* Message) {
    const uint32_t system_id = (uint32_t)convert_bits_into_output(&Message[13], 19);
    const uint8_t site_code = (uint8_t)convert_bits_into_output(&Message[32], 8);
    const uint16_t svc_info = (uint16_t)convert_bits_into_output(&Message[40], 16);
    const uint32_t rst_info = (uint32_t)convert_bits_into_output(&Message[56], 24);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Service Information - System ID [%05X] (Integrator %u; System Code %u) Site Code %u%s ",
                system_id, (unsigned)(system_id >> 15), (unsigned)(system_id & 0x7FFFU), (unsigned)site_code,
                nxdn_idas_site_code_valid(site_code) ? "" : " (Reserved)");
    DSD_FPRINTF(stderr, "SVC [%04X] RST [%06X] ", svc_info, rst_info);
    /* SIF1-SIF5 are the Type-C flags; the rest are reserved or spare on Type-D (Table 6.6-40). */
    nxdn_srv_info_handler(state, (uint16_t)(svc_info & 0xF800U));
    DSD_FPRINTF(stderr, "%s", KNRM);

    if (!nxdn_confirm_is_confirmed(state)) {
        return;
    }
    if (system_id != 0U) {
        state->nxdn_idas_system_id = system_id;
    }
    if (nxdn_idas_site_code_valid(site_code)) {
        state->nxdn_idas_site_code = site_code;
        if (system_id != 0U) {
            dsd_rxid_note_nxdn_typed_system(state, system_id, site_code);
        }
    }
}

/*
 * Type-D (IDAS) Adjacent Site Information (NXDN TS 1-E §6.4.3.2, Fig 6.4-40): after the message type, 7 spare bits and
 * the Index (§6.6.46), then two sites of Adjacent Site Option (5 bits, §6.6.45), Adjacent Site System ID (19) and
 * Adjacent Site Code (8); 80 elements. A null System ID fills an unused entry. Printed only: another site's identity.
 */
static void
NXDN_decode_adj_site_type_d(const uint8_t* Message) {
    const uint8_t index = Message[15];
    DSD_FPRINTF(stderr, "%s", KYEL);
    for (size_t i = 0U; i < 2U; i++) {
        const size_t base = 16U + (i * 32U);
        const uint8_t option = (uint8_t)convert_bits_into_output(&Message[base], 5);
        const uint32_t system_id = (uint32_t)convert_bits_into_output(&Message[base + 5U], 19);
        const uint8_t site_code = (uint8_t)convert_bits_into_output(&Message[base + 24U], 8);
        if (system_id == 0U) {
            continue;
        }
        DSD_FPRINTF(stderr, "\n Adjacent Site %u - System ID [%05X] Site Code %u%s",
                    (unsigned)(index * 2U) + (unsigned)i + 1U, system_id, (unsigned)site_code,
                    (option & 0x10U) ? " - Isolated" : "");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
NXDN_decode_site_info(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits) {
    UNUSED(opts);

    enum {
        NXDN_SITE_INFO_MIN_BITS = 144U,
    };

    if (message_bits < NXDN_SITE_INFO_MIN_BITS) {
        DSD_FPRINTF(stderr, " SITE_INFO Too Short (%zu bits); ", message_bits);
        return;
    }

    uint32_t location_id = 0;
    uint16_t cs_info = 0;  //channel structure information
    uint16_t svc_info = 0; //service information
    uint32_t rst_info = 0; //restriction information
    uint32_t ca_info = 0;  //channel access information
    uint8_t version_num = 0;
    uint8_t adj_alloc = 0; //number of adjacent sites

    uint16_t channel1 = 0;
    uint16_t channel2 = 0;
    long int freq1 = 0;
    long int freq2 = 0;
    UNUSED2(freq1, freq2);

    location_id = (uint32_t)convert_bits_into_output(&Message[8], 24);
    cs_info = (uint16_t)convert_bits_into_output(&Message[32], 16);
    svc_info = (uint16_t)convert_bits_into_output(&Message[48], 16);
    rst_info = (uint32_t)convert_bits_into_output(&Message[64], 24);
    ca_info = (uint32_t)convert_bits_into_output(&Message[88], 24);
    version_num = (uint8_t)convert_bits_into_output(&Message[112], 8);
    adj_alloc = (uint8_t)convert_bits_into_output(&Message[120], 4);
    channel1 = (uint16_t)convert_bits_into_output(&Message[124], 10);
    channel2 = (uint16_t)convert_bits_into_output(&Message[134], 10);

    //check the channel access information first
    nxdn_ca_info_handler(state, ca_info);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr,
                "\n Location ID [%06X] CSC [%04X] SVC [%04X] RST [%06X] \n          CA [%06X] V[%X] ADJ [%01X] ",
                location_id, cs_info, svc_info, rst_info, ca_info, version_num, adj_alloc);
    nxdn_location_id_handler(state, location_id, 0);

    //run the srv info
    nxdn_srv_info_handler(state, svc_info);

    //run the rst info, if not zero
    if (rst_info) {
        nxdn_rst_info_handler(state, rst_info);
    }

    //only get frequencies if using channel version of message and not dfa
    if (state->nxdn_rcn == 0) {
        if (channel1 != 0) {
            DSD_FPRINTF(stderr, "\n Control Channel 1 [%03X][%04d] ", channel1, channel1);
            // freq1 not used
        }
        if (channel2 != 0) {
            DSD_FPRINTF(stderr, "\n Control Channel 2 [%03X][%04d] ", channel2, channel2);
            // freq2 not used
        }
    } else {
        ; //DFA version does not carry an OFN/IFN value, so no freqs
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
NXDN_decode_adj_site(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits) {
    enum {
        NXDN_ADJ_SITE_CH_MIN_BITS = 128U,
        NXDN_ADJ_SITE_DFA_MIN_BITS = 104U,
    };

    //the size of this PDU can vary, but the adj_site_location_id and/or channel will be NULL or 0 if not enough space to fill it
    //will want to monitor this PDU for potential overflow related issues with the Message or ElementContent size

    //up to four adj_site_location_ids can be conveyed -- see 6.4.3.4 for more information
    DSD_FPRINTF(stderr, "%s", KYEL);

    //Channel Version
    if (state->nxdn_rcn == 0) {
        if (message_bits < NXDN_ADJ_SITE_CH_MIN_BITS) {
            DSD_FPRINTF(stderr, " ADJ_SITE(CH) Too Short (%zu bits); ", message_bits);
            DSD_FPRINTF(stderr, "%s", KNRM);
            return;
        }
        //1
        const uint32_t adj1_site = (uint32_t)convert_bits_into_output(&Message[8], 24);
        const uint8_t adj1_opt = (uint8_t)convert_bits_into_output(&Message[32], 6);
        const uint16_t adj1_chan = (uint16_t)convert_bits_into_output(&Message[38], 10);
        //2
        const uint32_t adj2_site = (uint32_t)convert_bits_into_output(&Message[48], 24);
        const uint8_t adj2_opt = (uint8_t)convert_bits_into_output(&Message[72], 6);
        const uint16_t adj2_chan = (uint16_t)convert_bits_into_output(&Message[78], 10);
        //3
        const uint32_t adj3_site = (uint32_t)convert_bits_into_output(&Message[88], 24);
        const uint8_t adj3_opt = (uint8_t)convert_bits_into_output(&Message[112], 6);
        const uint16_t adj3_chan = (uint16_t)convert_bits_into_output(&Message[118], 10);

        nxdn_adj_site_channel_entry(opts, state, adj1_site, adj1_opt, adj1_chan);
        nxdn_adj_site_channel_entry(opts, state, adj2_site, adj2_opt, adj2_chan);
        nxdn_adj_site_channel_entry(opts, state, adj3_site, adj3_opt, adj3_chan);
    }

    //DFA Version
    if (state->nxdn_rcn == 1) {
        if (message_bits < NXDN_ADJ_SITE_DFA_MIN_BITS) {
            DSD_FPRINTF(stderr, " ADJ_SITE(DFA) Too Short (%zu bits); ", message_bits);
            DSD_FPRINTF(stderr, "%s", KNRM);
            return;
        }
        //1
        const uint32_t adj1_site = (uint32_t)convert_bits_into_output(&Message[8], 24);
        const uint8_t adj1_opt = (uint8_t)convert_bits_into_output(&Message[32], 6);
        const uint8_t adj1_bw = (uint8_t)convert_bits_into_output(&Message[38], 2);
        const uint16_t adj1_chan = (uint16_t)convert_bits_into_output(&Message[40], 16);
        //2
        const uint32_t adj2_site = (uint32_t)convert_bits_into_output(&Message[56], 24);
        const uint8_t adj2_opt = (uint8_t)convert_bits_into_output(&Message[80], 6);
        const uint8_t adj2_bw = (uint8_t)convert_bits_into_output(&Message[86], 2);
        const uint16_t adj2_chan = (uint16_t)convert_bits_into_output(&Message[88], 16);

        nxdn_adj_site_dfa_entry(opts, state, adj1_site, adj1_opt, adj1_bw, adj1_chan);
        nxdn_adj_site_dfa_entry(opts, state, adj2_site, adj2_opt, adj2_bw, adj2_chan);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

struct nxdn_vcall_info {
    uint8_t message_type;
    uint8_t mfid;
    uint8_t has_mfid;
    uint8_t cc_option;
    uint8_t call_type;
    uint8_t voice_call_option;
    uint8_t cipher_type;
    uint8_t key_id;
    uint8_t idas;
    uint8_t rep1;
    uint16_t source_unit_id;
    uint16_t destination_id;
};

static void
nxdn_vcall_parse_fields(dsd_state* state, const uint8_t* Message, struct nxdn_vcall_info* info, uint8_t message_type,
                        size_t body_offset, int apply_type_d_truncation) {
    DSD_MEMSET(info, 0, sizeof(*info));
    info->message_type = message_type;
    info->cc_option = (uint8_t)convert_bits_into_output(&Message[body_offset], 8);
    info->call_type = (uint8_t)convert_bits_into_output(&Message[body_offset + 8U], 3);
    info->voice_call_option = (uint8_t)convert_bits_into_output(&Message[body_offset + 11U], 5);
    info->source_unit_id = (uint16_t)convert_bits_into_output(&Message[body_offset + 16U], 16);
    info->destination_id = (uint16_t)convert_bits_into_output(&Message[body_offset + 32U], 16);
    info->cipher_type = (uint8_t)convert_bits_into_output(&Message[body_offset + 48U], 2);
    info->key_id = (uint8_t)convert_bits_into_output(&Message[body_offset + 50U], 6);

    state->NxdnElementsContent.CCOption = info->cc_option;
    state->NxdnElementsContent.CallType = info->call_type;
    state->NxdnElementsContent.VoiceCallOption = info->voice_call_option;
    state->NxdnElementsContent.SourceUnitID = info->source_unit_id;
    state->NxdnElementsContent.DestinationID = info->destination_id;
    state->NxdnElementsContent.CipherType = info->cipher_type;
    state->NxdnElementsContent.KeyID = info->key_id;

    info->idas = (apply_type_d_truncation && strcmp(state->nxdn_location_category, "Type-D") == 0) ? 1U : 0U;
    if (info->idas != 0U) {
        info->rep1 = (uint8_t)((info->source_unit_id >> 11) & 0x1FU);
        info->source_unit_id &= 0x7FFU;
        info->destination_id &= 0x7FFU;
    }
}

static void
nxdn_vcall_parse(dsd_state* state, const uint8_t* Message, struct nxdn_vcall_info* info) {
    nxdn_vcall_parse_fields(state, Message, info, nxdn_message_type_from_bits(Message), 8U, 1);
}

static uint8_t
nxdn_arib_vcall_normalized_message_type(uint8_t message_type) {
    if (message_type == 0x21U) {
        return 0x01U;
    }
    if (message_type == 0x28U) {
        return 0x08U;
    }
    return message_type;
}

static void
nxdn_vcall_parse_arib(dsd_state* state, const uint8_t* Message, struct nxdn_vcall_info* info) {
    const uint8_t message_type = nxdn_message_type_from_bits(Message);
    nxdn_vcall_parse_fields(state, Message, info, nxdn_arib_vcall_normalized_message_type(message_type), 16U, 0);
    info->mfid = (uint8_t)convert_bits_into_output(&Message[8], 8);
    info->has_mfid = 1U;
}

static void
nxdn_vcall_print_color(uint8_t message_type) {
    if (message_type == 0x01U) {
        DSD_FPRINTF(stderr, "%s", KGRN);
    } else if (message_type == 0x07U || message_type == 0x08U) {
        DSD_FPRINTF(stderr, "%s", KYEL);
    } else if (message_type == 0x11U) {
        DSD_FPRINTF(stderr, "%s", KRED);
    }
}

static void
nxdn_vcall_update_crypto_flags(dsd_state* state, const struct nxdn_vcall_info* info) {
    if (info->cipher_type == 2U || info->cipher_type == 3U) {
        state->NxdnElementsContent.PartOfCurrentEncryptedFrame = 1;
        state->NxdnElementsContent.PartOfNextEncryptedFrame = 2;
    } else {
        state->NxdnElementsContent.PartOfCurrentEncryptedFrame = 1;
        state->NxdnElementsContent.PartOfNextEncryptedFrame = 1;
    }
}

static void
nxdn_vcall_print_voice_option(const struct nxdn_vcall_info* info) {
    uint8_t duplex_mode[32] = {0};
    uint8_t transmission_mode[32] = {0};
    if (info->message_type == 0x01U) {
        NXDN_Voice_Call_Option_To_Str(info->voice_call_option, duplex_mode, transmission_mode);
        DSD_FPRINTF(stderr, "%s %s (%02X) - ", duplex_mode, transmission_mode, info->voice_call_option);
    } else if (info->message_type == 0x07U) {
        DSD_FPRINTF(stderr, "Transmission Release Ex - ");
    } else if (info->message_type == 0x08U) {
        DSD_FPRINTF(stderr, "  Transmission Release  - ");
    } else if (info->message_type == 0x11U) {
        DSD_FPRINTF(stderr, "       Disconnect       - ");
    }
}

static void
nxdn_vcall_print_summary(dsd_state* state, const struct nxdn_vcall_info* info) {
    nxdn_vcall_print_color(info->message_type);
    DSD_FPRINTF(stderr, "\n ");
    if (info->has_mfid != 0U) {
        DSD_FPRINTF(stderr, "MFID: %02X; ", info->mfid);
    }
    if (info->cc_option & 0x80U) {
        DSD_FPRINTF(stderr, "Emergency ");
    }
    if (info->cc_option & 0x40U) {
        DSD_FPRINTF(stderr, "Visitor ");
    }
    if (info->cc_option & 0x20U) {
        DSD_FPRINTF(stderr, "Priority Paging ");
    }

    nxdn_vcall_update_crypto_flags(state, info);
    DSD_FPRINTF(stderr, "%s - ", NXDN_Call_Type_To_Str(info->call_type));
    nxdn_vcall_print_voice_option(info);
    DSD_FPRINTF(stderr, "Src=%u - Dst/TG=%u ", info->source_unit_id & 0xFFFF, info->destination_id & 0xFFFF);
    if (info->idas) {
        DSD_FPRINTF(stderr, "- Prefix Ch: %d ", info->rep1);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

// Load the AES-256 key at @p key_index for an NXDN cipher 3 call. It is usable only when all four segments are present
// (by their import flags, zero values allowed): a narrower key, or another ID's scalar on one of its cells, never is.
static void
nxdn_load_aes_key_index(dsd_state* state, uint32_t key_index) {
    state->A1[0] = state->rkey_array[key_index + 0x000];
    state->A2[0] = state->rkey_array[key_index + 0x101];
    state->A3[0] = state->rkey_array[key_index + 0x201];
    state->A4[0] = state->rkey_array[key_index + 0x301];
    state->aes_key_segments[0] = (uint8_t)keyring_kid_aes_segments(state, (int)key_index);
    state->aes_key_loaded[0] = state->aes_key_segments[0] >= 4U ? 1 : 0;

    for (int i = 0; i < 8; i++) {
        state->aes_key[i + 0] = (state->A1[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 8] = (state->A2[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 16] = (state->A3[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 24] = (state->A4[0] >> (56 - (i * 8))) & 0xFF;
    }
    state->R = state->A1[0];
    state->scalar_key_present[0] = 0;
}

/** The keyring key an NXDN call decrypts with under the keyloader, and where it came from. */
typedef struct {
    int key_id; /* -1: the call's ID and destination hold none of this cipher's key */
    dsd_call_key_source source;
} nxdn_key_choice;

// The one choice the VCALL and VCALL_IV loaders and the key source the call reports all make, so loading and
// reporting cannot disagree. Cipher-specific presence: a scalar (an imported zero included) for the scrambler and DES,
// all four AES-256 segments for AES, so another ID's scalar parked on one of this ID's AES cells (ID + 0x101 ...)
// never reads as this ID's key. The scrambler on NXDN48 falls back to the destination's key; AES to key 0.
static nxdn_key_choice
nxdn_select_key(const dsd_opts* opts, const dsd_state* state, uint8_t cipher_type, int key_id, uint64_t destination) {
    nxdn_key_choice choice = {-1, DSD_CALL_KEY_DEFAULT};
    if (cipher_type == 3U) {
        choice.key_id = 0;
        if (keyring_kid_aes_segments(state, key_id) >= 4) {
            choice.key_id = key_id;
            choice.source = DSD_CALL_KEY_SIGNALED;
        }
    } else if (cipher_type == 2U) {
        if (keyring_kid_scalar_present(state, key_id)) {
            choice.key_id = key_id;
            choice.source = DSD_CALL_KEY_SIGNALED;
        }
    } else if (cipher_type == 1U && opts != NULL
               && dsd_frame_sync_active_nxdn_variant(opts, state) == DSD_NXDN_VARIANT_48) {
        if (keyring_kid_scalar_present(state, key_id)) {
            choice.key_id = key_id;
            choice.source = DSD_CALL_KEY_SIGNALED;
        } else if (destination < sizeof(state->rkey_array) / sizeof(state->rkey_array[0])
                   && keyring_kid_scalar_present(state, (int)destination)) {
            choice.key_id = (int)destination;
            choice.source = DSD_CALL_KEY_DESTINATION;
        }
    }
    return choice;
}

// Load what nxdn_select_key() chose: the scalar for the scrambler and DES, the AES key for AES.
static void
nxdn_load_selected_key(const dsd_opts* opts, dsd_state* state, uint8_t cipher_type, int key_id, uint64_t destination) {
    if (state->keyloader != 1) {
        return;
    }
    const nxdn_key_choice choice = nxdn_select_key(opts, state, cipher_type, key_id, destination);
    if (cipher_type == 3U) {
        nxdn_load_aes_key_index(state, (uint32_t)choice.key_id);
    } else if (choice.key_id >= 0) {
        (void)keyring_load_scalar_key(state, 0, choice.key_id);
    }
}

static void
nxdn_vcall_load_key(const dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_info* info) {
    nxdn_load_selected_key(opts, state, info->cipher_type, (int)info->key_id, info->destination_id);
}

static void
nxdn_vcall_print_cipher(const dsd_opts* opts, const dsd_state* state, const struct nxdn_vcall_info* info) {
    if (info->cipher_type != 0U && info->message_type == 0x01U) {
        DSD_FPRINTF(stderr, "\n  %s", KYEL);
        DSD_FPRINTF(stderr, "%s - ", NXDN_Cipher_Type_To_Str(info->cipher_type));
        DSD_FPRINTF(stderr, "Key ID %u - ", info->key_id & 0xFFU);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
    if (info->cipher_type == 0x01U && dsd_key_scalar_present(state, 0)) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[16];
        DSD_FPRINTF(stderr, "Value: %s",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, state->R, 5U));
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
    if (info->cipher_type == 0x02U && dsd_key_scalar_present(state, 0)) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[17];
        DSD_FPRINTF(stderr, "Value: %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->R, 16U, 0));
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
    if (info->cipher_type == 0x03U && dsd_key_scalar_present(state, 0)) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[17];
        DSD_FPRINTF(stderr, "KS: %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->R, 16U, 0));
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static dsd_call_kind
nxdn_vcall_kind(uint8_t call_type) {
    if (call_type == 4U) {
        return DSD_CALL_KIND_PRIVATE_VOICE;
    }
    if (call_type == 0U || call_type == 1U) {
        return DSD_CALL_KIND_GROUP_VOICE;
    }
    return DSD_CALL_KIND_VOICE;
}

static int
nxdn_vcall_has_key(const dsd_state* state, uint8_t cipher_type) {
    if (state == NULL) {
        return 0;
    }
    if (cipher_type == 3U) {
        return dsd_key_aes_slot_complete(state, 0, 4U);
    }
    if (cipher_type == 1U || cipher_type == 2U) {
        return dsd_key_scalar_present(state, 0);
    }
    return 0;
}

// @p received: the DSD_CALL_CRYPTO_RX_* fields this publication carries from a checked VCALL (0 for a republish).
static void
nxdn_vcall_publish_crypto(dsd_opts* opts, dsd_state* state, uint8_t cipher_type, uint8_t key_id, uint8_t received,
                          uint8_t contradicted, uint8_t enc_observed) {
    const int has_key = nxdn_vcall_has_key(state, cipher_type);
    const dsd_call_crypto_update update = {
        .classification = cipher_type == 0U ? DSD_CALL_CRYPTO_CLEAR
                          : has_key         ? DSD_CALL_CRYPTO_DECRYPTABLE
                                            : DSD_CALL_CRYPTO_ENCRYPTED_PENDING,
        .algid = cipher_type,
        .kid = key_id,
        .mi = state->payload_miN,
        .audio_permitted = (uint8_t)(cipher_type == 0U || has_key),
        .received = received,
        .contradicted = contradicted,
        .enc_observed = enc_observed,
    };
    if (dsd_call_state_update_crypto(state, 0U, &update) > 0) {
        dsd_call_snapshot call;
        if (dsd_call_state_get(state, 0, &call) > 0) {
            dsd_call_key_source source = DSD_CALL_KEY_DIRECT;
            int selected = -1;
            if (state->keyloader == 1) {
                // The loaders' own choice (nxdn_select_key()), so the report names the key that decrypts.
                const nxdn_key_choice choice = nxdn_select_key(opts, state, cipher_type, key_id, call.ota_target_id);
                source = choice.source;
                selected = choice.key_id;
            }
            (void)dsd_call_state_note_key_selection(state, 0, call.epoch, source, key_id, selected,
                                                    cipher_type ? has_key : -1, 0);
        }
        dsd_event_sync_slot(opts, state, 0U);
    }
}

static void
nxdn_vcall_publish(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_info* info) {
    int protocol = DSD_SYNC_IS_NXDN(state->synctype) ? state->synctype : state->lastsynctype;
    if (!DSD_SYNC_IS_NXDN(protocol)) {
        protocol = DSD_SYNC_NXDN_POS;
    }
    /* A VCALL carries no channel or frequency of its own (issue #575). The call may name only the voice channel the
     * receiver followed a grant to: the followed route (nxdn_follow_valid()) while the receiver is still there, else
     * trunk_vc_freq[] while tuned, with no channel. Without trunking no grant is followed, and trunk_vc_freq[] holds
     * whatever a DMR grant decoded with trunking off wrote for display; a grant decoded while tuned (a duplicate
     * assignment for another call) moves nxdn_grant_chan without moving the receiver, so it never names the channel. */
    const int route_held = nxdn_follow_valid(opts, state);
    const long int followed_freq =
        route_held ? state->nxdn_follow.freq_hz : (dsd_opts_trunk_vc_followed(opts) ? state->trunk_vc_freq[0] : 0);
    const uint16_t followed_chan = route_held ? state->nxdn_follow.chan : 0U;
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = nxdn_vcall_kind(info->call_type),
        .ota_target_id = info->destination_id,
        .policy_target_id = info->destination_id,
        .ota_source_id = (info->voice_call_option & 0x0FU) < 4U ? info->source_unit_id : 0U,
        .channel = followed_chan,
        .frequency_hz = followed_freq,
        .service_options = info->cc_option,
        .emergency = (uint8_t)((info->cc_option & 0x80U) != 0U),
        .has_service_metadata = 1U,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
    // Publish the applied classification, not the raw observation: a quarantined contradiction
    // must not flap the published crypto any more than it may flap the audio gate. The cipher and
    // key id are received only once the air itself has corroborated the applied encrypted
    // classification: this VCALL passed its channel's CRC on a confirmed transmission and carried
    // the key id. A forced scrambler is the decoder's choice, not a received cipher.
    // Received only from this VCALL itself: it must carry the cipher being published. A new call that changed cipher
    // without a decoded release finds the hysteresis still holding the previous call's cipher, which this VCALL did not
    // carry. A VCALL contradicting a received field withdraws it.
    const int carries = (uint32_t)info->cipher_type == state->nxdn_cipher_type;
    const uint8_t received =
        carries && nxdn_cipher_received_enc(state) ? (uint8_t)(DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID) : 0U;
    uint8_t contradicted = 0U;
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) > 0) {
        if ((call.crypto_received & DSD_CALL_CRYPTO_RX_ALG) != 0U && call.received_algid != info->cipher_type) {
            contradicted |= (uint8_t)DSD_CALL_CRYPTO_RX_ALG;
        }
        if ((call.crypto_received & DSD_CALL_CRYPTO_RX_KID) != 0U && call.received_kid != info->key_id) {
            contradicted |= (uint8_t)DSD_CALL_CRYPTO_RX_KID;
        }
    }
    // This checked VCALL's own cipher says whether the call is encrypted, whatever the applied (possibly forced or
    // held) cipher is.
    nxdn_vcall_publish_crypto(opts, state, (uint8_t)state->nxdn_cipher_type, (uint8_t)state->nxdn_key, received,
                              contradicted,
                              info->cipher_type != 0U ? DSD_CALL_ENC_OBSERVED_ENCRYPTED : DSD_CALL_ENC_OBSERVED_CLEAR);
    dsd_event_sync_slot(opts, state, 0U);
}

static void
nxdn_vcall_apply_state(dsd_state* state, const struct nxdn_vcall_info* info) {
    if (info->message_type == 0x01U) {
        // A VCALL reaches here only through a channel decoder whose CRC passed -- the element
        // decoder is never handed unverified content -- and even then mutates the live cipher only
        // through the classification hysteresis: trellis miscorrections that survive the short
        // CRCs are exactly one element away from muting a clear call -- or unmuting an encrypted
        // one -- and, under lockout, permanently blocking the talkgroup.
        state->nxdn_key = info->key_id;
        state->nxdn_cipher_type = nxdn_cipher_observe(state, info->cipher_type, 0);
    } else {
        DSD_SNPRINTF(state->generic_talker_alias[0], sizeof(state->generic_talker_alias[0]), "%s", "");
        nxdn_alias_reset(state);
    }

    state->dmr_encL = (state->nxdn_cipher_type != 0) ? 1 : 0;
    if (state->nxdn_cipher_type == 0 || nxdn_vcall_has_key(state, (uint8_t)state->nxdn_cipher_type)) {
        state->dmr_encL = 0;
    }
}

static void
nxdn_vcall_run_enc_lockout(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_info* info) {
    if (opts->trunk_enable != 1 || opts->trunk_tune_enc_calls != 0 || info->message_type != 0x01U
        || info->destination_id == 0) {
        return;
    }
    const int is_group = (info->call_type == 4U) ? 0 : 1;
    if (state->dmr_encL != 1) {
        // Corroborated clear -- or encrypted-but-decryptable -- voice on this
        // target releases any retained lockout entry so a talkgroup that
        // stopped encrypting (or gained a key) recovers.
        const int established_recoverable =
            nxdn_cipher_established_clear(state)
            || (nxdn_cipher_established_enc(state) && nxdn_vcall_has_key(state, (uint8_t)state->nxdn_cipher_type));
        if (established_recoverable) {
            (void)dsd_enc_lockout_release(state, info->destination_id, is_group);
        }
        return;
    }
    // The lockout entry is permanent for the session and the synthesized disconnect drops the
    // channel, so the lockout acts only on corroborated evidence. The element was CRC-verified by
    // the channel decoder that handed it over, and a lone non-clear observation stays tentative
    // (or quarantined) in the hysteresis above and must repeat -- one superframe -- before it may
    // lock the talkgroup out.
    if (!nxdn_cipher_established_enc(state)) {
        return;
    }

    if (dsd_enc_lockout_note(state, info->destination_id, is_group, (int)state->nxdn_cipher_type,
                             (int)state->nxdn_key)) {
        char note[128];
        DSD_SNPRINTF(note, sizeof note, "Target: %d; has been locked out; Encryption Lock Out Enabled.",
                     info->destination_id);
        dsd_event_note_current_call(opts, state, 0U, note);
    }

    // Deliberately unconditional (the event above fires once per lock, the
    // disconnect fires per corroborated VCALL): retrying the synthesized
    // disconnect until the trunking layer actually drops the channel is what
    // lets an already-locked target still force a release.
    //
    // This fabricated DISC is the one element content no CRC ever covered, and deliberately so: it
    // exists to drive the same teardown a received DISC would -- nxdn_message_type()'s call and
    // alias reset, and nxdn_element_handle_disc()'s return to the control channel.
    uint8_t dbits[96];
    DSD_MEMSET(dbits, 0, sizeof(dbits));
    dbits[3] = 1;
    dbits[7] = 1;
    NXDN_Elements_Content_decode(opts, state, dbits, sizeof(dbits));
}

// Release and disconnect variants that reach nxdn_vcall_process. TX_REL_EX (0x07) ends the epoch
// only here: unlike TX_REL/DISC it deliberately skips the alias/cipher/keyloader resets in
// nxdn_message_type(), so this is the one site where its end reason is recorded.
static int
nxdn_vcall_message_type_is_release(uint8_t message_type) {
    return message_type == 0x07U || message_type == 0x08U || message_type == 0x11U;
}

static void
nxdn_vcall_process(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_info* info) {
    if (info->message_type != 0x01U) {
        // A release reaching here was CRC-verified by the channel decoder that handed it over, so
        // it is positive end evidence decoded over the air -- the terminator reason lets the event
        // layer keep an audible epoch whose call identity never decoded, where EXPLICIT reads as a
        // retune and drops the row.
        const dsd_call_end_reason reason =
            nxdn_vcall_message_type_is_release(info->message_type) ? DSD_CALL_END_TERMINATOR : DSD_CALL_END_EXPLICIT;
        if (dsd_call_state_end_ex(state, 0U, 0.0, reason) > 0) {
            dsd_event_sync_slot(opts, state, 0U);
        }
        nxdn_call_identity_end(opts, state);
    }
    nxdn_vcall_print_summary(state, info);
    nxdn_vcall_load_key(opts, state, info);
    nxdn_vcall_print_cipher(opts, state, info);
    nxdn_vcall_apply_state(state, info);
    if (info->message_type == 0x01U && nxdn_confirm_is_confirmed(state)) {
        // Held, like the publish below, until the frame's content has confirmed the transmission:
        // a miscorrected VCALL carries a garbage destination_id, and letting it refresh the
        // conventional scan hold would park the coordinator on noise for a full activity_hold_ms
        // per corrupt burst. The encryption
        // flag is the corroborated classification nxdn_vcall_apply_state() just applied, not the
        // raw element field -- a lone flipped cipher_type would otherwise drop the hold mid-call
        // under --enc-lockout, the exact defect nxdn_enc_class.c exists to prevent.
        dsd_trunk_scan_hook_nxdn_conventional_activity(
            opts, state, info->destination_id, info->source_unit_id,
            nxdn_vcall_kind(info->call_type) == DSD_CALL_KIND_PRIVATE_VOICE ? 1 : 0, state->nxdn_cipher_type != 0U, 0);
        nxdn_vcall_publish(opts, state, info);
        // Classified as admission classifies its assignment (nxdn_vcall_assgn_admit()): call type 4 is private.
        nxdn_call_identity_reconcile(opts, state, info->call_type == 4U ? 1 : 0, nxdn_receiver_on_type_d(opts, state));
    }
    nxdn_vcall_run_enc_lockout(opts, state, info);
}

static void
NXDN_decode_VCALL(dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
    struct nxdn_vcall_info info;
    nxdn_vcall_parse(state, Message, &info);
    nxdn_vcall_process(opts, state, &info);
}

static void
NXDN_decode_VCALL_ARIB(dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
    struct nxdn_vcall_info info;
    nxdn_vcall_parse_arib(state, Message, &info);
    nxdn_vcall_process(opts, state, &info);
}

static unsigned long long int
nxdn_vcall_iv_extract(const dsd_state* state, const uint8_t* Message) {
    if (strcmp(state->nxdn_location_category, "Type-D") == 0) {
        return (unsigned long long int)convert_bits_into_output(&Message[8], 22);
    }
    return (unsigned long long int)convert_bits_into_output(&Message[8], 64);
}

static void
nxdn_vcall_iv_prepare_cipher(dsd_state* state) {
    // The VCALL's choice again (nxdn_select_key()), never the signalled ID unconditionally: the key the IV keys
    // stays the one the VCALL loaded and reported.
    if ((state->nxdn_cipher_type == 0x02 || state->nxdn_cipher_type == 0x03) && state->keyloader == 1) {
        nxdn_load_selected_key(NULL, state, (uint8_t)state->nxdn_cipher_type, state->nxdn_key, 0U);
    }
    if (state->nxdn_cipher_type == 0x03) {
        LFSR128n(state);
    }
    if (state->nxdn_cipher_type == 0x02 && dsd_key_scalar_present(state, 0)) {
        state->nxdn_new_iv = 1;
    }
    if (state->nxdn_cipher_type == 0x03 && dsd_key_aes_slot_complete(state, 0, 4U)) {
        state->nxdn_new_iv = 1;
    }
}

static void
NXDN_decode_VCALL_IV(dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
    state->payload_miN = nxdn_vcall_iv_extract(state, Message);
    DSD_FPRINTF(stderr, "\n  VCALL_IV: %016llX", state->payload_miN);
    if (state->nxdn_cipher_type == 0x02 || state->nxdn_cipher_type == 0x03) {
        nxdn_vcall_iv_prepare_cipher(state);
    }
    nxdn_vcall_publish_crypto(opts, state, (uint8_t)state->nxdn_cipher_type, (uint8_t)state->nxdn_key, 0U, 0U,
                              DSD_CALL_ENC_OBSERVED_NONE);
}

struct nxdn_scch_info {
    time_t now;
    uint8_t direction;
    uint8_t sf;
    uint8_t opcode;
    uint8_t area;
    uint8_t rep1;
    uint8_t rep2;
    uint8_t site_type;
    uint8_t site_code;
    uint8_t gu;
    uint8_t iv_type;
    uint8_t call_opt;
    uint8_t key_id;
    uint8_t cipher;
    uint16_t id;
    unsigned long long int iv_a;
    unsigned long long int iv_b;
    unsigned long long int iv_c;
};

static void
nxdn_scch_parse(const uint8_t* Message, uint8_t direction, time_t now, struct nxdn_scch_info* info) {
    DSD_MEMSET(info, 0, sizeof(*info));
    info->now = now;
    info->direction = direction;
    info->sf = (uint8_t)convert_bits_into_output(&Message[0], 2);
    info->opcode = (uint8_t)(direction << 2 | info->sf);
    info->area = Message[2];
    info->rep1 = (uint8_t)convert_bits_into_output(&Message[3], 5);
    info->rep2 = (uint8_t)convert_bits_into_output(&Message[8], 5);
    info->id = (uint16_t)convert_bits_into_output(&Message[13], 11);
    /* The site ID message's fields (NXDN TS 1-E §6.5.3.1 Fig 6.5-8): Site Type 2 bits, Site Code 8. */
    info->site_type = (uint8_t)convert_bits_into_output(&Message[3], 2);
    info->site_code = (uint8_t)convert_bits_into_output(&Message[5], 8);
    info->gu = Message[24];
    info->iv_a = (uint64_t)convert_bits_into_output(&Message[13], 12);
    info->iv_b = (uint64_t)convert_bits_into_output(&Message[18], 6);
    info->iv_c = (uint64_t)convert_bits_into_output(&Message[8], 5);
    info->iv_type = Message[24];
    info->call_opt = (uint8_t)convert_bits_into_output(&Message[13], 3);
    info->key_id = (uint8_t)convert_bits_into_output(&Message[18], 6);
    info->cipher = (uint8_t)convert_bits_into_output(&Message[16], 2);
}

static void
nxdn_scch_print_payload_label(const dsd_opts* opts, const struct nxdn_scch_info* info) {
    if (opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "%s ", info->direction == 0U ? "ISM" : "OSM");
    if (info->sf == 0U) {
        DSD_FPRINTF(stderr, "INFO4 ");
    } else if (info->sf == 1U) {
        DSD_FPRINTF(stderr, "INFO3 ");
    } else if (info->sf == 2U) {
        DSD_FPRINTF(stderr, "INFO2 ");
    } else {
        DSD_FPRINTF(stderr, "INFO1 ");
    }
    DSD_FPRINTF(stderr, "- ");
    DSD_FPRINTF(stderr, "%02X ", info->opcode);
}

static void
nxdn_scch_prepare_type_d(const dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "Type-D");
    if (!nxdn_confirm_is_confirmed(state)) {
        /* SCCH carries a 7-bit CRC, which noise clears often enough that a RAN and a scan
         * hold cannot rest on one (issue #398). */
        return;
    }
    /* The area bit, which the terminal shows as "IDAS - Area" where the RAN goes; not a RAN (issue #575). The received
       identity takes it once the SCCH run itself is confirmed: its 7-bit CRC passes noise one time in 128. */
    if (nxdn_confirm_scch_confirmed(state)) {
        dsd_rxid_note_nxdn_idas_area(state, info->area);
    }
    state->nxdn_last_ran = info->area;
    state->nxdn_last_ran_stand_in = 1U;
    state->last_cc_sync_time = info->now;
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    nxdn_cc_hunt_note_type_d(opts, state);
}

/* Site Type (NXDN TS 1-E §6.6.4, Table 6.6-2): the coverage the roaming algorithm selects on. */
static void
nxdn_scch_print_site_type(uint8_t site_type) {
    DSD_FPRINTF(stderr, "Site Type: %d ", site_type);
    if (site_type == 0U) {
        DSD_FPRINTF(stderr, "Reserved; ");
    } else if (site_type == 1U) {
        DSD_FPRINTF(stderr, "Wide; ");
    } else if (site_type == 2U) {
        DSD_FPRINTF(stderr, "Middle; ");
    } else {
        DSD_FPRINTF(stderr, "Narrow; ");
    }
}

/*
 * The site ID message (NXDN TS 1-E §6.5.3.1 Fig 6.5-8): Structure(2) Area(1) Site Type(2) Site Code(8) ID=2041(11)
 * Spare(1). It names a site code and nothing else: no system and no RAN, so it writes neither; its area is
 * nxdn_scch_prepare_type_d()'s, as every SCCH's is. Its 7-bit CRC passes noise one time in 128, so the site code is
 * kept only once SCCHs of NXDN_CONFIRM_WEAK_OBSERVES frames running have passed theirs.
 */
static void
nxdn_scch_handle_site_id(dsd_state* state, const struct nxdn_scch_info* info) {
    DSD_FPRINTF(stderr, "Site ID Message - ");
    DSD_FPRINTF(stderr, "Area: %d; ", info->area);
    nxdn_scch_print_site_type(info->site_type);
    if (!nxdn_idas_site_code_valid(info->site_code)) {
        DSD_FPRINTF(stderr, "Site Code: %d (Reserved); ", info->site_code);
        return;
    }
    DSD_FPRINTF(stderr, "Site Code: %d; ", info->site_code);
    if (nxdn_confirm_scch_confirmed(state)) {
        state->nxdn_idas_site_code = info->site_code;
        dsd_rxid_note_nxdn_idas_site(state, info->site_type, info->site_code);
    }
}

static int
nxdn_scch_should_tune(const dsd_opts* opts, const struct nxdn_scch_info* info) {
    if (info->rep1 == 31U) {
        return 1;
    }
    if (info->gu == 0U) {
        return (opts->trunk_tune_group_calls == 1) ? 1 : 0;
    }
    return (opts->trunk_tune_private_calls == 1) ? 1 : 0;
}

static void
nxdn_scch_update_control_channel_from_map(dsd_state* state, const struct nxdn_scch_info* info) {
    if (state->trunk_chan_map[31] != 0) {
        state->p25_cc_freq = state->trunk_chan_map[31];
        state->trunk_cc_freq = state->p25_cc_freq;
    } else if (state->trunk_chan_map[info->rep2] != 0) {
        state->p25_cc_freq = state->trunk_chan_map[info->rep2];
        state->trunk_cc_freq = state->p25_cc_freq;
    }
}

/* While voice is live on the receiver's repeater, whether the busy message's call takes the receiver: only one on
 * another repeater, of the held talkgroup over a call that is not of it, else an outranking preempting candidate
 * (@p preempted set). */
static int
nxdn_scch_live_takeover(const dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info, long int freq,
                        const dsd_tg_policy_decision* decision, int* preempted) {
    if (info->rep1 != 31U && nxdn_idas_assoc(opts, state, info->rep1) == NXDN_IDAS_OTHER) {
        const long int rx = nxdn_receiver_freq(opts, state);
        const dsd_nxdn_call_identity* const cur = nxdn_current_identity(opts, state, rx);
        const int hold_matches = (state->tg_hold != 0U && state->tg_hold == info->id) ? 1 : 0;
        dsd_tg_policy_call_route candidate;
        DSD_MEMSET(&candidate, 0, sizeof candidate);
        candidate.target_id = info->id;
        candidate.freq_hz = freq;
        candidate.channel = info->rep1;
        candidate.slot = -1;
        candidate.requires_tuner_retune = 1;
        if (nxdn_hold_takes_over(state, cur, hold_matches)) {
            return 1;
        }
        if (nxdn_preempts(opts, state, cur, rx, &candidate, decision)) {
            *preempted = 1;
            return 1;
        }
    }
    return 0;
}

/* The receiver went to the busy message's go-to repeater at @p freq: reset the SACCH reassembly and sync, load the
 * call's key, and take the route (or, for repeater 31, release it on the return to the control channel). */
static void
nxdn_scch_busy_tuned(dsd_state* state, const struct nxdn_scch_info* info, long int freq, int is_private_call,
                     const dsd_tg_policy_decision* decision, nxdn_follow_how how) {
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    state->lastsynctype = DSD_SYNC_NONE;
    dsd_nxdn96_sync_clear(&state->nxdn96_sync);
    (void)keyring_load_scalar_key(state, 0, (int)info->id);
    if (state->M == 1) {
        nxdn_cipher_force(state, 0x1);
    }
    if (info->rep1 == 31U) {
        nxdn_follow_release(state); // the termination's return to the control channel
    } else {
        nxdn_note_follow(state, info->id, 0U, is_private_call, 0, info->rep1, freq, info->rep1, decision, how);
    }
}

/* Tune to the admitted busy message's go-to repeater at @p freq (@p preempted: a preemption). A message repeated past
 * the voice window for the call the receiver still holds follows that same call in place; a follow to another carrier
 * ends the call left there (nxdn_end_left_call()). */
static void
nxdn_scch_follow_busy(dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info, long int freq,
                      const dsd_tg_policy_decision* decision, int preempted) {
    const int is_private_call = (info->gu == 1U) ? 1 : 0;
    const int held = nxdn_follow_holds_call(opts, state, info->id, is_private_call, freq);
    const int leaves = info->rep1 != 31U && nxdn_tune_leaves_carrier(opts, state, freq);
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq, 0, NULL);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        return;
    }
    if (leaves) {
        nxdn_end_left_call(opts, state);
    }
    const nxdn_follow_how how = preempted ? NXDN_FOLLOW_PREEMPT : (held ? NXDN_FOLLOW_HELD : NXDN_FOLLOW_NEW);
    nxdn_scch_busy_tuned(state, info, freq, is_private_call, decision, how);
}

/* A busy repeater message names a call on go-to repeater rep1 (31: the call there ended, back to the control channel).
 * With no voice on the receiver's repeater for a second it is followed as before. While voice is live there, only a
 * call on another repeater (nxdn_idas_assoc()) may take the receiver: one of the held talkgroup over a call that is not
 * of it, else an outranking preempting candidate (nxdn_preempts()); a message for the receiver's own repeater, or one
 * whose repeater cannot be placed, never moves it. */
static void
nxdn_scch_apply_busy_tune(dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    if (!nxdn_scch_should_tune(opts, info) || info->rep1 == 0U) {
        return;
    }

    nxdn_scch_update_control_channel_from_map(state, info);
    nxdn_print_group_label(state, info->id);

    const long int freq = nxdn_channel_to_frequency(opts, state, info->rep1);
    if (freq == 0) {
        nxdn_trunk_diag_log_missing_channel_once(opts, state, info->rep1, "tune");
    }

    dsd_tg_policy_decision policy_decision;
    const int is_private_call = (info->gu == 1U) ? 1 : 0;
    const int policy_allowed =
        nxdn_policy_tune_allowed(opts, state, info->id, 0, is_private_call, 0, 0, &policy_decision);
    int admitted = opts->trunk_enable == 1 && policy_allowed && state->p25_cc_freq != 0 && freq != 0;
    int preempted = 0;
    if (admitted && (info->now - state->last_vc_sync_time) <= 1) {
        admitted = nxdn_scch_live_takeover(opts, state, info, freq, &policy_decision, &preempted);
    }
    if (admitted) {
        nxdn_scch_follow_busy(opts, state, info, freq, &policy_decision, preempted);
    } else if (opts->trunk_enable == 1) {
        nxdn_policy_log_block(opts, is_private_call, info->id, 0, &policy_decision);
    }
}

static void
nxdn_scch_update_busy_display(dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    if (info->rep1 == 31U) {
        DSD_FPRINTF(stderr, "\n%s ", KRED);
    } else {
        DSD_FPRINTF(stderr, "\n%s ", KGRN);
    }

    DSD_FPRINTF(stderr, " Channel Update - CH: %d - TGT: %d ", info->rep1, info->id);
    DSD_FPRINTF(stderr, "%s ", info->gu == 0U ? "Group Call" : "Private Call");
    if (info->rep1 == 31U) {
        DSD_FPRINTF(stderr, "Termination ");
        // Channel-update termination signaling: positive over-the-air end evidence.
        if (dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_TERMINATOR) > 0) {
            dsd_event_sync_slot(opts, state, 0U);
        }
        nxdn_call_identity_end(opts, state);
    } else if (info->rep1 != 0U) {
        const dsd_call_observation observation = {
            .protocol = DSD_SYNC_NXDN_POS,
            .slot = 0U,
            .kind = info->gu == 0U ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE,
            .ota_target_id = info->id,
            .policy_target_id = info->id,
            .channel = info->rep1,
        };
        char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
        if (info->gu == 0U) {
            DSD_SNPRINTF(notice, sizeof(notice), "Active Group Ch: %d TG: %d-%d; ", info->rep1, info->rep2, info->id);
        } else {
            DSD_SNPRINTF(notice, sizeof(notice), "Active Private Ch: %d TGT: %d-%d; ", info->rep1, info->rep2,
                         info->id);
        }
        (void)dsd_recent_activity_publish(state, info->rep1, &observation, notice, 0U);
    }
}

/* The class an SCCH naming @p target_id gives the live call @p call: the message's own (@p kind, a busy message's G/U
 * bit), else the call's when the message names the call's target or none, else none: a message naming another target
 * (an INFO2 target message) names another call, which a previous call's private or group class never describes. */
static dsd_call_kind
nxdn_scch_named_kind(const dsd_call_snapshot* call, uint32_t target_id, dsd_call_kind kind) {
    if (kind != DSD_CALL_KIND_UNKNOWN) {
        return kind;
    }
    if (target_id != 0U && call->policy_target_id != 0U && target_id != call->policy_target_id) {
        return DSD_CALL_KIND_VOICE;
    }
    return call->kind;
}

/* The live call takes an SCCH's source or target (0: keep the call's), and @p kind when the message classifies it
 * (a busy message's G/U bit; DSD_CALL_KIND_UNKNOWN keeps the call's for the same target, and gives a new target no
 * class, nxdn_scch_named_kind()), so a later unclassified message for the same target reads the class this one
 * verified, never a previous call's. */
static void
nxdn_scch_enrich_identity(dsd_state* state, uint32_t source_id, uint32_t target_id, dsd_call_kind kind) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_NXDN(call.protocol)) {
        return;
    }
    dsd_call_observation observation = {
        .protocol = call.protocol,
        .slot = 0U,
        .kind = nxdn_scch_named_kind(&call, target_id, kind),
        .ota_target_id = target_id != 0U ? target_id : call.ota_target_id,
        .policy_target_id = target_id != 0U ? target_id : call.policy_target_id,
        .ota_source_id = source_id != 0U ? source_id : call.ota_source_id,
        .channel = call.channel,
        .frequency_hz = call.frequency_hz,
        .service_options = call.service_options,
        .emergency = call.emergency,
        .priority = call.priority,
        .has_service_metadata = call.has_service_metadata,
    };
    if (source_id == 0U) {
        DSD_MEMCPY(observation.source_text, call.source_text, sizeof(observation.source_text));
    }
    if (target_id == 0U) {
        DSD_MEMCPY(observation.target_text, call.target_text, sizeof(observation.target_text));
    }
    DSD_MEMCPY(observation.route_text[0], call.route_text[0], sizeof(observation.route_text[0]));
    DSD_MEMCPY(observation.route_text[1], call.route_text[1], sizeof(observation.route_text[1]));

    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) > 0) {
        const dsd_call_crypto_update crypto = {
            .classification = call.crypto,
            .algid = call.algid,
            .kid = call.kid,
            .mi = call.mi,
            .audio_permitted = call.audio_permitted,
        };
        (void)dsd_call_state_update_crypto(state, 0U, &crypto);
        if (call.media_active != 0U) {
            (void)dsd_call_state_update_media(state, 0U, 1, 0.0);
        }
    }
}

static void
nxdn_scch_handle_busy(dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    DSD_FPRINTF(stderr, "%s", (info->gu && info->rep1 == 0U) ? "REG_COMM; " : "Busy Repeater Message - ");
    DSD_FPRINTF(stderr, "Area: %d; ", info->area);
    DSD_FPRINTF(stderr, "Go to Repeater: %d; ", info->rep1);
    DSD_FPRINTF(stderr, "Home Repeater: %d; ", info->rep2);
    nxdn_scch_update_busy_display(opts, state, info);
    /* The live call takes the message's target only when the go-to repeater is the receiver's own: a busy message for
     * another repeater is another call. */
    if (info->rep1 != 31U && info->now - state->last_vc_sync_time < 1
        && nxdn_idas_assoc(opts, state, info->rep1) == NXDN_IDAS_MATCH) {
        // The message's G/U bit classifies its target, for the canonical call as for the held one.
        nxdn_scch_enrich_identity(state, 0U, info->id,
                                  info->gu ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE);
        nxdn_call_identity_reconcile(opts, state, info->gu ? 1 : 0, 1);
    }
    nxdn_scch_apply_busy_tune(opts, state, info);
}

/* The INFO4 messages whose ID field is a fixed code rather than a unit (NXDN TS 1-E §6.5.3.1 Figs 6.5-5..6.5-8): idle
 * (2046), halt (2045) and free (2044) repeater, and site ID (2041). Returns whether this was one. */
static int
nxdn_scch_handle_repeater_status(dsd_state* state, const struct nxdn_scch_info* info) {
    if (info->id == 2046U) {
        DSD_FPRINTF(stderr, "Idle Repeater Message - ");
        DSD_FPRINTF(stderr, "Area: %d; ", info->area);
        DSD_FPRINTF(stderr, "Repeater 1: %d; ", info->rep1);
        DSD_FPRINTF(stderr, "Repeater 2: %d; ", info->rep2);
        (void)dsd_recent_activity_clear(state, info->rep1);
        (void)dsd_recent_activity_clear(state, info->rep2);
    } else if (info->id == 2045U) {
        DSD_FPRINTF(stderr, "Halt Repeater Message - ");
        DSD_FPRINTF(stderr, "Area: %d; ", info->area);
        DSD_FPRINTF(stderr, "Repeater 1: %d; ", info->rep1);
        DSD_FPRINTF(stderr, "Repeater 2: %d; ", info->rep2);
    } else if (info->id == 2044U) {
        DSD_FPRINTF(stderr, "Free Repeater Message - ");
        DSD_FPRINTF(stderr, "Area: %d; ", info->area);
        DSD_FPRINTF(stderr, "Free Repeater 1: %d; ", info->rep1);
        DSD_FPRINTF(stderr, "Free Repeater 2: %d; ", info->rep2);
    } else if (info->id == 2041U) {
        nxdn_scch_handle_site_id(state, info);
    } else {
        return 0;
    }
    return 1;
}

static void
nxdn_scch_handle_info4(dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    (void)dsd_recent_activity_expire(state, 0U, DSD_RECENT_ACTIVITY_TTL_MS);

    if (!nxdn_scch_handle_repeater_status(state, info)) {
        nxdn_scch_handle_busy(opts, state, info);
    }
}

/* The live call's transmission (0: none active). */
static uint64_t
nxdn_live_call_transmission(const dsd_state* state) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return 0U;
    }
    return dsd_call_snapshot_transmission(&call);
}

/* An INFO3 source update of the live call (transmission @p before) whose source differs from the call's makes the
 * canonical layer open another epoch for it (dsd_call_state_observe(): a source change), of the same target and class,
 * since the enrichment carries both over. It names the same call, so a held call bound to @p before follows it there,
 * its follow time kept: a source-only update never ends, unknowns or restarts the dwell of a known destination. The
 * source itself stays out of the held call (INFO3 may misname the talker, nxdn_held_policy_source()). */
static void
nxdn_call_identity_follow_source_update(dsd_state* state, uint64_t before) {
    dsd_nxdn_call_identity* const id = &state->nxdn_call;
    const uint64_t after = nxdn_live_call_transmission(state);
    if (!id->valid || id->gap || before == 0U || id->call_epoch != before || after == 0U || after == before) {
        return;
    }
    id->call_epoch = after;
    id->call_seen_m = dsd_decode_now_mono_s();
}

static void
nxdn_scch_handle_info3(dsd_state* state, const struct nxdn_scch_info* info) {
    DSD_FPRINTF(stderr, "Source Message - ");
    DSD_FPRINTF(stderr, "Area: %d; ", info->area);
    DSD_FPRINTF(stderr, "Free Repeater 1: %d; ", info->rep1);
    if (info->id == 31U) {
        DSD_FPRINTF(stderr, "\n%s ", KYEL);
        DSD_FPRINTF(stderr, " Call IV A: %04llX", info->iv_a);
    } else {
        DSD_FPRINTF(stderr, "\n%s ", KGRN);
        DSD_FPRINTF(stderr, " Source Update - Prefix CH: %d SRC: %d - (%d-%d) ", info->rep2, info->id, info->rep2,
                    info->id);
        if (info->now - state->last_vc_sync_time < 1) {
            const uint64_t before = nxdn_live_call_transmission(state);
            nxdn_scch_enrich_identity(state, info->id, 0U, DSD_CALL_KIND_UNKNOWN);
            nxdn_call_identity_follow_source_update(state, before);
        }
    }
}

static void
nxdn_scch_handle_info2(const dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    /* An outbound INFO2 has the INFO4 format (NXDN TS 1-E §6.5.3.3; TS 2-E Tables 5.1-2..5.1-4, "OSM INFO4 / INFO2"):
     * its idle, halt, free repeater and site ID messages carry a fixed code where a busy repeater message carries the
     * destination, and name no unit, so they never become the call's target. */
    if (info->direction == 1U && nxdn_scch_handle_repeater_status(state, info)) {
        return;
    }
    DSD_FPRINTF(stderr, "Target Message - ");
    DSD_FPRINTF(stderr, "Area: %d; ", info->area);
    DSD_FPRINTF(stderr, "Go to Repeater: %d; ", info->rep1);
    if (info->id == 31U) {
        DSD_FPRINTF(stderr, "\n%s ", KYEL);
        DSD_FPRINTF(stderr, " Call IV A: %04llX; ", info->iv_a);
        state->payload_miN = info->iv_a << 11;
    } else {
        DSD_FPRINTF(stderr, "\n%s ", KGRN);
        DSD_FPRINTF(stderr, " Target Update - Prefix CH: %d SRC: %d - (%d-%d) ", info->rep2, info->id, info->rep2,
                    info->id);
        /* As a busy repeater message's (nxdn_scch_handle_busy()): only for the receiver's own go-to repeater. */
        if (info->now - state->last_vc_sync_time < 1 && nxdn_idas_assoc(opts, state, info->rep1) == NXDN_IDAS_MATCH) {
            nxdn_scch_enrich_identity(state, 0U, info->id, DSD_CALL_KIND_UNKNOWN);
            nxdn_call_identity_reconcile(opts, state, -1, 1);
        }
    }
}

static void
nxdn_scch_handle_info1(dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    uint8_t duplex_mode[32] = {0};
    uint8_t transmission_mode[32] = {0};
    DSD_FPRINTF(stderr, "Call Option - ");
    DSD_FPRINTF(stderr, "Area: %d; ", info->area);
    DSD_FPRINTF(stderr, "Free Repeater 1: %d; ", info->rep1);
    if (info->iv_type == 0U) {
        DSD_FPRINTF(stderr, "Free Repeater 2: %d; ", info->rep2);
        DSD_FPRINTF(stderr, "\n%s ", KYEL);
        NXDN_Voice_Call_Option_To_Str(info->call_opt, duplex_mode, transmission_mode);
        DSD_FPRINTF(stderr, " %s %s ", duplex_mode, transmission_mode);
        if (info->cipher) {
            DSD_FPRINTF(stderr, "- %s - ", NXDN_Cipher_Type_To_Str(info->cipher));
            DSD_FPRINTF(stderr, "Key ID: %d; ", info->key_id);
        }
        // One SCCH observation must not flip the classification the VCALLs established -- in
        // either direction: a corrupt call option could mute a clear call, or silently unmute
        // an encrypted one mid-stream. The hysteresis holds it until it repeats.
        state->nxdn_cipher_type = nxdn_cipher_observe(state, (uint8_t)info->cipher, 0);
        state->nxdn_key = info->key_id;
        nxdn_vcall_publish_crypto(opts, state, (uint8_t)state->nxdn_cipher_type, info->key_id, 0U, 0U,
                                  DSD_CALL_ENC_OBSERVED_NONE);
    } else {
        DSD_FPRINTF(stderr, "\n%s ", KYEL);
        DSD_FPRINTF(stderr, "Call IV B: %04llX; ", info->iv_b);
        DSD_FPRINTF(stderr, "Call IV C: %04llX; ", info->iv_c);
        state->payload_miN = state->payload_miN | (info->iv_c << 6);
        state->payload_miN = state->payload_miN | info->iv_b;
        DSD_FPRINTF(stderr, "Completed IV: %016llX", state->payload_miN);
        nxdn_vcall_publish_crypto(opts, state, (uint8_t)state->nxdn_cipher_type, (uint8_t)state->nxdn_key, 0U, 0U,
                                  DSD_CALL_ENC_OBSERVED_NONE);
    }
}

//SCCH messages have a unique format that can be used in a super frame, but each 'unit'
//can also (mostly) be decoded seperately, except an enc IV
void
NXDN_decode_scch(dsd_opts* opts, dsd_state* state, const uint8_t* Message, uint8_t direction) {
    const time_t now = dsd_decode_time();
    struct nxdn_scch_info info;
    nxdn_scch_parse(Message, direction, now, &info);

    DSD_FPRINTF(stderr, "\n "); //initial line break
    nxdn_scch_print_payload_label(opts, &info);
    nxdn_scch_prepare_type_d(opts, state, &info);

    if (info.opcode == 0x04U || info.opcode == 0x00U) {
        nxdn_scch_handle_info4(opts, state, &info);
    }
    if (info.opcode == 0x05U || info.opcode == 0x01U) {
        nxdn_scch_handle_info3(state, &info);
    }
    if (info.opcode == 0x06U || info.opcode == 0x02U) {
        nxdn_scch_handle_info2(opts, state, &info);
    }
    if (info.opcode == 0x07U || info.opcode == 0x03U) {
        nxdn_scch_handle_info1(opts, state, &info);
    }
}

static char*
NXDN_Call_Type_To_Str(uint8_t CallType) {
    char* Ptr = NULL;

    switch (CallType) {
        case 0: Ptr = "Broadcast Call"; break;
        case 1: Ptr = "Group Call"; break;
        case 2: Ptr = "Idle"; break;         //"Unspecified Call" This value is used only on Idle Burst TX_REL message.
        case 3: Ptr = "Session Call"; break; //"reserved" is session call on Type D
        case 4: Ptr = "Private Call"; break;
        case 5: Ptr = "Reserved"; break;
        case 6: Ptr = "PSTN Interconnect Call"; break;
        case 7: Ptr = "PSTN Speed Dial Call"; break;
        default: Ptr = "Unknown Call Type"; break;
    }

    return Ptr;
} /* End NXDN_Call_Type_To_Str() */

static void
NXDN_Voice_Call_Option_To_Str(uint8_t VoiceCallOption, uint8_t* Duplex, uint8_t* TransmissionMode) {
    static const char* const modes[16] = {
        "4800bps/EHR",     "Reserved 1",     "9600bps/EHR",     "9600bps/EFR",     "Reserved 4",      "Reserved 5",
        "Reserved 6",      "Reserved 7",     "4800bps/EHR S:1", "Reserved 9; S:1", "9600bps/EHR S:1", "9600bps/EFR S:1",
        "Reserved C; S1;", "Reserved D; S1", "Reserved E; S1",  "Reserved F: S1",
    };

    Duplex[0] = 0;
    TransmissionMode[0] = 0;

    if (VoiceCallOption & 0x10) {
        DSD_SNPRINTF((char*)Duplex, 32, "%s", "Duplex");
    } else {
        DSD_SNPRINTF((char*)Duplex, 32, "%s", "Half Duplex");
    }

    DSD_SNPRINTF((char*)TransmissionMode, 32, "%s", modes[VoiceCallOption & 0x0FU]);
} /* End NXDN_Voice_Call_Option_To_Str() */

static char*
NXDN_Cipher_Type_To_Str(uint8_t CipherType) {
    char* Ptr = NULL;

    switch (CipherType) {
        case 0: Ptr = ""; break; /* Non-ciphered mode / clear call */
        case 1: Ptr = "Scrambler"; break;
        case 2: Ptr = "DES"; break;
        case 3: Ptr = "AES"; break;
        default: Ptr = "Unknown Cipher Type"; break;
    }

    return Ptr;
} /* End NXDN_Cipher_Type_To_Str() */
