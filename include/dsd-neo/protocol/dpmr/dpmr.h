// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief dPMR interfaces the layers above the protocol need.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forget the CRC evidence gathered for the current dPMR transmission.
 *
 * Defined in src/protocol/dpmr/dpmr_confirm.c and declared here so the engine can clear it
 * with the carrier; the rest of that module's interface is private to the protocol.
 */
void dpmr_confirm_reset(dsd_state* state);

/**
 * @brief Whether the current dPMR transmission has produced a CRC-verified CCH.
 *
 * The sticky per-transmission flag, not this frame's evidence. Dispatch reads it to decide
 * whether a sync alone may open a call row, which it may not until something has decoded.
 */
int dpmr_confirm_is_confirmed(const dsd_state* state);

/**
 * @brief A dPMR frame whose sync the decoder accepts but which carries no check of its own (FS4, or an FS1 or FS3
 * synctype no checked header or end frame stands behind) to a scan quiet check, through @p opts (issue #834): PARTIAL, held for
 * DPMR_CONFIRM_PARTIAL_HOLD_MS, so a key-up just after a quiet reading waits for its first CCH verdict. @p opts may be
 * NULL (no quiet check).
 */
void dpmr_confirm_note_unchecked_frame(const dsd_opts* opts, dsd_state* state);

/** @brief Message types of a dPMR header (ETSI TS 102 658 V2.6.1 Table 5.3) this decoder names. */
enum {
    DPMR_MT_COMMUNICATIONS_START = 0x0U, /**< Opens a voice or data transmission, or an unsolicited status (5.2.9). */
    DPMR_MT_CONNECTION_REQUEST = 0x1U,
    DPMR_MT_DISCONNECT_REQUEST = 0x2U,
    DPMR_MT_ACK = 0x3U,
    DPMR_MT_MAINTENANCE_OR_SYSTEM_REQUEST = 0x4U,
    DPMR_MT_SYSTEM_ACK = 0x5U,
    DPMR_MT_SYSTEM_DELIVERY = 0x6U,
    DPMR_MT_STATUS_POLLING_RESPONSE = 0x7U,
};

/** @brief A dPMR header (Message_Frame) whose MI passed its CRC-8 (TS 102 658 5.2, 11.5): its MT and PARMS. */
typedef struct {
    uint32_t message_type; /**< MT (DPMR_MT_*) */
    uint32_t called_id;    /**< ID0 + 1: the called station's 24-bit air interface ID (A.1.2.1.1.6) */
    uint32_t calling_id;   /**< ID2 + 3 */
    uint32_t mode;         /**< M, Communications Mode (Table 5.55) */
    uint32_t version;      /**< V (Table 5.111) */
    uint32_t format;       /**< F, Comms Format (5.5.6) */
    uint32_t emergency;    /**< EP */
    uint32_t mi_type;      /**< MI_TYPE */
    uint32_t mi_detail;    /**< MI_DET */
    int color_code;        /**< The channel code between MI0 and MI1 as a colour code, or -1 for none (6.1.5) */
    uint8_t copies;        /**< DSD_DPMR_SYNC_COPY_*: the MI copies that passed */
} dpmr_header;

/** @brief A dPMR End_Frame whose END passed its CRC-7 (TS 102 658 5.3, 11.6). */
typedef struct {
    uint32_t end_type; /**< ET (Table 5.61): 0 normal, 1 with a status message */
    uint32_t ack_request;
    uint32_t tx_wait;
    uint32_t status; /**< STAT, a Status_Code when ET is 1 */
    uint8_t copies;  /**< DSD_DPMR_SYNC_COPY_* */
} dpmr_end_frame;

/**
 * @brief Take the header the frame sync checked for the FS1 sync being dispatched (dsd_dpmr_frame_sync::checked_*).
 *
 * 1 with @p out filled when the sync was accepted on a checked header at this symbol, once; 0 otherwise (an FS1 sync
 * no header checked behind, which nothing on air produces).
 */
int dpmr_take_checked_header(dsd_state* state, dpmr_header* out);

/** @brief Take the end frame the frame sync checked for the FS3 sync being dispatched, as dpmr_take_checked_header(). */
int dpmr_take_checked_end(dsd_state* state, dpmr_end_frame* out);

/**
 * @brief Whether a checked header opens a call: a Communications_Start header (MT 0000) whose Communications Mode is
 * voice or data (Table 5.55: 000 to 101). Every other message type (a connection or disconnect request, an ACK, a
 * maintenance, system or status message) and mode 110/111 open none.
 */
int dpmr_header_opens_call(const dpmr_header* header);

/**
 * @brief Act on a checked header: it confirms the transmission and, when it opens a call (dpmr_header_opens_call()),
 * publishes that call with its checked identity and service, colour code and scrambling, once. The caller ends the
 * previous call and forgets its scrambling first.
 */
void dpmr_process_header(dsd_opts* opts, dsd_state* state, const dpmr_header* header);

/** @brief Report a checked end frame (printed, and to a scan quiet check and a signal search as checked content). */
void dpmr_process_end(const dsd_opts* opts, dsd_state* state, const dpmr_end_frame* end);

/** @brief dPMRVoiceFS2Frame_t::Scrambled values (issue #655). */
enum {
    DPMR_HALF_CLEAR = 0U,      /**< Clear voice. */
    DPMR_HALF_SCRAMBLED = 1U,  /**< Scrambled voice (Version 3). */
    DPMR_HALF_UNVERIFIED = 2U, /**< No verified half says which since the transmission's boundary. */
};

/**
 * @brief Forget the scrambling the current transmission's CRC-verified halves signalled (issue #655).
 *
 * A half whose CRC-7 fails is played by the transmission's last verified half, so a transmission boundary the engine
 * sees (a checked header that opens a call, a checked end frame) must not hand one call's verdict to the next.
 */
void dpmr_scrambling_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_ */
