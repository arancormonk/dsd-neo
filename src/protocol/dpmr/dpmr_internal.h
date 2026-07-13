// SPDX-License-Identifier: ISC

#ifndef DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H
#define DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include "dsd-neo/core/state.h"

#include <stdbool.h>
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
void dpmr_convert_air_interface_id(uint32_t ai_id, char id[8]);
uint8_t dpmr_extract_cch_crc(const uint8_t cch_bits[48]);
void dpmr_update_superframe_part(dsd_opts* opts, dsd_state* state, const dpmr_superframe_part* part);
void dpmr_print_ids(dsd_state* state, const char called_id[8], const char calling_id[8]);

#endif /* DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H */
