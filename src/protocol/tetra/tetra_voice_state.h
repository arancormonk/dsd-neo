// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The speech path's part of the TETRA decoder state (tetra_state_t::voice).
 *
 * Lives and dies with the TETRA state: tetra_voice_release() runs when the state is dropped, and the engine's
 * carrier-boundary reset (tetra_voice_reset()) forgets the speech in progress, as do a BSCH of another cell and the end
 * of the heard speech. The speech in progress belongs to one transmission: speech of another call, timeslot or talker,
 * after a TETRA call ended, or after a pause in it of TETRA_CALL_SPEECH_END_S, starts over from a reset.
 * Decoder-thread private.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_STATE_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_STATE_H_

#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/protocol/tetra/tetra_codec_abi.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tetra_voice {
    /* The codec library (--tetra-codec), loaded at the first speech frame that would play; its functions; the
       decoder instance, made again after a reset. */
    struct dsd_dynlib* lib;
    dsd_tetra_codec_create_fn create;
    dsd_tetra_codec_decode_fn decode;
    dsd_tetra_codec_destroy_fn destroy;
    void* codec;

    /* Frames the codec decoded, of them concealed bad frames, and frames it failed on. */
    uint32_t frames;
    uint32_t concealed;
    uint32_t codec_errors;

    /* Decoded PCM not played yet: the output takes whole DSD_VOICE_FRAME_SAMPLES frames, a speech frame is 240. */
    uint16_t carry_len;
    short carry[DSD_VOICE_FRAME_SAMPLES];

    uint8_t codec_status;       /* tetra_codec_status */
    uint8_t last_decode_failed; /* the codec failed on the last frame it was given */
    /* Speech integrity: 1 once a CRC-passed burst started the speech; the bad frames since the last good one. */
    uint8_t confirmed;
    uint8_t bad_run;
    /* Decode clock: the last burst of the speech in progress with a good frame; 0 for none. A burst that comes
       TETRA_CALL_SPEECH_END_S or more after it is another transmission's. */
    double last_good_m;

    /* The transmission the speech in progress belongs to (bound): the kind, call identifier (TETRA_SPEECH_CALL),
       timeslot, target and transmitting party of its speech (tetra_speech; the party once known), and the count of
       ended TETRA calls (tetra_calls_t::ends) when it started. */
    uint32_t owner_target;
    uint32_t owner_source;
    uint32_t owner_ends;
    uint16_t owner_call_id;
    uint8_t owner_kind;
    uint8_t owner_tn;
    uint8_t bound;
} tetra_voice_t;

/** Unload the codec and clear @p voice; the TETRA state's cleanup calls it. */
void tetra_voice_release(tetra_voice_t* voice);

/** Call @p old_id goes on as call @p new_id (D-INFO, D-CALL RESTORE): speech in progress of it stays its speech. */
void tetra_voice_call_renamed(tetra_voice_t* voice, uint16_t old_id, uint16_t new_id);

/** Call @p call_id starts its next transmission (a grant after D-TX CEASED): speech in progress of it, whose talker
 * may no longer be known, is not the next transmission's and starts over. */
void tetra_voice_transmission_started(tetra_voice_t* voice, uint16_t call_id);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_STATE_H_ */
