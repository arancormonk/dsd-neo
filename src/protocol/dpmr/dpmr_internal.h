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
/* A frame no half of which may play (its transmission is not confirmed yet): its halves' place in the scrambler is
   learned (dsd_state::dpmr_scramble_place) and nothing else is written. */
void dpmr_track_voice_frames(dsd_state* state);
void dpmr_deinterleave_6x12(const uint8_t* input, uint8_t* output);
uint8_t dpmr_crc7(const uint8_t* input, uint32_t bit_length);
/* The dialled string of a 24-bit air interface ID (TS 102 658 A.1.2.1.1.6, A.1.2.3.3): seven digits and '*' by the B2
   algorithm; "n******" and "*******" for the All Talkgroup IDs F8339C..F833A6 (Tables A.3, A.4); '#' and six hex
   digits for an ID no dialled string maps to. */
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
/* A checked dPMR call to trunk scan's policy table (issue #890): its called (@p target, when @p target_known) and calling
   (@p source, when @p source_known) stations by their raw 24-bit air interface IDs (TS 102 658 A.1.2.1.1.6; the dial
   string is the display's), its kind by its checked Communications Mode (Table 5.55; 110 and 111 leave it unknown),
   and its scrambling as the decoder reads it from the checked Version: 3 (binary 11, "Manufacturer Specific" in Table
   5.111, no scrambler flag of the standard's) is the scrambled indication the voice path already decrypts by
   (dpmr_resolve_half_scrambling(), issue #655), so the scan's encrypted-call rule and the audio agree. dPMR has no
   group or individual bit every call carries (Table 5.4, A.1.3.3.5), so the party is unknown. */
/* A station a report does not name is taken from the transmission's checked record of it (dsd_state::dpmr_scan_target,
   dpmr_scan_source), which the report updates: alternating 0/1 and 2/3 pairs each name one station, and the call's
   other one, checked by its header or the other pair, still decides the talkgroup and radio policy. */
void dpmr_report_scan_activity(const dsd_opts* opts, dsd_state* state, uint32_t target, int target_known,
                               uint32_t source, int source_known, uint32_t mode, uint32_t version);

#endif /* DSD_NEO_SRC_PROTOCOL_DPMR_DPMR_INTERNAL_H */
