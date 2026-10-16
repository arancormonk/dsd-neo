// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA speech: from decoded traffic-channel bits to the digital voice output.
 *
 * The speech channel decoder (tetra_fec.h) yields the type-1 bits of a burst and whether its class-2 CRC passed.
 * This module orders them into 137-bit speech frames (EN 300 395-2 tables 5 and 6), decodes them with mbelib-neo's
 * TETRA speech decoder (mbe_tetraDecodeFrame()) and plays the PCM through dsd_audio_play_decoded_pcm16_record().
 *
 * A frame plays only on the followed traffic channel and timeslot, when its encryption class is audible
 * (dsd_voice_crypto_class_audible()) and the talkgroup policy allows audio; it is recorded only when the record
 * policy allows that too. Speech starts with a burst whose CRC passed: until then nothing reaches the decoder. A
 * frame whose CRC failed after that goes to the decoder as a bad frame, which it conceals, and a run of them ends the speech
 * until the next CRC-passed burst. Decoder thread only.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_VOICE_H
#define DSD_NEO_PROTOCOL_TETRA_VOICE_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Coded bits in one speech frame, B1..B137 (EN 300 395-2 table 3). */
#define TETRA_VOICE_FRAME_BITS     137
/** PCM samples in one speech frame: 30 ms at 8000 Hz. */
#define TETRA_VOICE_FRAME_SAMPLES  240

/** Bad-frame run that ends speech: four 30 ms frames, by which the decoder's concealment has faded it by 24 dB. */
#define TETRA_VOICE_CONCEAL_FRAMES 4

/**
 * @brief Order the 274 type-1 bits of a full-slot burst (EN 300 395-2 table 5: classes 0, 1 and 2, each alternating
 * between the slot's first frame A and second frame B) into two speech frames of bits B1..B137, A then B.
 */
void tetra_voice_reorder_full(const uint8_t* type1, uint8_t* frames);

/** @brief Order the 137 type-1 bits of a half-slot burst (EN 300 395-2 table 6) into one speech frame. */
void tetra_voice_reorder_half(const uint8_t* type1, uint8_t* frame);

/**
 * @brief The two speech frames of a full-slot traffic burst.
 *
 * Only speech the voice gates let through is decoded: trunking on, the followed call's on its timeslots once its
 * signalling proved it on the carrier; not trunking, the timeslot being heard; direct mode always. The speech's call
 * decides its talkgroup and encryption: an air-encrypted call is decrypted here, once the bits are back in speech
 * frame order, with key stream bits TETRA_AIR_KSS_TCH_S on (tetra_air_decrypt_voice(); frame A's B1..B137 take
 * KSS(0..136), frame B's KSS(137..273)), and whether a key stream applied decides the frames' encryption class. Speech
 * whose CRC passed publishes its call and marks its media before it plays.
 *
 * @param opts  Options; may be NULL.
 * @param type1 The 274 type-1 bits in table 5's class order, as the speech channel decoder gives them
 *              (tetra_speech_full_decode()).
 * @param good  1 when the class-2 CRC passed and the burst was resolved as traffic, else 0.
 */
void tetra_voice_full_slot(dsd_opts* opts, dsd_state* state, const uint8_t* type1, int good);

/**
 * @brief The speech frame in the second half of a burst whose first half was stolen for signalling (EN 300 395-2
 * clause 5.6). The stolen frame reaches the codec as a bad frame first, so the speech keeps its timing; the speech
 * frame's bits B1..B137, back in speech frame order, take key stream bits TETRA_AIR_KSS_TCH_S_HALF on.
 *
 * @param type1 The 137 type-1 bits in table 6's class order (tetra_speech_half_decode()).
 */
void tetra_voice_half_slot(dsd_opts* opts, dsd_state* state, const uint8_t* type1, int good);

/**
 * @brief Forget the speech in progress: the speech decoder's state, the stream's integrity and the PCM not yet played.
 * The engine calls this at carrier boundaries.
 */
void tetra_voice_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_VOICE_H */
