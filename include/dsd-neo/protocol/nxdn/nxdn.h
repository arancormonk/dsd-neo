// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode one NXDN frame.
 *
 * @return 0 while the current transmission has produced no CRC-verified content, 1 once it
 *         has, and 2 when this frame is itself one that checked out.
 *
 * The 0/1 boundary is nxdn_confirm_is_confirmed(), the sticky per-transmission flag rather
 * than the per-frame one: a real call confirms on its first FACCH and every frame after it
 * answers at least 1, while a stream of noise clearing the weak sync word and LICH never
 * does. The SPS hunt refuses those frames the dwell their 182 symbols would otherwise buy
 * (#391).
 *
 * The 2 separates out the frames that carried their own passing CRC, which is what the caller
 * treats as proof of the profile the frame was read on (#445). It has to be this frame's own
 * check and not the transmission's standing: a sticky answer would let the noise syncs between
 * transmissions hold a profile that nothing is decoding on. A frame rejected before its body is
 * read therefore cannot answer 2, however much the transmission has proved before now.
 */
int nxdn_frame(dsd_opts* opts, dsd_state* state);

/**
 * @brief The channel an NXDN frame's LICH names, which dsd_state::nxdn_frame_channel holds while its channels decode.
 *
 * Messages that share a type code across channels do not share a layout: a Type-D (IDAS) SRV_INFO on an RTCH2 is not
 * the Type-C one on a control channel (NXDN TS 1-E §6.4.3.1), and only a control channel's site messages say where
 * the control channel is.
 */
typedef enum {
    NXDN_FRAME_CHANNEL_NONE = 0,    /**< No frame's channels are decoding. */
    NXDN_FRAME_CHANNEL_CONTROL = 1, /**< A Type-C control channel (RCCH, its CAC). */
    NXDN_FRAME_CHANNEL_TRAFFIC = 2, /**< A Type-C or conventional traffic or data channel (RTCH, RDCH), or DCR. */
    NXDN_FRAME_CHANNEL_TYPE_D = 3,  /**< A Type-D (IDAS) traffic channel (RTCH2). */
} nxdn_frame_channel;

/**
 * @brief Whether the held call (dsd_state::nxdn_call) is on the route NXDN followed (dsd_state::nxdn_follow) and the
 * receiver is still tuned there: trunking on and a voice channel tuned (dsd_opts_trunk_vc_followed()) at the route's
 * frequency, and the held call heard there or at no known frequency. An engine-level release (the hangtime return, a
 * trunk scan or operator move) leaves the receiver elsewhere without a word to NXDN, and this turns false.
 */
int nxdn_held_call_on_followed_route(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Slot 0's canonical call was just ended outside NXDN's own message handling (the engine's no-carrier pass: a
 * sync loss, with no TX_REL heard): a held call bound to that call (dsd_state::nxdn_call) takes its end time now, as a
 * release does, so a call that begins within the hangtime of the end is measured from the end, not from when the
 * identity was last read. One still awaiting its call binds to the ended call when that is the first to begin after it
 * was named, and only while its followed route holds (nxdn_held_call_on_followed_route()): a follow abandoned before
 * its call began never names a call heard there later. Changes nothing else and writes no talkgroup policy ledger (no
 * tick guard needed): whether the held call outlives the end is decided by its next read.
 */
void nxdn_held_call_note_call_end(const dsd_opts* opts, dsd_state* state);

/* Cipher-classification hysteresis for the NXDN cipher field (values 0..3).
 * Backed by dsd_state.nxdn_cipher_class*; see src/protocol/nxdn/nxdn_enc_class.c. */
uint8_t nxdn_cipher_observe(dsd_state* state, uint8_t cipher, int strong);
void nxdn_cipher_force(dsd_state* state, uint8_t cipher);
void nxdn_cipher_class_reset(dsd_state* state);

/**
 * @brief Forget the CRC evidence gathered for the current NXDN transmission.
 *
 * Defined in src/protocol/nxdn/nxdn_confirm.c and declared here so the engine can clear it
 * with the carrier; the rest of that module's interface is private to the protocol.
 */
void nxdn_confirm_reset(dsd_state* state);

/**
 * @brief Whether the frame just closed carried a passing CRC of its own.
 *
 * Defined in src/protocol/nxdn/nxdn_confirm.c and declared here for the same reason as
 * nxdn_confirm_reset(): the grading stays inside that module, and nxdn_frame() reports the
 * answer outwards as its 2 (#445).
 */
int nxdn_confirm_frame_proved(const dsd_state* state);
int nxdn_cipher_established_enc(const dsd_state* state);
int nxdn_cipher_established_clear(const dsd_state* state);
/* Whether the applied encrypted cipher is also what the air itself corroborated (the over-the-air-only
 * classification agrees and is established): a forced cipher is the decoder's choice, never received. */
int nxdn_cipher_received_enc(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_ */
