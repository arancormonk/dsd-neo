// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief M17 protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The four RF frame decoders return non-zero when the frame validated something -- a CRC, a
 * LICH, a PRBS9 lock -- or when an earlier frame in the same transmission already did. A zero
 * means the decoder consumed a frame's worth of symbols and proved nothing, which is what the
 * SPS hunt needs to hear to stop crediting a false sync (#399, #419). */
int processM17STR(dsd_opts* opts, dsd_state* state);
int processM17PKT(dsd_opts* opts, dsd_state* state);
int processM17LSF(dsd_opts* opts, dsd_state* state);
int processM17BRT(dsd_opts* opts, dsd_state* state);
int processM17IPF(dsd_opts* opts, dsd_state* state);

/** @brief Forget what the current M17 transmission proved; the next one proves itself again. */
void m17_confirm_reset(dsd_state* state);

/** @brief Whether the current M17 transmission has produced CRC-verified content. */
int m17_confirm_is_confirmed(const dsd_state* state);
int encodeM17STR(dsd_opts* opts, dsd_state* state);
void encodeM17BRT(dsd_opts* opts, dsd_state* state);
int encodeM17PKT(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_H_H */
