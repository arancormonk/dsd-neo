// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Which TETRA speech is heard, the call it belongs to, and its encryption class.
 *
 * Trunking on, only the followed call's speech is heard: on its traffic carrier once the tune settled, on its
 * timeslots as the TDMA clock places the burst (none while the clock waits for the carrier's BSCH after a retune), once
 * CRC-passed signalling of the call proved it on that carrier (tetra_confirm), and while its record does not place it on
 * another channel. Not trunking, the
 * receiver hears one timeslot at a time: the first with speech that plays (its encryption class and its call's
 * talkgroup policy let it), until that speech stops for TETRA_CALL_SPEECH_END_S; speech that does not play holds
 * nothing (tetra_voice_hold()). Each timeslot of a carrier may be a traffic channel of its own call (EN 300 392-2
 * clause 9.4.2.2, table 9.27). On the downlink that is a timeslot the TDMA clock places, once the carrier's BSCH set
 * it; the uplink has no BSCH, and its speech is heard as it comes.
 * Direct mode carries one call on its channel (EN 300 396-3) and is never held back by the trunked gates. Frame 18 of a
 * trunked mode carrier carries signalling only (EN 300 392-2 clause 19.4.3.1), so a burst the TDMA clock places there
 * is no one's speech (tetra_tdma_control_frame()).
 *
 * Decoder-thread private.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_GATE_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_GATE_H_

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include "tetra_calls.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether the speech burst being decoded is heard and, when it is, the call it belongs to in @p sp.
 *
 * The followed call's speech belongs to the followed call. Heard trunked speech belongs to the call record whose
 * traffic channel is the burst's timeslot of the tuned frequency (tetra_calls_on_timeslot()), else to no record: its
 * air encryption is then that of the last addressed MAC-RESOURCE on its timeslot, or on the carrier when its timeslot
 * had none. Direct mode speech belongs to the direct mode call, judged by its destination and security class. A burst
 * in frame 18 of a trunked mode carrier is not heard.
 *
 * @return 1 when the burst is heard, 0 when it is not (@p sp is cleared).
 */
int tetra_voice_gate(const dsd_opts* opts, const dsd_state* state, tetra_speech* sp);

/**
 * @brief Not trunking, whether the receiver hears the timeslot of @p sp's burst, which tetra_voice_gate() let through,
 * once @p sp->audible says whether it plays (tetra_voice_audible()).
 *
 * The timeslot whose speech canonical slot 0 shows is heard. Another is not while that speech goes on (until it stops
 * for TETRA_CALL_SPEECH_END_S) and either holds the receiver, its speech having played within
 * TETRA_CALL_SPEECH_END_S, or @p sp would not play either. Speech that does not play holds nothing and renews no hold:
 * a muted call never keeps one that plays off the receiver, and two muted calls do not take canonical slot 0 from each
 * other at every burst. Trunking on and in direct mode, 1: their gates are tetra_voice_gate()'s.
 */
int tetra_voice_hold(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp);

/**
 * @brief Whether @p sp's burst plays: its encryption class (tetra_voice_crypto_class(), @p keystream_applied) is audible
 * under the user's encrypted-audio and reverse mutes, and the talkgroup policy lets its call play, judged on the
 * identities its canonical call carries (tetra_calls_speech_muted()) before that call is published.
 */
int tetra_voice_audible(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp, int keystream_applied);

/**
 * @brief The encryption class of @p sp's speech: undecryptable when sealed (end-to-end encrypted, or a direct mode call
 * not known clear: no direct mode key stream is implemented); clear without air interface encryption; else decryptable
 * when tetra_air_decrypt_voice() applied a key stream (@p keystream_applied) and undecryptable when it did not.
 */
dsd_voice_crypto_class tetra_voice_crypto_class(const tetra_speech* sp, int keystream_applied);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_GATE_H_ */
