// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Which TETRA speech is heard, the call it belongs to, and its encryption class.
 *
 * Trunking on, only the followed call's speech is heard: on its traffic carrier once the tune settled, on its
 * timeslots as the TDMA clock places the burst (none while the clock waits for the carrier's BSCH after a retune), and
 * once CRC-passed signalling of the call proved it on that carrier (tetra_confirm). Not trunking, the
 * receiver hears one timeslot at a time: the first with speech, until its speech stops for TETRA_CALL_SPEECH_END_S;
 * each timeslot of a carrier may be a traffic channel of its own call (EN 300 392-2 clause 9.4.2.2, table 9.27).
 * Direct mode carries one call on its channel (EN 300 396-3) and is never held back by the trunked gates.
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
 * had none. Direct mode speech belongs to the direct mode call, judged by its destination and security class.
 *
 * @return 1 when the burst is heard, 0 when it is not (@p sp is cleared).
 */
int tetra_voice_gate(const dsd_opts* opts, const dsd_state* state, tetra_speech* sp);

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
