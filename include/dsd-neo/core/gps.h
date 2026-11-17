// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief GPS/NMEA decode interfaces.
 *
 * Declares GPS helper entrypoints implemented in `src/core/gps/dsd_gps.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dsd_position_origin;
struct dsd_event_closing_call;

/** Octets in an L3Harris GPS update body: Part A octets 2-8, then Part B octets 2-8 (TIA-102.AABF-D-1 §7.3.35-36). */
#define DSD_HARRIS_GPS_BODY_OCTETS 14

/**
 * Where a P25 link-control or MAC GPS report came from and which checks it passed (issue #800). The decoder admits
 * the report with these verdicts (dsd_position_admit()); nothing reaches `-L`, history or the feed without
 * @c pos_verified, and the source, target and group flag only with @c source_verified.
 */
typedef struct dsd_gps_report_ctx {
    /** The reception captured with the report's first fragment (L3Harris Part A); NULL for a single message, which
     * takes the current reception. */
    const struct dsd_position_origin* origin;
    /** The call the message carrying the report has just ended, captured before it did (dsd_event_capture_closing_call();
     * a P25 Phase 1 TDULC, spec N2): when valid, the history row is written through
     * dsd_event_enrich_position_closing(). NULL otherwise (Phase 2). */
    const struct dsd_event_closing_call* closing;
    /** The call the report belongs to: its epoch (dsd_call_snapshot::epoch); 0 enriches the slot's ACTIVE call, if
     * any. Ignored when @c closing is valid. */
    uint64_t call_epoch;
    uint32_t source_id;      /**< the call's source; 0 = none */
    uint32_t target_id;      /**< the call's target; 0 = none */
    int protocol;            /**< DSD_SYNC_* of the carrier */
    uint8_t slot;            /**< lane: 0 or 1 */
    uint8_t pos_verified;    /**< 1 when the message's own check passed (hard RS decode, MAC CRC, whole segment) */
    uint8_t source_verified; /**< 1 when source, target and group flag came from checked provenance */
    uint8_t target_group;    /**< 1 when @c target_id is a talkgroup */
} dsd_gps_report_ctx;

void dmr_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]);
/**
 * Motorola MFID $90 LCO $06 embedded GPS (layout from one non-official source: FORMAT_PROVISIONAL, D12).
 * @return the dsd_position_admit() result (0 when nothing was admitted).
 */
int apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[], const dsd_gps_report_ctx* ctx);
void lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* input);
void lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count, uint32_t src);
void nmea_iec_61162_1(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int type);
uint8_t nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot,
                              int len_bytes);
void nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src);
/**
 * L3Harris GPS update (TIA-102.AABF-D-1 §7.3.35-36), Phase 1 link control or Phase 2 MAC.
 *
 * @param body DSD_HARRIS_GPS_BODY_OCTETS octets: Part A octets 2-8 then Part B octets 2-8 (on Phase 2, the 14 octets
 *        after the `AA A4 11` header).
 * @param ctx Provenance and verdicts; NULL decodes and prints only.
 * @return the dsd_position_admit() result: 0 when nothing was admitted (no fix, invalid fields, unchecked message).
 */
int nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* body, const dsd_gps_report_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_ */
