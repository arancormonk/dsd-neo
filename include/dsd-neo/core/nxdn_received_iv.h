// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief The IV an NXDN call received over the air (dsd_state::nxdn_rx_iv), apart from the working register.
 *
 * NXDN keeps one working value for its ciphers (dsd_state::payload_miN): a DES or AES call's IV, but a scrambled
 * call's 15-stage register, which the key loads (NXDN TS 1-D §5.3.1, TS 1-F §16.4.2). Only an IV the air carried (a
 * CRC-verified VCALL_IV) is ever a call's MI, its decryption IV or what the terminal shows, and only for the call it
 * came for: the active NXDN call on slot 0 whose transmission it is bound to, or, for one received while no call was
 * open yet (late entry), the call that opens next on the same carrier.
 */

#ifndef DSD_NEO_CORE_NXDN_RECEIVED_IV_H
#define DSD_NEO_CORE_NXDN_RECEIVED_IV_H

#include <dsd-neo/core/call_state.h>
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

/** Whether the received IV is the active NXDN call's (bound to its transmission, or waiting for the first call after
 * it); the IV in @p iv_out when it is. */
static inline int
dsd_nxdn_received_iv_for_active_call(const dsd_state* state, unsigned long long* iv_out) {
    if (state == NULL || state->nxdn_rx_iv_valid == 0U) {
        return 0;
    }
    const uint64_t transmission = dsd_nxdn_active_call_transmission(state);
    if (transmission == 0U
        || (state->nxdn_rx_iv_transmission != 0U && state->nxdn_rx_iv_transmission != transmission)) {
        return 0;
    }
    if (iv_out != NULL) {
        *iv_out = state->nxdn_rx_iv;
    }
    return 1;
}

/** Forget the received IV (a carrier or transmission boundary). */
static inline void
dsd_nxdn_received_iv_clear(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->nxdn_rx_iv = 0ULL;
    state->nxdn_rx_iv_transmission = 0U;
    state->nxdn_rx_iv_valid = 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_CORE_NXDN_RECEIVED_IV_H */
