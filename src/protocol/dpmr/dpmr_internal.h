// SPDX-License-Identifier: ISC

#ifndef DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H
#define DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include "dsd-neo/core/state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t frame_number[2];
    uint32_t id_value;
    bool crc_ok[2];
    bool hamming_ok[2][2];
} dpmr_superframe_part;

void dpmr_play_voice_frames(dsd_opts* opts, dsd_state* state,
                            char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24]);
void dpmr_deinterleave_6x12(const uint8_t* input, uint8_t* output);
uint8_t dpmr_crc7(const uint8_t* input, uint32_t bit_length);
/* The highest AI ID B2 maps to a dial string: "999****" (TS 102 658 A.1.2.1.1.6, Table A.1). */
#define DPMR_AI_ID_DIAL_STRING_MAX      0xDF6767U
/* Room for any rendering dpmr_convert_air_interface_id() makes: a 7-character dial string, or an 8-digit number. */
#define DPMR_AIR_INTERFACE_ID_TEXT_SIZE 9U
void dpmr_convert_air_interface_id(uint32_t ai_id, char* id, size_t id_size);
uint8_t dpmr_extract_cch_crc(const uint8_t cch_bits[48]);
void dpmr_update_superframe_part(dsd_opts* opts, dsd_state* state, const dpmr_superframe_part* part);
void dpmr_print_ids(const dsd_state* state);
void dpmr_publish_call(dsd_opts* opts, dsd_state* state);
void dpmr_resolve_half_scrambling(dsd_state* state);

#endif /* DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H */
