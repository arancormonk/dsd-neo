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
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_crc.h"

typedef void (*nxdn_element_handler_fn)(dsd_opts* opts, dsd_state* state, const uint8_t* elements,
                                        size_t elements_bits);

struct nxdn_element_dispatch_entry {
    uint8_t message_type;
    nxdn_element_handler_fn handler;
};

static uint8_t nxdn_alias_crc_ok(const dsd_state* state);
static void nxdn_reset_data_call_state(dsd_state* state);
static void nxdn_data_call_option_to_str(uint8_t data_call_option, char* duplex, size_t duplex_sz, char* mode,
                                         size_t mode_sz);
static void nxdn_print_group_label(const dsd_state* state, uint32_t id);
static void nxdn_anchor_control_channel_from_current_tuner(const dsd_opts* opts, dsd_state* state, int only_if_missing);
static nxdn_element_handler_fn nxdn_element_dispatch_handler(uint8_t message_type);
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
static void nxdn_pdu_scrambler_keystream_creation(uint8_t* ks, int lfsr, int len_bits);
static void nxdn_lfsr128_expand_iv_from_mi64(uint64_t mi, uint8_t out[16]);
static int nxdn_load_data_aes_key(const dsd_state* state, uint8_t key_id, uint8_t out_key[32]);
static void nxdn_sdcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* Message);
static void nxdn_dcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits);
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
static void NXDN_decode_srv_info(const dsd_opts* opts, dsd_state* state, const uint8_t* Message);
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
        NXDN_Elements_Content_decode(opts, state, CrcCorrect, SACCH, sizeof(SACCH));
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

static void
nxdn_element_handle_idle(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    UNUSED2(elements, elements_bits);
    if (dsd_call_state_end(state, 0U, 0.0) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

static void
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
nxdn_element_handle_dcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    nxdn_dcall_header(opts, state, elements, elements_bits);
}

static void
nxdn_element_handle_dcall_data(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    nxdn_dcall_data(opts, state, state->data_header_format[0], elements, elements_bits);
}

static void
nxdn_element_handle_call_assignment(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    UNUSED(elements_bits);
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
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " ALIAS");
    DSD_FPRINTF(stderr, "%s", KNRM);
    nxdn_alias_decode_prop(opts, state, elements, nxdn_alias_crc_ok(state));
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
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    nxdn_anchor_control_channel_from_current_tuner(opts, state, 1);
}

static void
nxdn_element_handle_srv_info(const dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    UNUSED(elements_bits);
    NXDN_decode_srv_info(opts, state, elements);
    nxdn_element_mark_control_sync(opts, state);
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
    NXDN_decode_adj_site(opts, state, elements, elements_bits);
    nxdn_element_mark_control_sync(opts, state);
}

static void
nxdn_element_handle_tx_release(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    UNUSED(elements_bits);
    nxdn_reset_data_call_state(state);
    NXDN_decode_VCALL(opts, state, elements);
}

static void
nxdn_element_handle_vcall(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    UNUSED(elements_bits);
    NXDN_decode_VCALL(opts, state, elements);
}

static void
nxdn_element_handle_disc(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    UNUSED(elements_bits);
    nxdn_reset_data_call_state(state);
    NXDN_decode_VCALL(opts, state, elements);
    nxdn_alias_reset(state);

    if ((opts->trunk_enable == 1) && state->p25_cc_freq != 0 && (opts->trunk_is_tuned == 1)) {
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, state->p25_cc_freq, 0, NULL);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            return;
        }
        opts->trunk_is_tuned = 0;

        DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
        DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
        (void)dsd_recent_activity_clear_all(state);
        if (state->M == 0) {
            state->nxdn_cipher_type = 0;
        }
    }
}

static void
nxdn_element_handle_vcall_iv(dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    UNUSED(elements_bits);
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
NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, const uint8_t* ElementsContent,
                             size_t elements_bits) {
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

    nxdn_message_type(opts, state, MessageTypeExt);

    /* Save the "Message Type" field */
    state->NxdnElementsContent.MessageType = MessageType;

    /* Set the CRC state */
    state->NxdnElementsContent.VCallCrcIsGood = CrcCorrect;

    if (MessageTypeExt == 0xE7U) {
        nxdn_alias_decode_arib(opts, state, ElementsContent, nxdn_alias_crc_ok(state));
        return;
    }

    if (MessageTypeExt == 0xE1U || MessageTypeExt == 0xE8U) {
        NXDN_decode_VCALL_ARIB(opts, state, ElementsContent);
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
nxdn_reset_data_call_state(dsd_state* state) {
    if (state == NULL) {
        return;
    }

    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    state->data_header_blocks[0] = 1;
    state->data_header_padding[0] = 0;
    state->data_header_format[0] = 0;
    state->data_header_valid[0] = 0;

    state->payload_algid = 0;
    state->payload_keyid = 0;
    state->payload_mi = 0;
    DSD_MEMSET(state->aes_ivR, 0, sizeof(state->aes_ivR));

    state->dmr_lrrp_source[0] = 0;
    state->dmr_lrrp_target[0] = 0;
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

static void
nxdn_pdu_scrambler_keystream_creation(uint8_t* ks, int lfsr, int len_bits) {
    if (ks == NULL || len_bits <= 0) {
        return;
    }

    for (int i = 0; i < len_bits; i++) {
        ks[i] = (uint8_t)(lfsr & 0x1);
        const int bit = ((lfsr >> 1) ^ (lfsr >> 0)) & 1;
        lfsr = (lfsr >> 1) | (bit << 14);
    }
}

static void
nxdn_lfsr128_expand_iv_from_mi64(uint64_t mi, uint8_t out[16]) {
    if (out == NULL) {
        return;
    }

    DSD_MEMSET(out, 0, 16U);
    uint64_t lfsr = mi;
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)((lfsr >> (56 - (i * 8))) & 0xFFU);
    }

    int x = 64;
    for (int cnt = 0; cnt < 64; cnt++) {
        uint64_t bit = ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 1U;
        lfsr = (lfsr << 1) | bit;
        out[x / 8] = (uint8_t)((out[x / 8] << 1) | (uint8_t)bit);
        x++;
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

static void
nxdn_sdcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
    UNUSED(opts);

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
}

static void
nxdn_dcall_header(dsd_opts* opts, dsd_state* state, const uint8_t* Message, size_t message_bits) {
    UNUSED(opts);

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
    int aes_key_loaded = 0;
    if (state->payload_algid != 0) {
        if (state->payload_algid == 3) {
            aes_key_loaded = nxdn_load_data_aes_key(state, (uint8_t)state->payload_keyid, aes_key);
        } else if (state->keyloader == 1) {
            key = state->rkey_array[state->payload_keyid];
        } else {
            key = state->R;
        }

        DSD_FPRINTF(stderr, "\n Encrypted Data; Cipher: %d; Key ID: %02X;", state->payload_algid, state->payload_keyid);
        if (state->payload_algid > 1) {
            DSD_FPRINTF(stderr, " IV: %016llX;", state->payload_mi);
        }
    }

    if (state->payload_algid == 1 && key != 0ULL) {
        char key_text[16];
        DSD_FPRINTF(stderr, " Key: %s;",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, key, 5U));
        nxdn_pdu_scrambler_keystream_creation(ks, (int)(key & 0x7FFFU), ctx->total_bits);
    } else if (state->payload_algid == 2 && key != 0ULL) {
        char key_text[17];
        DSD_FPRINTF(stderr, " Key: %s;",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, key, 16U, 0));
        const int nblocks = (ctx->total_bytes + 7) / 8;
        uint8_t ks_bytes[NXDN_DCALL_MAX_BYTES];
        DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));
        des_ofb_keystream_output(state->payload_mi, key, ks_bytes, nblocks);
        unpack_byte_array_into_bit_array(ks_bytes, ks, nblocks * 8);
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
        unpack_byte_array_into_bit_array(ks_bytes, ks, nblocks * 16);
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
    DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].text_message,
                 sizeof(state->event_history_s[0].Event_History_Items[0].text_message), "%s", event_text);
    dsd_event_history_mark_dirty(&state->event_history_s[0]);
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
    unpack_byte_array_into_bit_array(reverse_bytes, reverse_bits, core_len);
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
    //6.5.2 Location ID
    uint8_t category_bit = location_id >> 22;
    uint32_t sys_code = 0;
    uint16_t site_code = 0;

    char category[14]; //G, R, or L

    if (category_bit == 0) {
        sys_code = ((location_id & 0x3FFFFF) >> 12); //10 bits
        site_code = location_id & 0x3FF;             //12 bits
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
    if (type == 0) {
        state->nxdn_last_ran = site_code % 64; //Table 6.3-4 RAN for Trunked Radio Systems
        if (site_code != 0) {
            state->nxdn_location_site_code = site_code;
        }
        if (sys_code != 0) {
            state->nxdn_location_sys_code = sys_code;
        }
        DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "%s", category);
        DSD_FPRINTF(stderr, "\n Location Information - Cat: %s - Sys Code: %d - Site Code %d ", category, sys_code,
                    site_code);
    } else {
        DSD_FPRINTF(stderr, "\n Adjacent Information - Cat: %s - Sys Code: %d - Site Code %d ", category, sys_code,
                    site_code);
    }
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
    } else {
        rc = dsd_tg_policy_evaluate_group_call(opts, state, target, source, 0, data_call, &decision);
        if (rc == 0 && allow_source_fallback && source != 0 && source != target
            && decision.match == DSD_TG_POLICY_MATCH_NONE) {
            dsd_tg_policy_decision source_decision;
            if (dsd_tg_policy_evaluate_group_call(opts, state, source, source, 0, data_call, &source_decision) == 0
                && source_decision.match != DSD_TG_POLICY_MATCH_NONE) {
                decision = source_decision;
            }
        }
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

static void
nxdn_vcall_assgn_adjust_duplicate(dsd_opts* opts, const dsd_state* state, time_t now,
                                  struct nxdn_vcall_assgn_info* info) {
    if (info->message_type == 0x05U && opts->trunk_is_tuned == 1 && opts->trunk_enable == 1
        && (now - state->last_vc_sync_time) > opts->trunk_hangtime) {
        info->message_type = 0x04U;
        opts->trunk_is_tuned = 0;
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
    };
    (void)dsd_recent_activity_publish(state, (uint8_t)dup, &observation, notice, 0U);
}

static int
nxdn_vcall_assgn_should_tune(const dsd_opts* opts, const struct nxdn_vcall_assgn_info* info) {
    if (info->message_type == 0x0DU || info->message_type == 0x0EU) {
        return (opts->trunk_tune_data_calls == 1) ? 1 : 0;
    }
    if (info->message_type != 0x04U) {
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
    return 1;
}

static void
nxdn_vcall_assgn_load_scrambler_key(const dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_assgn_info* info) {
    if (state->rkey_array[info->destination_id] != 0) {
        state->R = state->rkey_array[info->destination_id];
        DSD_FPRINTF(stderr, " %s", KYEL);
        char key_text[24];
        DSD_FPRINTF(stderr, " Key Loaded: %s",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys,
                                              state->rkey_array[info->destination_id], 0U));
        state->payload_miN = state->R;
    }
    if (state->M == 1) {
        state->nxdn_cipher_type = 0x1;
    }
}

static int
nxdn_vcall_assgn_can_tune(const dsd_opts* opts, const dsd_state* state, int policy_allowed, int hold_matches,
                          long int freq) {
    if (!opts || !state || opts->trunk_enable != 1 || !policy_allowed || state->p25_cc_freq == 0 || freq == 0) {
        return 0;
    }
    return (opts->trunk_is_tuned == 0 || hold_matches) ? 1 : 0;
}

static void
nxdn_vcall_assgn_apply_tune(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_assgn_info* info, long int freq) {
    nxdn_print_group_label(state, info->destination_id != 0U ? info->destination_id : info->source_unit_id);
    if (info->destination_id != 0U) {
        nxdn_print_group_label(state, info->source_unit_id);
    }
    const int is_private_call = (info->call_type == 4U) ? 1 : 0;
    const int data_call = (info->message_type == 0x0DU || info->message_type == 0x0EU) ? 1 : 0;
    const int hold_matches = (state->tg_hold != 0 && state->tg_hold == info->destination_id) ? 1 : 0;
    dsd_tg_policy_decision policy_decision;
    const int policy_allowed = nxdn_policy_tune_allowed(opts, state, info->destination_id, info->source_unit_id,
                                                        is_private_call, data_call, 1, &policy_decision);
    if (nxdn_vcall_assgn_can_tune(opts, state, policy_allowed, hold_matches, freq)) {
        if (!nxdn_vcall_assgn_setup_tuned_call(opts, state, freq)) {
            return;
        }
        nxdn_vcall_assgn_load_scrambler_key(opts, state, info);
    } else if (opts->trunk_enable == 1) {
        nxdn_policy_log_block(opts, is_private_call, info->destination_id, info->source_unit_id, &policy_decision);
    }
}

void
NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
    const time_t now = time(NULL);
    struct nxdn_vcall_assgn_info info;
    nxdn_vcall_assgn_parse(state, Message, &info);
    nxdn_vcall_assgn_print(state, &info);
    nxdn_vcall_assgn_adjust_duplicate(opts, state, now, &info);
    nxdn_vcall_assgn_track_active_channel(opts, state, &info);
    if (nxdn_vcall_assgn_should_tune(opts, &info)) {
        const long int freq = nxdn_vcall_assgn_frequency(opts, state, &info);
        nxdn_anchor_control_channel_from_current_tuner(opts, state, 1);
        nxdn_vcall_assgn_apply_tune(opts, state, &info, freq);
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
    if (state->trunk_lcn_freq[0] == 0 && freq1 != 0) {
        state->trunk_lcn_freq[0] = freq1;
        state->p25_cc_freq = freq1;
        state->trunk_cc_freq = freq1;
        state->lcn_freq_count = 1;
    }

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
NXDN_decode_srv_info(const dsd_opts* opts, dsd_state* state, const uint8_t* Message) {
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

    nxdn_anchor_control_channel_from_current_tuner(opts, state, 0);

    //clear stale active channel listing -- consider best placement for this (NXDN Type C Trunking -- inside SRV_INFO)
    if (dsd_recent_activity_expire(state, 0U, DSD_RECENT_ACTIVITY_TTL_MS) > 0) {
        state->nxdn_grant_chan = 0;
        state->nxdn_grant_freq = 0;
    }
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

static void
nxdn_vcall_load_aes_key(dsd_state* state, uint32_t key_index) {
    state->A1[0] = state->rkey_array[key_index + 0x000];
    state->A2[0] = state->rkey_array[key_index + 0x101];
    state->A3[0] = state->rkey_array[key_index + 0x201];
    state->A4[0] = state->rkey_array[key_index + 0x301];
    state->aes_key_loaded[0] =
        (state->A1[0] == 0 && state->A2[0] == 0 && state->A3[0] == 0 && state->A4[0] == 0) ? 0 : 1;

    for (int i = 0; i < 8; i++) {
        state->aes_key[i + 0] = (state->A1[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 8] = (state->A2[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 16] = (state->A3[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 24] = (state->A4[0] >> (56 - (i * 8))) & 0xFF;
    }
    state->R = state->A1[0];
}

static void
nxdn_vcall_load_key(const dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_info* info) {
    if (state->keyloader != 1) {
        return;
    }
    if (info->cipher_type == 1U && dsd_frame_sync_active_nxdn_variant(opts, state) == DSD_NXDN_VARIANT_48) {
        if (state->rkey_array[info->key_id] != 0) {
            state->R = state->rkey_array[info->key_id];
        } else if (state->rkey_array[info->destination_id] != 0) {
            state->R = state->rkey_array[info->destination_id];
        }
    } else if (info->cipher_type == 2U && state->rkey_array[info->key_id] != 0) {
        state->R = state->rkey_array[info->key_id];
    } else if (info->cipher_type == 3U) {
        uint32_t key_index = 0;
        if (state->rkey_array[info->key_id] != 0) {
            key_index = info->key_id;
        }
        nxdn_vcall_load_aes_key(state, key_index);
    }
}

static void
nxdn_vcall_print_cipher(const dsd_opts* opts, const dsd_state* state, const struct nxdn_vcall_info* info) {
    if (info->cipher_type != 0U && info->message_type == 0x01U) {
        DSD_FPRINTF(stderr, "\n  %s", KYEL);
        DSD_FPRINTF(stderr, "%s - ", NXDN_Cipher_Type_To_Str(info->cipher_type));
        DSD_FPRINTF(stderr, "Key ID %u - ", info->key_id & 0xFFU);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
    if (info->cipher_type == 0x01U && state->R > 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[16];
        DSD_FPRINTF(stderr, "Value: %s",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, state->R, 5U));
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
    if (info->cipher_type == 0x02U && state->R > 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[17];
        DSD_FPRINTF(stderr, "Value: %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->R, 16U, 0));
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
    if (info->cipher_type == 0x03U && state->R > 0) {
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

static void
nxdn_vcall_publish_crypto(dsd_opts* opts, dsd_state* state, uint8_t cipher_type, uint8_t key_id) {
    const dsd_call_crypto_update update = {
        .classification = cipher_type == 0U ? DSD_CALL_CRYPTO_CLEAR
                          : state->R != 0U  ? DSD_CALL_CRYPTO_DECRYPTABLE
                                            : DSD_CALL_CRYPTO_ENCRYPTED_PENDING,
        .algid = cipher_type,
        .kid = key_id,
        .mi = state->payload_miN,
        .audio_permitted = (uint8_t)(cipher_type == 0U || state->R != 0U),
    };
    if (dsd_call_state_update_crypto(state, 0U, &update) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

static void
nxdn_vcall_publish(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_info* info) {
    int protocol = DSD_SYNC_IS_NXDN(state->synctype) ? state->synctype : state->lastsynctype;
    if (!DSD_SYNC_IS_NXDN(protocol)) {
        protocol = DSD_SYNC_NXDN_POS;
    }
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = nxdn_vcall_kind(info->call_type),
        .ota_target_id = info->destination_id,
        .policy_target_id = info->destination_id,
        .ota_source_id = (info->voice_call_option & 0x0FU) < 4U ? info->source_unit_id : 0U,
        .channel = state->nxdn_grant_chan,
        .frequency_hz = state->trunk_vc_freq[0],
        .service_options = info->cc_option,
        .emergency = (uint8_t)((info->cc_option & 0x80U) != 0U),
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
    nxdn_vcall_publish_crypto(opts, state, info->cipher_type, info->key_id);
    dsd_event_sync_slot(opts, state, 0U);
}

static void
nxdn_vcall_apply_state(dsd_state* state, const struct nxdn_vcall_info* info) {
    if (info->message_type == 0x01U) {
        state->nxdn_key = info->key_id;
        state->nxdn_cipher_type = info->cipher_type;
    } else {
        DSD_SNPRINTF(state->generic_talker_alias[0], sizeof(state->generic_talker_alias[0]), "%s", "");
        nxdn_alias_reset(state);
    }

    state->dmr_encL = (state->nxdn_cipher_type != 0) ? 1 : 0;
    if (state->nxdn_cipher_type == 0 || state->R != 0) {
        state->dmr_encL = 0;
    }
}

static int
nxdn_vcall_lockout_label(dsd_state* state, uint16_t destination_id, char gm[8], char gn[50]) {
    dsd_tg_policy_entry lockout_entry;
    if (destination_id != 0 && dsd_tg_policy_lookup_label(state, destination_id, gm, 8, gn, 50)) {
        return 1;
    }
    if (dsd_tg_policy_make_exact_entry(destination_id, "DE", "ENC LO", DSD_TG_POLICY_SOURCE_ENC_LOCKOUT, &lockout_entry)
            == 0
        && dsd_tg_policy_upsert_exact(state, &lockout_entry, DSD_TG_POLICY_UPSERT_ADD_IF_MISSING) == 0) {
        DSD_SNPRINTF(gm, 8, "%s", "DE");
        DSD_SNPRINTF(gn, 50, "%s", "ENC LO");
        return 0;
    }
    return 1;
}

static void
nxdn_vcall_run_enc_lockout(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_info* info) {
    if (opts->trunk_enable != 1 || opts->trunk_tune_enc_calls != 0 || info->message_type != 0x01U
        || state->dmr_encL != 1) {
        return;
    }

    char gm[8] = {0};
    char gn[50] = {0};
    const int locked = nxdn_vcall_lockout_label(state, info->destination_id, gm, gn);
    if (info->destination_id != 0 && locked == 0) {
        DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].internal_str,
                     sizeof(state->event_history_s[0].Event_History_Items[0].internal_str),
                     "Target: %d; has been locked out; Encryption Lock Out Enabled.", info->destination_id);
        dsd_event_history_mark_dirty(&state->event_history_s[0]);
        watchdog_event_current(opts, state, 0);
    }

    uint8_t dbits[96];
    DSD_MEMSET(dbits, 0, sizeof(dbits));
    dbits[3] = 1;
    dbits[7] = 1;
    if ((strcmp(gm, "DE") == 0) && (strcmp(gn, "ENC LO") == 0)) {
        NXDN_Elements_Content_decode(opts, state, 1, dbits, sizeof(dbits));
    }
}

static void
nxdn_vcall_process(dsd_opts* opts, dsd_state* state, const struct nxdn_vcall_info* info) {
    if (info->message_type != 0x01U && dsd_call_state_end(state, 0U, 0.0) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
    nxdn_vcall_print_summary(state, info);
    nxdn_vcall_load_key(opts, state, info);
    nxdn_vcall_print_cipher(opts, state, info);
    nxdn_vcall_apply_state(state, info);
    if (info->message_type == 0x01U && state->NxdnElementsContent.VCallCrcIsGood != 0U) {
        nxdn_vcall_publish(opts, state, info);
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
nxdn_vcall_iv_load_aes_key(dsd_state* state) {
    state->A1[0] = state->rkey_array[state->nxdn_key + 0x000];
    state->A2[0] = state->rkey_array[state->nxdn_key + 0x101];
    state->A3[0] = state->rkey_array[state->nxdn_key + 0x201];
    state->A4[0] = state->rkey_array[state->nxdn_key + 0x301];
    state->aes_key_loaded[0] =
        (state->A1[0] == 0 && state->A2[0] == 0 && state->A3[0] == 0 && state->A4[0] == 0) ? 0 : 1;

    for (int i = 0; i < 8; i++) {
        state->aes_key[i + 0] = (state->A1[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 8] = (state->A2[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 16] = (state->A3[0] >> (56 - (i * 8))) & 0xFF;
        state->aes_key[i + 24] = (state->A4[0] >> (56 - (i * 8))) & 0xFF;
    }

    state->R = state->A1[0];
}

static void
nxdn_vcall_iv_prepare_cipher(dsd_state* state) {
    if (state->nxdn_cipher_type == 0x03 && state->keyloader == 1) {
        nxdn_vcall_iv_load_aes_key(state);
    }
    if (state->nxdn_cipher_type == 0x03) {
        LFSR128n(state);
    }
    if (state->nxdn_cipher_type == 0x02 && state->keyloader == 1 && state->rkey_array[state->nxdn_key] != 0) {
        state->R = state->rkey_array[state->nxdn_key];
    }
    if (state->nxdn_cipher_type == 0x02 && state->R != 0) {
        state->nxdn_new_iv = 1;
    }
    if (state->nxdn_cipher_type == 0x03 && state->aes_key_loaded[0] == 1) {
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
    nxdn_vcall_publish_crypto(opts, state, (uint8_t)state->nxdn_cipher_type, (uint8_t)state->nxdn_key);
}

struct nxdn_scch_info {
    time_t now;
    uint8_t direction;
    uint8_t sf;
    uint8_t opcode;
    uint8_t area;
    uint8_t rep1;
    uint8_t rep2;
    uint8_t sitet;
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
    info->sitet = (uint8_t)convert_bits_into_output(&Message[3], 5);
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
nxdn_scch_prepare_type_d(dsd_state* state, const struct nxdn_scch_info* info) {
    DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "Type-D");
    state->nxdn_last_ran = info->area;
    state->last_cc_sync_time = info->now;
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
}

static void
nxdn_scch_print_site_type(uint8_t sitet) {
    DSD_FPRINTF(stderr, "Site Type: %d ", sitet);
    if (sitet == 0U) {
        DSD_FPRINTF(stderr, "Reserved; ");
    } else if (sitet == 1U) {
        DSD_FPRINTF(stderr, "Wide; ");
    } else if (sitet == 2U) {
        DSD_FPRINTF(stderr, "Middle; ");
    } else {
        DSD_FPRINTF(stderr, "Narrow; ");
    }
}

static void
nxdn_scch_handle_site_id(dsd_state* state, const struct nxdn_scch_info* info) {
    DSD_FPRINTF(stderr, "Site ID Message - ");
    DSD_FPRINTF(stderr, "Area: %d; ", info->area);
    nxdn_scch_print_site_type(info->sitet);
    DSD_FPRINTF(stderr, "Site Code: %d ", info->rep2);
    if (info->rep2 == 0U || info->rep2 >= 251U) {
        DSD_FPRINTF(stderr, "Reserved; ");
    } else {
        DSD_FPRINTF(stderr, "Open Access; ");
    }
    state->nxdn_location_site_code = info->sitet;
    state->nxdn_location_sys_code = info->sitet;
    state->nxdn_last_ran = info->sitet;
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
    if (opts->trunk_enable == 1 && policy_allowed && state->p25_cc_freq != 0
        && ((info->now - state->last_vc_sync_time) > 1) && freq != 0) {
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq, 0, NULL);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            return;
        }
        DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
        DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
        state->lastsynctype = DSD_SYNC_NONE;
        if (state->rkey_array[info->id] != 0) {
            state->R = state->rkey_array[info->id];
        }
        if (state->M == 1) {
            state->nxdn_cipher_type = 0x1;
        }
    } else if (opts->trunk_enable == 1) {
        nxdn_policy_log_block(opts, is_private_call, info->id, 0, &policy_decision);
    }
}

static void
nxdn_scch_update_busy_display(dsd_state* state, const struct nxdn_scch_info* info) {
    if (info->rep1 == 31U) {
        DSD_FPRINTF(stderr, "\n%s ", KRED);
    } else {
        DSD_FPRINTF(stderr, "\n%s ", KGRN);
    }

    DSD_FPRINTF(stderr, " Channel Update - CH: %d - TGT: %d ", info->rep1, info->id);
    DSD_FPRINTF(stderr, "%s ", info->gu == 0U ? "Group Call" : "Private Call");
    if (info->rep1 == 31U) {
        DSD_FPRINTF(stderr, "Termination ");
    }
    if (info->rep1 != 0U && info->rep1 != 31U) {
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

static void
nxdn_scch_enrich_identity(dsd_state* state, uint32_t source_id, uint32_t target_id) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_NXDN(call.protocol)) {
        return;
    }
    const dsd_call_observation observation = {
        .protocol = call.protocol,
        .slot = 0U,
        .kind = call.kind,
        .ota_target_id = target_id,
        .policy_target_id = target_id,
        .ota_source_id = source_id,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
}

static void
nxdn_scch_handle_busy(dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    DSD_FPRINTF(stderr, "%s", (info->gu && info->rep1 == 0U) ? "REG_COMM; " : "Busy Repeater Message - ");
    DSD_FPRINTF(stderr, "Area: %d; ", info->area);
    DSD_FPRINTF(stderr, "Go to Repeater: %d; ", info->rep1);
    DSD_FPRINTF(stderr, "Home Repeater: %d; ", info->rep2);
    nxdn_scch_update_busy_display(state, info);
    nxdn_scch_apply_busy_tune(opts, state, info);
}

static void
nxdn_scch_handle_info4(dsd_opts* opts, dsd_state* state, const struct nxdn_scch_info* info) {
    (void)dsd_recent_activity_expire(state, 0U, DSD_RECENT_ACTIVITY_TTL_MS);

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
        nxdn_scch_handle_busy(opts, state, info);
    }
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
            nxdn_scch_enrich_identity(state, info->id, 0U);
        }
    }
}

static void
nxdn_scch_handle_info2(dsd_state* state, const struct nxdn_scch_info* info) {
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
        if (info->now - state->last_vc_sync_time < 1) {
            nxdn_scch_enrich_identity(state, 0U, info->id);
        }
    }
}

static void
nxdn_scch_handle_info1(dsd_state* state, const struct nxdn_scch_info* info) {
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
            state->nxdn_cipher_type = info->cipher;
            state->nxdn_key = info->key_id;
        }
    } else {
        DSD_FPRINTF(stderr, "\n%s ", KYEL);
        DSD_FPRINTF(stderr, "Call IV B: %04llX; ", info->iv_b);
        DSD_FPRINTF(stderr, "Call IV C: %04llX; ", info->iv_c);
        state->payload_miN = state->payload_miN | (info->iv_c << 6);
        state->payload_miN = state->payload_miN | info->iv_b;
        DSD_FPRINTF(stderr, "Completed IV: %016llX", state->payload_miN);
    }
}

//SCCH messages have a unique format that can be used in a super frame, but each 'unit'
//can also (mostly) be decoded seperately, except an enc IV
void
NXDN_decode_scch(dsd_opts* opts, dsd_state* state, const uint8_t* Message, uint8_t direction) {
    const time_t now = time(NULL);
    struct nxdn_scch_info info;
    nxdn_scch_parse(Message, direction, now, &info);

    DSD_FPRINTF(stderr, "\n "); //initial line break
    nxdn_scch_print_payload_label(opts, &info);
    nxdn_scch_prepare_type_d(state, &info);

    if (info.opcode == 0x04U || info.opcode == 0x00U) {
        nxdn_scch_handle_info4(opts, state, &info);
    }
    if (info.opcode == 0x05U || info.opcode == 0x01U) {
        nxdn_scch_handle_info3(state, &info);
    }
    if (info.opcode == 0x06U || info.opcode == 0x02U) {
        nxdn_scch_handle_info2(state, &info);
    }
    if (info.opcode == 0x07U || info.opcode == 0x03U) {
        nxdn_scch_handle_info1(state, &info);
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

static uint8_t
nxdn_alias_crc_ok(const dsd_state* state) {
    if (state == NULL) {
        return 0U;
    }

    /* FACCH1/SACCH superframe CRC drives alias acceptance when available. */
    if (!state->nxdn_sacch_non_superframe) {
        return (uint8_t)((state->NxdnElementsContent.VCallCrcIsGood != 0U) ? 1U : 0U);
    }

    /* Standalone SACCH frames do not carry the same assembled CRC context. */
    return 1U;
}
