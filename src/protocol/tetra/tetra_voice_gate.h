// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief What the TETRA speech path asks of the call before a frame plays: its encryption class and talkgroup.
 *
 * The channel and timeslot gates are public (tetra_voice.h). Decoder-thread private.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_GATE_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_GATE_H_

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The encryption class of the speech the TETRA state is receiving.
 *
 * Trunked mode takes the air-interface encryption mode of the call's MAC-RESOURCE (EN 300 392-2 clause 21.4.3.1):
 * clear when it is "not encrypted", else decryptable when tetra_air_decrypt_voice() applied a keystream
 * (@p keystream_applied) and undecryptable when it did not. Direct mode takes the DMAC-SYNC air interface encryption
 * state (EN 300 396-3 clause 9.3.2): an encrypted direct-mode call is undecryptable, since no direct-mode keystream is
 * implemented.
 */
dsd_voice_crypto_class tetra_voice_crypto_class(const dsd_state* state, int keystream_applied);

/** @brief The group address the policy judges the call's speech by: the call's GSSI when known, else 0. */
unsigned long tetra_voice_talkgroup(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_VOICE_GATE_H_ */
