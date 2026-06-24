// SPDX-License-Identifier: GPL-3.0-or-later
//NXDN descramble/deperm/depuncture and crc/utility functions
//Reworked portions from Osmocom OP25

/* -*- c++ -*- */
/*
 * NXDN Encoder/Decoder (C) Copyright 2019 Max H. Parke KA1RBI
 *
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/trellis.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/nxdn/nxdn_alias_decode.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_convolution.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"

int load_i(const uint8_t val[], int len);
uint8_t crc6(const uint8_t buf[], int len);
uint16_t crc12f(const uint8_t buf[], int len);
uint16_t crc15(const uint8_t buf[], int len);
uint16_t crc16cac(const uint8_t buf[], int len);
uint8_t crc7_scch(const uint8_t bits[], int len);
uint32_t nxdn_message_crc32(const uint8_t* input, int len);

static int
nxdn_dcr_is_sb0_message_type(uint8_t message_type) {
    // SACCH2 sf_mes 0x01 identifies call (SB0) traffic; other values are PDU/End/Idle.
    return message_type == 0x01U;
}

static void
nxdn_unpack_bytes_msb(const uint8_t* bytes, size_t byte_count, uint8_t* bits) {
    for (size_t i = 0; i < byte_count; i++) {
        const uint8_t value = bytes[i];
        for (size_t bit = 0; bit < 8U; bit++) {
            bits[(i * 8U) + bit] = (value >> (7U - bit)) & 1U;
        }
    }
}

static void
nxdn_pack_bits_msb(const uint8_t* bits, size_t byte_count, uint8_t* bytes) {
    for (size_t i = 0; i < byte_count; i++) {
        bytes[i] = (uint8_t)ConvertBitIntoBytes(&bits[i * 8U], 8);
    }
}

static uint16_t
nxdn_bits_to_u16(const uint8_t* bits, int len) {
    return (uint16_t)convert_bits_into_output(bits, len);
}

static int
nxdn_sacch_part_of_frame(uint8_t sf) {
    switch (sf) {
        case 2: return 1;
        case 1: return 2;
        case 0: return 3;
        default: return 0;
    }
}

static int
nxdn_ran_from_trellis(const uint8_t* trellis_buf) {
    return (trellis_buf[2] << 5) | (trellis_buf[3] << 4) | (trellis_buf[4] << 3) | (trellis_buf[5] << 2)
           | (trellis_buf[6] << 1) | trellis_buf[7];
}

static void
nxdn_reset_payload_seed_if_forced(dsd_state* state) {
    if ((state->nxdn_cipher_type == 1 || state->M == 1) && state->R != 0) {
        state->payload_miN = state->R;
    }
}

static void
nxdn_advance_payload_seed_for_part(dsd_state* state, int part_of_frame) {
    const char ambe_temp[49] = {0};
    char ambe_d[49] = {0};

    for (int start = 0; start < part_of_frame; start++) {
        LFSRN(ambe_temp, ambe_d, state);
        LFSRN(ambe_temp, ambe_d, state);
        LFSRN(ambe_temp, ambe_d, state);
        LFSRN(ambe_temp, ambe_d, state);
    }
}

static void
nxdn_prepare_sacch_payload_seed(dsd_state* state, int part_of_frame) {
    if (part_of_frame == 0) {
        nxdn_reset_payload_seed_if_forced(state);
    }
    if (state->nxdn_cipher_type == 0x1) {
        nxdn_reset_payload_seed_if_forced(state);
        if (part_of_frame != 0) {
            nxdn_advance_payload_seed_for_part(state, part_of_frame);
        }
    }
}

static void
nxdn_depermute_rel(const uint8_t* input, const uint8_t* reliab, size_t len, const uint16_t* perm, uint8_t* deperm,
                   uint8_t* deperm_rel) {
    for (size_t i = 0; i < len; i++) {
        deperm[perm[i]] = input[i];
        deperm_rel[perm[i]] = reliab[i];
    }
}

static void
nxdn_depermute_rel_u8(const uint8_t* input, const uint8_t* reliab, size_t len, const uint8_t* perm, uint8_t* deperm,
                      uint8_t* deperm_rel) {
    for (size_t i = 0; i < len; i++) {
        deperm[perm[i]] = input[i];
        deperm_rel[perm[i]] = reliab[i];
    }
}

static void
nxdn_depuncture_12_5_rel(const uint8_t* deperm, const uint8_t* deperm_rel, uint8_t* depunc, uint8_t* depunc_rel) {
    size_t out = 0;
    static const uint8_t map[] = {0, 1, 2, 3, 4, 0xFFU, 5, 6, 7, 8, 9, 0xFFU};

    for (size_t p = 0; p < 60U; p += 10U) {
        for (size_t i = 0; i < sizeof(map); i++) {
            if (map[i] == 0xFFU) {
                depunc[out] = 0;
                depunc_rel[out++] = 0;
            } else {
                depunc[out] = deperm[p + map[i]];
                depunc_rel[out++] = deperm_rel[p + map[i]];
            }
        }
    }
}

static void
nxdn_depuncture_16_9_rel(const uint8_t* deperm, const uint8_t* deperm_rel, uint8_t* depunc, uint8_t* depunc_rel) {
    size_t out = 0;

    for (size_t i = 0; i < 144U; i += 3U) {
        depunc[out] = deperm[i + 0U];
        depunc_rel[out++] = deperm_rel[i + 0U];
        depunc[out] = 0;
        depunc_rel[out++] = 0;
        depunc[out] = deperm[i + 1U];
        depunc_rel[out++] = deperm_rel[i + 1U];
        depunc[out] = deperm[i + 2U];
        depunc_rel[out++] = deperm_rel[i + 2U];
    }
}

static void
nxdn_depuncture_12_group_rel(const uint8_t* deperm, const uint8_t* deperm_rel, size_t groups, uint8_t* depunc,
                             uint8_t* depunc_rel) {
    size_t out = 0;
    static const uint8_t map[] = {0, 1, 2, 0xFFU, 3, 4, 5, 6, 7, 8, 9, 0xFFU, 10, 11};

    for (size_t group = 0; group < groups; group++) {
        const size_t base = group * 12U;
        for (size_t i = 0; i < sizeof(map); i++) {
            if (map[i] == 0xFFU) {
                depunc[out] = 0;
                depunc_rel[out++] = 0;
            } else {
                depunc[out] = deperm[base + map[i]];
                depunc_rel[out++] = deperm_rel[base + map[i]];
            }
        }
    }
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_nxdn_test_depermute_12_5(const uint8_t input[60], const uint8_t reliab[60], uint8_t deperm[60],
                                 uint8_t deperm_rel[60]) {
    nxdn_depermute_rel_u8(input, reliab, 60U, PERM_12_5, deperm, deperm_rel);
}

void
dsd_neo_nxdn_test_depermute_16_9(const uint8_t input[144], const uint8_t reliab[144], uint8_t deperm[144],
                                 uint8_t deperm_rel[144]) {
    nxdn_depermute_rel_u8(input, reliab, 144U, PERM_16_9, deperm, deperm_rel);
}

void
dsd_neo_nxdn_test_depermute_12_25(const uint8_t input[300], const uint8_t reliab[300], uint8_t deperm[300],
                                  uint8_t deperm_rel[300]) {
    nxdn_depermute_rel(input, reliab, 300U, PERM_12_25, deperm, deperm_rel);
}

void
dsd_neo_nxdn_test_depermute_12_29(const uint8_t input[348], const uint8_t reliab[348], uint8_t deperm[348],
                                  uint8_t deperm_rel[348]) {
    nxdn_depermute_rel(input, reliab, 348U, PERM_12_29, deperm, deperm_rel);
}

void
dsd_neo_nxdn_test_depuncture_12_5(const uint8_t deperm[60], const uint8_t deperm_rel[60], uint8_t depunc[72],
                                  uint8_t depunc_rel[72]) {
    nxdn_depuncture_12_5_rel(deperm, deperm_rel, depunc, depunc_rel);
}

void
dsd_neo_nxdn_test_depuncture_16_9(const uint8_t deperm[144], const uint8_t deperm_rel[144], uint8_t depunc[192],
                                  uint8_t depunc_rel[192]) {
    nxdn_depuncture_16_9_rel(deperm, deperm_rel, depunc, depunc_rel);
}

void
dsd_neo_nxdn_test_depuncture_12_group(const uint8_t* deperm, const uint8_t* deperm_rel, size_t groups, uint8_t* depunc,
                                      uint8_t* depunc_rel) {
    nxdn_depuncture_12_group_rel(deperm, deperm_rel, groups, depunc, depunc_rel);
}

int
dsd_neo_nxdn_test_dcr_is_sb0_message_type(uint8_t message_type) {
    return nxdn_dcr_is_sb0_message_type(message_type);
}

void
dsd_neo_nxdn_test_unpack_bytes_msb(const uint8_t* bytes, size_t byte_count, uint8_t* bits) {
    nxdn_unpack_bytes_msb(bytes, byte_count, bits);
}

void
dsd_neo_nxdn_test_pack_bits_msb(const uint8_t* bits, size_t byte_count, uint8_t* bytes) {
    nxdn_pack_bits_msb(bits, byte_count, bytes);
}

uint16_t
dsd_neo_nxdn_test_bits_to_u16(const uint8_t* bits, int len) {
    return nxdn_bits_to_u16(bits, len);
}

int
dsd_neo_nxdn_test_sacch_part_of_frame(uint8_t sf) {
    return nxdn_sacch_part_of_frame(sf);
}

int
dsd_neo_nxdn_test_ran_from_trellis(const uint8_t* trellis_buf) {
    return nxdn_ran_from_trellis(trellis_buf);
}

void
dsd_neo_nxdn_test_reset_payload_seed_if_forced(dsd_state* state) {
    nxdn_reset_payload_seed_if_forced(state);
}

void
dsd_neo_nxdn_test_prepare_sacch_payload_seed(dsd_state* state, int part_of_frame) {
    nxdn_prepare_sacch_payload_seed(state, part_of_frame);
}
#endif

static void
nxdn_conv_decode_soft(const uint8_t* depunc, const uint8_t* depunc_rel, size_t depunc_len, uint8_t* m_data,
                      int chainback_bits) {
    CNXDNConvolution_start();
    for (size_t i = 0; i < depunc_len / 2U; i++) {
        const uint8_t s0 = depunc[i * 2U] << 1;
        const uint8_t s1 = depunc[(i * 2U) + 1U] << 1;
        const uint8_t r0 = depunc_rel[i * 2U];
        const uint8_t r1 = depunc_rel[(i * 2U) + 1U];
        CNXDNConvolution_decode_soft(s0, s1, r0, r1);
    }
    CNXDNConvolution_chainback(m_data, chainback_bits);
}

static void
nxdn_hard_fallback_decode(uint8_t* trellis_buf, size_t trellis_size, uint8_t* m_data, size_t m_data_bytes,
                          const uint8_t* depunc, int chainback_bits) {
    DSD_MEMSET(trellis_buf, 0, trellis_size);
    DSD_MEMSET(m_data, 0, m_data_bytes);
    trellis_decode(trellis_buf, depunc, chainback_bits);
    nxdn_pack_bits_msb(trellis_buf, m_data_bytes, m_data);
}

static void
nxdn_print_last_ran(const dsd_state* state) {
    if (state->nxdn_last_ran != (unsigned int)-1) {
        DSD_FPRINTF(stderr, " RAN %02d ", state->nxdn_last_ran);
    } else {
        DSD_FPRINTF(stderr, "        ");
    }
}

static void
nxdn_print_colored_last_ran(const dsd_state* state) {
    DSD_FPRINTF(stderr, "%s", KCYN);
    nxdn_print_last_ran(state);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
nxdn_print_crc_error_colored(void) {
    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, " (CRC ERR)");
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
nxdn_print_sacch_payload(const dsd_opts* opts, const char* label, const uint8_t* m_data, uint8_t crc, uint8_t check) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n%s", label);
    for (int i = 0; i < 4; i++) {
        DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
    }
    if (crc != check) {
        nxdn_print_crc_error_colored();
    }
}

static void
nxdn_reset_sacch_segments(dsd_state* state) {
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
}

static void
nxdn_handle_sacch_non_superframe(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                                 uint8_t crc, uint8_t check) {
    nxdn_print_last_ran(state);

    uint8_t nsf_sacch[26];
    DSD_MEMSET(nsf_sacch, 0, sizeof(nsf_sacch));
    DSD_MEMCPY(nsf_sacch, trellis_buf + 8, 24U);

    const int sacch_crc_ok = (crc == check);
    if (sacch_crc_ok) {
        state->nxdn_last_ran = nxdn_ran_from_trellis(trellis_buf);
        state->nxdn_part_of_frame = 3;
        DSD_FPRINTF(stderr, "PF 1/1");
        nxdn_reset_payload_seed_if_forced(state);
        NXDN_Elements_Content_decode(opts, state, 1, nsf_sacch, sizeof(nsf_sacch));
    } else {
        state->nxdn_part_of_frame = 0;
        DSD_FPRINTF(stderr, "PF X/1");
        nxdn_reset_payload_seed_if_forced(state);
        DSD_FPRINTF(stderr, " IDLE");
    }

    nxdn_print_sacch_payload(opts, " SACCH NSF ", m_data, crc, check);
    nxdn_reset_sacch_segments(state);
}

static void
nxdn_handle_sacch_superframe(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                             uint8_t crc, uint8_t check) {
    const uint8_t sf = (trellis_buf[0] << 1) | trellis_buf[1];
    const int part_of_frame = nxdn_sacch_part_of_frame(sf);
    const int valid_sequence =
        nxdn_sacch_segment_sequence_is_valid((uint8_t)(crc == check), state->nxdn_part_of_frame, part_of_frame);

    if (!valid_sequence) {
        nxdn_reset_sacch_segments(state);
    }

    state->nxdn_part_of_frame = part_of_frame;
    nxdn_print_colored_last_ran(state);
    if (valid_sequence) {
        DSD_FPRINTF(stderr, "PF %d/4", part_of_frame + 1);
    } else {
        DSD_FPRINTF(stderr, "PF X/4");
    }
    nxdn_prepare_sacch_payload_seed(state, part_of_frame);

    if (crc == check) {
        const int ran = nxdn_ran_from_trellis(trellis_buf);
        state->nxdn_ran = state->nxdn_last_ran = ran;
        state->nxdn_sf = sf;
        state->nxdn_part_of_frame = part_of_frame;
        state->nxdn_sacch_frame_segcrc[part_of_frame] = 0;
    } else {
        state->nxdn_sacch_frame_segcrc[part_of_frame] = 1;
    }

    for (int i = 0; i < 18; i++) {
        state->nxdn_sacch_frame_segment[part_of_frame][i] = trellis_buf[i + 8];
    }

    if (part_of_frame == 3) {
        NXDN_SACCH_Full_decode(opts, state);
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n SACCH SF Segment #%d ", part_of_frame + 1);
        for (int i = 0; i < 4; i++) {
            DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
        }
        if (crc != check) {
            DSD_FPRINTF(stderr, " CRC ERR - %02X %02X", crc, check);
        }
    }
}

static void
nxdn_handle_sacch(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data, uint8_t crc,
                  uint8_t check) {
    if (crc != check) {
        nxdn_reset_sacch_segments(state);
    }

    if (state->nxdn_sacch_non_superframe == 1) {
        nxdn_handle_sacch_non_superframe(opts, state, trellis_buf, m_data, crc, check);
    } else if (state->nxdn_sacch_non_superframe == 0) {
        nxdn_handle_sacch_superframe(opts, state, trellis_buf, m_data, crc, check);
    }
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_nxdn_test_sacch_state_update(dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[32],
                                     const uint8_t m_data[5], uint8_t crc, uint8_t check) {
    nxdn_handle_sacch(opts, state, trellis_buf, m_data, crc, check);
}
#endif

static void
nxdn_print_pich_tch_name(uint8_t lich) {
    if (lich == 0x08U) {
        DSD_FPRINTF(stderr, " PICH Payload ");
    } else {
        DSD_FPRINTF(stderr, " TCH Payload ");
    }
}

static void
nxdn_print_dcr_source_target(uint16_t source, uint16_t target, uint8_t gi) {
    DSD_FPRINTF(stderr, "\n ");
    DSD_FPRINTF(stderr, "Source: %d; Target: %d; ", source, target);
    DSD_FPRINTF(stderr, "%s; ", gi ? "Private" : "Group");
}

static void
nxdn_handle_dcr_csm_alias(const dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf) {
    char csm_alias[32];
    DSD_MEMSET(csm_alias, 0, sizeof(csm_alias));

    if (nxdn_dcr_decode_csm_alias(trellis_buf, csm_alias, sizeof(csm_alias))) {
        DSD_FPRINTF(stderr, "\n Call Sign Memory: %s; ", csm_alias + 4);
        DSD_SNPRINTF(state->generic_talker_alias[0], sizeof(state->generic_talker_alias[0]), "%s", csm_alias);
        if (state->event_history_s != NULL) {
            DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].alias,
                         sizeof(state->event_history_s[0].Event_History_Items[0].alias), "%s; ", csm_alias);
        }
    } else if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n Call Sign Memory: decode error; ");
    }
}

static void
nxdn_handle_dcr_opcode_payload(const uint8_t* trellis_buf, uint8_t opcode, uint8_t gi, uint16_t source,
                               uint16_t target) {
    if (opcode == 0x0FU) {
        nxdn_print_dcr_source_target(source, target, gi);
        DSD_FPRINTF(stderr, "Data Preamble; ");
        const uint8_t countdown = (uint8_t)ConvertBitIntoBytes(&trellis_buf[64], 8);
        DSD_FPRINTF(stderr, "Countdown: %d; ", countdown);
    }

    if (opcode == 0x32U) {
        nxdn_print_dcr_source_target(source, target, gi);
        DSD_FPRINTF(stderr, "Precoded Message; ");
        const uint8_t idx = (uint8_t)ConvertBitIntoBytes(&trellis_buf[64], 8);
        DSD_FPRINTF(stderr, "Index#: %d;", idx);
    }
}

static void
nxdn_handle_pich_tch_crc_ok(const dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, uint8_t lich) {
    const uint8_t opcode = (uint8_t)ConvertBitIntoBytes(&trellis_buf[0], 8);
    const uint8_t gi = trellis_buf[16];
    const uint16_t source = (uint16_t)ConvertBitIntoBytes(&trellis_buf[24], 16);
    const uint16_t target = (uint16_t)ConvertBitIntoBytes(&trellis_buf[40], 16);
    const int is_dcr_sb0 = (lich == 0x08U) && nxdn_dcr_is_sb0_message_type(state->nxdn_dcr_sf_message_type);

    if (is_dcr_sb0) {
        nxdn_handle_dcr_csm_alias(opts, state, trellis_buf);
    } else {
        nxdn_handle_dcr_opcode_payload(trellis_buf, opcode, gi, source, target);
    }
}

static void
nxdn_print_pich_tch_crc_error(uint8_t lich) {
    DSD_FPRINTF(stderr, "\n ");
    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, "%s", (lich == 0x08U) ? "PICH (CRC ERR)" : "TCH (CRC ERR)");
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
nxdn_print_pich_tch_payload(const dsd_opts* opts, const uint8_t* m_data, uint16_t crc, uint16_t check, uint8_t lich) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    nxdn_print_pich_tch_name(lich);
    for (int i = 0; i < 12; i++) {
        DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
    }
    if (crc != check) {
        nxdn_print_crc_error_colored();
    }
}

static void
nxdn_handle_pich_tch(const dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                     uint16_t crc, uint16_t check, uint8_t lich) {
    if (crc == check) {
        nxdn_handle_pich_tch_crc_ok(opts, state, trellis_buf, lich);
    } else if (opts->payload == 0) {
        nxdn_print_pich_tch_crc_error(lich);
    }

    nxdn_print_pich_tch_payload(opts, m_data, crc, check, lich);
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_nxdn_test_pich_tch_state_update(const dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[96],
                                        const uint8_t m_data[12], uint16_t crc, uint16_t check, uint8_t lich) {
    nxdn_handle_pich_tch(opts, state, trellis_buf, m_data, crc, check, lich);
}
#endif

static void
nxdn_print_facch2_udch_name(uint8_t type) {
    if (type == 0) {
        DSD_FPRINTF(stderr, " UDCH");
    }
    if (type == 1) {
        DSD_FPRINTF(stderr, " FACCH2");
    }
}

static void
nxdn_print_udch_data(const uint8_t* m_data) {
    DSD_FPRINTF(stderr, "\n UDCH Data: ");
    for (int i = 0; i < 24; i++) {
        DSD_FPRINTF(stderr, "%02X", m_data[i]);
    }

    DSD_FPRINTF(stderr, "\n UDCH Data: ASCII - ");
    for (int i = 0; i < 24; i++) {
        if (m_data[i] <= 0x7E && m_data[i] >= 0x20) {
            DSD_FPRINTF(stderr, "%c", m_data[i]);
        } else {
            DSD_FPRINTF(stderr, " ");
        }
    }
}

static void
nxdn_print_facch2_udch_payload(const dsd_opts* opts, const uint8_t* m_data, uint16_t crc, uint16_t check,
                               uint8_t type) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    nxdn_print_facch2_udch_name(type);
    DSD_FPRINTF(stderr, " Payload\n  ");
    for (int i = 0; i < 26; i++) {
        if (i == 13) {
            DSD_FPRINTF(stderr, "\n  ");
        }
        DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
    }
    if (crc != check) {
        nxdn_print_crc_error_colored();
    }
}

static void
nxdn_handle_facch2_udch(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                        uint16_t crc, uint16_t check, uint8_t type) {
    const uint8_t sf = (trellis_buf[0] << 1) | trellis_buf[1];
    const int ran = nxdn_ran_from_trellis(trellis_buf);

    if (crc == check) {
        state->nxdn_last_ran = (unsigned int)ran;
        nxdn_print_last_ran(state);
        state->nxdn_part_of_frame = 3 - sf;
    } else {
        DSD_FPRINTF(stderr, "        ");
        state->nxdn_part_of_frame = 0;
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    nxdn_print_facch2_udch_name(type);
    DSD_FPRINTF(stderr, "%s", KNRM);

    uint8_t f2u_message_buffer[400];
    DSD_MEMSET(f2u_message_buffer, 0, sizeof(f2u_message_buffer));
    for (int i = 0; i < 199 - 8 - 15; i++) {
        f2u_message_buffer[i] = trellis_buf[i + 8];
    }

    if (crc == check) {
        state->data_header_format[0] = 1;
        NXDN_Elements_Content_decode(opts, state, 1, f2u_message_buffer, (size_t)(199 - 8 - 15));
    }
    if (type == 0 && crc == check) {
        nxdn_print_udch_data(m_data);
    }

    nxdn_print_facch2_udch_payload(opts, m_data, crc, check, type);
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_nxdn_test_facch2_udch_state_update(dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[208],
                                           const uint8_t m_data[26], uint16_t crc, uint16_t check, uint8_t type) {
    nxdn_handle_facch2_udch(opts, state, trellis_buf, m_data, crc, check, type);
}
#endif

static int cac_fail = 0;

static void
nxdn_reset_after_cac_fail(dsd_state* state) {
    state->synctype = DSD_SYNC_P25P1_POS;
    state->lastsynctype = DSD_SYNC_NONE;
    state->carrier = 0;
    state->last_cc_sync_time = time(NULL) + 2;
    cac_fail = 0;

    state->center = 0.0f;
    state->jitter = -1;
    state->synctype = DSD_SYNC_NONE;
    state->min = -4.0f;
    state->max = 4.0f;
    state->lmid = 0.0f;
    state->umid = 0.0f;
    state->minref = -3.2f;
    state->maxref = 3.2f;
    state->lastsample = 0.0f;
    for (int i = 0; i < 128; i++) {
        state->sbuf[i] = 0.0f;
    }
    state->sidx = 0;
    for (int i = 0; i < 1024; i++) {
        state->maxbuf[i] = 4.0f;
    }
    for (int i = 0; i < 1024; i++) {
        state->minbuf[i] = -4.0f;
    }
    state->midx = 0;
    dsd_state_invalidate_minmax_sums(state);
    state->symbolcnt = 0;
    state->dibit_buf_p = state->dibit_buf + 200;
    DSD_MEMSET(state->dibit_buf, 0, sizeof(int) * 200);
    state->offset = 0;

    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    state->data_header_blocks[0] = 1;
    state->data_header_padding[0] = 0;
    state->data_header_format[0] = 0;
    state->data_header_valid[0] = 0;
    state->payload_algid = 0;
    state->payload_keyid = 0;
    state->payload_mi = 0;
    DSD_MEMSET(state->aes_ivR, 0, sizeof(state->aes_ivR));
}

static void
nxdn_update_cac_fail_state(dsd_state* state, uint16_t crc) {
    if (crc != 0) {
        nxdn_print_crc_error_colored();
        cac_fail++;
    } else {
        cac_fail = 0;
    }

    if (cac_fail > 10) {
        nxdn_reset_after_cac_fail(state);
    }
}

static void
nxdn_print_cac_payload(const dsd_opts* opts, const uint8_t* m_data) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " CAC Payload\n  ");
    for (int i = 0; i < 22; i++) {
        DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
        if (i == 10) {
            DSD_FPRINTF(stderr, "\n  ");
        }
    }
}

static void
nxdn_handle_cac(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data, uint16_t crc) {
    uint8_t cac_message_buffer[147];
    DSD_MEMSET(cac_message_buffer, 0, sizeof(cac_message_buffer));
    for (int i = 0; i < 147; i++) {
        cac_message_buffer[i] = trellis_buf[i + 8];
    }

    nxdn_print_last_ran(state);
    if (crc == 0) {
        state->data_header_format[0] = 2;
        state->nxdn_last_ran = nxdn_ran_from_trellis(trellis_buf);
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " CAC");
    DSD_FPRINTF(stderr, "%s", KNRM);
    nxdn_update_cac_fail_state(state, crc);

    if (crc == 0) {
        NXDN_Elements_Content_decode(opts, state, 1, cac_message_buffer, sizeof(cac_message_buffer));
    }
    nxdn_print_cac_payload(opts, m_data);
    rotate_symbol_out_file(opts, state);
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_nxdn_test_cac_state_update(dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[176],
                                   const uint8_t m_data[22], uint16_t crc) {
    nxdn_handle_cac(opts, state, trellis_buf, m_data, crc);
}
#endif

struct nxdn_sacch2_fields {
    uint8_t sf_fb;
    uint8_t sf_num;
    uint8_t sf_mes;
    uint8_t sf_pof;
    uint8_t crc;
    uint8_t check;
};

static int
nxdn_sacch2_crc_ok(const struct nxdn_sacch2_fields* fields) {
    return fields->crc == fields->check;
}

static struct nxdn_sacch2_fields
nxdn_sacch2_fields_from_trellis(const uint8_t* trellis_buf, uint8_t crc, uint8_t check) {
    struct nxdn_sacch2_fields fields;
    fields.sf_fb = trellis_buf[0];
    fields.sf_num = (uint8_t)nxdn_bits_to_u16(trellis_buf + 1, 2);
    fields.sf_mes = (uint8_t)nxdn_bits_to_u16(trellis_buf + 3, 5);
    fields.sf_pof = 3U - fields.sf_num;
    fields.crc = crc;
    fields.check = check;
    return fields;
}

static void
nxdn_print_sacch2_message_label(uint8_t sf_mes) {
    if (sf_mes == 0x01) {
        DSD_FPRINTF(stderr, "Call; ");
    } else if (sf_mes == 0x02) {
        DSD_FPRINTF(stderr, "PDU;  ");
    } else if (sf_mes == 0x1E) {
        DSD_FPRINTF(stderr, "End;  ");
    } else if (sf_mes == 0x00) {
        DSD_FPRINTF(stderr, "Idle; ");
    } else {
        DSD_FPRINTF(stderr, "Res: %02X; ", sf_mes);
    }
}

static void
nxdn_print_sacch2_header(dsd_state* state, const struct nxdn_sacch2_fields* fields) {
    state->nxdn_dcr_sf_message_type = 0xFFU;
    if (!nxdn_sacch2_crc_ok(fields)) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "SACCH (CRC ERR)");
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }

    state->nxdn_dcr_sf_message_type = fields->sf_mes;
    if (fields->sf_fb && fields->sf_pof) {
        DSD_FPRINTF(stderr, "PF: %d/1; ", fields->sf_num + 1);
    } else {
        DSD_FPRINTF(stderr, "PF: %d/4; ", fields->sf_pof + 1);
    }
    nxdn_print_sacch2_message_label(fields->sf_mes);
}

static uint8_t
nxdn_update_sacch2_segment_crc(dsd_state* state, const struct nxdn_sacch2_fields* fields) {
    state->nxdn_sacch_frame_segcrc[fields->sf_num] = nxdn_sacch2_crc_ok(fields) ? 0 : 1;

    uint8_t crc_sf_check = 0;
    for (int i = 0; i < 4; i++) {
        crc_sf_check += state->nxdn_sacch_frame_segcrc[i];
    }
    return crc_sf_check;
}

static void
nxdn_store_sacch2_frame(dsd_state* state, const uint8_t* trellis_buf, const struct nxdn_sacch2_fields* fields) {
    const int sf_size = 18;
    const int bf_idx = 8;
    const int sf_idx = sf_size * fields->sf_pof;

    if (fields->sf_fb && fields->sf_pof) {
        DSD_MEMCPY(state->dmr_pdu_sf[0], trellis_buf + bf_idx, (size_t)sf_size * sizeof(uint8_t));
    } else {
        DSD_MEMCPY(state->dmr_pdu_sf[0] + sf_idx, trellis_buf + bf_idx, (size_t)sf_size * sizeof(uint8_t));
    }
}

static void
nxdn_update_sacch2_identity_state(dsd_state* state, const struct nxdn_sacch2_fields* fields) {
    if (fields->sf_fb && state->M == 1) {
        state->payload_miN = 0;
    }
    if (!nxdn_sacch2_crc_ok(fields)) {
        return;
    }

    state->gi[0] = 0;
    state->nxdn_last_ran = 7;
    state->nxdn_last_tg = 777;
    state->nxdn_last_rid = 777;
    DSD_SNPRINTF(state->generic_talker_alias[0], sizeof(state->generic_talker_alias[0]), "%s", "JPN DCR");
    DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].alias,
                 sizeof(state->event_history_s[0].Event_History_Items[0].alias), "%s; ", "JPN DCR");
    if (fields->sf_fb) {
        state->payload_miN = 0;
    }
}

static void
nxdn_print_sacch2_complete_message(const dsd_opts* opts, dsd_state* state, const struct nxdn_sacch2_fields* fields,
                                   uint8_t crc_sf_check) {
    const int single_frame_ok = fields->sf_fb && fields->sf_pof && nxdn_sacch2_crc_ok(fields);
    const int multi_frame_ok = fields->sf_num == 0 && crc_sf_check == 0;
    if (!single_frame_ok && !multi_frame_ok) {
        return;
    }

    const uint8_t cipher = (uint8_t)nxdn_bits_to_u16(state->dmr_pdu_sf[0], 2);
    const uint16_t user_code = nxdn_bits_to_u16(state->dmr_pdu_sf[0] + 2, 9);
    DSD_FPRINTF(stderr, "UC: %03d; ", user_code);
    if (cipher == 0x01) {
        DSD_FPRINTF(stderr, "Scrambler; ");
        state->nxdn_cipher_type = 1;
        if (state->R != 0) {
            char key_text[24];
            DSD_FPRINTF(stderr, "Key: %s; ",
                        dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, state->R, 0U));
        }
    } else if (cipher != 0x00) {
        DSD_FPRINTF(stderr, "Reserved Comms: %d; ", cipher);
    }

    state->dmr_encL = (state->nxdn_cipher_type != 0 && state->R == 0) ? 1 : 0;
    const uint8_t mfid = (uint8_t)nxdn_bits_to_u16(state->dmr_pdu_sf[0] + 11, 7);
    if (mfid != 0) {
        DSD_FPRINTF(stderr, "MFID: %02X; ", mfid);
    }
    if (fields->sf_fb == 0 && fields->sf_num == 0) {
        const unsigned long long mes_hex = (unsigned long long)convert_bits_into_output(state->dmr_pdu_sf[0] + 18, 54);
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, " Message: %014llX; ", mes_hex);
    }
}

static void
nxdn_print_sacch2_payload(const dsd_opts* opts, const dsd_state* state, const struct nxdn_sacch2_fields* fields,
                          const uint8_t* m_data) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n DCR SACCH ");
    for (int i = 0; i < 4; i++) {
        DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
    }
    if (fields->sf_num == 0) {
        DSD_FPRINTF(stderr, "\n DCR SFULL ");
        for (int i = 0; i < 9; i++) {
            DSD_FPRINTF(stderr, "[%02X]", (uint8_t)nxdn_bits_to_u16(state->dmr_pdu_sf[0] + ((size_t)i * 8U), 8));
        }
    }
}

static void
nxdn_reset_sacch2_if_done(dsd_state* state, const struct nxdn_sacch2_fields* fields) {
    if (fields->sf_num != 0) {
        return;
    }

    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
}

static void
nxdn_handle_sacch2(const dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                   uint8_t crc, uint8_t check) {
    const struct nxdn_sacch2_fields fields = nxdn_sacch2_fields_from_trellis(trellis_buf, crc, check);
    nxdn_print_sacch2_header(state, &fields);
    const uint8_t crc_sf_check = nxdn_update_sacch2_segment_crc(state, &fields);
    nxdn_store_sacch2_frame(state, trellis_buf, &fields);
    nxdn_update_sacch2_identity_state(state, &fields);
    nxdn_print_sacch2_complete_message(opts, state, &fields, crc_sf_check);
    nxdn_print_sacch2_payload(opts, state, &fields, m_data);
    nxdn_reset_sacch2_if_done(state, &fields);
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_nxdn_test_sacch2_state_update(const dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[32],
                                      const uint8_t m_data[5], uint8_t crc, uint8_t check) {
    nxdn_handle_sacch2(opts, state, trellis_buf, m_data, crc, check);
}
#endif

struct nxdn_facch3_udch2_message {
    uint8_t bits[160];
    uint8_t bytes[24];
    uint16_t crc[2];
    uint16_t check[2];
};

static int
nxdn_facch3_udch2_crc_ok(const struct nxdn_facch3_udch2_message* message) {
    return message->crc[0] == message->check[0] && message->crc[1] == message->check[1];
}

static int
nxdn_facch3_udch2_crc_failed(const struct nxdn_facch3_udch2_message* message) {
    return message->crc[0] != message->check[0] || message->crc[1] != message->check[1];
}

static void
nxdn_store_facch3_udch2_block(struct nxdn_facch3_udch2_message* message, size_t block, const uint8_t* trellis_buf,
                              const uint8_t* m_data) {
    for (size_t i = 0; i < 80U; i++) {
        message->bits[i + (block * 80U)] = trellis_buf[i];
    }
    for (size_t i = 0; i < 12U; i++) {
        message->bytes[i + (block * 12U)] = m_data[i];
    }
}

static void
nxdn_decode_facch3_udch2_block_soft(const uint8_t* bits, const uint8_t* reliab, size_t block,
                                    struct nxdn_facch3_udch2_message* message) {
    uint8_t deperm[144];
    uint8_t deperm_rel[144];
    uint8_t depunc[192];
    uint8_t depunc_rel[192];
    uint8_t trellis_buf[96];
    uint8_t m_data[12];
    const size_t offset = block * 144U;

    DSD_MEMSET(deperm, 0, sizeof(deperm));
    DSD_MEMSET(deperm_rel, 255, sizeof(deperm_rel));
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 255, sizeof(depunc_rel));
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));
    DSD_MEMSET(m_data, 0, sizeof(m_data));

    nxdn_depermute_rel_u8(bits + offset, reliab + offset, 144U, PERM_16_9, deperm, deperm_rel);
    nxdn_depuncture_16_9_rel(deperm, deperm_rel, depunc, depunc_rel);
    nxdn_conv_decode_soft(depunc, depunc_rel, sizeof(depunc), m_data, 92);
    nxdn_unpack_bytes_msb(m_data, 12U, trellis_buf);

    message->crc[block] = nxdn_facch_crc12_payload_from_trellis(trellis_buf);
    message->check[block] = nxdn_facch_crc12_check_from_trellis(trellis_buf);
    if (message->crc[block] != message->check[block]) {
        message->crc[block] = 1;
        message->check[block] = 0;
        nxdn_hard_fallback_decode(trellis_buf, sizeof(trellis_buf), m_data, 12U, depunc, 92);
        message->crc[block] = nxdn_facch_crc12_payload_from_trellis(trellis_buf);
        message->check[block] = nxdn_facch_crc12_check_from_trellis(trellis_buf);
    }
    nxdn_store_facch3_udch2_block(message, block, trellis_buf, m_data);
}

static void
nxdn_print_facch3_udch2_name(uint8_t type) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    if (type == 0) {
        DSD_FPRINTF(stderr, " UDCH2");
    }
    if (type == 1) {
        DSD_FPRINTF(stderr, " FACCH3");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
nxdn_decode_facch3_udch2_content(dsd_opts* opts, dsd_state* state, const struct nxdn_facch3_udch2_message* message) {
    if (!nxdn_facch3_udch2_crc_ok(message)) {
        return;
    }

    state->data_header_format[0] = 1;
    NXDN_Elements_Content_decode(opts, state, 1, message->bits, 160U);
}

static void
nxdn_print_udch2_hex_data(const uint8_t* bytes) {
    DSD_FPRINTF(stderr, "\n UDCH2 Data: ");
    for (int i = 0; i < 22; i++) {
        if (i == 10) {
            DSD_FPRINTF(stderr, " ");
        }
        if (i == 10 || i == 11) {
            continue;
        }
        DSD_FPRINTF(stderr, "%02X", bytes[i]);
    }
}

static void
nxdn_print_udch2_soft_data(const struct nxdn_facch3_udch2_message* message) {
    nxdn_print_udch2_hex_data(message->bytes);
    if (nxdn_facch3_udch2_crc_failed(message)) {
        DSD_FPRINTF(stderr, "%s", KRED);
        if (message->crc[0] != message->check[0]) {
            DSD_FPRINTF(stderr, " (CRC ERR P0)");
        }
        if (message->crc[1] != message->check[1]) {
            DSD_FPRINTF(stderr, " (CRC ERR P1)");
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static void
nxdn_print_facch3_udch2_payload_header(uint8_t type) {
    DSD_FPRINTF(stderr, "\n");
    if (type == 0) {
        DSD_FPRINTF(stderr, " UDCH2");
    }
    if (type == 1) {
        DSD_FPRINTF(stderr, " FACCH3");
    }
}

static void
nxdn_print_facch3_udch2_payload_soft(const dsd_opts* opts, const struct nxdn_facch3_udch2_message* message,
                                     uint8_t type) {
    if (opts->payload != 1) {
        return;
    }

    nxdn_print_facch3_udch2_payload_header(type);
    DSD_FPRINTF(stderr, " Payload\n  ");
    for (int i = 0; i < 22; i++) {
        if (i == 10) {
            DSD_FPRINTF(stderr, "\n  ");
        }
        if (i == 10 || i == 11) {
            continue;
        }
        DSD_FPRINTF(stderr, "[%02X]", message->bytes[i]);
    }
    if (nxdn_facch3_udch2_crc_failed(message)) {
        nxdn_print_crc_error_colored();
    }
}

static void
nxdn_handle_facch3_udch2_soft(dsd_opts* opts, dsd_state* state, const struct nxdn_facch3_udch2_message* message,
                              uint8_t type) {
    nxdn_print_facch3_udch2_name(type);
    nxdn_decode_facch3_udch2_content(opts, state, message);
    if (type == 0) {
        nxdn_print_udch2_soft_data(message);
    }
    nxdn_print_facch3_udch2_payload_soft(opts, message, type);
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_neo_nxdn_test_facch3_udch2_state_update(dsd_opts* opts, dsd_state* state, const uint8_t bits[160],
                                            const uint8_t bytes[24], uint16_t crc0, uint16_t check0, uint16_t crc1,
                                            uint16_t check1, uint8_t type) {
    struct nxdn_facch3_udch2_message message;
    DSD_MEMSET(&message, 0, sizeof(message));
    DSD_MEMCPY(message.bits, bits, sizeof(message.bits));
    DSD_MEMCPY(message.bytes, bytes, sizeof(message.bytes));
    message.crc[0] = crc0;
    message.check[0] = check0;
    message.crc[1] = crc1;
    message.check[1] = check1;
    nxdn_handle_facch3_udch2_soft(opts, state, &message, type);
}

void
dsd_neo_nxdn_test_facch3_udch2_store_block(uint8_t bits[160], uint8_t bytes[24], size_t block,
                                           const uint8_t trellis_buf[96], const uint8_t m_data[12]) {
    if (bits == NULL || bytes == NULL || trellis_buf == NULL || m_data == NULL || block > 1U) {
        return;
    }

    struct nxdn_facch3_udch2_message message;
    DSD_MEMSET(&message, 0, sizeof(message));
    DSD_MEMCPY(message.bits, bits, sizeof(message.bits));
    DSD_MEMCPY(message.bytes, bytes, sizeof(message.bytes));
    nxdn_store_facch3_udch2_block(&message, block, trellis_buf, m_data);
    DSD_MEMCPY(bits, message.bits, sizeof(message.bits));
    DSD_MEMCPY(bytes, message.bytes, sizeof(message.bytes));
}
#endif

struct nxdn_message_label {
    uint8_t type;
    const char* label;
};

const char*
nxdn_message_type_label(uint8_t message_type) {
    static const struct nxdn_message_label labels[] = {
        {0x00, " CALL_RESP"},
        {0x01, " VCALL"},
        {0x02, " VCALL_REC_REQ"},
        {0x03, " VCALL_IV"},
        {0x04, " VCALL_ASSGN"},
        {0x05, " VCALL_ASSGN_DUP"},
        {0x06, " CALL_CONN_RESP"},
        {0x07, " TX_REL_EX"},
        {0x08, " TX_REL"},
        {0x09, " DCALL_HEADER"},
        {0x0A, " DCALL_REC_REQ"},
        {0x0B, " DCALL_DATA"},
        {0x0C, " DCALL_ACK"},
        {0x0D, " DCALL_ASSGN_DUP"},
        {0x0E, " DCALL_ASSGN"},
        {0x0F, " HEAD_DLY"},
        {0x10, " IDLE"},
        {0x11, " DISC"},
        {0x17, " DST_ID_INFO"},
        {0x18, " SITE_INFO"},
        {0x19, " SRV_INFO"},
        {0x1A, " CCH_INFO"},
        {0x1B, " ADJ_SITE_INFO"},
        {0x1C, " FAIL_STAT_INFO"},
        {0x20, " REG_RESP"},
        {0x22, " REG_C_RESP"},
        {0x23, " REG_COMM"},
        {0x24, " GRP_REG_RESP"},
        {0x28, " AUTH_INQ_REQ"},
        {0x29, " AUTH_INQ_RESP"},
        {0x2A, " AUTH_INQ_REQ2"},
        {0x2B, " AUTH_INQ_RESP2"},
        {0x30, " STAT_INQ_REQ"},
        {0x31, " STAT_INQ_RESP"},
        {0x32, " STAT_REQ"},
        {0x33, " STAT_RESP"},
        {0x34, " REM_CON_REQ"},
        {0x35, " REM_CON_RESP"},
        {0x36, " REM_CON_E_REQ"},
        {0x37, " REM_CON_E_RESP"},
        {0x38, " SDCALL_REQ_HEADER"},
        {0x39, " SDCALL_REQ_DATA"},
        {0x3A, " SDCALL_IV"},
        {0x3B, " SDCALL_RESP"},
        {0x3F, ""},
        {0x81, ""},
        {0x88, ""},
        {0x90, ""},
        {0xE1, " VCALL_STD_B54"},
        {0xE2, " GPS_HEADER"},
        {0xE3, " GPS_DATA"},
        {0xE4, " BEARER_HEADER"},
        {0xE5, " BEARER_DATA"},
        {0xE7, " ALIAS_STD_B54"},
        {0xE8, " TX_REL_STD_B54"},
    };

    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (labels[i].type == message_type) {
            return labels[i].label;
        }
    }
    return NULL;
}

static int
nxdn_message_type_resets_call(uint8_t message_type) {
    return message_type == 0x08U || message_type == 0x11U || message_type == 0xE8U;
}

static int
nxdn_message_type_resets_gain(uint8_t message_type) {
    return message_type == 0x07U || nxdn_message_type_resets_call(message_type);
}

/*
 * Soft-decision FACCH depermute/decode path.
 * Uses per-bit reliability values to weight the convolution decoder.
 */
void
nxdn_deperm_facch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144], uint8_t frame) {
    static uint8_t facch1_storage[12];
    uint8_t deperm[144];
    uint8_t deperm_rel[144];
    uint8_t depunc[192];
    uint8_t depunc_rel[192];
    uint8_t trellis_buf[96];
    uint16_t crc = 1;
    uint16_t check = 0;

    DSD_MEMSET(deperm, 0, sizeof(deperm));
    DSD_MEMSET(deperm_rel, 255, sizeof(deperm_rel));
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 255, sizeof(depunc_rel));

    // Deperm: shuffle bits and reliability in parallel
    nxdn_depermute_rel_u8(bits, reliab, 144U, PERM_16_9, deperm, deperm_rel);
    nxdn_depuncture_16_9_rel(deperm, deperm_rel, depunc, depunc_rel);

    // Soft convolution decode
    uint8_t m_data[20];
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));

    nxdn_conv_decode_soft(depunc, depunc_rel, sizeof(depunc), m_data, 92);
    nxdn_unpack_bytes_msb(m_data, 12U, trellis_buf);

    crc = nxdn_facch_crc12_payload_from_trellis(trellis_buf);
    check = nxdn_facch_crc12_check_from_trellis(trellis_buf);

    // Fallback to hard-decision if soft decode fails
    if (crc != check) {
        nxdn_hard_fallback_decode(trellis_buf, sizeof(trellis_buf), m_data, 12U, depunc, 92);
        crc = nxdn_facch_crc12_payload_from_trellis(trellis_buf);
        check = nxdn_facch_crc12_check_from_trellis(trellis_buf);
    }

    const int duplicate = frame == 2U && memcmp(facch1_storage, m_data, sizeof(facch1_storage)) == 0;
    DSD_MEMSET(facch1_storage, 0, sizeof(facch1_storage));
    if (frame == 1U) {
        DSD_MEMCPY(facch1_storage, m_data, sizeof(facch1_storage));
    }

    state->data_header_format[0] = 3;
    if (crc == check && !duplicate) {
        NXDN_Elements_Content_decode(opts, state, 1, trellis_buf, sizeof(trellis_buf));
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, " FACCH1 Payload ");
        for (int i = 0; i < 12; i++) {
            DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
        }
        if (crc != check) {
            DSD_FPRINTF(stderr, "%s", KRED);
            DSD_FPRINTF(stderr, " (CRC ERR)");
            DSD_FPRINTF(stderr, "%s", KNRM);
        }
    }
}

/*
 * Soft-decision SACCH depermute/decode path.
 * Uses per-bit reliability values to weight the convolution decoder.
 */
void
nxdn_deperm_sacch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]) {
    uint8_t deperm[60];
    uint8_t deperm_rel[60];
    uint8_t depunc[72];
    uint8_t depunc_rel[72];
    uint8_t trellis_buf[32];

    DSD_MEMSET(deperm, 0, sizeof(deperm));
    DSD_MEMSET(deperm_rel, 255, sizeof(deperm_rel));
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 255, sizeof(depunc_rel));
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));

    uint8_t crc = 1;
    uint8_t check = 0;

    // Deperm with reliability
    nxdn_depermute_rel_u8(bits, reliab, 60U, PERM_12_5, deperm, deperm_rel);
    nxdn_depuncture_12_5_rel(deperm, deperm_rel, depunc, depunc_rel);

    uint8_t m_data[5];

    DSD_MEMSET(m_data, 0, sizeof(m_data));

    nxdn_conv_decode_soft(depunc, depunc_rel, sizeof(depunc), m_data, 32);
    nxdn_unpack_bytes_msb(m_data, 4U, trellis_buf);

    crc = crc6(trellis_buf, 26);
    check = (uint8_t)nxdn_bits_to_u16(trellis_buf + 26, 6);

    // Fallback to hard-decision if soft decode fails
    if (crc != check) {
        nxdn_hard_fallback_decode(trellis_buf, sizeof(trellis_buf), m_data, 4U, depunc, 32);
        crc = crc6(trellis_buf, 26);
        check = (uint8_t)nxdn_bits_to_u16(trellis_buf + 26, 6);
    }

    nxdn_handle_sacch(opts, state, trellis_buf, m_data, crc, check);
}

void
nxdn_message_type(const dsd_opts* opts, dsd_state* state, uint8_t MessageType) {
    //NOTE: Most Req/Resp (request and respone) share same message type but differ depending on channel type
    //RTCH Outbound will take precedent when differences may occur (except CALL_ASSGN)
    DSD_FPRINTF(stderr, "%s", KYEL);
    const char* label = nxdn_message_type_label(MessageType);
    if (label != NULL) {
        if (label[0] != '\0') {
            DSD_FPRINTF(stderr, "%s", label);
        }
    } else {
        DSD_FPRINTF(stderr, " Unknown Message Type: %02X;", MessageType);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);

    //Zero out stale values on DISC or TX_REL only (IDLE messaages occur often on NXDN96 VCH, and randomly on Type-C FACCH1 steals for some reason)
    if (nxdn_message_type_resets_call(MessageType)) {
        nxdn_alias_reset(state);
        state->nxdn_last_rid = 0;
        state->nxdn_last_tg = 0;
        state->nxdn_cipher_type = 0; // Force will reactivate it if needed during voice tx
        if (state->keyloader == 1) {
            state->R = 0;
        }
        DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
        DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
        DSD_SNPRINTF(state->nxdn_call_type, sizeof(state->nxdn_call_type), "%s", "");
    }

    if (nxdn_message_type_resets_gain(MessageType)) {
        //reset gain
        if (opts->floating_point == 1) {
            state->aout_gain = opts->audio_gain;
        }
    }
}

int
load_i(const uint8_t val[], int len) {
    int acc = 0;
    for (int i = 0; i < len; i++) {
        acc = (acc << 1) + (val[i] & 1);
    }
    return acc;
}

uint8_t
crc6(const uint8_t buf[], int len) {
    uint8_t s[6];
    for (int i = 0; i < 6; i++) {
        s[i] = 1;
    }
    for (int i = 0; i < len; i++) {
        const uint8_t a = buf[i] ^ s[0];
        s[0] = a ^ s[1];
        s[1] = s[2];
        s[2] = s[3];
        s[3] = a ^ s[4];
        s[4] = a ^ s[5];
        s[5] = a;
    }
    return load_i(s, 6);
}

uint16_t
crc16cac(const uint8_t buf[], int len) {
    uint32_t crc = 0xc3ee;                    //not sure why this though
    uint32_t poly = (1 << 12) + (1 << 5) + 1; //poly is fine
    for (int i = 0; i < len; i++) {
        crc = ((crc << 1) | buf[i]) & 0x1ffff;
        if (crc & 0x10000) {
            crc = (crc & 0xffff) ^ poly;
        }
    }
    crc = crc ^ 0xffff;
    return crc & 0xffff;
}

uint32_t
nxdn_message_crc32(const uint8_t* input, int len) {
    uint32_t crc = 0xFFFFFFFFU;
    const uint32_t poly = 0x04C11DB7U;

    if (input == NULL || len <= 0) {
        return crc;
    }

    for (int i = 0; i < len; i++) {
        uint32_t in = (uint32_t)(input[i] & 1U);
        uint32_t msb = (crc >> 31U) & 1U;
        if ((msb ^ in) != 0U) {
            crc = (crc << 1U) ^ poly;
        } else {
            crc <<= 1U;
        }
    }

    return crc;
}

uint8_t
crc7_scch(const uint8_t bits[], int len) {
    uint8_t s[7];
    for (int i = 0; i < 7; i++) {
        s[i] = 1;
    }
    for (int i = 0; i < len; i++) {
        const uint8_t a = bits[i] ^ s[0];
        s[0] = s[1];
        s[1] = s[2];
        s[2] = s[3];
        s[3] = a ^ s[4];
        s[4] = s[5];
        s[5] = s[6];
        s[6] = a;
    }
    return load_i(s, 7);
}

/*
 * Soft-decision CAC depermute/decode path.
 */
void
nxdn_deperm_cac_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[300], const uint8_t reliab[300]) {
    uint8_t deperm[300];
    uint8_t deperm_rel[300];
    uint8_t depunc[350];
    uint8_t depunc_rel[350];
    uint8_t trellis_buf[176];
    uint16_t crc = 0;

    DSD_MEMSET(deperm, 0, sizeof(deperm));
    DSD_MEMSET(deperm_rel, 255, sizeof(deperm_rel));
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 255, sizeof(depunc_rel));

    nxdn_depermute_rel(bits, reliab, 300U, PERM_12_25, deperm, deperm_rel);
    nxdn_depuncture_12_group_rel(deperm, deperm_rel, 25U, depunc, depunc_rel);

    uint8_t m_data[22];
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));
    DSD_MEMSET(m_data, 0, sizeof(m_data));

    nxdn_conv_decode_soft(depunc, depunc_rel, sizeof(depunc), m_data, 171);
    nxdn_unpack_bytes_msb(m_data, 22U, trellis_buf);

    crc = crc16cac(trellis_buf, 171);

    if (crc != 0) {
        nxdn_hard_fallback_decode(trellis_buf, sizeof(trellis_buf), m_data, 22U, depunc, 171);
        crc = crc16cac(trellis_buf, 171);
    }

    nxdn_handle_cac(opts, state, trellis_buf, m_data, crc);
}

/*
 * Soft-decision FACCH2/UDCH depermute/decode path.
 */
void
nxdn_deperm_facch2_udch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[348], const uint8_t reliab[348],
                             uint8_t type) {
    uint8_t deperm[348];
    uint8_t deperm_rel[348];
    uint8_t depunc[406];
    uint8_t depunc_rel[406];
    uint8_t trellis_buf[208];
    uint16_t crc = 0;
    uint16_t check = 0;

    DSD_MEMSET(deperm, 0, sizeof(deperm));
    DSD_MEMSET(deperm_rel, 255, sizeof(deperm_rel));
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 255, sizeof(depunc_rel));

    nxdn_depermute_rel(bits, reliab, 348U, PERM_12_29, deperm, deperm_rel);
    nxdn_depuncture_12_group_rel(deperm, deperm_rel, 29U, depunc, depunc_rel);

    uint8_t m_data[26];
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));
    DSD_MEMSET(m_data, 0, sizeof(m_data));

    nxdn_conv_decode_soft(depunc, depunc_rel, sizeof(depunc), m_data, 199);
    nxdn_unpack_bytes_msb(m_data, 26U, trellis_buf);

    crc = nxdn_facch2_udch_crc15_payload_from_trellis(trellis_buf);
    check = nxdn_facch2_udch_crc15_check_from_trellis(trellis_buf);

    if (crc != check) {
        nxdn_hard_fallback_decode(trellis_buf, sizeof(trellis_buf), m_data, 26U, depunc, 199);
        crc = nxdn_facch2_udch_crc15_payload_from_trellis(trellis_buf);
        check = nxdn_facch2_udch_crc15_check_from_trellis(trellis_buf);
    }

    nxdn_handle_facch2_udch(opts, state, trellis_buf, m_data, crc, check, type);
}

/*
 * Soft-decision SCCH depermute/decode path.
 */
void
nxdn_deperm_scch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60], uint8_t direction) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " SCCH");

    uint8_t deperm[60];
    uint8_t deperm_rel[60];
    uint8_t depunc[72];
    uint8_t depunc_rel[72];
    uint8_t trellis_buf[32];

    DSD_MEMSET(deperm, 0, sizeof(deperm));
    DSD_MEMSET(deperm_rel, 255, sizeof(deperm_rel));
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 255, sizeof(depunc_rel));
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));

    uint8_t crc = 0;
    uint8_t check = 0;

    nxdn_depermute_rel_u8(bits, reliab, 60U, PERM_12_5, deperm, deperm_rel);
    nxdn_depuncture_12_5_rel(deperm, deperm_rel, depunc, depunc_rel);

    uint8_t m_data[5];

    DSD_MEMSET(m_data, 0, sizeof(m_data));

    nxdn_conv_decode_soft(depunc, depunc_rel, sizeof(depunc), m_data, 32);
    nxdn_unpack_bytes_msb(m_data, 4U, trellis_buf);

    crc = crc7_scch(trellis_buf, 25);
    check = nxdn_scch_crc7_check_from_trellis(trellis_buf);

    if (crc != check) {
        nxdn_hard_fallback_decode(trellis_buf, sizeof(trellis_buf), m_data, 4U, depunc, 32);
        crc = crc7_scch(trellis_buf, 25);
        check = nxdn_scch_crc7_check_from_trellis(trellis_buf);
    }

    const uint8_t sf = (trellis_buf[0] << 1) | trellis_buf[1];
    const int part_of_frame = nxdn_sacch_part_of_frame(sf);

    if (part_of_frame == 0 && state->nxdn_cipher_type == 0x1) {
        nxdn_reset_payload_seed_if_forced(state);
    }

    if (crc == check) {
        NXDN_decode_scch(opts, state, trellis_buf, direction);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n SCCH Payload ");
        for (int i = 0; i < 4; i++) {
            DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
        }
        if (crc != check) {
            DSD_FPRINTF(stderr, "%s", KRED);
            DSD_FPRINTF(stderr, " (CRC ERR)");
            DSD_FPRINTF(stderr, "%s", KNRM);
        }
    }
}

/*
 * Soft-decision SACCH2 depermute/decode path.
 */
void
nxdn_deperm_sacch2_soft(const dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]) {
    uint8_t deperm[60];
    uint8_t deperm_rel[60];
    uint8_t depunc[72];
    uint8_t depunc_rel[72];
    uint8_t trellis_buf[32];

    DSD_MEMSET(deperm, 0, sizeof(deperm));
    DSD_MEMSET(deperm_rel, 255, sizeof(deperm_rel));
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 255, sizeof(depunc_rel));
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));

    uint8_t crc = 1;
    uint8_t check = 0;

    nxdn_depermute_rel_u8(bits, reliab, 60U, PERM_12_5, deperm, deperm_rel);
    nxdn_depuncture_12_5_rel(deperm, deperm_rel, depunc, depunc_rel);

    uint8_t m_data[5];

    DSD_MEMSET(m_data, 0, sizeof(m_data));

    nxdn_conv_decode_soft(depunc, depunc_rel, sizeof(depunc), m_data, 32);
    nxdn_unpack_bytes_msb(m_data, 4U, trellis_buf);

    crc = crc6(trellis_buf, 26);
    check = (uint8_t)nxdn_bits_to_u16(trellis_buf + 26, 6);

    if (crc != check) {
        nxdn_hard_fallback_decode(trellis_buf, sizeof(trellis_buf), m_data, 4U, depunc, 32);
        crc = crc6(trellis_buf, 26);
        check = (uint8_t)nxdn_bits_to_u16(trellis_buf + 26, 6);
    }

    nxdn_handle_sacch2(opts, state, trellis_buf, m_data, crc, check);
}

/*
 * Soft-decision PICH/TCH depermute/decode path.
 */
void
nxdn_deperm_pich_tch_soft(const dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144],
                          uint8_t lich) {
    uint8_t deperm[144];
    uint8_t deperm_rel[144];
    uint8_t depunc[192];
    uint8_t depunc_rel[192];
    uint8_t trellis_buf[96];
    uint16_t crc = 1;
    uint16_t check = 0;

    DSD_MEMSET(deperm, 0, sizeof(deperm));
    DSD_MEMSET(deperm_rel, 255, sizeof(deperm_rel));
    DSD_MEMSET(depunc, 0, sizeof(depunc));
    DSD_MEMSET(depunc_rel, 255, sizeof(depunc_rel));

    nxdn_depermute_rel_u8(bits, reliab, 144U, PERM_16_9, deperm, deperm_rel);
    nxdn_depuncture_16_9_rel(deperm, deperm_rel, depunc, depunc_rel);

    uint8_t m_data[20];
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));

    nxdn_conv_decode_soft(depunc, depunc_rel, sizeof(depunc), m_data, 92);
    nxdn_unpack_bytes_msb(m_data, 12U, trellis_buf);

    crc = nxdn_facch_crc12_payload_from_trellis(trellis_buf);
    check = nxdn_facch_crc12_check_from_trellis(trellis_buf);

    if (crc != check) {
        nxdn_hard_fallback_decode(trellis_buf, sizeof(trellis_buf), m_data, 12U, depunc, 92);
        crc = nxdn_facch_crc12_payload_from_trellis(trellis_buf);
        check = nxdn_facch_crc12_check_from_trellis(trellis_buf);
    }

    nxdn_handle_pich_tch(opts, state, trellis_buf, m_data, crc, check, lich);
}

/*
 * Soft-decision FACCH3/UDCH2 depermute/decode path.
 * Processes two 144-bit blocks with separate CRCs (same structure as original).
 */
void
nxdn_deperm_facch3_udch2_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[288], const uint8_t reliab[288],
                              uint8_t type) {
    struct nxdn_facch3_udch2_message message;
    DSD_MEMSET(&message, 0, sizeof(message));
    for (size_t block = 0; block < 2U; block++) {
        nxdn_decode_facch3_udch2_block_soft(bits, reliab, block, &message);
    }

    nxdn_handle_facch3_udch2_soft(opts, state, &message, type);
}
