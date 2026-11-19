// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief The IV an NXDN call received over the air (dsd_state::nxdn_rx_iv), apart from the working register.
 *
 * NXDN keeps one working value for its ciphers (dsd_state::payload_miN): a DES or AES call's IV, but a scrambled
 * call's 15-stage register, which the key loads (NXDN TS 1-D §5.3.1, TS 1-F §16.4.2). Only an IV the air carried (a
 * CRC-verified VCALL_IV) is ever a call's MI, its decryption IV or what the terminal shows, and it fails closed: it
 * counts only if it came while its call was open on the current carrier, and only for that call. Nothing is kept for a
 * call not yet open; a call, target, Type-D destination prefix or carrier change drops it. A DES or AES call is
 * decryptable only once its keystream session started from such an IV (dsd_state::nxdn_ks_session_transmission).
 */

#ifndef DSD_NEO_CORE_NXDN_RECEIVED_IV_H
#define DSD_NEO_CORE_NXDN_RECEIVED_IV_H

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The transmission of the active NXDN call on slot 0 (dsd_call_snapshot_transmission()), 0 when there is none. */
static inline uint64_t
dsd_nxdn_active_call_transmission(const dsd_state* state) {
    dsd_call_snapshot call;
    if (state == NULL || dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_NXDN(call.protocol)) {
        return 0U;
    }
    return dsd_call_snapshot_transmission(&call);
}

/** Whether the received IV is the active NXDN call's: received in that call's transmission on this carrier. The IV in
 * @p iv_out when it is. */
static inline int
dsd_nxdn_received_iv_for_active_call(const dsd_state* state, unsigned long long* iv_out) {
    if (state == NULL || state->nxdn_rx_iv_valid == 0U || state->nxdn_rx_iv_carrier != state->carrier_seq) {
        return 0;
    }
    const uint64_t transmission = dsd_nxdn_active_call_transmission(state);
    if (transmission == 0U || state->nxdn_rx_iv_transmission != transmission) {
        return 0;
    }
    if (iv_out != NULL) {
        *iv_out = state->nxdn_rx_iv;
    }
    return 1;
}

/** Whether the running DES/AES keystream session started from the active NXDN call's received IV. */
static inline int
dsd_nxdn_keystream_session_for_active_call(const dsd_state* state) {
    return dsd_nxdn_received_iv_for_active_call(state, NULL) && state->nxdn_ks_session_transmission != 0U
           && state->nxdn_ks_session_transmission == state->nxdn_rx_iv_transmission;
}

/** Whether the cipher the call selected is DES (2) or AES (3) with a key for it loaded. */
static inline int
dsd_nxdn_cipher_key_present(const dsd_state* state) {
    if (state->nxdn_cipher_type == 0x02) {
        return dsd_key_scalar_present(state, 0);
    }
    if (state->nxdn_cipher_type == 0x03) {
        return dsd_key_aes_slot_complete(state, 0, 4U);
    }
    return 0;
}

/**
 * Arm the DES/AES keystream once the cipher, a key for it and the active call's received IV are all present, whatever
 * order they came in: the working IV (and AES's expanded one) takes the received IV, and the keystream starts at the
 * next session start (dsd_state::nxdn_new_iv, dsd_mbe.c). A session already running from that IV, or one armed, is left
 * alone. Returns whether it armed.
 */
static inline int
dsd_nxdn_cipher_arm_if_ready(dsd_state* state) {
    unsigned long long iv = 0ULL;
    if (state == NULL || state->nxdn_new_iv != 0U || !dsd_nxdn_cipher_key_present(state)
        || !dsd_nxdn_received_iv_for_active_call(state, &iv)
        || state->nxdn_ks_session_transmission == state->nxdn_rx_iv_transmission) {
        return 0;
    }
    state->payload_miN = iv;
    for (size_t i = 0U; i < sizeof(state->aes_iv); i++) {
        state->aes_iv[i] = state->nxdn_rx_iv_aes[i];
    }
    state->nxdn_new_iv = 1U;
    return 1;
}

/** Drop the received IV and the keystream session started from it (a call, target or carrier change, an end). */
static inline void
dsd_nxdn_received_iv_clear(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->nxdn_rx_iv = 0ULL;
    state->nxdn_rx_iv_transmission = 0U;
    state->nxdn_rx_iv_carrier = 0U;
    state->nxdn_rx_iv_valid = 0U;
    for (size_t i = 0U; i < sizeof(state->nxdn_rx_iv_aes); i++) {
        state->nxdn_rx_iv_aes[i] = 0U;
    }
    state->nxdn_ks_session_transmission = 0U;
    state->nxdn_new_iv = 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_CORE_NXDN_RECEIVED_IV_H */
