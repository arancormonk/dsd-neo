// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral text for the system identity received on the carrier being decoded.
 *
 * Built from the received identity record alone (dsd_state::rx_id, <dsd-neo/core/received_identity.h>), never from
 * decoder state that seeds, presets and other carriers also write, so the Spectrum and Monitor screens, the terminal
 * and the Android app name the same system the same way. The identity is a list of tokens, the access code first
 * where the protocol has one ("CC 1", "Rest LSN 3", "Free LCN 4"; "NAC 293", "SYS 348", "RFSS 3", "SITE 21",
 * "WACN BEE00"; "RAN 5"; "MCC 262", "MNC 1001", "CC 25"). The compact tokens identify the system on one short line, most useful first, so a
 * narrow line keeps the leading ones; the full form adds details (Tier III system code, NXDN category, LRA, EDACS
 * addressing).
 *
 * The record form (dsd_app_site_identity_view_record()) reads a record alone, with no decoder state: a call history
 * row's frozen copy names its site in the Monitor's words (dsd_app_site_history_text()). The live view is the record
 * form plus what only the running decoder knows: the EDACS ESK setting and whether the carrier is live.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SITE_IDENTITY_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SITE_IDENTITY_VIEW_H_

#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSD_APP_SITE_IDENTITY_MAX_TOKENS 12
/** @brief Capacity of one token's text, terminator included. */
#define DSD_APP_SITE_IDENTITY_TOKEN_SIZE 72
/** @brief A buffer for a history row's site line (dsd_app_site_history_text()), as every frontend sizes it. */
#define DSD_APP_SITE_HISTORY_TEXT_SIZE   (DSD_APP_SITE_IDENTITY_MAX_TOKENS * DSD_APP_SITE_IDENTITY_TOKEN_SIZE)

/** @brief One part of the identity. UTF-8, always terminated. */
typedef struct {
    char text[DSD_APP_SITE_IDENTITY_TOKEN_SIZE];
    uint8_t compact; /**< 1 = shown on the compact line too; 0 = full form only. */
} dsd_app_site_identity_token;

/** @brief Display-ready identity of the carrier being decoded. */
typedef struct {
    /** "DMR", "P25", "NXDN", "IDAS", "dPMR", "M17", "TETRA", "EDACS", or "" when nothing was received. */
    char protocol[8];
    uint8_t family; /**< dsd_rxid_family */
    /** 1 while frame sync is on the record's protocol and some part was received since the last no-carrier pass;
     * 0 while it shows what was last heard on this carrier. */
    uint8_t live;
    uint32_t epoch;      /**< The record's epoch: it changes whenever the record was reset or replaced. */
    uint32_t cc_freq_hz; /**< The system's control channel, where this receiver heard it; 0 = not known. */
    size_t token_count;
    dsd_app_site_identity_token tokens[DSD_APP_SITE_IDENTITY_MAX_TOKENS];
} dsd_app_site_identity;

/**
 * @brief Fill @p out from @p state's received identity record.
 *
 * Zeroes @p out first; a NULL @p state, or a record with nothing received, leaves no protocol and no tokens.
 */
void dsd_app_site_identity_view(const dsd_state* state, dsd_app_site_identity* out);

/**
 * @brief Fill @p out from the received identity record @p id alone, as dsd_app_site_identity_view() names it.
 *
 * Reads no decoder state: the EDACS addressing word comes from the record (dsd_received_identity::edacs_ea_mode), the
 * live-only ESK word is left out, and dsd_app_site_identity::live stays 0. Zeroes @p out first; a NULL @p id, or a
 * record with nothing received, leaves no protocol and no tokens.
 */
void dsd_app_site_identity_view_record(const dsd_received_identity* id, dsd_app_site_identity* out);

/** @brief Which tokens dsd_app_site_identity_format() joins. */
enum {
    DSD_APP_SITE_IDENTITY_FULL = 0,    /**< Every token. */
    DSD_APP_SITE_IDENTITY_COMPACT = 1, /**< Compact tokens only. */
};

/**
 * @brief Join @p view's tokens with " · " (U+00B7), optionally after the protocol.
 *
 * @param view          A filled view; NULL writes "".
 * @param form          DSD_APP_SITE_IDENTITY_FULL or DSD_APP_SITE_IDENTITY_COMPACT.
 * @param with_protocol Non-zero to lead with the protocol ("P25 · NAC 293 · …").
 * @param out           Receives the text, cut short and terminated when it does not fit; NULL writes nothing.
 * @param out_size      Capacity of @p out.
 * @return The length written ("" when there are no tokens to show, even with @p with_protocol).
 */
size_t dsd_app_site_identity_format(const dsd_app_site_identity* view, int form, int with_protocol, char* out,
                                    size_t out_size);

/**
 * @brief The call's own channel as a token: "LSN 5", "LPCN 12", "LCN 3" (decimal).
 *
 * @param kind    dsd_call_channel_kind (<dsd-neo/core/call_channel.h>).
 * @param channel The channel number in that numbering.
 * @param buf     Receives the token, cut short and terminated when it does not fit; NULL writes nothing.
 * @param cap     Capacity of @p buf.
 * @return The length written; "" (0) for kind NONE, an unknown kind or channel 0.
 */
size_t dsd_app_call_channel_token(uint8_t kind, uint32_t channel, char* buf, size_t cap);

/**
 * @brief The site line of a call history row: the record form of @p id, full and led by the protocol, then the
 * call's channel token, joined with " · ".
 *
 * Only DMR names a channel kind, so a row with a channel and no identity reads "DMR · LPCN 12".
 *
 * @param id      The row's site identity; NULL is a row with none.
 * @param kind    dsd_call_channel_kind of @p channel.
 * @param channel The call's channel number.
 * @param buf     Receives the line, cut short and terminated when it does not fit; NULL writes nothing.
 * @param cap     Capacity of @p buf.
 * @return The length written; "" (0) when neither the identity nor the channel has anything to show.
 */
size_t dsd_app_site_history_text(const dsd_received_identity* id, uint8_t kind, uint32_t channel, char* buf,
                                 size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SITE_IDENTITY_VIEW_H_ */
