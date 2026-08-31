// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief ProVoice protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_PROVOICE_PROVOICE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_PROVOICE_PROVOICE_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode one ProVoice frame.
 *
 * @return 1 once this transmission has proved itself and 0 until then. Nothing inside the
 *         736 dibits a frame consumes can fail a check -- there is no CRC, no BCH and no
 *         parity, and the IMBE error counts are soft corrections -- so the evidence is that
 *         a second frame arrived behind its own exact 32-symbol sync word. A lone false
 *         match buys no dwell on the 9600/2 profile ProVoice shares with EDACS, which
 *         already reports its own BCH verdict (#391, #421). The evidence lives in
 *         dsd_state::provoice_confirmed and clears with the carrier.
 */
int processProVoice(dsd_opts* opts, dsd_state* state);

/**
 * @brief Forget the evidence gathered for the current ProVoice transmission.
 *
 * Defined in src/protocol/provoice/provoice_confirm.c and declared here so the engine can
 * clear it with the carrier; the rest of that module's interface is private to the protocol.
 */
void provoice_confirm_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_PROVOICE_PROVOICE_H_ */
