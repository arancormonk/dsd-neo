// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The D-STAR slow-data assemblies (the D-PRS record and the text message being collected), held in the D-STAR
 * slow-data state extension slot (DSD_STATE_EXT_PROTO_DSTAR_SLOW_DATA, issue #800).
 *
 * Decoder-thread private: no frontend or snapshot reads it (the text being shown is dsd_state::dstar_txt, an admitted
 * position dsd_state::dstar_gps). The transmission they are bound to (dsd_state::dstar_tx_seq) and the call a checked
 * header named (dsd_state::dstar_tx_call_epoch) stay in dsd_state, where the core's carrier end advances the
 * transmission: an assembly bound to the transmission before resets at its next use.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DSTAR_DSTAR_SD_STATE_H_
#define DSD_NEO_SRC_PROTOCOL_DSTAR_DSTAR_SD_STATE_H_

#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The longest D-STAR D-PRS (GPS-A) record kept, "$$CRC" through its closing CR. */
#define DSTAR_DPRS_RECORD_MAX 300U

/**
 * What a D-STAR slow-data assembly (a D-PRS record, the text message) is bound to from its first byte or block
 * (dstar_slow_data.c): the transmission (dsd_state::dstar_tx_seq), the call a checked header of that transmission named
 * on the current reception (dstar_tx_named_call(), 0: none), whether the transmission was confirmed
 * (dsd_state::dstar_confirmed) and the reception (dsd_position_origin_capture()). A block heard when any of them has
 * changed resets the assembly before it is used: nothing assembled across two transmissions, two receptions, a header
 * naming another call or the confirmation reaches the evidence, a position or a call.
 */
typedef struct dstar_sd_binding {
    uint32_t tx;
    uint64_t call_epoch;
    dsd_position_origin origin;
    uint8_t confirmed;
} dstar_sd_binding;

/**
 * One D-STAR D-PRS (GPS-A) record being collected from the GPS blocks (type 3) of the slow data (dstar_slow_data.c):
 * from "$$CRC" through the CR that ends it (JARL STD 6.0 section 6.8.1). It spans superframes, so it is bound
 * (dstar_sd_binding) from its first byte, the hunt for "$$CRC" included. A record that grows past DSTAR_DPRS_RECORD_MAX
 * bytes is dropped, and one whose CR was lost gives way to the next "$$CRC". A changed binding, an RF header or the
 * carrier's end (dstar_confirm_reset()) clears it.
 */
typedef struct dstar_dprs_assembly {
    uint8_t text[DSTAR_DPRS_RECORD_MAX]; /* until "$$CRC" matched: the last bytes seen, at most five */
    uint16_t len;
    uint8_t collecting;    /* 1 once "$$CRC" began a record */
    dstar_sd_binding bind; /* captured with the first byte (len 0 -> 1) */
} dstar_dprs_assembly;

/** The D-STAR text message being assembled from the slow data's text blocks (dsd_state::dstar_txt holds its text): the
 * blocks held (bit n: the block at place n) and what they are bound to (dstar_sd_binding), captured with the first. */
typedef struct dstar_text_assembly {
    uint8_t places;
    dstar_sd_binding bind;
} dstar_text_assembly;

/** The D-STAR slow-data state extension. */
typedef struct dstar_sd_state {
    dstar_dprs_assembly dprs;
    dstar_text_assembly text;
} dstar_sd_state;

/**
 * @brief The state's slow-data assemblies, created empty on first use.
 * @return NULL when @p state is NULL or the extension could not be created: the caller then assembles nothing.
 */
dstar_sd_state* dstar_sd_st(dsd_state* state);

/** @brief Empty both assemblies, when the extension exists (never creates it). */
void dstar_sd_state_clear(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_DSTAR_DSTAR_SD_STATE_H_ */
