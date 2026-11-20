// SPDX-License-Identifier: ISC

#ifndef DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H
#define DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <dsd-neo/core/call_state.h>
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
void dpmr_print_ids(const dsd_state* state);
void dpmr_publish_call(dsd_opts* opts, dsd_state* state);
void dpmr_resolve_half_scrambling(dsd_state* state);

/* dPMRVoiceFS2Frame_t::BoundIdKind values. */
enum {
    DPMR_FRAME_ID_NONE = 0U,
    DPMR_FRAME_ID_CALLED = 1U,  /* halves numbered 0/1: ID0 and ID1, the called station (Tables 5.5, 5.6) */
    DPMR_FRAME_ID_CALLING = 2U, /* halves numbered 2/3: ID2 and ID3, the calling station (Tables 5.7, 5.8) */
};

/* What dPMR publishes of a call (dpmr_header.c). */
/* The call kind a checked Communications Mode names (TS 102 658 Table 5.55): voice, data, or unknown for 110 and 111. */
dsd_call_kind dpmr_call_kind_for_mode(uint32_t mode);
/* The colour code @p color_code (-1 none) as this carrier's, on the confirmed path. */
void dpmr_publish_color_code(dsd_state* state, int color_code);
/* The call's crypto classification by @p scrambled (a DPMR_HALF_* value other than DPMR_HALF_UNVERIFIED) and the
   indication @p enc_observed (dsd_call_crypto_update::enc_observed) the publishing frame checked itself. */
void dpmr_publish_crypto(dsd_state* state, unsigned int scrambled, uint8_t enc_observed);

#endif /* DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H */
